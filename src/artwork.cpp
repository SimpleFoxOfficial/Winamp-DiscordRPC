#include "artwork.h"
#include "coverart.h"
#include "http.h"
#include "upload.h"
#include "util.h"

#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include <algorithm>
#include <vector>

namespace {

// A failed lookup is retried after this long rather than cached forever, so a
// later tag fix or a network outage does not blacklist a track permanently.
const long long kNegativeCacheSeconds = 3600;

// Much shorter when we know the artwork exists and only the upload fell over.
// An hour of no cover because the image host returned one HTTP 500 is a poor
// trade when the picture is sitting right there in the file.
const long long kTransientRetrySeconds = 60;

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

// iTunes serves artwork at a size encoded in the filename. The search API
// returns a 100x100 thumbnail; swapping the token yields the full-size cover.
std::string UpscaleItunesArtwork(const std::string& url) {
    const std::string from = "100x100";
    const size_t at = url.rfind(from);
    if (at == std::string::npos) return url;
    return url.substr(0, at) + "600x600" + url.substr(at + from.size());
}

long long NowSeconds() {
    return util::UnixMillis() / 1000;
}

bool LooksLikeStream(const std::wstring& file) {
    return file.rfind(L"http://", 0) == 0 || file.rfind(L"https://", 0) == 0;
}

} // namespace

void ArtworkResolver::SetOptions(const Options& options) {
    std::lock_guard<std::mutex> lock(mutex_);
    options_ = options;
}

std::wstring ArtworkResolver::MakeKey(const std::wstring& artist,
                                      const std::wstring& album,
                                      const std::wstring& title) {
    // Key on the release where possible so every track of an album shares one
    // lookup; fall back to the track for singles and untagged files.
    return ToLower(artist) + L"\x1f" + ToLower(album.empty() ? title : album);
}

bool ArtworkResolver::LookupFresh(const std::wstring& key, std::string* url) const {
    auto it = cache_.find(key);
    if (it == cache_.end()) return false;
    if (it->second.expiresAt != 0 && it->second.expiresAt <= NowSeconds()) return false;
    if (it->second.url.empty()) return false;
    *url = it->second.url;
    return true;
}

void ArtworkResolver::Store(const std::wstring& key, const std::string& url,
                            long long lifetimeSeconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry e;
    e.url       = url;
    e.expiresAt = (lifetimeSeconds > 0) ? (NowSeconds() + lifetimeSeconds) : 0;
    cache_[key] = e;
    dirty_      = true;
}

bool ArtworkResolver::TryCached(const std::wstring& artist,
                                const std::wstring& album,
                                const std::wstring& title,
                                std::string* url) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return LookupFresh(MakeKey(artist, album, title), url);
}

bool ArtworkResolver::IsFresh(const std::wstring& artist,
                              const std::wstring& album,
                              const std::wstring& title) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::wstring key = MakeKey(artist, album, title);
    auto it = cache_.find(key);
    if (it == cache_.end()) return false;
    return it->second.expiresAt == 0 || it->second.expiresAt > NowSeconds();
}

ArtworkResolver::ArtStatus ArtworkResolver::ExtractAndUpload(const std::wstring& audioFile,
                                                             std::string* url) {
    Options opts;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        opts = options_;
    }

    std::vector<unsigned char> jpeg;
    if (!coverart::ExtractJpeg(audioFile, opts.maxEdge, &jpeg) || jpeg.empty()) {
        return ArtStatus::NoArtwork;
    }

    // Identical artwork across tracks (common when the album tag is missing and
    // every track keys differently) should only ever be uploaded once.
    const std::string hash = coverart::HashBytes(jpeg);
    if (!hash.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = uploads_.find(hash);
        if (it != uploads_.end() && !it->second.url.empty() &&
            (it->second.expiresAt == 0 || it->second.expiresAt > NowSeconds())) {
            *url = it->second.url;
            return ArtStatus::Ok;
        }
    }

    std::string uploaded;
    if (!upload::Send(opts.host, opts.expiryHours, jpeg, &uploaded)) {
        return ArtStatus::UploadFailed;
    }

    const long long lifetime = upload::LifetimeSeconds(opts.host, opts.expiryHours);
    if (!hash.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry e;
        e.url       = uploaded;
        e.expiresAt = (lifetime > 0) ? (NowSeconds() + lifetime) : 0;
        uploads_[hash] = e;
        dirty_ = true;
    }

    *url = uploaded;
    return ArtStatus::Ok;
}

bool ArtworkResolver::QueryItunes(const std::wstring& artist,
                                  const std::wstring& album,
                                  const std::wstring& title,
                                  std::string* url) {
    std::wstring termW = artist;
    if (!album.empty())      termW += L" " + album;
    else if (!title.empty()) termW += L" " + title;
    termW = util::Trim(termW);
    if (termW.empty()) return false;

    const std::string term = util::UrlEncode(util::ToUtf8(termW));
    const std::wstring endpoint =
        L"https://itunes.apple.com/search?term=" + util::FromUtf8(term) +
        L"&entity=" + (album.empty() ? L"song" : L"album") + L"&limit=1";

    std::string body;
    if (!http::Get(endpoint, &body)) return false;

    std::string art;
    if (!util::JsonFindString(body, "artworkUrl100", &art) || art.empty()) return false;

    *url = UpscaleItunesArtwork(art);
    return true;
}

bool ArtworkResolver::QueryDeezer(const std::wstring& artist,
                                  const std::wstring& album,
                                  const std::wstring& title,
                                  std::string* url) {
    std::wstring queryW = artist;
    if (!album.empty())      queryW += L" " + album;
    else if (!title.empty()) queryW += L" " + title;
    queryW = util::Trim(queryW);
    if (queryW.empty()) return false;

    const std::string q = util::UrlEncode(util::ToUtf8(queryW));
    const std::wstring endpoint =
        L"https://api.deezer.com/search/" + std::wstring(album.empty() ? L"track" : L"album") +
        L"?q=" + util::FromUtf8(q) + L"&limit=1";

    std::string body;
    if (!http::Get(endpoint, &body)) return false;

    std::string art;
    if (!util::JsonFindString(body, "cover_xl", &art) || art.empty()) {
        if (!util::JsonFindString(body, "cover_big", &art) || art.empty()) return false;
    }
    *url = art;
    return true;
}

bool ArtworkResolver::Resolve(const std::wstring& artist,
                              const std::wstring& album,
                              const std::wstring& title,
                              const std::wstring& audioFile,
                              std::string* url) {
    const std::wstring key = MakeKey(artist, album, title);

    Options opts;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (LookupFresh(key, url)) return true;
        // A cached negative result that has not aged out yet.
        auto it = cache_.find(key);
        if (it != cache_.end() && it->second.url.empty() &&
            it->second.expiresAt > NowSeconds()) {
            return false;
        }
        opts = options_;
    }

    // 1. The file's own artwork, which is always the correct cover.
    bool uploadFellOver = false;
    if (opts.useEmbedded && !audioFile.empty() && !LooksLikeStream(audioFile)) {
        std::string uploaded;
        const ArtStatus status = ExtractAndUpload(audioFile, &uploaded);
        if (status == ArtStatus::Ok) {
            Store(key, uploaded, upload::LifetimeSeconds(opts.host, opts.expiryHours));
            util::Log(L"embedded cover for '%s / %s'", artist.c_str(), album.c_str());
            *url = uploaded;
            return true;
        }
        uploadFellOver = (status == ArtStatus::UploadFailed);
    }

    // 2. Catalogue lookup. These URLs are stable, so they never expire.
    if (opts.useOnlineLookup && !(artist.empty() && album.empty() && title.empty())) {
        std::string found;
        if (QueryItunes(artist, album, title, &found) ||
            QueryDeezer(artist, album, title, &found)) {
            Store(key, found, 0);
            util::Log(L"catalogue cover for '%s / %s' -> %s", artist.c_str(), album.c_str(),
                      util::FromUtf8(found).c_str());
            *url = found;
            return true;
        }
    }

    if (uploadFellOver) {
        util::Log(L"cover upload failed for '%s / %s'; retrying shortly",
                  artist.c_str(), album.c_str());
        Store(key, std::string(), kTransientRetrySeconds);
    } else {
        util::Log(L"no cover found for '%s / %s'", artist.c_str(), album.c_str());
        Store(key, std::string(), kNegativeCacheSeconds);
    }
    return false;
}

bool ArtworkResolver::ResolveImageSetting(const std::wstring& setting, std::string* imageRef) {
    imageRef->clear();
    if (setting.empty()) return false;

    // Anything that is not an existing file is taken as a Discord asset key,
    // which is the better option for a static image: Discord hosts it itself,
    // permanently, and nothing has to be uploaded at play time.
    if (GetFileAttributesW(setting.c_str()) == INVALID_FILE_ATTRIBUTES) {
        *imageRef = util::ToUtf8(setting);
        return true;
    }

    const std::wstring key = L"\x1f static \x1f" + ToLower(setting);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string cached;
        if (LookupFresh(key, &cached)) {
            *imageRef = cached;
            return true;
        }
    }

    Options opts;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        opts = options_;
    }

    std::vector<unsigned char> jpeg;
    if (!coverart::LoadImageAsJpeg(setting, opts.maxEdge, &jpeg) || jpeg.empty()) {
        util::Log(L"could not read image %s", setting.c_str());
        return false;
    }

    std::string uploaded;
    if (!upload::Send(opts.host, opts.expiryHours, jpeg, &uploaded)) return false;

    Store(key, uploaded, upload::LifetimeSeconds(opts.host, opts.expiryHours));
    util::Log(L"static image uploaded -> %s", util::FromUtf8(uploaded).c_str());
    *imageRef = uploaded;
    return true;
}

void ArtworkResolver::LoadCache(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    cachePath_ = path;
    cache_.clear();
    uploads_.clear();

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8") != 0 || !f) return;

    wchar_t line[4096];
    while (fgetws(line, _countof(line), f)) {
        std::wstring text(line);
        while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) text.pop_back();
        if (text.empty()) continue;

        // Format: <kind> \t <key> \t <expiry> \t <url>
        // kind is "r" for a release key or "u" for an upload content hash.
        std::vector<std::wstring> parts;
        size_t start = 0;
        for (;;) {
            const size_t tab = text.find(L'\t', start);
            if (tab == std::wstring::npos) { parts.push_back(text.substr(start)); break; }
            parts.push_back(text.substr(start, tab - start));
            start = tab + 1;
        }
        if (parts.size() < 4) continue;

        Entry e;
        e.expiresAt = _wtoi64(parts[2].c_str());
        e.url       = util::ToUtf8(parts[3]);

        // Drop anything already expired rather than carrying it forward.
        if (e.expiresAt != 0 && e.expiresAt <= NowSeconds()) continue;

        if (parts[0] == L"u") {
            uploads_[util::ToUtf8(parts[1])] = e;
        } else {
            cache_[parts[1]] = e;
        }
    }
    fclose(f);
    dirty_ = false;
}

void ArtworkResolver::SaveCache() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cachePath_.empty() || !dirty_) return;

    FILE* f = nullptr;
    if (_wfopen_s(&f, cachePath_.c_str(), L"w, ccs=UTF-8") != 0 || !f) return;

    for (const auto& kv : cache_) {
        fwprintf(f, L"r\t%s\t%lld\t%s\n", kv.first.c_str(), kv.second.expiresAt,
                 util::FromUtf8(kv.second.url).c_str());
    }
    for (const auto& kv : uploads_) {
        fwprintf(f, L"u\t%s\t%lld\t%s\n", util::FromUtf8(kv.first).c_str(),
                 kv.second.expiresAt, util::FromUtf8(kv.second.url).c_str());
    }
    fclose(f);
    dirty_ = false;
}

void ArtworkResolver::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
    uploads_.clear();
    dirty_ = true;
}
