#pragma once

#include <windows.h>
#include <string>

enum class PlayState {
    Stopped = 0,
    Playing = 1,
    Paused  = 3,
};

struct TrackInfo {
    PlayState    state = PlayState::Stopped;
    std::wstring file;         // full path, or the URL for a stream
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring albumArtist;
    int          positionMs = 0;
    int          lengthMs   = -1;   // -1 when unknown (live streams)
    bool         isStream   = false;
    int          listPos    = -1;   // playlist index, to spot entry changes

    // True when this represents a different piece of audio than `o`, as opposed
    // to the same track having merely advanced.
    bool IsDifferentTrack(const TrackInfo& o) const {
        return file != o.file || title != o.title || artist != o.artist || album != o.album;
    }
};

// Reads the current playback state out of Winamp over WM_WA_IPC.
//
// Must be called from a thread that can safely SendMessage to Winamp's window.
// Winamp returns pointers into its own address space for string queries, valid
// only in-process and only until it reuses the buffer, so every string is
// copied out immediately.
//
// `previous` may be null. When it describes the same playlist entry, the tag
// queries are skipped and its metadata reused: those are the only calls that
// hand Winamp a pointer into our memory, and repeating them twice a second for
// a track that has not changed is both wasteful and needless exposure.
bool ReadWinampState(HWND hwndWinamp, const TrackInfo* previous, TrackInfo* out);
