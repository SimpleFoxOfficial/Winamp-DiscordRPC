#include "winamp_state.h"
#include "util.h"
#include "../third_party/winamp/wa_ipc_min.h"

#include <algorithm>

namespace {

// For IPCs whose arguments are plain scalars. Abandoning one of these on
// timeout is harmless: Winamp is left holding nothing of ours, so the worst
// case is a stale reading on this tick.
LRESULT IpcSend(HWND hwnd, WPARAM wParam, LPARAM lParam) {
    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(hwnd, WM_WA_IPC, wParam, lParam,
                             SMTO_ABORTIFHUNG | SMTO_NORMAL, 2000, &result)) {
        return 0;
    }
    return (LRESULT)result;
}

// Everything a metadata query hands to Winamp, in one heap block.
//
// The struct, the filename it points at and the buffer Winamp writes into must
// all outlive the call *even if we stop waiting for it*. SendMessageTimeout
// only stops us waiting -- the message stays queued and Winamp may service it
// much later. If any of this lived on the stack, Winamp would then write its
// answer into a frame the worker thread had already reused, corrupting whatever
// now occupies it. Switching playlists keeps Winamp's main thread busy for
// seconds, which is exactly when a timeout fires, so this is a real race and
// not a theoretical one.
struct ExtInfoRequest {
    extendedFileInfoStructW efs;
    wchar_t filename[2048];
    wchar_t metadata[64];
    wchar_t ret[1024];
};

// Generous, because abandoning one of these is the expensive case: the block
// then has to be leaked. Still well inside the worker's shutdown budget.
const UINT kExtInfoTimeoutMs = 5000;

// Copies out of Winamp's buffer under SEH. Kept free of C++ objects because
// MSVC refuses __try in any function that requires object unwinding (C2712).
size_t SafeCopyW(const wchar_t* src, wchar_t* dst, size_t dstChars) {
    __try {
        size_t i = 0;
        while (i + 1 < dstChars && src[i] != L'\0') {
            dst[i] = src[i];
            ++i;
        }
        dst[i] = L'\0';
        return i;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dst[0] = L'\0';
        return 0;
    }
}

// Copies a wide string Winamp handed back, guarding against a null or stale
// pointer from an unexpected Winamp build.
std::wstring CopyIpcString(LRESULT raw) {
    const wchar_t* p = (const wchar_t*)raw;
    if (!p) return std::wstring();
    wchar_t buf[4096];
    const size_t len = SafeCopyW(p, buf, _countof(buf));
    return std::wstring(buf, len);
}

// Queries one metadata field via IPC_GET_EXTENDED_FILE_INFOW (Winamp 5.13+).
//
// The pre-5.13 ANSI fallback was dropped deliberately: it doubled the number of
// calls that hand Winamp a pointer of ours, and nothing that can run this
// plugin is old enough to need it.
std::wstring GetExtendedInfo(HWND hwnd, const std::wstring& file, const wchar_t* field) {
    ExtInfoRequest* req = new (std::nothrow) ExtInfoRequest();
    if (!req) return std::wstring();
    ZeroMemory(req, sizeof(*req));

    if (file.empty() || file.size() >= _countof(req->filename) ||
        wcslen(field) >= _countof(req->metadata)) {
        delete req;
        return std::wstring();
    }

    wcscpy_s(req->filename, _countof(req->filename), file.c_str());
    wcscpy_s(req->metadata, _countof(req->metadata), field);
    req->efs.filename = req->filename;
    req->efs.metadata = req->metadata;
    req->efs.ret      = req->ret;
    req->efs.retlen   = _countof(req->ret);

    // Note the absence of SMTO_ABORTIFHUNG: it returns the moment Winamp stops
    // pumping, which during a playlist switch is normal rather than fatal, and
    // every such early return costs us a leaked block.
    DWORD_PTR result = 0;
    const LRESULT delivered = SendMessageTimeoutW(hwnd, WM_WA_IPC, (WPARAM)&req->efs,
                                                  IPC_GET_EXTENDED_FILE_INFOW,
                                                  SMTO_NORMAL, kExtInfoTimeoutMs, &result);
    if (!delivered) {
        // Winamp may still service the message, so this block has to stay valid
        // for the life of the process. Leak it on purpose -- a few KB beats
        // letting Winamp write into memory we have handed back.
        util::Log(L"extended file info timed out for '%s'; leaking the request buffer",
                  field);
        return std::wstring();
    }

    std::wstring value;
    if (result && req->ret[0] != L'\0') value = util::Trim(req->ret);
    delete req;
    return value;
}

bool LooksLikeStream(const std::wstring& file) {
    return file.rfind(L"http://", 0) == 0 ||
           file.rfind(L"https://", 0) == 0 ||
           file.rfind(L"mms://", 0) == 0 ||
           file.rfind(L"rtsp://", 0) == 0;
}

// Splits Winamp's formatted playlist title, normally "Artist - Title", used
// when the input plugin exposes no structured tags (typical for streams).
void SplitPlaylistTitle(const std::wstring& text, std::wstring* artist, std::wstring* title) {
    const size_t sep = text.find(L" - ");
    if (sep != std::wstring::npos) {
        *artist = util::Trim(text.substr(0, sep));
        *title  = util::Trim(text.substr(sep + 3));
    } else {
        *title = util::Trim(text);
    }
}

// Strips a leading "12. " track-number prefix Winamp adds to playlist titles.
std::wstring StripPlaylistIndex(const std::wstring& s) {
    size_t i = 0;
    while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') ++i;
    if (i > 0 && i + 1 < s.size() && s[i] == L'.' && s[i + 1] == L' ') {
        return util::Trim(s.substr(i + 2));
    }
    return s;
}

} // namespace

bool ReadWinampState(HWND hwndWinamp, const TrackInfo* previous, TrackInfo* out) {
    if (!IsWindow(hwndWinamp)) return false;

    TrackInfo info;

    const LRESULT playing = IpcSend(hwndWinamp, 0, IPC_ISPLAYING);
    switch (playing) {
        case 1:  info.state = PlayState::Playing; break;
        case 3:  info.state = PlayState::Paused;  break;
        default: info.state = PlayState::Stopped; break;
    }

    if (info.state == PlayState::Stopped) {
        *out = info;
        return true;
    }

    const LRESULT listPos = IpcSend(hwndWinamp, 0, IPC_GETLISTPOS);
    info.listPos = (int)listPos;
    info.file = CopyIpcString(IpcSend(hwndWinamp, (WPARAM)listPos, IPC_GETPLAYLISTFILEW));
    info.isStream = LooksLikeStream(info.file);

    info.positionMs = (int)IpcSend(hwndWinamp, 0, IPC_GETOUTPUTTIME);

    const LRESULT lengthSec = IpcSend(hwndWinamp, 1, IPC_GETOUTPUTTIME);
    info.lengthMs = (lengthSec > 0) ? (int)(lengthSec * 1000) : -1;

    const bool sameEntry = previous != nullptr &&
                           previous->listPos == info.listPos &&
                           previous->file == info.file &&
                           !previous->title.empty();

    if (sameEntry) {
        info.title       = previous->title;
        info.artist      = previous->artist;
        info.album       = previous->album;
        info.albumArtist = previous->albumArtist;
    } else if (!info.isStream) {
        // Tag reads go through the input plugin and are the only calls that
        // pass Winamp a pointer of ours, so they run once per track, not per
        // poll. Streams skip them entirely -- their titles arrive through the
        // playlist title below, which is a plain scalar query.
        info.title       = GetExtendedInfo(hwndWinamp, info.file, L"title");
        info.artist      = GetExtendedInfo(hwndWinamp, info.file, L"artist");
        info.album       = GetExtendedInfo(hwndWinamp, info.file, L"album");
        info.albumArtist = GetExtendedInfo(hwndWinamp, info.file, L"albumartist");
    }

    // Streams and untagged files yield nothing above; fall back to the
    // formatted playlist title, which Winamp keeps current for stream metadata.
    if (info.title.empty() || info.artist.empty()) {
        std::wstring playlistTitle =
            StripPlaylistIndex(CopyIpcString(IpcSend(hwndWinamp, (WPARAM)listPos, IPC_GETPLAYLISTTITLEW)));
        if (!playlistTitle.empty()) {
            std::wstring pArtist, pTitle;
            SplitPlaylistTitle(playlistTitle, &pArtist, &pTitle);
            if (info.title.empty())  info.title  = pTitle;
            if (info.artist.empty()) info.artist = pArtist;
        }
    }

    // Last resort: show the bare filename so the status is never blank.
    if (info.title.empty() && !info.file.empty()) {
        size_t slash = info.file.find_last_of(L"\\/");
        std::wstring name = (slash == std::wstring::npos) ? info.file : info.file.substr(slash + 1);
        size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) name = name.substr(0, dot);
        info.title = name;
    }

    if (info.albumArtist.empty()) info.albumArtist = info.artist;

    *out = info;
    return true;
}
