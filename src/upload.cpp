#include "upload.h"
#include "http.h"
#include "util.h"

#include <algorithm>

namespace upload {

namespace {

const wchar_t* kLitterboxEndpoint = L"https://litterbox.catbox.moe/resources/internals/api.php";
const wchar_t* kCatboxEndpoint    = L"https://catbox.moe/user/api.php";

// Both hosts answer a successful upload with the bare URL as the whole body.
bool LooksLikeUrl(const std::string& s) {
    return s.rfind("https://", 0) == 0 && s.find(' ') == std::string::npos;
}

std::string TrimAscii(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (unsigned char)s[b] <= ' ') ++b;
    while (e > b && (unsigned char)s[e - 1] <= ' ') --e;
    return s.substr(b, e - b);
}

int NormaliseExpiry(int hours) {
    // The API only accepts these four values.
    const int allowed[] = { 1, 12, 24, 72 };
    for (int a : allowed) if (hours == a) return a;
    return 72;
}

} // namespace

bool Send(Host host, int expiryHours,
          const std::vector<unsigned char>& jpeg, std::string* url) {
    if (jpeg.empty()) return false;

    std::vector<std::pair<std::string, std::string>> fields;
    fields.emplace_back("reqtype", "fileupload");

    std::wstring endpoint;
    if (host == Host::Litterbox) {
        endpoint = kLitterboxEndpoint;
        fields.emplace_back("time", std::to_string(NormaliseExpiry(expiryHours)) + "h");
    } else {
        endpoint = kCatboxEndpoint;
        // Omitting userhash is what makes a Catbox upload anonymous.
    }

    std::vector<char> body;
    std::wstring contentType;
    http::BuildMultipart(fields, "fileToUpload", "cover.jpg", "image/jpeg",
                         jpeg, &body, &contentType);

    std::string response;
    if (!http::Post(endpoint, contentType, body, &response)) {
        util::Log(L"cover upload failed (%s)", HostToString(host));
        return false;
    }

    const std::string trimmed = TrimAscii(response);
    if (!LooksLikeUrl(trimmed)) {
        util::Log(L"unexpected upload reply: %s", util::FromUtf8(trimmed.substr(0, 200)).c_str());
        return false;
    }

    *url = trimmed;
    util::Log(L"uploaded cover (%zu bytes) -> %s", jpeg.size(), util::FromUtf8(trimmed).c_str());
    return true;
}

long long LifetimeSeconds(Host host, int expiryHours) {
    if (host == Host::Catbox) return 0;   // permanent
    // Refresh an hour early so a long listening session never ends up pointing
    // Discord at a URL that has just been reaped.
    const long long hours = NormaliseExpiry(expiryHours);
    return (hours - 1) * 3600LL;
}

Host HostFromString(const std::wstring& name) {
    std::wstring lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](wchar_t c) { return (wchar_t)towlower(c); });
    if (lower == L"catbox") return Host::Catbox;
    return Host::Litterbox;
}

const wchar_t* HostToString(Host host) {
    return host == Host::Catbox ? L"catbox" : L"litterbox";
}

} // namespace upload
