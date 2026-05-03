#include "upload_tiktok.h"

#include "http.h"
#include "json.h"
#include "util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cwctype>
#include <vector>

namespace UploadTikTok {

namespace {

// 16 MB falls inside TikTok's [5 MB, 64 MB] chunk-size window and keeps
// the per-PUT buffer modest. We also clamp `total_chunk_count` to 1000
// (the API max) and grow chunk size to fit if needed.
const long long DEFAULT_CHUNK = 16LL * 1024 * 1024;

std::string mimeForFile(const std::wstring& path) {
    auto endsWith = [&](const wchar_t* sfx) {
        size_t L = std::wcslen(sfx);
        if (path.size() < L) return false;
        for (size_t i = 0; i < L; ++i) {
            wchar_t a = (wchar_t)std::towlower(path[path.size() - L + i]);
            wchar_t b = (wchar_t)std::towlower(sfx[i]);
            if (a != b) return false;
        }
        return true;
    };
    if (endsWith(L".mp4"))  return "video/mp4";
    if (endsWith(L".mov"))  return "video/quicktime";
    if (endsWith(L".webm")) return "video/webm";
    if (endsWith(L".mkv"))  return "video/x-matroska";
    return "application/octet-stream";
}

bool readChunk(HANDLE h, long long offset, long long n,
               std::vector<unsigned char>& out) {
    LARGE_INTEGER li{};
    li.QuadPart = offset;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    out.resize((size_t)n);
    long long readSoFar = 0;
    while (readSoFar < n) {
        DWORD want = (DWORD)((n - readSoFar > (long long)0x7FFFFFFF)
                                ? 0x7FFFFFFF
                                : (n - readSoFar));
        DWORD got = 0;
        if (!ReadFile(h, out.data() + readSoFar, want, &got, nullptr) || got == 0)
            return false;
        readSoFar += got;
    }
    return true;
}

}  // namespace

UploadResult uploadInbox(const std::wstring& filePath,
                         const std::string&  accessToken,
                         const LogFn&        log,
                         const ProgressFn&   progress,
                         const std::atomic<bool>& cancel) {
    UploadResult r;

    HANDLE h = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        r.error = L"Dosya acilamadi: " + filePath;
        return r;
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) {
        CloseHandle(h);
        r.error = L"GetFileSizeEx failed";
        return r;
    }
    long long total = sz.QuadPart;
    if (total == 0) {
        CloseHandle(h);
        r.error = L"Bos dosya";
        return r;
    }

    long long chunkSize = (total < DEFAULT_CHUNK) ? total : DEFAULT_CHUNK;
    long long totalChunks = (total + chunkSize - 1) / chunkSize;
    if (totalChunks > 1000) {
        chunkSize = (total + 999) / 1000;
        totalChunks = (total + chunkSize - 1) / chunkSize;
    }

    log(L"[upload] " + filePath);
    log(L"[upload] size=" + std::to_wstring(total)
        + L"  chunks=" + std::to_wstring(totalChunks)
        + L"  chunk_size=" + std::to_wstring(chunkSize));

    // ---- INIT ----
    Json initBody = Json::object();
    Json src = Json::object();
    src.set("source",            Json("FILE_UPLOAD"));
    src.set("video_size",        Json((long long)total));
    src.set("chunk_size",        Json((long long)chunkSize));
    src.set("total_chunk_count", Json((long long)totalChunks));
    initBody.set("source_info", src);

    std::vector<std::wstring> auth = {
        L"Authorization: Bearer " + s2w(accessToken)
    };
    HttpResponse initResp = httpPostJson(
        L"https://open.tiktokapis.com/v2/post/publish/inbox/video/init/",
        initBody.dump(), auth);
    if (initResp.status == 0) {
        CloseHandle(h);
        r.error = L"INIT network: " + initResp.error;
        return r;
    }
    if (initResp.status != 200) {
        CloseHandle(h);
        r.error = L"INIT HTTP " + std::to_wstring(initResp.status)
                + L": " + s2w(initResp.body);
        return r;
    }
    Json ij = Json::parse(initResp.body);
    std::string err_code = ij.path("error.code").asStr();
    if (!err_code.empty() && err_code != "ok") {
        CloseHandle(h);
        r.error = L"INIT error: " + s2w(err_code) + L" — "
                + s2w(ij.path("error.message").asStr());
        return r;
    }
    std::string publish_id = ij.path("data.publish_id").asStr();
    std::string upload_url = ij.path("data.upload_url").asStr();
    if (upload_url.empty()) {
        CloseHandle(h);
        r.error = L"INIT response missing upload_url";
        return r;
    }
    r.publish_id = publish_id;
    log(L"[upload] publish_id=" + s2w(publish_id));

    // ---- PUT chunks ----
    std::string mime = mimeForFile(filePath);
    std::vector<unsigned char> buf;
    long long sent = 0;
    if (progress) progress(0, total);

    for (long long i = 0; i < totalChunks; ++i) {
        if (cancel.load()) {
            CloseHandle(h);
            r.error = L"cancelled";
            return r;
        }
        long long start = i * chunkSize;
        long long end   = (i + 1 == totalChunks) ? (total - 1)
                                                  : (start + chunkSize - 1);
        long long thisChunk = end - start + 1;

        if (!readChunk(h, start, thisChunk, buf)) {
            CloseHandle(h);
            r.error = L"read failed at offset " + std::to_wstring(start);
            return r;
        }

        char rangeBuf[80];
        std::snprintf(rangeBuf, sizeof(rangeBuf),
                      "bytes %lld-%lld/%lld", start, end, total);
        std::vector<std::wstring> hdrs = {
            L"Content-Type: "  + s2w(mime),
            L"Content-Range: " + s2w(rangeBuf),
        };

        HttpResponse pr = httpPutBytes(s2w(upload_url), buf, hdrs);
        // TikTok returns 206 Partial Content while more chunks are
        // expected, then 201 Created (sometimes 200) on the final chunk.
        if (pr.status == 0) {
            CloseHandle(h);
            r.error = L"PUT network: " + pr.error;
            return r;
        }
        if (pr.status != 206 && pr.status != 201 && pr.status != 200) {
            CloseHandle(h);
            r.error = L"PUT HTTP " + std::to_wstring(pr.status)
                    + L": " + s2w(pr.body);
            return r;
        }
        sent += thisChunk;
        if (progress) progress(sent, total);
        log(L"[upload] chunk " + std::to_wstring(i + 1) + L"/"
            + std::to_wstring(totalChunks) + L" ok ("
            + std::to_wstring(sent) + L"/" + std::to_wstring(total) + L")");
    }
    CloseHandle(h);

    // ---- Status (informational only — Inbox uploads complete
    // asynchronously on TikTok's side). ----
    Json statusBody = Json::object();
    statusBody.set("publish_id", Json(publish_id));
    HttpResponse sr = httpPostJson(
        L"https://open.tiktokapis.com/v2/post/publish/status/fetch/",
        statusBody.dump(), auth);
    if (sr.status == 200) {
        Json sj = Json::parse(sr.body);
        std::string st = sj.path("data.status").asStr();
        if (!st.empty()) log(L"[upload] status=" + s2w(st));
    }

    r.success = true;
    return r;
}

}  // namespace UploadTikTok
