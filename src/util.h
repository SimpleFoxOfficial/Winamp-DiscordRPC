#pragma once

#include <windows.h>
#include <string>

namespace util {

// UTF-16 <-> UTF-8. Discord's IPC protocol is UTF-8 throughout; Winamp hands
// us UTF-16, so every string crosses this boundary exactly once.
std::string  ToUtf8(const std::wstring& s);
std::wstring FromUtf8(const std::string& s);

// Trims ASCII whitespace from both ends.
std::wstring Trim(const std::wstring& s);

// Escapes a string for embedding in a JSON string literal (quotes, backslash,
// control characters). Input and output are both UTF-8.
std::string JsonEscape(const std::string& s);

// Pulls the value of a top-level-ish string key out of a JSON document, e.g.
// JsonFindString(body, "artworkUrl100"). Returns false if the key is absent or
// its value is not a string. This is a deliberate shortcut, not a parser: it
// scans for "key" followed by a colon and a string, which is all we need for
// the two artwork APIs we query.
bool JsonFindString(const std::string& json, const std::string& key, std::string* out);

// Percent-encodes a UTF-8 string for use in a query string.
std::string UrlEncode(const std::string& s);

// Milliseconds since the Unix epoch.
long long UnixMillis();

// Writes a line to the debugger and, when enabled, to the log file.
void Log(const wchar_t* fmt, ...);
void SetLogFile(const std::wstring& path);
void SetLoggingEnabled(bool enabled);

} // namespace util
