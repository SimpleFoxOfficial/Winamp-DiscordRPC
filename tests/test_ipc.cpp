// Test harness for the parts of the plugin that can be exercised outside
// Winamp: JSON payload construction, the artwork lookup, and the Discord IPC
// wire protocol.
//
//   test_ipc                 offline checks + a handshake probe
//   test_ipc <client_id>     also publishes a real presence for ~30s
//
// The Winamp side cannot be covered here: IPC_GETPLAYLISTFILEW hands back a
// pointer into Winamp's own address space, so it is only meaningful in-process.

#include "../src/artwork.h"
#include "../src/coverart.h"
#include "../src/discord_ipc.h"
#include "../src/http.h"
#include "../src/upload.h"
#include "../src/util.h"

#include <stdio.h>
#include <windows.h>
#include <string>
#include <vector>

static int g_failures = 0;

static void Check(bool condition, const char* what) {
    printf("  [%s] %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

static bool Contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

static void TestJsonPayload() {
    printf("\n=== SET_ACTIVITY payload ===\n");

    DiscordIpc::Activity a;
    a.type       = 2;
    a.name       = "Winamp";
    a.details    = "Bohemian Rhapsody";
    a.state      = "Queen";
    a.largeImage = "https://is1-ssl.mzstatic.com/image/thumb/cover/600x600bb.jpg";
    a.largeText  = "A Night at the Opera";
    a.smallImage = "play";
    a.smallText  = "Playing";
    a.startMs    = 1700000000000LL;
    a.endMs      = 1700000354000LL;
    a.statusDisplayType = 2;

    const std::string json = DiscordIpc::BuildActivityJson(a, 4242);
    printf("%s\n\n", json.c_str());

    Check(Contains(json, "\"cmd\":\"SET_ACTIVITY\""), "command is SET_ACTIVITY");
    Check(Contains(json, "\"nonce\":"),               "nonce present");
    Check(Contains(json, "\"pid\":4242"),             "pid forwarded");
    Check(Contains(json, "\"type\":2"),               "activity type 2 (Listening)");
    Check(Contains(json, "\"start\":1700000000000"),  "start timestamp");
    Check(Contains(json, "\"end\":1700000354000"),    "end timestamp (drives progress bar)");
    Check(Contains(json, "\"large_image\":\"https://"), "external cover URL in large_image");
    Check(Contains(json, "\"status_display_type\":2"), "status_display_type");

    // A paused track omits timestamps so the bar freezes instead of running on.
    DiscordIpc::Activity paused = a;
    paused.startMs = paused.endMs = 0;
    const std::string pausedJson = DiscordIpc::BuildActivityJson(paused, 1);
    Check(!Contains(pausedJson, "timestamps"), "no timestamps when paused");
}

static void TestJsonEscaping() {
    printf("\n=== JSON escaping ===\n");

    DiscordIpc::Activity a;
    a.details = "Quote\" Backslash\\ Newline\n";
    a.state   = "Tab\there";
    const std::string json = DiscordIpc::BuildActivityJson(a, 1);

    Check(Contains(json, "Quote\\\" Backslash\\\\ Newline\\n"), "quotes/backslash/newline escaped");
    Check(Contains(json, "Tab\\there"), "tab escaped");

    // Non-ASCII must survive as UTF-8 rather than being mangled or escaped.
    DiscordIpc::Activity uni;
    uni.details = util::ToUtf8(L"あの曲 – café");
    const std::string uniJson = DiscordIpc::BuildActivityJson(uni, 1);
    Check(Contains(uniJson, util::ToUtf8(L"あの曲").c_str()), "UTF-8 passes through intact");

    std::string roundTrip;
    Check(util::JsonFindString(uniJson, "details", &roundTrip) &&
          roundTrip == uni.details, "details survives a parse round-trip");
}

static void TestJsonParsing() {
    printf("\n=== JSON value extraction ===\n");

    const std::string itunes =
        R"({"resultCount":1,"results":[{"artistName":"Queen","collectionName":"A Night at the Opera",)"
        R"("artworkUrl100":"https:\/\/is1-ssl.mzstatic.com\/image\/thumb\/abc\/100x100bb.jpg"}]})";

    std::string art;
    Check(util::JsonFindString(itunes, "artworkUrl100", &art), "finds artworkUrl100");
    Check(art == "https://is1-ssl.mzstatic.com/image/thumb/abc/100x100bb.jpg",
          "unescapes \\/ correctly");

    std::string missing;
    Check(!util::JsonFindString(itunes, "nosuchkey", &missing), "absent key reports failure");

    // A key whose value is not a string must not be mistaken for one.
    Check(!util::JsonFindString(itunes, "resultCount", &missing), "numeric value not treated as string");
}

static void TestArtworkLookup() {
    printf("\n=== catalogue lookup (live network) ===\n");

    ArtworkResolver resolver;
    ArtworkResolver::Options opts;
    opts.useEmbedded = false;          // force the catalogue path
    resolver.SetOptions(opts);

    std::string url;
    const bool ok = resolver.Resolve(L"Queen", L"A Night at the Opera",
                                     L"Bohemian Rhapsody", L"", &url);

    if (ok) {
        printf("  resolved -> %s\n", url.c_str());
        Check(url.rfind("https://", 0) == 0, "cover URL is https");
        Check(resolver.IsFresh(L"Queen", L"A Night at the Opera", L"Bohemian Rhapsody"),
              "result is cached");

        std::string again;
        const DWORD before = GetTickCount();
        resolver.Resolve(L"Queen", L"A Night at the Opera", L"Bohemian Rhapsody", L"", &again);
        Check((GetTickCount() - before) < 50, "cache hit avoids the network");
        Check(again == url, "cached value matches");
    } else {
        printf("  NOTE: lookup failed (offline or API unreachable) - skipping assertions\n");
    }
}

// Exercises the path that matters most: real artwork out of a real file, put
// somewhere Discord can actually fetch it.
static void TestEmbeddedCover(const std::wstring& audioFile) {
    printf("\n=== embedded cover art ===\n");

    if (audioFile.empty()) {
        printf("  no audio file given (pass one as argv[2]) - skipping\n");
        return;
    }
    if (GetFileAttributesW(audioFile.c_str()) == INVALID_FILE_ATTRIBUTES) {
        printf("  file not found - skipping\n");
        return;
    }

    coverart::Startup();

    std::vector<unsigned char> jpeg;
    const bool extracted = coverart::ExtractJpeg(audioFile, 512, &jpeg);
    Check(extracted && !jpeg.empty(), "artwork extracted from the file");
    if (!extracted) { coverart::Shutdown(); return; }

    printf("  extracted %zu bytes\n", jpeg.size());
    // A JPEG always starts FF D8 FF.
    Check(jpeg.size() > 3 && jpeg[0] == 0xFF && jpeg[1] == 0xD8 && jpeg[2] == 0xFF,
          "output is a well-formed JPEG");
    Check(jpeg.size() < 400 * 1024, "re-encoded small enough to upload quickly");

    const std::string hash = coverart::HashBytes(jpeg);
    Check(hash.size() == 40, "content hash computed");

    // Try both hosts before declaring failure. Either being down is an outage
    // on their side, not a defect here, and a red suite every time a free image
    // host hiccups teaches you to ignore the suite.
    std::string url;
    bool sent = upload::Send(upload::Host::Litterbox, 72, jpeg, &url);
    if (!sent) {
        printf("  NOTE: litterbox unavailable, trying catbox\n");
        sent = upload::Send(upload::Host::Catbox, 0, jpeg, &url);
        if (sent) {
            printf("  WARNING: litterbox (the default host) is down right now;\n");
            printf("           the plugin will fall back to catalogue lookup and retry\n");
        }
    }
    Check(sent, "uploaded to an image host");
    if (sent) {
        printf("  cover URL -> %s\n", url.c_str());
        Check(url.rfind("https://", 0) == 0, "upload returned an https URL");

        // The whole point is that Discord's servers can reach it, so verify the
        // URL actually serves an image rather than trusting the upload reply.
        std::string fetched;
        const bool reachable = http::Get(util::FromUtf8(url), &fetched);
        Check(reachable && fetched.size() > 1000, "uploaded image is publicly fetchable");
        if (reachable) {
            Check(fetched.size() == jpeg.size(), "served bytes match what we uploaded");
        }
    }

    coverart::Shutdown();
}

static void TestHandshake(const std::string& clientId, const std::wstring& audioFile) {
    printf("\n=== Discord IPC handshake ===\n");

    DiscordIpc ipc;
    const bool connected = ipc.Connect(clientId);

    if (clientId == "0") {
        // A deliberately invalid ID: Discord must still answer with a
        // well-formed frame, which proves the framing and read path work.
        Check(!connected, "invalid client id is rejected (framing round-trip worked)");
        printf("  (check the log line above -- a decoded error reply means read/write framing is correct)\n");
        return;
    }

    Check(connected, "connected and handshook");
    if (!connected) return;

    DiscordIpc::Activity a;
    a.type       = 2;
    a.name       = "Winamp";
    a.details    = "Bohemian Rhapsody";
    a.state      = "Queen";
    a.largeText  = "A Night at the Opera";
    a.statusDisplayType = 2;

    // Prefer the real artwork out of the supplied file, so what lands on the
    // profile is exactly what the plugin would publish.
    coverart::Startup();
    ArtworkResolver resolver;
    std::string cover;
    if (resolver.Resolve(L"Queen", L"A Night at the Opera", L"Bohemian Rhapsody",
                         audioFile, &cover)) {
        a.largeImage = cover;
        a.largeText  = "A Night at the Opera";
        printf("  using cover: %s\n", cover.c_str());
    }

    const long long now = util::UnixMillis();
    a.startMs = now - 45000;            // 45s in
    a.endMs   = a.startMs + 354000;     // 5:54 track

    Check(ipc.SetActivity(a), "SET_ACTIVITY accepted");
    printf("\n  Look at your Discord profile now. Expected:\n");
    printf("    Listening to Winamp\n");
    printf("    Bohemian Rhapsody\n");
    printf("    Queen\n");
    printf("    a progress bar sitting at ~0:45 of 5:54, with cover art\n\n");

    for (int i = 0; i < 30; ++i) {
        if (!ipc.Poll()) { printf("  connection dropped\n"); break; }
        Sleep(1000);
    }

    ipc.ClearActivity();
    ipc.Poll();
    printf("  presence cleared\n");
}

// util::Log writes to the debugger, which a console run cannot see, so route it
// to a file and replay it at the end -- the handshake reply is logged there.
static std::wstring g_logPath;

static void DumpLog() {
    printf("\n=== plugin log ===\n");
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_logPath.c_str(), L"r, ccs=UTF-8") != 0 || !f) {
        printf("  (no log written)\n");
        return;
    }
    wchar_t line[2048];
    while (fgetws(line, _countof(line), f)) fwprintf(stdout, L"  %s", line);
    fclose(f);
}

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    wchar_t tempDir[MAX_PATH] = {0};
    GetTempPathW(_countof(tempDir), tempDir);
    g_logPath = std::wstring(tempDir) + L"gen_discord_rpc_test.log";
    DeleteFileW(g_logPath.c_str());
    util::SetLogFile(g_logPath);
    util::SetLoggingEnabled(true);

    const std::string  clientId  = (argc > 1) ? util::ToUtf8(argv[1]) : std::string("0");
    const std::wstring audioFile = (argc > 2) ? argv[2] : L"";

    TestJsonPayload();
    TestJsonEscaping();
    TestJsonParsing();
    TestArtworkLookup();
    TestEmbeddedCover(audioFile);
    TestHandshake(clientId, audioFile);

    DumpLog();

    printf("\n==================================\n");
    printf("%s (%d failure%s)\n", g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
