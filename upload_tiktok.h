#pragma once
#include <atomic>
#include <functional>
#include <string>

// TikTok Content Posting API client. Currently only implements the
// "Inbox" / draft flow that sandbox / unaudited apps are allowed to call:
//
//   POST /v2/post/publish/inbox/video/init/   -> publish_id, upload_url
//   PUT  upload_url (chunked)                 -> uploads bytes
//   POST /v2/post/publish/status/fetch/       -> status (informational)
//
// The video lands in the user's "Drafts" inside the TikTok app. The user
// has to open the app and tap Post to publish — direct posting requires
// app review and the video.publish scope, which we don't ask for.
namespace UploadTikTok {

struct UploadResult {
    bool         success = false;
    std::string  publish_id;
    std::wstring error;
};

using LogFn      = std::function<void(const std::wstring&)>;
using ProgressFn = std::function<void(long long sent, long long total)>;

UploadResult uploadInbox(const std::wstring& filePath,
                         const std::string&  accessToken,
                         const LogFn&        log,
                         const ProgressFn&   progress,
                         const std::atomic<bool>& cancel);

}  // namespace UploadTikTok
