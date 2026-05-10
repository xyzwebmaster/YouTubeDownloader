#pragma once
#include <atomic>
#include <functional>
#include <string>

// Browser-driven social uploads. Spawns Python helpers that drive
// Chromium via Playwright with persistent profiles, so the user logs
// in once and the cookies are reused on every upload.
//
// This is a deliberately less-stable path than the OAuth-based
// upload_tiktok module: it depends on Python + Playwright at runtime,
// it can break whenever TikTok changes the upload page, and TikTok's
// terms of service treat automated posting as ban-worthy. Use a test
// account if account safety matters.
namespace UploadBrowser {

struct UploadResult {
    bool         success = false;
    std::wstring error;
};

struct LoginStatus {
    bool         success = false;
    bool         logged_in = false;
    std::wstring username;
    std::wstring error;
};

using LogFn   = std::function<void(const std::wstring&)>;
using StageFn = std::function<void(const std::wstring& stage)>;

// Open the TikTok helper in `setup` mode: visible Chromium pointing
// at TikTok's login page. Blocks until the user closes the window.
UploadResult runSetup(const LogFn& log, const std::atomic<bool>& cancel);

// Same setup flow for Instagram's browser helper.
UploadResult runInstagramSetup(const LogFn& log, const std::atomic<bool>& cancel);

// Headless probe of the persistent profile. Tells us whether the
// user is currently signed in and (best-effort) their @handle.
LoginStatus  getStatus(const LogFn& log, const std::atomic<bool>& cancel);

// Upload a single TikTok file with an optional caption. Streams stage names
// ("open", "uploading", "ready", "posted") through `stage`.
UploadResult uploadOne(const std::wstring& filePath,
                       const std::wstring& caption,
                       const LogFn& log,
                       const StageFn& stage,
                       const std::atomic<bool>& cancel);

// Upload a single Instagram file with the already-composed caption.
UploadResult uploadInstagramOne(const std::wstring& filePath,
                                const std::wstring& caption,
                                const LogFn& log,
                                const StageFn& stage,
                                const std::atomic<bool>& cancel);

// True only if both the script and a Python interpreter exist.
bool         isPythonAvailable();
std::wstring scriptPath();   // .exe-relative path to tiktok_uploader.py
bool         isInstagramAvailable();
std::wstring instagramScriptPath();   // .exe-relative path to instagram_uploader.py

}  // namespace UploadBrowser
