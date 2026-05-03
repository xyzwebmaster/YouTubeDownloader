#pragma once
#include <atomic>
#include <functional>
#include <string>

// TikTok OAuth 2.0 authorization-code flow with PKCE. The client opens
// the user's default browser, then this module spins up a tiny localhost
// HTTP server on a fixed port to receive the redirect.
namespace OAuth {

struct Tokens {
    std::string access_token;
    std::string refresh_token;
    long long   expires_at = 0;          // unix epoch seconds
    long long   refresh_expires_at = 0;
    std::string open_id;
    std::string scope;
};

struct AuthResult {
    bool         success = false;
    Tokens       tokens;
    std::wstring error;                  // populated when !success
};

using LogFn = std::function<void(const std::wstring&)>;

// Run the full PKCE flow. Blocks until the user finishes (or cancel goes
// true). Intended to be called on a worker thread; the caller forwards
// log lines to its UI through `log`.
AuthResult tiktokAuthorize(const std::string& clientKey,
                           const std::string& clientSecret,
                           const std::string& redirectUri,
                           int                 listenerPort,
                           const LogFn&        log,
                           const std::atomic<bool>& cancel);

// Refresh an expired access token. Pure HTTP call, no listener needed.
AuthResult tiktokRefresh(const std::string& clientKey,
                         const std::string& clientSecret,
                         const std::string& refreshToken);

// Persist / load / clear tokens via the Settings module under the
// "tiktok.*" namespace. saveTokens() also calls Settings::save().
void saveTokens(const Tokens& t);
bool loadTokens(Tokens& out);   // returns false if access_token is empty
void clearTokens();

// True if the access token isn't empty and has at least 30s of life left.
bool isTokenValid(const Tokens& t);

}  // namespace OAuth
