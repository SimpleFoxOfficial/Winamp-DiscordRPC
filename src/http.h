#pragma once

#include <string>
#include <vector>

namespace http {

// Performs a plain HTTPS GET. Returns false on any transport or non-200
// response. `url` must be absolute and https://. Blocking -- callers run this
// on the worker thread, never on Winamp's UI thread.
bool Get(const std::wstring& url, std::string* body, int timeoutMs = 8000);

// POSTs a pre-built body with an explicit Content-Type. Used for multipart
// form uploads, whose body is assembled by the caller.
bool Post(const std::wstring& url, const std::wstring& contentType,
          const std::vector<char>& body, std::string* response, int timeoutMs = 30000);

// Builds an RFC 7578 multipart/form-data body. `fileField` is emitted last, as
// some endpoints expect the file part after the plain text fields.
void BuildMultipart(const std::vector<std::pair<std::string, std::string>>& fields,
                    const std::string& fileField,
                    const std::string& fileName,
                    const std::string& fileContentType,
                    const std::vector<unsigned char>& fileBytes,
                    std::vector<char>* body,
                    std::wstring* contentType);

} // namespace http
