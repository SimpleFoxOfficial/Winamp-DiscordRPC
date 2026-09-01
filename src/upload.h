#pragma once

#include <string>
#include <vector>

// Publishes cover art to an anonymous image host so Discord can fetch it.
//
// This step is unavoidable for showing real embedded artwork: Discord's servers
// (not the desktop client) resolve `assets.large_image` URLs, so the image has
// to be reachable from the public internet. A localhost or LAN address cannot
// work, and Discord's own asset upload endpoint requires the application
// owner's account token, which a plugin has no business holding.
namespace upload {

enum class Host {
    Litterbox,   // anonymous, auto-expiring
    Catbox,      // anonymous, permanent
};

// Uploads JPEG bytes and returns the public URL.
//
// `expiryHours` applies to Litterbox only and must be 1, 12, 24 or 72;
// Catbox uploads never expire. Blocking -- worker thread only.
bool Send(Host host, int expiryHours,
          const std::vector<unsigned char>& jpeg, std::string* url);

// Seconds until a URL from this host should be treated as stale, or 0 when it
// never expires. Deliberately shorter than the host's real lifetime so a cover
// is refreshed before Discord is left pointing at a dead link.
long long LifetimeSeconds(Host host, int expiryHours);

Host        HostFromString(const std::wstring& name);
const wchar_t* HostToString(Host host);

} // namespace upload
