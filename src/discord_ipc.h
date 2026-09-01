#pragma once

#include <windows.h>
#include <string>

// A minimal Discord Rich Presence client speaking the local IPC protocol
// directly over a named pipe.
//
// We do not use Discord's official discord-rpc C library because it cannot set
// `activity.type` (needed for "Listening to ..." instead of "Playing ...") and
// predates external image URLs in assets. The wire protocol is small enough to
// implement outright:
//
//   frame := int32 opcode (LE) | int32 payload length (LE) | UTF-8 JSON
//
// Opcodes: 0 HANDSHAKE, 1 FRAME, 2 CLOSE, 3 PING, 4 PONG.
class DiscordIpc {
public:
    // The activity to display. Empty strings are omitted from the payload.
    struct Activity {
        int         type = 2;          // 2 = Listening ("Listening to <name>")
        std::string name;              // application name override
        std::string details;           // line 1 -- track title
        std::string state;             // line 2 -- artist
        std::string largeImage;        // asset key OR an https:// URL
        std::string largeText;         // tooltip -- album
        std::string smallImage;        // asset key OR an https:// URL
        std::string smallText;
        long long   startMs = 0;       // 0 = omit
        long long   endMs   = 0;       // 0 = omit; start+end draws a progress bar
        int         statusDisplayType = -1; // -1 omit, 0 name, 1 state, 2 details

        bool operator==(const Activity& o) const;
        bool operator!=(const Activity& o) const { return !(*this == o); }
    };

    DiscordIpc() = default;
    ~DiscordIpc();

    DiscordIpc(const DiscordIpc&) = delete;
    DiscordIpc& operator=(const DiscordIpc&) = delete;

    // Opens discord-ipc-0..9 and performs the handshake. Returns false if no
    // Discord client is listening.
    bool Connect(const std::string& clientId);
    void Disconnect();
    bool IsConnected() const { return pipe_ != INVALID_HANDLE_VALUE; }

    // Sends SET_ACTIVITY. Returns false if the pipe broke, in which case the
    // caller should Disconnect() and retry later.
    bool SetActivity(const Activity& activity);

    // Sends SET_ACTIVITY with a null activity, clearing the status.
    bool ClearActivity();

    // Drains any pending frames from Discord and answers PINGs. Must be called
    // periodically; a full pipe buffer would otherwise stall writes. Returns
    // false if the connection was lost.
    bool Poll();

    // Serialises a SET_ACTIVITY command. Public so the test harness can assert
    // on the exact wire payload without needing a live Discord connection.
    static std::string BuildActivityJson(const Activity& a, DWORD pid);

    // True once if Discord reported an error for a command we sent, clearing
    // the flag. Lets the caller retry without the optional newer fields rather
    // than silently publishing nothing.
    bool TakeActivityError();

private:
    bool WriteFrame(int opcode, const std::string& payload);
    bool ReadFrameBlocking(int timeoutMs, int* opcode, std::string* payload);

    HANDLE pipe_          = INVALID_HANDLE_VALUE;
    bool   activityError_  = false;
};
