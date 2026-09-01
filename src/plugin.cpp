// gen_discord_rpc - Winamp general purpose plugin publishing the current track
// to Discord as a Rich Presence status, with album art and a progress bar.
//
// Threading: Winamp calls init()/config()/quit() on its UI thread. All polling,
// network I/O and pipe traffic happens on a single worker thread so the UI
// thread is never blocked.

#include "../third_party/winamp/gen.h"
#include "../third_party/winamp/wa_ipc_min.h"

#include "artwork.h"
#include "config.h"
#include "coverart.h"
#include "discord_ipc.h"
#include "upload.h"
#include "util.h"
#include "winamp_state.h"

#include <objbase.h>
#include <string>

HWND      g_hwndWinamp = nullptr;
HINSTANCE g_hInstance  = nullptr;

namespace {

// How often we sample Winamp. Only affects how quickly a track change is
// noticed -- presence is pushed on change, not on every tick.
const DWORD kPollIntervalMs = 500;

// Discord rate-limits SET_ACTIVITY (roughly 5 per 20s). Stay well inside it.
const DWORD kMinUpdateIntervalMs = 2000;

// A jump larger than this between predicted and actual position means the user
// seeked, so the timestamps must be rebuilt.
const int kSeekToleranceMs = 2500;

// Backoff between attempts to find a running Discord client.
const DWORD kReconnectIntervalMs = 10000;

HANDLE          g_workerThread = nullptr;
HANDLE          g_stopEvent    = nullptr;
DiscordIpc      g_discord;
ArtworkResolver g_artwork;

// Trims a UTF-8 string to Discord's per-field limit of 128 characters, counted
// in characters rather than bytes, without splitting a multi-byte sequence.
std::string ClampField(const std::wstring& text, size_t maxChars = 128) {
    std::wstring s = util::Trim(text);
    if (s.size() > maxChars) {
        s.resize(maxChars - 1);
        s += L'\x2026';   // horizontal ellipsis, escaped so the source encoding
                          // cannot change what the compiler sees
    }
    return util::ToUtf8(s);
}

// Discord rejects `details` and `state` shorter than 2 characters, and a
// single-character field is almost certainly junk anyway.
std::string ClampFieldMin2(const std::wstring& text) {
    std::wstring s = util::Trim(text);
    if (s.size() == 1) s += L' ';
    return ClampField(s);
}

// Set after Discord rejects a command, so the next attempt drops the newer
// optional fields. Degrading to a plain presence beats publishing nothing on a
// client build that validates them strictly.
bool g_dropOptionalFields = false;

bool BuildActivity(const TrackInfo& track, DiscordIpc::Activity* out) {
    DiscordIpc::Activity a;
    a.type = g_config.activityType;
    a.name = util::ToUtf8(g_config.activityName);
    a.statusDisplayType = g_dropOptionalFields ? -1 : g_config.statusDisplayType;

    a.details = ClampFieldMin2(track.title);

    std::wstring stateText = track.artist;
    if (track.state == PlayState::Paused) {
        stateText = stateText.empty() ? L"Paused" : stateText + L" (paused)";
    }
    a.state = ClampFieldMin2(stateText);

    if (!track.album.empty()) {
        a.largeText = ClampField(track.album);
    } else if (!track.artist.empty()) {
        a.largeText = ClampField(track.artist);
    }

    // Album art. Only the cache is consulted here so a track change publishes
    // instantly; extracting and uploading the real cover takes a second or two
    // and happens right after, triggering a refresh.
    std::string artUrl;
    if (g_config.showAlbumArt && !track.isStream &&
        g_artwork.TryCached(track.artist, track.album, track.title, &artUrl)) {
        a.largeImage = artUrl;
    } else {
        std::string placeholder;
        if (g_artwork.ResolveImageSetting(g_config.fallbackImage, &placeholder)) {
            a.largeImage = placeholder;
        }
    }

    // The small image rides in the bottom-right corner of the album art. Like
    // the placeholder it takes either a Discord asset key or a path to a local
    // file, so a Winamp badge can be set up without touching the dev portal.
    const std::wstring& smallKey = (track.state == PlayState::Paused)
                                       ? g_config.smallImagePaused
                                       : g_config.smallImagePlaying;
    if (!smallKey.empty()) {
        std::string smallRef;
        if (g_artwork.ResolveImageSetting(smallKey, &smallRef)) {
            a.smallImage = smallRef;
            a.smallText  = (track.state == PlayState::Paused) ? "Paused" : "Playing";
        }
    }

    // Timestamps. Supplying start AND end is what turns Discord's elapsed
    // counter into a Spotify-style progress bar, so we anchor both to the
    // current playback position. A paused track gets neither, which freezes the
    // bar instead of letting it run on without the audio.
    //
    // Winamp reports a briefly negative position while the next track's decoder
    // primes -- seen as pos=-68ms on a gapless auto-advance -- so clamp rather
    // than treat it as invalid.
    const int positionMs = (track.positionMs > 0) ? track.positionMs : 0;

    if (g_config.showProgressBar && track.state == PlayState::Playing && track.lengthMs > 0) {
        const long long now = util::UnixMillis();
        a.startMs = now - positionMs;
        a.endMs   = a.startMs + track.lengthMs;
    } else if (track.state == PlayState::Playing && track.isStream) {
        // A live stream genuinely has no end, so a count-up is the honest
        // display. Local files never land here: if their length has not settled
        // yet we publish with no timer at all and let the worker retry, rather
        // than flashing a bare counter where a bar belongs.
        a.startMs = util::UnixMillis() - positionMs;
    }

    *out = a;
    return !a.details.empty();
}

// True when this track should be showing a progress bar but the activity we
// built has no end timestamp -- i.e. Winamp had not reported a usable length
// yet, so the presence needs rebuilding once it does.
bool ProgressBarPending(const TrackInfo& track, const DiscordIpc::Activity& a) {
    return g_config.showProgressBar && track.state == PlayState::Playing &&
           !track.isStream && a.endMs == 0;
}

// Predicts where playback should be, to tell a seek apart from normal advance.
bool LooksLikeSeek(const TrackInfo& current, const TrackInfo& last, DWORD lastTick) {
    if (last.state != PlayState::Playing || current.state != PlayState::Playing) return false;
    // Clamp both ends: a negative reading around a track boundary would
    // otherwise show up as a multi-second jump and be mistaken for a seek.
    const int lastPos    = (last.positionMs > 0) ? last.positionMs : 0;
    const int currentPos = (current.positionMs > 0) ? current.positionMs : 0;
    const int elapsed    = (int)(GetTickCount() - lastTick);
    const int expected   = lastPos + elapsed;
    return abs(currentPos - expected) > kSeekToleranceMs;
}

DWORD WINAPI WorkerProc(LPVOID) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    TrackInfo             lastTrack;
    TrackInfo             diagLast;
    DiscordIpc::Activity  lastActivity;
    bool                  hasLastActivity = false;
    DWORD                 lastSampleTick  = GetTickCount();
    DWORD                 lastUpdateTick  = 0;
    DWORD                 lastConnectTry  = 0;
    bool                  presenceCleared = true;
    bool                  forceUpdate     = true;

    const std::string clientId = util::ToUtf8(g_config.clientId);

    while (WaitForSingleObject(g_stopEvent, kPollIntervalMs) == WAIT_TIMEOUT) {
        if (!g_config.enabled) continue;

        // With no Discord application configured there is nothing to publish
        // to, but keep reading Winamp so the debug log can confirm the player
        // side works before the user goes and creates one.
        if (clientId.empty()) {
            TrackInfo probe;
            if (g_config.logging && ReadWinampState(g_hwndWinamp, &diagLast, &probe) &&
                (probe.IsDifferentTrack(diagLast) || probe.state != diagLast.state)) {
                util::Log(L"no client id set; would publish: artist='%s' title='%s' "
                          L"album='%s' pos=%dms len=%dms stream=%d state=%d",
                          probe.artist.c_str(), probe.title.c_str(), probe.album.c_str(),
                          probe.positionMs, probe.lengthMs, probe.isStream ? 1 : 0,
                          (int)probe.state);
                diagLast = probe;
            }
            continue;
        }

        if (!g_discord.IsConnected()) {
            const DWORD now = GetTickCount();
            if (now - lastConnectTry < kReconnectIntervalMs) continue;
            lastConnectTry = now;
            if (!g_discord.Connect(clientId)) continue;
            // A fresh connection has no presence attached to it.
            hasLastActivity = false;
            presenceCleared = true;
            forceUpdate     = true;
        }

        if (!g_discord.Poll()) {
            util::Log(L"lost connection to discord, will retry");
            g_discord.Disconnect();
            continue;
        }

        // Discord rejected our last command. Retry once without the newer
        // optional fields before giving up on this connection.
        if (g_discord.TakeActivityError() && !g_dropOptionalFields) {
            util::Log(L"activity rejected; retrying without status_display_type");
            g_dropOptionalFields = true;
            forceUpdate          = true;
            hasLastActivity      = false;
        }

        TrackInfo track;
        if (!ReadWinampState(g_hwndWinamp, &lastTrack, &track)) continue;

        const DWORD sampleTick = GetTickCount();
        const bool  trackChanged = track.IsDifferentTrack(lastTrack);
        const bool  stateChanged = track.state != lastTrack.state;
        const bool  seeked       = LooksLikeSeek(track, lastTrack, lastSampleTick);
        const bool  lengthKnown  = track.lengthMs != lastTrack.lengthMs;

        lastTrack      = track;
        lastSampleTick = sampleTick;

        const bool shouldHide =
            track.state == PlayState::Stopped ||
            (track.state == PlayState::Paused && !g_config.showWhenPaused);

        if (shouldHide) {
            if (!presenceCleared) {
                if (g_discord.ClearActivity()) {
                    presenceCleared = true;
                    hasLastActivity = false;
                    util::Log(L"cleared presence");
                } else {
                    g_discord.Disconnect();
                }
            }
            continue;
        }

        if (!(trackChanged || stateChanged || seeked || lengthKnown || forceUpdate ||
              presenceCleared)) {
            continue;
        }

        // Honour the rate limit, but keep the pending change so the next tick
        // publishes it rather than dropping it.
        if (lastUpdateTick != 0 && (sampleTick - lastUpdateTick) < kMinUpdateIntervalMs) {
            forceUpdate = true;
            continue;
        }

        DiscordIpc::Activity activity;
        if (!BuildActivity(track, &activity)) continue;

        // Timestamps drift by a few ms every sample; ignore that when deciding
        // whether anything actually changed, or we would republish constantly.
        DiscordIpc::Activity comparable = activity;
        DiscordIpc::Activity lastComparable = lastActivity;
        comparable.startMs = comparable.endMs = 0;
        lastComparable.startMs = lastComparable.endMs = 0;

        // Zeroing both above also hides the one timestamp difference that does
        // matter: gaining or losing the bar itself. Compare that separately, or
        // a presence published without an end timestamp would look unchanged
        // forever and never get upgraded to a progress bar.
        const bool shapeSame = hasLastActivity &&
                               (activity.startMs > 0) == (lastActivity.startMs > 0) &&
                               (activity.endMs   > 0) == (lastActivity.endMs   > 0);

        const bool contentSame = hasLastActivity && shapeSame && comparable == lastComparable;
        if (contentSame && !seeked && !stateChanged && !forceUpdate) continue;

        if (!g_discord.SetActivity(activity)) {
            util::Log(L"SetActivity failed, dropping connection");
            g_discord.Disconnect();
            continue;
        }

        lastActivity    = activity;
        hasLastActivity = true;
        presenceCleared = false;
        lastUpdateTick  = sampleTick;
        forceUpdate     = false;

        util::Log(L"presence: %s - %s [%s] pos=%dms len=%dms bar=%d",
                  track.artist.c_str(), track.title.c_str(), track.album.c_str(),
                  track.positionMs, track.lengthMs, activity.endMs > 0 ? 1 : 0);

        // Winamp does not always know a track's length the instant it starts.
        // Keep asking until it does, so the bar appears a beat later instead of
        // the presence being stuck without one until the next track.
        if (ProgressBarPending(track, activity)) {
            util::Log(L"length not settled yet; will retry the progress bar");
            forceUpdate = true;
        }

        // The presence above went out with the placeholder because the cover
        // was not cached yet. Resolve it for real now -- extracting the file's
        // artwork and uploading it blocks for a moment -- then schedule one
        // refresh so the correct cover replaces the placeholder.
        if (g_config.showAlbumArt && !track.isStream &&
            !g_artwork.IsFresh(track.artist, track.album, track.title)) {
            std::string url;
            if (g_artwork.Resolve(track.artist, track.album, track.title, track.file, &url) &&
                url != activity.largeImage) {
                forceUpdate = true;
            }
        }

        g_artwork.SaveCache();
    }

    if (g_discord.IsConnected()) {
        g_discord.ClearActivity();
        g_discord.Disconnect();
    }
    g_artwork.SaveCache();
    CoUninitialize();
    return 0;
}

void StartWorker() {
    if (g_workerThread) return;
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent) return;
    g_workerThread = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
}

void StopWorker() {
    if (!g_workerThread) return;
    SetEvent(g_stopEvent);
    // The worker can be mid-HTTP-request; give it room, then stop waiting so a
    // hung network call cannot block Winamp's shutdown.
    if (WaitForSingleObject(g_workerThread, 10000) == WAIT_TIMEOUT) {
        util::Log(L"worker did not exit in time");
    }
    CloseHandle(g_workerThread);
    CloseHandle(g_stopEvent);
    g_workerThread = nullptr;
    g_stopEvent    = nullptr;
}

int  PluginInit();
void PluginConfig();
void PluginQuit();

winampGeneralPurposePlugin g_plugin = {
    GPPHDR_VER,
    (char*)"Discord Rich Presence v1.0.0",
    PluginInit,
    PluginConfig,
    PluginQuit,
    nullptr,
    nullptr,
};

// Pushes the current configuration into the artwork resolver.
void ApplyArtworkOptions() {
    ArtworkResolver::Options opts;
    opts.useEmbedded     = g_config.useEmbeddedArt;
    opts.useOnlineLookup = g_config.useOnlineLookup;
    opts.host            = upload::HostFromString(g_config.uploadHost);
    opts.expiryHours     = g_config.uploadExpiryHours;
    opts.maxEdge         = g_config.artMaxEdge;
    g_artwork.SetOptions(opts);
}

int PluginInit() {
    g_hwndWinamp = g_plugin.hwndParent;
    g_hInstance  = g_plugin.hDllInstance;

    g_config.Load(GetSettingsPath(g_hwndWinamp));
    util::SetLogFile(GetLogPath(g_hwndWinamp));
    util::SetLoggingEnabled(g_config.logging);
    coverart::Startup();
    g_artwork.LoadCache(GetCachePath(g_hwndWinamp));
    ApplyArtworkOptions();

    util::Log(L"plugin loaded (client id %s)",
              g_config.clientId.empty() ? L"<not set>" : g_config.clientId.c_str());

    StartWorker();
    return 0;
}

void PluginConfig() {
    ShowConfigDialog(g_hwndWinamp, g_hInstance);
}

void PluginQuit() {
    StopWorker();
    coverart::Shutdown();
    util::Log(L"plugin unloaded");
}

} // namespace

// Called from the settings dialog after the configuration changes, so the
// worker picks up a new client ID or a toggled "enabled" flag.
void RestartWorker() {
    StopWorker();
    g_artwork.LoadCache(GetCachePath(g_hwndWinamp));
    ApplyArtworkOptions();
    StartWorker();
}

extern "C" __declspec(dllexport) winampGeneralPurposePlugin* winampGetGeneralPurposePlugin() {
    return &g_plugin;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInstance = (HINSTANCE)module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
