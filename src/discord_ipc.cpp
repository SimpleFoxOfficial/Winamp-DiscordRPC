#include "discord_ipc.h"
#include "util.h"

#include <objbase.h>
#include <stdio.h>
#include <vector>

namespace {

enum Opcode {
    OP_HANDSHAKE = 0,
    OP_FRAME     = 1,
    OP_CLOSE     = 2,
    OP_PING      = 3,
    OP_PONG      = 4,
};

// Discord requires a unique nonce per command.
std::string MakeNonce() {
    GUID guid;
    if (FAILED(CoCreateGuid(&guid))) {
        char buf[32];
        sprintf_s(buf, sizeof(buf), "%08x%04x", (unsigned)GetTickCount(), (unsigned)(rand() & 0xFFFF));
        return buf;
    }
    char buf[64];
    sprintf_s(buf, sizeof(buf),
              "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
              guid.Data1, guid.Data2, guid.Data3,
              guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
              guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return buf;
}

void AppendJsonString(std::string* out, const char* key, const std::string& value) {
    *out += "\"";
    *out += key;
    *out += "\":\"";
    *out += util::JsonEscape(value);
    *out += "\"";
}

} // namespace

bool DiscordIpc::Activity::operator==(const Activity& o) const {
    return type == o.type && name == o.name && details == o.details &&
           state == o.state && largeImage == o.largeImage && largeText == o.largeText &&
           smallImage == o.smallImage && smallText == o.smallText &&
           startMs == o.startMs && endMs == o.endMs &&
           statusDisplayType == o.statusDisplayType;
}

DiscordIpc::~DiscordIpc() {
    Disconnect();
}

bool DiscordIpc::Connect(const std::string& clientId) {
    Disconnect();
    if (clientId.empty()) return false;

    // Discord opens one pipe per running client instance (stable, PTB, canary).
    for (int i = 0; i < 10; ++i) {
        wchar_t path[64];
        swprintf_s(path, _countof(path), L"\\\\.\\pipe\\discord-ipc-%d", i);

        HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;

        pipe_ = h;

        std::string handshake = "{\"v\":1,\"client_id\":\"" + util::JsonEscape(clientId) + "\"}";
        if (!WriteFrame(OP_HANDSHAKE, handshake)) {
            Disconnect();
            continue;
        }

        // Discord answers with DISPATCH/READY. Anything else (usually a CLOSE
        // frame carrying an error) means the client ID is bad.
        int opcode = 0;
        std::string payload;
        if (!ReadFrameBlocking(5000, &opcode, &payload)) {
            util::Log(L"handshake timed out on discord-ipc-%d", i);
            Disconnect();
            continue;
        }
        if (opcode == OP_CLOSE) {
            util::Log(L"handshake rejected: %s", util::FromUtf8(payload).c_str());
            Disconnect();
            return false; // a bad client ID will fail on every pipe
        }
        if (payload.find("\"READY\"") == std::string::npos) {
            util::Log(L"unexpected handshake reply: %s", util::FromUtf8(payload).c_str());
            Disconnect();
            continue;
        }

        util::Log(L"connected to discord-ipc-%d", i);
        return true;
    }
    return false;
}

void DiscordIpc::Disconnect() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    activityError_ = false;
}

bool DiscordIpc::WriteFrame(int opcode, const std::string& payload) {
    if (pipe_ == INVALID_HANDLE_VALUE) return false;

    std::vector<char> frame(8 + payload.size());
    const int len = (int)payload.size();
    memcpy(&frame[0], &opcode, 4);
    memcpy(&frame[4], &len, 4);
    if (!payload.empty()) memcpy(&frame[8], payload.data(), payload.size());

    DWORD written = 0;
    const char* p = frame.data();
    DWORD remaining = (DWORD)frame.size();
    while (remaining > 0) {
        if (!WriteFile(pipe_, p, remaining, &written, nullptr) || written == 0) {
            util::Log(L"WriteFile failed, err=%lu", GetLastError());
            return false;
        }
        p += written;
        remaining -= written;
    }
    return true;
}

bool DiscordIpc::ReadFrameBlocking(int timeoutMs, int* opcode, std::string* payload) {
    if (pipe_ == INVALID_HANDLE_VALUE) return false;

    // The pipe is opened in blocking mode, so poll with PeekNamedPipe rather
    // than risk parking a worker thread inside ReadFile forever.
    const DWORD deadline = GetTickCount() + (DWORD)timeoutMs;
    DWORD avail = 0;
    for (;;) {
        if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &avail, nullptr)) return false;
        if (avail >= 8) break;
        if ((int)(GetTickCount() - deadline) >= 0) return false;
        Sleep(20);
    }

    char header[8];
    DWORD got = 0;
    if (!ReadFile(pipe_, header, 8, &got, nullptr) || got != 8) return false;

    int op = 0, len = 0;
    memcpy(&op, header, 4);
    memcpy(&len, header + 4, 4);
    if (len < 0 || len > (1 << 20)) return false; // sanity bound

    std::string body;
    body.resize((size_t)len);
    size_t off = 0;
    while (off < (size_t)len) {
        if (!ReadFile(pipe_, &body[off], (DWORD)(len - off), &got, nullptr) || got == 0) return false;
        off += got;
    }

    *opcode  = op;
    *payload = body;
    return true;
}

bool DiscordIpc::Poll() {
    if (pipe_ == INVALID_HANDLE_VALUE) return false;

    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &avail, nullptr)) {
            util::Log(L"pipe peek failed, err=%lu", GetLastError());
            return false;
        }
        if (avail < 8) return true;

        int opcode = 0;
        std::string payload;
        if (!ReadFrameBlocking(1000, &opcode, &payload)) return false;

        if (opcode == OP_PING) {
            if (!WriteFrame(OP_PONG, payload)) return false;
        } else if (opcode == OP_CLOSE) {
            util::Log(L"discord closed the connection: %s", util::FromUtf8(payload).c_str());
            return false;
        } else if (payload.find("\"evt\":\"ERROR\"") != std::string::npos) {
            util::Log(L"discord error frame: %s", util::FromUtf8(payload).c_str());
            activityError_ = true;
        }
    }
}

bool DiscordIpc::TakeActivityError() {
    const bool had = activityError_;
    activityError_ = false;
    return had;
}

std::string DiscordIpc::BuildActivityJson(const Activity& a, DWORD pid) {
    std::string act = "{";
    bool first = true;
    auto comma = [&]() { if (!first) act += ","; first = false; };

    comma();
    act += "\"type\":" + std::to_string(a.type);

    if (!a.name.empty())    { comma(); AppendJsonString(&act, "name", a.name); }
    if (!a.details.empty()) { comma(); AppendJsonString(&act, "details", a.details); }
    if (!a.state.empty())   { comma(); AppendJsonString(&act, "state", a.state); }

    if (a.startMs > 0 || a.endMs > 0) {
        comma();
        act += "\"timestamps\":{";
        bool tfirst = true;
        if (a.startMs > 0) { act += "\"start\":" + std::to_string(a.startMs); tfirst = false; }
        // Supplying BOTH start and end is what makes Discord render a progress
        // bar rather than a plain elapsed counter.
        if (a.endMs > 0)   { if (!tfirst) act += ","; act += "\"end\":" + std::to_string(a.endMs); }
        act += "}";
    }

    if (!a.largeImage.empty() || !a.smallImage.empty()) {
        comma();
        act += "\"assets\":{";
        bool afirst = true;
        auto acomma = [&]() { if (!afirst) act += ","; afirst = false; };
        // large_image/small_image accept either an uploaded asset key or a
        // plain https:// URL -- the Discord client proxies external URLs
        // through media.discordapp.net on our behalf.
        if (!a.largeImage.empty()) { acomma(); AppendJsonString(&act, "large_image", a.largeImage); }
        if (!a.largeText.empty())  { acomma(); AppendJsonString(&act, "large_text",  a.largeText); }
        if (!a.smallImage.empty()) { acomma(); AppendJsonString(&act, "small_image", a.smallImage); }
        if (!a.smallText.empty())  { acomma(); AppendJsonString(&act, "small_text",  a.smallText); }
        act += "}";
    }

    if (a.statusDisplayType >= 0) {
        comma();
        act += "\"status_display_type\":" + std::to_string(a.statusDisplayType);
    }

    act += "}";

    std::string out = "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"" + MakeNonce() + "\",\"args\":{\"pid\":" +
                      std::to_string((unsigned long)pid) + ",\"activity\":" + act + "}}";
    return out;
}

bool DiscordIpc::SetActivity(const Activity& activity) {
    if (pipe_ == INVALID_HANDLE_VALUE) return false;
    const std::string payload = BuildActivityJson(activity, GetCurrentProcessId());
    return WriteFrame(OP_FRAME, payload);
}

bool DiscordIpc::ClearActivity() {
    if (pipe_ == INVALID_HANDLE_VALUE) return false;
    std::string payload = "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"" + MakeNonce() +
                          "\",\"args\":{\"pid\":" + std::to_string((unsigned long)GetCurrentProcessId()) +
                          ",\"activity\":null}}";
    return WriteFrame(OP_FRAME, payload);
}
