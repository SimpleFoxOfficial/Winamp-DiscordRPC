#pragma once

#include <windows.h>
#include <string>

struct Config {
    bool         enabled           = true;
    std::wstring clientId;                       // Discord application ID (required)
    std::wstring activityName      = L"Winamp";  // renders as "Listening to <name>"
    int          activityType      = 2;          // 0 Playing, 2 Listening, 3 Watching, 5 Competing
    bool         showAlbumArt      = true;      // master switch for cover art
    bool         useEmbeddedArt    = true;      // read art from the file, upload it
    bool         useOnlineLookup   = true;      // fall back to iTunes/Deezer
    std::wstring uploadHost        = L"litterbox";
    int          uploadExpiryHours = 72;        // litterbox only: 1, 12, 24 or 72
    int          artMaxEdge        = 512;
    bool         showProgressBar   = true;
    bool         showWhenPaused    = true;
    int          statusDisplayType = 2;          // 0 name, 1 state, 2 details, -1 omit
    // Either a Discord asset key, or a path to a local image which the plugin
    // uploads once and caches.
    std::wstring fallbackImage     = L"winamp";
    std::wstring smallImagePlaying = L"play";
    std::wstring smallImagePaused  = L"pause";
    bool         logging           = false;

    void Load(const std::wstring& iniPath);
    void Save(const std::wstring& iniPath) const;
};

extern Config g_config;

// <winamp ini dir>\Plugins\gen_discord_rpc.ini
std::wstring GetSettingsPath(HWND hwndWinamp);
std::wstring GetCachePath(HWND hwndWinamp);
std::wstring GetLogPath(HWND hwndWinamp);

// Modal settings dialog shown from Winamp's "Configure" button.
void ShowConfigDialog(HWND parent, HINSTANCE instance);
