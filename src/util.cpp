#include "util.h"

#include <stdio.h>
#include <stdarg.h>
#include <mutex>

namespace util {

namespace {
std::mutex   g_logMutex;
std::wstring g_logPath;
bool         g_logEnabled = false;
} // namespace

std::string ToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n')) ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r' || s[e - 1] == L'\n')) --e;
    return s.substr(b, e - b);
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    sprintf_s(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;   // UTF-8 continuation bytes pass through
                }
        }
    }
    return out;
}

// Decodes a JSON string literal starting at json[pos] (which must be the
// opening quote). Advances pos past the closing quote.
static bool DecodeJsonString(const std::string& json, size_t& pos, std::string* out) {
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;
    std::string result;
    while (pos < json.size()) {
        char c = json[pos];
        if (c == '"') { ++pos; *out = result; return true; }
        if (c == '\\') {
            if (++pos >= json.size()) return false;
            char e = json[pos++];
            switch (e) {
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                case '/':  result += '/';  break;
                case 'b':  result += '\b'; break;
                case 'f':  result += '\f'; break;
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                case 'u': {
                    if (pos + 4 > json.size()) return false;
                    unsigned int cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = json[pos + i];
                        cp <<= 4;
                        if      (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        else return false;
                    }
                    pos += 4;
                    // Re-encode as UTF-8. Surrogate pairs are left as-is; the
                    // artwork URLs we parse are ASCII in practice.
                    if (cp < 0x80) {
                        result += (char)cp;
                    } else if (cp < 0x800) {
                        result += (char)(0xC0 | (cp >> 6));
                        result += (char)(0x80 | (cp & 0x3F));
                    } else {
                        result += (char)(0xE0 | (cp >> 12));
                        result += (char)(0x80 | ((cp >> 6) & 0x3F));
                        result += (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: return false;
            }
        } else {
            result += c;
            ++pos;
        }
    }
    return false;
}

bool JsonFindString(const std::string& json, const std::string& key, std::string* out) {
    const std::string needle = "\"" + key + "\"";
    size_t at = 0;
    while ((at = json.find(needle, at)) != std::string::npos) {
        size_t pos = at + needle.size();
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                     json[pos] == '\r' || json[pos] == '\n')) ++pos;
        if (pos < json.size() && json[pos] == ':') {
            ++pos;
            while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                         json[pos] == '\r' || json[pos] == '\n')) ++pos;
            if (pos < json.size() && json[pos] == '"') {
                return DecodeJsonString(json, pos, out);
            }
        }
        at += needle.size();
    }
    return false;
}

std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

long long UnixMillis() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER li;
    li.LowPart  = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;
    // FILETIME is 100ns ticks since 1601-01-01; shift to the Unix epoch.
    const long long kEpochDelta = 116444736000000000LL;
    return ((long long)li.QuadPart - kEpochDelta) / 10000LL;
}

void SetLogFile(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logPath = path;
}

void SetLoggingEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logEnabled = enabled;
}

void Log(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);

    OutputDebugStringW(L"[gen_discord_rpc] ");
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");

    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_logEnabled || g_logPath.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_logPath.c_str(), L"a, ccs=UTF-8") == 0 && f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fwprintf(f, L"%02d:%02d:%02d.%03d  %s\n", st.wHour, st.wMinute, st.wSecond,
                 st.wMilliseconds, buf);
        fclose(f);
    }
}

} // namespace util
