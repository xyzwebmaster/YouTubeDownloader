#include "http.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <cstdio>

#include "util.h"

#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#endif

namespace {

struct HSession { HINTERNET h = nullptr; ~HSession(){ if(h) WinHttpCloseHandle(h);} };
struct HConnect { HINTERNET h = nullptr; ~HConnect(){ if(h) WinHttpCloseHandle(h);} };
struct HRequest { HINTERNET h = nullptr; ~HRequest(){ if(h) WinHttpCloseHandle(h);} };

static std::wstring formatLastError(DWORD code) {
    LPWSTR msg = nullptr;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
        GetModuleHandleW(L"winhttp.dll"),
        code, 0, (LPWSTR)&msg, 0, nullptr);
    std::wstring out;
    if (n && msg) {
        out.assign(msg, n);
        // strip trailing \r\n
        while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n')) out.pop_back();
        LocalFree(msg);
    }
    wchar_t buf[32];
    swprintf(buf, 32, L" (0x%08lx)", code);
    out += buf;
    return out;
}

// Split combined header block "Name: Value\r\nName: Value\r\n..." into a map.
static std::map<std::wstring, std::wstring> parseHeaderBlock(const std::wstring& blob) {
    std::map<std::wstring, std::wstring> out;
    size_t i = 0;
    while (i < blob.size()) {
        size_t e = blob.find(L"\r\n", i);
        if (e == std::wstring::npos) e = blob.size();
        std::wstring line = blob.substr(i, e - i);
        i = (e == blob.size()) ? e : e + 2;
        size_t colon = line.find(L':');
        if (colon == std::wstring::npos) continue;
        std::wstring name  = line.substr(0, colon);
        std::wstring value = line.substr(colon + 1);
        // trim leading space
        while (!value.empty() && (value.front() == L' ' || value.front() == L'\t'))
            value.erase(value.begin());
        // lowercase header name
        for (auto& c : name) c = (wchar_t)towlower(c);
        out[name] = value;
    }
    return out;
}

}  // namespace

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        bool unreserved =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

HttpResponse httpRequest(const std::wstring& method,
                         const std::wstring& url,
                         const std::vector<std::wstring>& extraHeaders,
                         const std::vector<unsigned char>& body) {
    HttpResponse r;

    // Crack URL into scheme/host/port/path.
    URL_COMPONENTSW uc{};
    uc.dwStructSize     = sizeof(uc);
    wchar_t scheme[16]={0}, host[256]={0}, path[2048]={0};
    uc.lpszScheme       = scheme;     uc.dwSchemeLength      = (DWORD)(sizeof(scheme) /sizeof(wchar_t));
    uc.lpszHostName     = host;       uc.dwHostNameLength    = (DWORD)(sizeof(host)   /sizeof(wchar_t));
    uc.lpszUrlPath      = path;       uc.dwUrlPathLength     = (DWORD)(sizeof(path)   /sizeof(wchar_t));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        r.error = L"WinHttpCrackUrl failed: " + formatLastError(GetLastError());
        return r;
    }
    bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    INTERNET_PORT port = uc.nPort ? uc.nPort : (secure ? INTERNET_DEFAULT_HTTPS_PORT
                                                        : INTERNET_DEFAULT_HTTP_PORT);

    HSession session;
    session.h = WinHttpOpen(L"YouTubeDownloader/1.0",
                            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) {
        r.error = L"WinHttpOpen failed: " + formatLastError(GetLastError());
        return r;
    }
    // Reasonable timeouts (resolve, connect, send, receive). Big uploads
    // need a long receive timeout because WinHttpReceiveResponse blocks
    // until the server acks.
    WinHttpSetTimeouts(session.h, 30000, 30000, 60000, 600000);

    HConnect connect;
    connect.h = WinHttpConnect(session.h, host, port, 0);
    if (!connect.h) {
        r.error = L"WinHttpConnect failed: " + formatLastError(GetLastError());
        return r;
    }

    DWORD reqFlags = secure ? WINHTTP_FLAG_SECURE : 0;
    HRequest req;
    req.h = WinHttpOpenRequest(connect.h, method.c_str(), path,
                               nullptr, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES, reqFlags);
    if (!req.h) {
        r.error = L"WinHttpOpenRequest failed: " + formatLastError(GetLastError());
        return r;
    }

    // Auto-follow up to 5 redirects (default behavior, but be explicit).
    DWORD redirOpt = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirOpt, sizeof(redirOpt));

    // Append headers
    std::wstring hdr;
    for (const auto& h : extraHeaders) {
        hdr += h;
        hdr += L"\r\n";
    }
    if (!hdr.empty()) {
        if (!WinHttpAddRequestHeaders(req.h, hdr.c_str(), (DWORD)hdr.size(),
                                      WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
            r.error = L"WinHttpAddRequestHeaders failed: " + formatLastError(GetLastError());
            return r;
        }
    }

    LPVOID bodyPtr = body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data();
    DWORD  bodyLen = (DWORD)body.size();
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            bodyPtr, bodyLen, bodyLen, 0)) {
        r.error = L"WinHttpSendRequest failed: " + formatLastError(GetLastError());
        return r;
    }
    if (!WinHttpReceiveResponse(req.h, nullptr)) {
        r.error = L"WinHttpReceiveResponse failed: " + formatLastError(GetLastError());
        return r;
    }

    // Status code.
    DWORD status = 0, statusSize = sizeof(status);
    if (WinHttpQueryHeaders(req.h,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                            WINHTTP_NO_HEADER_INDEX)) {
        r.status = (long)status;
    }

    // All response headers as a CRLF-separated blob.
    DWORD hSize = 0;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                        WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &hSize,
                        WINHTTP_NO_HEADER_INDEX);
    if (hSize > 2) {
        std::wstring buf(hSize / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                WINHTTP_HEADER_NAME_BY_INDEX, buf.data(), &hSize,
                                WINHTTP_NO_HEADER_INDEX)) {
            buf.resize(hSize / sizeof(wchar_t));
            r.headers = parseHeaderBlock(buf);
        }
    }

    // Body.
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) {
            r.error = L"WinHttpQueryDataAvailable failed: " + formatLastError(GetLastError());
            break;
        }
        if (!avail) break;
        size_t old = r.body.size();
        r.body.resize(old + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req.h, r.body.data() + old, avail, &got)) {
            r.error = L"WinHttpReadData failed: " + formatLastError(GetLastError());
            r.body.resize(old);
            break;
        }
        r.body.resize(old + got);
        if (got == 0) break;
    }
    return r;
}
