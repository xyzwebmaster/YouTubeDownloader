#include "download.h"

#include "app.h"
#include "process.h"
#include "util.h"

#include <atomic>
#include <cwchar>
#include <mutex>

static std::vector<std::wstring> buildDownloadArgs(const VideoEntry &v,
                                                   const DownloadOpts &o) {
    std::vector<std::wstring> a = {
        L"yt-dlp",
        L"--encoding", L"utf-8",
        L"--no-playlist",
        L"--ignore-errors",
        L"--no-warnings",
        L"--newline",
        L"-o",
        o.outputDir + L"\\%(uploader)s\\%(title).200B [%(id)s].%(ext)s",
    };
    if (o.audioOnly) {
        a.push_back(L"-x");
        if (o.audioFormat != L"best") {
            a.push_back(L"--audio-format");
            a.push_back(o.audioFormat);
        }
        a.push_back(L"--audio-quality");
        a.push_back(o.audioQuality);
    } else {
        std::wstring fmt;
        if (o.maxHeight < 0) {
            fmt = L"bestvideo*+bestaudio/best";
        } else {
            wchar_t b[256];
            swprintf(b, 256,
                     L"bestvideo[height<=%d]+bestaudio/best[height<=%d]/best",
                     o.maxHeight, o.maxHeight);
            fmt = b;
        }
        a.push_back(L"-f");
        a.push_back(fmt);
        if (!o.container.empty()) {
            a.push_back(L"--merge-output-format");
            a.push_back(o.container);
            a.push_back(L"--remux-video");
            a.push_back(o.container);
        }
    }
    a.push_back(v.url);
    return a;
}

void scanWorker(std::wstring channelBase, bool wantVideos, bool wantShorts) {
    auto runOne = [&](const std::wstring &target, bool isShorts) {
        postLog(L"[scan] fetching: " + target);
        std::vector<std::wstring> args = {
            L"yt-dlp",
            L"--encoding", L"utf-8",
            L"--flat-playlist",
            L"--no-warnings",
            L"--ignore-errors",
            L"--print", L"%(id)s\t%(title)s\t%(duration|0)s",
            target
        };
        std::wstring cmd = buildCmdLine(args);
        PipedProcess p = startProcess(cmd);
        if (!p.process) {
            postLog(L"[scan] failed to start yt-dlp. Is it installed and on PATH?");
            return;
        }
        {
            std::lock_guard<std::mutex> g(g_processMutex);
            g_currentProcess = p.process;
        }
        int code = readAllLinesAndWait(p, [&](const std::string &line) {
            if (g_cancelRequested.load()) return;
            std::wstring w = s2w(line);
            size_t t1 = w.find(L'\t');
            if (t1 == std::wstring::npos) return;
            size_t t2 = w.find(L'\t', t1 + 1);
            if (t2 == std::wstring::npos) return;
            VideoEntry e;
            e.id    = w.substr(0, t1);
            e.title = w.substr(t1 + 1, t2 - t1 - 1);
            std::wstring dur = w.substr(t2 + 1);
            try { e.duration = std::stoi(dur); } catch (...) { e.duration = 0; }
            e.isShort = isShorts;
            if (!e.id.empty()) {
                e.url = L"https://www.youtube.com/watch?v=" + e.id;
                auto *payload = new EntryPayload{std::move(e)};
                PostMessageW(g_hWnd, WM_APP_SCAN_ENTRY, 0, (LPARAM)payload);
            }
        });
        // Order matters: null the externally-visible pointer under the
        // mutex BEFORE closing handles. Otherwise UI-thread Cancel could
        // see a non-null g_currentProcess pointing at a closed (and
        // possibly recycled) handle and TerminateProcess the wrong PID.
        {
            std::lock_guard<std::mutex> g(g_processMutex);
            g_currentProcess = nullptr;
        }
        closePipedProcess(p);
        postLog(L"[scan] yt-dlp exit code: " + std::to_wstring(code));
    };

    if (wantVideos && !g_cancelRequested.load()) runOne(channelBase + L"/videos", false);
    if (wantShorts && !g_cancelRequested.load()) runOne(channelBase + L"/shorts", true);

    auto *d = new DonePayload{0, g_cancelRequested.load()};
    PostMessageW(g_hWnd, WM_APP_SCAN_DONE, 0, (LPARAM)d);
}

void downloadWorker(std::vector<int> queueCopy,
                    std::vector<VideoEntry> entriesCopy,
                    DownloadOpts opts) {
    int total = (int)queueCopy.size();
    for (int i = 0; i < total; ++i) {
        if (g_cancelRequested.load()) break;
        int idx = queueCopy[i];
        if (idx < 0 || idx >= (int)entriesCopy.size()) continue;
        const VideoEntry &v = entriesCopy[idx];

        wchar_t header[256];
        swprintf(header, 256, L"[%d/%d] %ls", i + 1, total, v.title.c_str());
        postLog(header);

        std::wstring cmd = buildCmdLine(buildDownloadArgs(v, opts));
        postLog(L"  -> " + cmd);
        PipedProcess p = startProcess(cmd);
        if (!p.process) {
            postLog(L"[download] failed to start yt-dlp.");
            break;
        }
        {
            std::lock_guard<std::mutex> g(g_processMutex);
            g_currentProcess = p.process;
        }
        int code = readAllLinesAndWait(p, [&](const std::string &line) {
            postLogA(line);
        });
        // See scanWorker for why null-then-close is the right order.
        {
            std::lock_guard<std::mutex> g(g_processMutex);
            g_currentProcess = nullptr;
        }
        closePipedProcess(p);
        if (code != 0)
            postLog(L"[download] exit code: " + std::to_wstring(code));

        PostMessageW(g_hWnd, WM_APP_PROGRESS_SET, (WPARAM)(i + 1), 0);
    }

    auto *d = new DonePayload{0, g_cancelRequested.load()};
    PostMessageW(g_hWnd, WM_APP_DL_DONE, 0, (LPARAM)d);
}
