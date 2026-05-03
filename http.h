#pragma once
#include <map>
#include <string>
#include <vector>

struct HttpResponse {
    long        status = 0;     // HTTP status code; 0 if transport-level failure.
    std::string body;           // raw response body bytes (UTF-8 if text).
    std::map<std::wstring, std::wstring> headers;
    std::wstring error;         // populated when status == 0.
};

// Synchronous HTTP request. Caller supplies "Name: Value" header lines and
// the raw request body (may be empty). https:// uses TLS; redirects are
// followed automatically.
HttpResponse httpRequest(const std::wstring& method,
                         const std::wstring& url,
                         const std::vector<std::wstring>& extraHeaders,
                         const std::vector<unsigned char>& body);

inline HttpResponse httpGet(const std::wstring& url,
                            const std::vector<std::wstring>& extraHeaders = {}) {
    return httpRequest(L"GET", url, extraHeaders, {});
}

inline HttpResponse httpPostJson(const std::wstring& url,
                                 const std::string& jsonBody,
                                 const std::vector<std::wstring>& extraHeaders = {}) {
    std::vector<std::wstring> h = extraHeaders;
    h.push_back(L"Content-Type: application/json; charset=UTF-8");
    return httpRequest(L"POST", url, h,
                       std::vector<unsigned char>(jsonBody.begin(), jsonBody.end()));
}

inline HttpResponse httpPostForm(const std::wstring& url,
                                 const std::string& formBody,
                                 const std::vector<std::wstring>& extraHeaders = {}) {
    std::vector<std::wstring> h = extraHeaders;
    h.push_back(L"Content-Type: application/x-www-form-urlencoded");
    return httpRequest(L"POST", url, h,
                       std::vector<unsigned char>(formBody.begin(), formBody.end()));
}

inline HttpResponse httpPutBytes(const std::wstring& url,
                                 const std::vector<unsigned char>& body,
                                 const std::vector<std::wstring>& extraHeaders = {}) {
    return httpRequest(L"PUT", url, extraHeaders, body);
}

// Percent-encoding for application/x-www-form-urlencoded bodies and query
// strings. RFC 3986 unreserved set is left untouched.
std::string urlEncode(const std::string& s);
