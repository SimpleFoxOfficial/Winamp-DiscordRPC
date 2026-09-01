#pragma once

#include "upload.h"

#include <map>
#include <mutex>
#include <string>

// Turns a track into a cover image URL Discord can display.
//
// Resolution order:
//   1. artwork embedded in the file (or a cover.jpg beside it), uploaded to an
//      anonymous host so Discord's servers can fetch it
//   2. a catalogue lookup on iTunes, then Deezer
//   3. nothing, so the caller falls back to the configured placeholder
//
// Step 1 is what makes bootlegs, live sets and self-tagged rips show the right
// cover -- a catalogue match cannot know about them.
//
// Everything here blocks on disk and network and must run on the worker thread.
class ArtworkResolver {
public:
    struct Options {
        bool         useEmbedded     = true;
        bool         useOnlineLookup = true;
        upload::Host host            = upload::Host::Litterbox;
        int          expiryHours     = 72;
        int          maxEdge         = 512;   // Discord displays this small
    };

    void SetOptions(const Options& options);

    // Full resolution. May extract, upload and hit the network, so it can take
    // a second or two on a cache miss.
    bool Resolve(const std::wstring& artist,
                 const std::wstring& album,
                 const std::wstring& title,
                 const std::wstring& audioFile,
                 std::string* url);

    // Cache-only lookup: never touches disk or network, so the caller can
    // publish presence immediately and refresh once the real cover lands.
    // Returns false on a miss *or* a cached negative result.
    bool TryCached(const std::wstring& artist,
                   const std::wstring& album,
                   const std::wstring& title,
                   std::string* url) const;

    // Whether a lookup has already been attempted and is still valid, so the
    // caller knows a Resolve() call would be free.
    bool IsFresh(const std::wstring& artist,
                 const std::wstring& album,
                 const std::wstring& title) const;

    // Resolves any configured static image -- the "no cover" placeholder or a
    // small badge. When `setting` names an image file on disk it is uploaded
    // (once, then cached) and its URL returned; otherwise the value passes
    // through unchanged as a Discord asset key.
    bool ResolveImageSetting(const std::wstring& setting, std::string* imageRef);

    void LoadCache(const std::wstring& path);
    void SaveCache() const;
    void Clear();

private:
    struct Entry {
        std::string url;          // empty = negative result
        long long   expiresAt;    // unix seconds, 0 = never
    };

    static std::wstring MakeKey(const std::wstring& artist,
                                const std::wstring& album,
                                const std::wstring& title);

    static bool QueryItunes(const std::wstring& artist, const std::wstring& album,
                            const std::wstring& title, std::string* url);
    static bool QueryDeezer(const std::wstring& artist, const std::wstring& album,
                            const std::wstring& title, std::string* url);

    enum class ArtStatus {
        Ok,
        NoArtwork,      // the file genuinely has none -- worth remembering
        UploadFailed,   // art exists, the host was unhappy -- retry shortly
    };

    // Extracts, deduplicates by content hash, and uploads. Caller holds no lock.
    ArtStatus ExtractAndUpload(const std::wstring& audioFile, std::string* url);

    bool LookupFresh(const std::wstring& key, std::string* url) const;   // mutex_ held
    void Store(const std::wstring& key, const std::string& url, long long lifetimeSeconds);

    mutable std::mutex           mutex_;
    std::map<std::wstring, Entry> cache_;       // release key -> url
    std::map<std::string, Entry>  uploads_;     // content hash -> url
    std::wstring                  cachePath_;
    Options                       options_;
    mutable bool                  dirty_ = false;
};
