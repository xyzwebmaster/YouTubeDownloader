#include "download.h"

#include "app.h"
#include "process.h"
#include "util.h"

#include <atomic>
#include <cwchar>
#include <map>
#include <mutex>

// Replace characters Windows' filesystem rejects (and a few that confuse
// the shell), so a playlist title can be safely used as a folder name.
// Also strips trailing dots and spaces — those are illegal at end of a
// path component on NTFS even though most tools tolerate them.
static std::wstring sanitizePathSegment(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (c == L'<' || c == L'>' || c == L':' || c == L'"' ||
            c == L'/' || c == L'\\' || c == L'|' || c == L'?' ||
            c == L'*' || c < 32) {
            out += L'_';
        } else {
            out += c;
        }
    }
    while (!out.empty() && (out.back() == L'.' || out.back() == L' '))
        out.pop_back();
    if (out.empty()) out = L"_";
    return out;
}

static std::vector<std::wstring> buildDownloadArgs(const VideoEntry &v,
                                                   const DownloadOpts &o) {
    std::wstring playlistSeg;
    if (!v.playlistTitle.empty()) {
        playlistSeg = sanitizePathSegment(v.playlistTitle) + L"\\";
    }
    std::vector<std::wstring> a = {
        L"yt-dlp",
        L"--encoding", L"utf-8",
        L"--no-playlist",
        L"--ignore-errors",
        L"--no-warnings",
        L"--newline",
        L"-o",
        o.outputDir + L"\\%(uploader)s\\" + playlistSeg
            + L"%(title).200B [%(id)s].%(ext)s",
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

// Run yt-dlp once and feed each non-empty stdout line to onLine. Manages
// the g_currentProcess handoff so the UI-thread Cancel path stays safe.
// Returns true if yt-dlp launched (regardless of exit code).
static bool runYtDlp(const std::vector<std::wstring>& args,
                     const std::function<void(const std::string&)>& onLine) {
    PipedProcess p = startProcess(buildCmdLine(args));
    if (!p.process) return false;
    {
        std::lock_guard<std::mutex> g(g_processMutex);
        g_currentProcess = p.process;
    }
    readAllLinesAndWait(p, onLine);
    {
        std::lock_guard<std::mutex> g(g_processMutex);
        g_currentProcess = nullptr;
    }
    closePipedProcess(p);
    return true;
}

// First pass for "Group by playlist": list a channel's playlists and
// then list the videos inside each. Returns a video_id -> playlist_title
// map; if a video appears in multiple playlists the first wins.
static std::map<std::wstring, std::wstring>
buildVideoPlaylistMap(const std::wstring& channelBase) {
    struct PI { std::wstring id, title; };
    std::vector<PI> playlists;

    postLog(L"[scan] enumerating playlists...");
    runYtDlp({
        L"yt-dlp", L"--encoding", L"utf-8", L"--flat-playlist",
        L"--no-warnings", L"--ignore-errors",
        L"--print", L"%(id)s\t%(title)s",
        channelBase + L"/playlists"
    }, [&](const std::string& line) {
        std::wstring w = s2w(line);
        size_t t = w.find(L'\t');
        if (t == std::wstring::npos) return;
        PI pi;
        pi.id    = w.substr(0, t);
        pi.title = w.substr(t + 1);
        if (!pi.id.empty()) playlists.push_back(std::move(pi));
    });
    postLog(L"[scan] " + std::to_wstring((long long)playlists.size()) + L" playlist(s) found");

    std::map<std::wstring, std::wstring> map;
    int idx = 0;
    for (const auto& pl : playlists) {
        if (g_cancelRequested.load()) break;
        ++idx;
        postLog(L"[scan] playlist " + std::to_wstring(idx) + L"/"
              + std::to_wstring((long long)playlists.size()) + L": " + pl.title);
        runYtDlp({
            L"yt-dlp", L"--encoding", L"utf-8", L"--flat-playlist",
            L"--no-warnings", L"--ignore-errors",
            L"--print", L"%(id)s",
            L"https://www.youtube.com/playlist?list=" + pl.id
        }, [&](const std::string& line) {
            std::wstring vid = trim(s2w(line));
            if (!vid.empty() && map.find(vid) == map.end())
                map[vid] = pl.title;
        });
    }
    postLog(L"[scan] " + std::to_wstring((long long)map.size())
          + L" video(s) tagged with a playlist");
    return map;
}

void scanWorker(std::wstring channelBase, bool wantVideos, bool wantShorts,
                bool groupByPlaylist) {
    std::map<std::wstring, std::wstring> videoToPlaylist;
    if (groupByPlaylist && !g_cancelRequested.load()) {
        videoToPlaylist = buildVideoPlaylistMap(channelBase);
    }

    auto runOne = [&](const std::wstring &target, bool isShorts) {
        postLog(L"[scan] fetching: " + target);
        runYtDlp({
            L"yt-dlp", L"--encoding", L"utf-8", L"--flat-playlist",
            L"--no-warnings", L"--ignore-errors",
            L"--print", L"%(id)s\t%(title)s\t%(duration|0)s",
            target
        }, [&](const std::string &line) {
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
            auto pit = videoToPlaylist.find(e.id);
            if (pit != videoToPlaylist.end()) e.playlistTitle = pit->second;
            if (!e.id.empty()) {
                e.url = L"https://www.youtube.com/watch?v=" + e.id;
                auto *payload = new EntryPayload{std::move(e)};
                PostMessageW(g_hWnd, WM_APP_SCAN_ENTRY, 0, (LPARAM)payload);
            }
        });
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
