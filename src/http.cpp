#include "http.h"
#include "util.h"

#include <windows.h>
#include <winhttp.h>
#include <vector>

namespace http {

namespace {

// RAII for the three WinHTTP handle types, which all close the same way.
struct WinHttpHandle {
    HINTERNET h = nullptr;
    explicit WinHttpHandle(HINTERNET handle = nullptr) : h(handle) {}
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    operator HINTERNET() const { return h; }
};

// Splits an absolute https URL into the pieces WinHTTP needs.
bool CrackUrl(const std::wstring& url, std::wstring* host, std::wstring* path, INTERNET_PORT* port) {
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize      = sizeof(uc);
    uc.dwSchemeLength    = (DWORD)-1;
    uc.dwHostNameLength  = (DWORD)-1;
    uc.dwUrlPathLength   = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;

    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) return false;
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) return false;

    host->assign(uc.lpszHostName, uc.dwHostNameLength);
    path->assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength) path->append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path->empty()) *path = L"/";
    *port = uc.nPort;
    return true;
}

// Drains the response body of an open request handle.
bool ReadBody(HINTERNET request, std::string* body) {
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) return false;
        if (avail == 0) break;

        std::vector<char> chunk(avail);
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), avail, &read)) return false;
        if (read == 0) break;
        body->append(chunk.data(), read);

        if (body->size() > (4u << 20)) break; // guard against a runaway response
    }
    return true;
}

} // namespace

bool Get(const std::wstring& url, std::string* body, int timeoutMs) {
    body->clear();

    std::wstring host, path;
    INTERNET_PORT port = 0;
    if (!CrackUrl(url, &host, &path, &port)) return false;
    const INTERNET_PORT uc_nPort = port;

    WinHttpHandle session(WinHttpOpen(L"gen_discord_rpc/1.0",
                                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return false;

    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    WinHttpHandle connect(WinHttpConnect(session, host.c_str(), uc_nPort, 0));
    if (!connect) return false;

    WinHttpHandle request(WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
                                             WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE));
    if (!request) return false;

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return false;
    if (!WinHttpReceiveResponse(request, nullptr)) return false;

    DWORD status = 0, statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                             WINHTTP_NO_HEADER_INDEX)) return false;
    if (status != 200) {
        util::Log(L"http %lu for %s", status, url.c_str());
        return false;
    }

    return ReadBody(request, body);
}

bool Post(const std::wstring& url, const std::wstring& contentType,
          const std::vector<char>& body, std::string* response, int timeoutMs) {
    response->clear();

    std::wstring host, path;
    INTERNET_PORT port = 0;
    if (!CrackUrl(url, &host, &path, &port)) return false;

    WinHttpHandle session(WinHttpOpen(L"gen_discord_rpc/1.0",
                                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return false;
    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    WinHttpHandle connect(WinHttpConnect(session, host.c_str(), port, 0));
    if (!connect) return false;

    WinHttpHandle request(WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr,
                                             WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE));
    if (!request) return false;

    const std::wstring headers = L"Content-Type: " + contentType;
    if (!WinHttpSendRequest(request, headers.c_str(), (DWORD)headers.size(),
                            (LPVOID)body.data(), (DWORD)body.size(),
                            (DWORD)body.size(), 0)) {
        util::Log(L"upload send failed, err=%lu", GetLastError());
        return false;
    }
    if (!WinHttpReceiveResponse(request, nullptr)) return false;

    DWORD status = 0, statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                             WINHTTP_NO_HEADER_INDEX)) return false;
    if (status != 200) {
        util::Log(L"http %lu posting to %s", status, url.c_str());
        return false;
    }
    return ReadBody(request, response);
}

void BuildMultipart(const std::vector<std::pair<std::string, std::string>>& fields,
                    const std::string& fileField,
                    const std::string& fileName,
                    const std::string& fileContentType,
                    const std::vector<unsigned char>& fileBytes,
                    std::vector<char>* body,
                    std::wstring* contentType) {
    char boundaryBuf[64];
    sprintf_s(boundaryBuf, sizeof(boundaryBuf), "----gendiscordrpc%08lx%08lx",
              (unsigned long)GetTickCount(), (unsigned long)(rand() & 0x7FFFFFFF));
    const std::string boundary = boundaryBuf;

    std::string head;
    for (const auto& kv : fields) {
        head += "--" + boundary + "\r\n";
        head += "Content-Disposition: form-data; name=\"" + kv.first + "\"\r\n\r\n";
        head += kv.second + "\r\n";
    }
    head += "--" + boundary + "\r\n";
    head += "Content-Disposition: form-data; name=\"" + fileField +
            "\"; filename=\"" + fileName + "\"\r\n";
    head += "Content-Type: " + fileContentType + "\r\n\r\n";

    const std::string tail = "\r\n--" + boundary + "--\r\n";

    body->clear();
    body->reserve(head.size() + fileBytes.size() + tail.size());
    body->insert(body->end(), head.begin(), head.end());
    body->insert(body->end(), fileBytes.begin(), fileBytes.end());
    body->insert(body->end(), tail.begin(), tail.end());

    *contentType = L"multipart/form-data; boundary=" + util::FromUtf8(boundary);
}

} // namespace http
