#include "oauth.h"

#include "http.h"
#include "json.h"
#include "settings.h"
#include "util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>

#include <cstring>
#include <ctime>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

// Optional compile-time embedded credentials. If `tiktok_creds.h` exists
// next to this file (gitignored — see tiktok_creds.h.example for the
// template), it defines TIKTOK_DEFAULT_CLIENT_KEY / _CLIENT_SECRET etc.
// and hasEmbeddedCreds() flips to true.
#if defined(__has_include)
#  if __has_include("tiktok_creds.h")
#    include "tiktok_creds.h"
#  endif
#endif
#ifndef TIKTOK_DEFAULT_CLIENT_KEY
#  define TIKTOK_DEFAULT_CLIENT_KEY     ""
#endif
#ifndef TIKTOK_DEFAULT_CLIENT_SECRET
#  define TIKTOK_DEFAULT_CLIENT_SECRET  ""
#endif
#ifndef TIKTOK_DEFAULT_REDIRECT_URI
#  define TIKTOK_DEFAULT_REDIRECT_URI   "http://localhost:53682/callback"
#endif
#ifndef TIKTOK_DEFAULT_LISTENER_PORT
#  define TIKTOK_DEFAULT_LISTENER_PORT  53682
#endif

namespace {

bool g_wsaInit = false;
bool ensureWsa() {
    if (g_wsaInit) return true;
    WSADATA d{};
    if (WSAStartup(MAKEWORD(2,2), &d) != 0) return false;
    g_wsaInit = true;
    return true;
}

std::vector<BYTE> sha256(const std::string& s) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    DWORD hashLen = 0, dummy = 0;
    BCryptGetProperty(alg, BCRYPT_HASH_LENGTH,
                      (PUCHAR)&hashLen, sizeof(hashLen), &dummy, 0);
    std::vector<BYTE> hash(hashLen);
    BCRYPT_HASH_HANDLE h = nullptr;
    BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    BCryptHashData(h, (PUCHAR)s.data(), (ULONG)s.size(), 0);
    BCryptFinishHash(h, hash.data(), hashLen, 0);
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return hash;
}

std::string base64url(const BYTE* in, size_t n) {
    static const char* A =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    for (size_t i = 0; i < n; i += 3) {
        unsigned b0 = in[i];
        unsigned b1 = (i + 1 < n) ? in[i + 1] : 0;
        unsigned b2 = (i + 2 < n) ? in[i + 2] : 0;
        unsigned t = (b0 << 16) | (b1 << 8) | b2;
        out += A[(t >> 18) & 0x3F];
        out += A[(t >> 12) & 0x3F];
        if (i + 1 < n) out += A[(t >> 6) & 0x3F];
        if (i + 2 < n) out += A[t & 0x3F];
    }
    return out;
}

std::string randomVerifier(size_t n) {
    static const char* A =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    std::vector<BYTE> rb(n);
    BCryptGenRandom(nullptr, rb.data(), (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::string out(n, ' ');
    for (size_t i = 0; i < n; ++i) out[i] = A[rb[i] % 66];
    return out;
}

std::string randomState() {
    BYTE rb[24];
    BCryptGenRandom(nullptr, rb, sizeof(rb), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return base64url(rb, sizeof(rb));
}

std::wstring buildAuthUrl(const std::string& clientKey,
                          const std::string& scope,
                          const std::string& redirectUri,
                          const std::string& state,
                          const std::string& challenge) {
    std::string url = "https://www.tiktok.com/v2/auth/authorize/";
    url += "?client_key=" + urlEncode(clientKey);
    url += "&scope=" + urlEncode(scope);
    url += "&response_type=code";
    url += "&redirect_uri=" + urlEncode(redirectUri);
    url += "&state=" + urlEncode(state);
    url += "&code_challenge=" + urlEncode(challenge);
    url += "&code_challenge_method=S256";
    return s2w(url);
}

// One-shot localhost HTTP listener: blocks until either a request lands
// on /callback, the timeout expires, or `cancel` flips.
struct CallbackResult {
    bool         ok = false;
    std::string  code;
    std::string  state;
    std::string  error_msg;        // populated when authorize endpoint sent ?error=...
    std::wstring transport_err;    // populated when something below the protocol broke
};

std::string urlDecode(const std::string& v) {
    auto hx = [](char c)->int{
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    std::string out;
    for (size_t j = 0; j < v.size(); ++j) {
        if (v[j] == '+') { out += ' '; }
        else if (v[j] == '%' && j + 2 < v.size()) {
            out += (char)((hx(v[j + 1]) << 4) | hx(v[j + 2]));
            j += 2;
        } else {
            out += v[j];
        }
    }
    return out;
}

std::string queryParam(const std::string& path, const std::string& key) {
    size_t q = path.find('?');
    if (q == std::string::npos) return "";
    std::string qs = path.substr(q + 1);
    size_t i = 0;
    while (i < qs.size()) {
        size_t e = qs.find('&', i);
        if (e == std::string::npos) e = qs.size();
        std::string pair = qs.substr(i, e - i);
        size_t eq = pair.find('=');
        std::string k = (eq == std::string::npos) ? pair : pair.substr(0, eq);
        std::string v = (eq == std::string::npos) ? std::string() : pair.substr(eq + 1);
        if (k == key) return urlDecode(v);
        i = e + 1;
    }
    return "";
}

CallbackResult listenForCallback(int port, const OAuth::LogFn& log,
                                 const std::atomic<bool>& cancel) {
    CallbackResult r;
    if (!ensureWsa()) { r.transport_err = L"WSAStartup failed"; return r; }

    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) { r.transport_err = L"socket() failed"; return r; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons((u_short)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(srv, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
        wchar_t buf[80];
        swprintf(buf, 80, L"bind() port %d failed (already in use?)", port);
        r.transport_err = buf;
        closesocket(srv);
        return r;
    }
    if (listen(srv, 1) == SOCKET_ERROR) {
        r.transport_err = L"listen() failed";
        closesocket(srv);
        return r;
    }

    log(L"[oauth] waiting for callback on http://127.0.0.1:" + std::to_wstring(port));

    SOCKET cli = INVALID_SOCKET;
    // 5-minute hard timeout — long enough for a slow user to finish login,
    // short enough that an abandoned attempt eventually unwinds.
    int waitedMs = 0;
    while (waitedMs < 5 * 60 * 1000) {
        if (cancel.load()) {
            closesocket(srv);
            r.transport_err = L"cancelled";
            return r;
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(srv, &rd);
        timeval tv{0, 500000};
        int sel = select(0, &rd, nullptr, nullptr, &tv);
        if (sel > 0 && FD_ISSET(srv, &rd)) {
            cli = accept(srv, nullptr, nullptr);
            break;
        }
        if (sel == SOCKET_ERROR) {
            r.transport_err = L"select() failed";
            closesocket(srv);
            return r;
        }
        waitedMs += 500;
    }
    closesocket(srv);
    if (cli == INVALID_SOCKET) {
        r.transport_err = L"timed out waiting for browser callback";
        return r;
    }

    // Read enough of the request to see the Request-Line + headers. We
    // don't care about the body.
    std::string req;
    char buf[4096];
    for (int i = 0; i < 64; ++i) {
        int got = recv(cli, buf, sizeof(buf), 0);
        if (got <= 0) break;
        req.append(buf, got);
        if (req.find("\r\n\r\n") != std::string::npos) break;
    }

    std::string path;
    {
        size_t sp1 = req.find(' ');
        size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : req.find(' ', sp1 + 1);
        if (sp1 != std::string::npos && sp2 != std::string::npos)
            path = req.substr(sp1 + 1, sp2 - sp1 - 1);
    }

    r.code      = queryParam(path, "code");
    r.state     = queryParam(path, "state");
    r.error_msg = queryParam(path, "error_description");
    if (r.error_msg.empty()) r.error_msg = queryParam(path, "error");
    r.ok = !r.code.empty();

    static const char ok_html[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Connection: close\r\n"
        "Content-Length: 188\r\n"
        "\r\n"
        "<html><body style='font-family:sans-serif;text-align:center;padding:48px'>"
        "<h2>TikTok connected</h2>"
        "<p>You can close this tab and return to the app.</p>"
        "</body></html>";
    static const char err_html[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Connection: close\r\n"
        "Content-Length: 174\r\n"
        "\r\n"
        "<html><body style='font-family:sans-serif;text-align:center;padding:48px'>"
        "<h2>Authorization failed</h2>"
        "<p>Check the app log and retry.</p>"
        "</body></html>";
    const char* html = r.ok ? ok_html : err_html;
    send(cli, html, (int)std::strlen(html), 0);
    shutdown(cli, SD_SEND);
    closesocket(cli);
    return r;
}

OAuth::AuthResult exchangeCodeForToken(const std::string& key,
                                       const std::string& secret,
                                       const std::string& code,
                                       const std::string& redirect,
                                       const std::string& verifier) {
    OAuth::AuthResult ar;
    std::string body;
    body += "client_key="    + urlEncode(key);
    body += "&client_secret=" + urlEncode(secret);
    body += "&code="         + urlEncode(code);
    body += "&grant_type=authorization_code";
    body += "&redirect_uri=" + urlEncode(redirect);
    body += "&code_verifier=" + urlEncode(verifier);

    HttpResponse resp = httpPostForm(L"https://open.tiktokapis.com/v2/oauth/token/", body);
    if (resp.status == 0) {
        ar.error = L"network: " + resp.error;
        return ar;
    }
    if (resp.status != 200) {
        ar.error = L"token endpoint HTTP " + std::to_wstring(resp.status)
                 + L": " + s2w(resp.body);
        return ar;
    }
    std::string err;
    Json j = Json::parse(resp.body, &err);
    if (!j.isObject()) {
        ar.error = L"bad token response: " + s2w(err);
        return ar;
    }
    if (j.has("error") && !j["error"].asStr().empty()) {
        ar.error = s2w("token error: " + j["error"].asStr() + " — "
                       + j["error_description"].asStr());
        return ar;
    }
    long long now = (long long)std::time(nullptr);
    ar.tokens.access_token       = j["access_token"].asStr();
    ar.tokens.refresh_token      = j["refresh_token"].asStr();
    ar.tokens.expires_at         = now + j["expires_in"].asI64();
    ar.tokens.refresh_expires_at = now + j["refresh_expires_in"].asI64();
    ar.tokens.open_id            = j["open_id"].asStr();
    ar.tokens.scope              = j["scope"].asStr();
    ar.success                   = !ar.tokens.access_token.empty();
    if (!ar.success) ar.error = L"empty access_token in response";
    return ar;
}

long long parseI64(const std::string& s) {
    if (s.empty()) return 0;
    try { return std::stoll(s); } catch (...) { return 0; }
}

}  // namespace

namespace OAuth {

AuthResult tiktokAuthorize(const std::string& clientKey,
                           const std::string& clientSecret,
                           const std::string& redirectUri,
                           int listenerPort,
                           const LogFn& log,
                           const std::atomic<bool>& cancel) {
    AuthResult ar;
    if (clientKey.empty() || clientSecret.empty()) {
        ar.error = L"Client Key ve Client Secret bos olamaz.";
        return ar;
    }
    if (redirectUri.empty()) {
        ar.error = L"Redirect URI bos olamaz.";
        return ar;
    }

    std::string       verifier   = randomVerifier(64);
    std::vector<BYTE> hash       = sha256(verifier);
    std::string       challenge  = base64url(hash.data(), hash.size());
    std::string       state      = randomState();

    // Scope choice: user.info.basic gives us open_id (so we can show
    // "Connected as ..."), video.upload is the inbox/draft scope that
    // sandbox-mode apps are allowed to call without app review.
    std::wstring authUrl = buildAuthUrl(clientKey,
                                        "user.info.basic,video.upload",
                                        redirectUri, state, challenge);
    log(L"[oauth] opening browser...");
    HINSTANCE rc = ShellExecuteW(nullptr, L"open", authUrl.c_str(),
                                 nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)rc <= 32) {
        ar.error = L"Tarayici acilamadi. URL'i manuel kopyala:\n" + authUrl;
        return ar;
    }

    CallbackResult cb = listenForCallback(listenerPort, log, cancel);
    if (!cb.ok) {
        if (!cb.transport_err.empty()) {
            ar.error = cb.transport_err;
        } else {
            ar.error = L"Yetkilendirme reddedildi: " + s2w(cb.error_msg);
        }
        return ar;
    }
    if (cb.state != state) {
        ar.error = L"State uyusmazligi (CSRF) — durduruldu.";
        return ar;
    }
    log(L"[oauth] code alindi, token istemi yapiliyor...");

    return exchangeCodeForToken(clientKey, clientSecret,
                                cb.code, redirectUri, verifier);
}

AuthResult tiktokRefresh(const std::string& clientKey,
                         const std::string& clientSecret,
                         const std::string& refreshToken) {
    AuthResult ar;
    std::string body;
    body += "client_key="     + urlEncode(clientKey);
    body += "&client_secret=" + urlEncode(clientSecret);
    body += "&grant_type=refresh_token";
    body += "&refresh_token=" + urlEncode(refreshToken);
    HttpResponse resp = httpPostForm(L"https://open.tiktokapis.com/v2/oauth/token/", body);
    if (resp.status == 0) { ar.error = L"network: " + resp.error; return ar; }
    if (resp.status != 200) {
        ar.error = L"refresh HTTP " + std::to_wstring(resp.status) + L": " + s2w(resp.body);
        return ar;
    }
    Json j = Json::parse(resp.body);
    if (j.has("error") && !j["error"].asStr().empty()) {
        ar.error = s2w("refresh error: " + j["error"].asStr() + " — "
                       + j["error_description"].asStr());
        return ar;
    }
    long long now = (long long)std::time(nullptr);
    ar.tokens.access_token       = j["access_token"].asStr();
    ar.tokens.refresh_token      = j["refresh_token"].asStr();
    ar.tokens.expires_at         = now + j["expires_in"].asI64();
    ar.tokens.refresh_expires_at = now + j["refresh_expires_in"].asI64();
    ar.tokens.open_id            = j["open_id"].asStr();
    ar.tokens.scope              = j["scope"].asStr();
    ar.success                   = !ar.tokens.access_token.empty();
    return ar;
}

void saveTokens(const Tokens& t) {
    Settings::set("tiktok.access_token",       t.access_token);
    Settings::set("tiktok.refresh_token",      t.refresh_token);
    Settings::set("tiktok.expires_at",         std::to_string(t.expires_at));
    Settings::set("tiktok.refresh_expires_at", std::to_string(t.refresh_expires_at));
    Settings::set("tiktok.open_id",            t.open_id);
    Settings::set("tiktok.scope",              t.scope);
    Settings::save();
}

bool loadTokens(Tokens& out) {
    out.access_token       = Settings::get("tiktok.access_token");
    out.refresh_token      = Settings::get("tiktok.refresh_token");
    out.expires_at         = parseI64(Settings::get("tiktok.expires_at"));
    out.refresh_expires_at = parseI64(Settings::get("tiktok.refresh_expires_at"));
    out.open_id            = Settings::get("tiktok.open_id");
    out.scope              = Settings::get("tiktok.scope");
    return !out.access_token.empty();
}

void clearTokens() {
    Settings::erase("tiktok.access_token");
    Settings::erase("tiktok.refresh_token");
    Settings::erase("tiktok.expires_at");
    Settings::erase("tiktok.refresh_expires_at");
    Settings::erase("tiktok.open_id");
    Settings::erase("tiktok.scope");
    Settings::save();
}

bool isTokenValid(const Tokens& t) {
    if (t.access_token.empty()) return false;
    long long now = (long long)std::time(nullptr);
    return t.expires_at > now + 30;
}

bool hasEmbeddedCreds() {
    return TIKTOK_DEFAULT_CLIENT_KEY[0] != '\0'
        && TIKTOK_DEFAULT_CLIENT_SECRET[0] != '\0';
}

std::string effectiveClientKey() {
    std::string s = Settings::get("tiktok.client_key");
    return !s.empty() ? s : std::string(TIKTOK_DEFAULT_CLIENT_KEY);
}

std::string effectiveClientSecret() {
    std::string s = Settings::get("tiktok.client_secret");
    return !s.empty() ? s : std::string(TIKTOK_DEFAULT_CLIENT_SECRET);
}

std::string effectiveRedirectUri() {
    std::string s = Settings::get("tiktok.redirect_uri");
    return !s.empty() ? s : std::string(TIKTOK_DEFAULT_REDIRECT_URI);
}

int effectiveListenerPort() {
    std::string s = Settings::get("tiktok.listener_port");
    if (!s.empty()) {
        try { return std::stoi(s); } catch (...) {}
    }
    return TIKTOK_DEFAULT_LISTENER_PORT;
}

}  // namespace OAuth
