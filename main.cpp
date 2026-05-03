// YouTube Bulk Downloader - Win32 GUI front-end for yt-dlp.
//
// Build (MinGW):
//   g++ -O2 -static -mwindows -DUNICODE -D_UNICODE \
//       -o YouTubeDownloader.exe main.cpp \
//       -lcomctl32 -lcomdlg32 -lshell32 -lole32 -luuid

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>

#include <atomic>
#include <cwchar>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ----- Comctl32 v6 manifest (modern controls + visual styles) -----
#pragma comment(linker,"/manifestdependency:\"type='win32' "                     \
                       "name='Microsoft.Windows.Common-Controls' "               \
                       "version='6.0.0.0' processorArchitecture='*' "            \
                       "publicKeyToken='6595b64144ccf1df' language='*'\"")

// ============================== IDs and messages ============================
enum : int {
    ID_URL_EDIT = 100,
    ID_TYPE_COMBO,
    ID_SCAN_BUTTON,
    ID_SELECT_ALL,
    ID_SELECT_NONE,
    ID_INVERT,
    ID_LIST,
    ID_VIDEO_RADIO,
    ID_AUDIO_RADIO,
    ID_VIDEO_QUALITY,
    ID_CONTAINER,
    ID_AUDIO_FORMAT,
    ID_AUDIO_QUALITY,
    ID_OUTPUT_EDIT,
    ID_BROWSE,
    ID_DOWNLOAD,
    ID_CANCEL,
    ID_PROGRESS,
    ID_LOG,
    ID_COUNT_LABEL,
};

#define WM_APP_LOG          (WM_APP + 1)
#define WM_APP_SCAN_ENTRY   (WM_APP + 2)
#define WM_APP_SCAN_DONE    (WM_APP + 3)
#define WM_APP_DL_DONE      (WM_APP + 4)
#define WM_APP_PROGRESS_SET (WM_APP + 5)

// ============================== Data types ==================================
struct VideoEntry {
    std::wstring id;
    std::wstring title;
    std::wstring url;
    int          duration = 0;
    bool         isShort  = false;
};

// Heap-owned message payloads (deleted by the receiver).
struct EntryPayload { VideoEntry entry; };
struct LogPayload   { std::wstring text; };
struct DonePayload  { int code = 0; bool cancelled = false; };

// ============================== Globals =====================================
static HINSTANCE g_hInstance = nullptr;
static HWND      g_hWnd = nullptr;

static HWND g_hUrl, g_hType, g_hScan;
static HWND g_hSelAll, g_hSelNone, g_hInvert, g_hCount;
static HWND g_hList;
static HWND g_hVideoRadio, g_hAudioRadio;
static HWND g_hVideoQuality, g_hContainer, g_hAudioFormat, g_hAudioQuality;
static HWND g_hOutput, g_hBrowse;
static HWND g_hDownload, g_hCancel, g_hProgress;
static HWND g_hLog;
static HFONT g_hFont = nullptr;

static std::vector<VideoEntry> g_entries;
static std::vector<int>        g_queue;
static std::atomic<bool>       g_busy{false};
static std::atomic<bool>       g_cancelRequested{false};
static int                     g_doneCount = 0;
static int                     g_total     = 0;

static std::mutex   g_processMutex;
static HANDLE       g_currentProcess = nullptr;
static std::thread  g_workerThread;

// ============================== Utilities ===================================
static std::wstring s2w(const std::string &s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
static std::string w2s(const std::wstring &w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring trim(std::wstring s) {
    auto issp = [](wchar_t c){ return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    while (!s.empty() && issp(s.front())) s.erase(s.begin());
    while (!s.empty() && issp(s.back()))  s.pop_back();
    return s;
}

static std::wstring formatDuration(int seconds) {
    if (seconds <= 0) return L"--:--";
    int h = seconds / 3600;
    int m = (seconds % 3600) / 60;
    int s = seconds % 60;
    wchar_t buf[32];
    if (h > 0) swprintf(buf, 32, L"%d:%02d:%02d", h, m, s);
    else       swprintf(buf, 32, L"%02d:%02d", m, s);
    return buf;
}

static std::wstring normalizeChannelUrl(std::wstring u) {
    u = trim(u);
    while (!u.empty() && u.back() == L'/') u.pop_back();
    static const wchar_t *suffixes[] = {
        L"/videos", L"/shorts", L"/streams", L"/live",
        L"/featured", L"/playlists", L"/community", L"/about", L"/channels"
    };
    for (auto sfx : suffixes) {
        size_t L = wcslen(sfx);
        if (u.size() >= L) {
            std::wstring tail = u.substr(u.size() - L);
            for (auto &c : tail) c = towlower(c);
            if (tail == sfx) { u.resize(u.size() - L); break; }
        }
    }
    return u;
}

// Send a log line to the UI thread.
static void postLog(const std::wstring &line) {
    auto *p = new LogPayload{line};
    PostMessageW(g_hWnd, WM_APP_LOG, 0, (LPARAM)p);
}
static void postLogA(const std::string &line) { postLog(s2w(line)); }

// ============================== Process pipe ================================
struct PipedProcess {
    HANDLE process    = nullptr;
    HANDLE thread     = nullptr;
    HANDLE stdoutRead = nullptr;
};

static PipedProcess startProcess(std::wstring cmdline) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0)) return {};
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outWrite;
    si.hStdError  = outWrite;
    si.hStdInput  = nullptr;

    PROCESS_INFORMATION pi{};
    std::wstring buf = std::move(cmdline);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr,
                             TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                             &si, &pi);
    CloseHandle(outWrite);
    if (!ok) {
        CloseHandle(outRead);
        return {};
    }
    PipedProcess p;
    p.process    = pi.hProcess;
    p.thread     = pi.hThread;
    p.stdoutRead = outRead;
    return p;
}

// Read all stdout, calling onLine for each \n-terminated line. Closes pipe.
// Returns process exit code (0 if not retrievable).
static int readAllLinesAndWait(PipedProcess &p,
                               const std::function<void(const std::string&)> &onLine) {
    char  buf[4096];
    std::string acc;
    DWORD got = 0;
    while (ReadFile(p.stdoutRead, buf, sizeof(buf), &got, nullptr) && got > 0) {
        acc.append(buf, got);
        for (;;) {
            size_t pos = acc.find_first_of("\r\n");
            if (pos == std::string::npos) break;
            std::string line = acc.substr(0, pos);
            // Skip the matched separator(s).
            size_t skip = 1;
            if (pos + 1 < acc.size() && acc[pos] == '\r' && acc[pos + 1] == '\n') skip = 2;
            acc.erase(0, pos + skip);
            if (!line.empty()) onLine(line);
        }
    }
    if (!acc.empty()) onLine(acc);

    WaitForSingleObject(p.process, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(p.process, &code);
    if (p.stdoutRead) CloseHandle(p.stdoutRead);
    if (p.thread)     CloseHandle(p.thread);
    if (p.process)    CloseHandle(p.process);
    p = {};
    return (int)code;
}

// Quote a single argv token for inclusion in a Windows command line.
// Implements the same rules MSVC's argv parser uses (handles spaces, quotes,
// trailing backslashes).
static std::wstring quoteArg(const std::wstring &arg) {
    if (!arg.empty() &&
        arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out;
    out.push_back(L'"');
    for (size_t i = 0; i < arg.size(); ) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') { ++backslashes; ++i; }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        } else if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(arg[i]);
        }
        ++i;
    }
    out.push_back(L'"');
    return out;
}
static std::wstring buildCmdLine(const std::vector<std::wstring> &args) {
    std::wstring out;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) out.push_back(L' ');
        out += quoteArg(args[i]);
    }
    return out;
}

// ============================== Workers (background) ========================
// Scan worker: invokes yt-dlp to enumerate channel videos / shorts.
static void scanWorker(std::wstring channelBase, bool wantVideos, bool wantShorts) {
    auto runOne = [&](const std::wstring &target, bool isShorts) {
        postLog(L"[scan] fetching: " + target);
        std::vector<std::wstring> args = {
            L"yt-dlp",
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
            // Tab-separated: id <TAB> title <TAB> duration
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
        {
            std::lock_guard<std::mutex> g(g_processMutex);
            g_currentProcess = nullptr;
        }
        postLog(L"[scan] yt-dlp exit code: " + std::to_wstring(code));
    };

    if (wantVideos && !g_cancelRequested.load()) runOne(channelBase + L"/videos", false);
    if (wantShorts && !g_cancelRequested.load()) runOne(channelBase + L"/shorts", true);

    auto *d = new DonePayload{0, g_cancelRequested.load()};
    PostMessageW(g_hWnd, WM_APP_SCAN_DONE, 0, (LPARAM)d);
}

// Build yt-dlp command line for a single download (audio or video).
struct DownloadOpts {
    bool         audioOnly    = false;
    int          maxHeight    = -1;        // -1 = best
    std::wstring container;                 // "" = auto
    std::wstring audioFormat;               // "best" or specific
    std::wstring audioQuality;              // e.g. "0", "320K"
    std::wstring outputDir;
};
static std::vector<std::wstring> buildDownloadArgs(const VideoEntry &v,
                                                   const DownloadOpts &o) {
    std::vector<std::wstring> a = {
        L"yt-dlp",
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

// Download worker: process the queue sequentially.
static void downloadWorker(std::vector<int> queueCopy, std::vector<VideoEntry> entriesCopy,
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
        {
            std::lock_guard<std::mutex> g(g_processMutex);
            g_currentProcess = nullptr;
        }
        if (code != 0)
            postLog(L"[download] exit code: " + std::to_wstring(code));

        PostMessageW(g_hWnd, WM_APP_PROGRESS_SET, (WPARAM)(i + 1), 0);
    }

    auto *d = new DonePayload{0, g_cancelRequested.load()};
    PostMessageW(g_hWnd, WM_APP_DL_DONE, 0, (LPARAM)d);
}

// ============================== UI helpers ==================================
static void appendLogToCtrl(const std::wstring &line) {
    int len = GetWindowTextLengthW(g_hLog);
    SendMessageW(g_hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    std::wstring withNl = line + L"\r\n";
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)withNl.c_str());
    SendMessageW(g_hLog, EM_SCROLLCARET, 0, 0);
}

static void setUiBusy(bool busy) {
    g_busy.store(busy);
    EnableWindow(g_hUrl, !busy);
    EnableWindow(g_hType, !busy);
    EnableWindow(g_hScan, !busy);
    EnableWindow(g_hDownload, !busy);
    EnableWindow(g_hCancel, busy);
    EnableWindow(g_hSelAll, !busy);
    EnableWindow(g_hSelNone, !busy);
    EnableWindow(g_hInvert, !busy);
}

static void updateModeUi() {
    bool video = SendMessage(g_hVideoRadio, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(g_hVideoQuality, video);
    EnableWindow(g_hContainer,    video);
    EnableWindow(g_hAudioFormat, !video);
    EnableWindow(g_hAudioQuality,!video);
}

static void updateCountLabel() {
    int total = ListView_GetItemCount(g_hList);
    int checked = 0;
    for (int i = 0; i < total; ++i)
        if (ListView_GetCheckState(g_hList, i)) ++checked;
    wchar_t buf[64];
    swprintf(buf, 64, L"%d / %d selected", checked, total);
    SetWindowTextW(g_hCount, buf);
}

static void clearVideoList() {
    ListView_DeleteAllItems(g_hList);
    g_entries.clear();
    updateCountLabel();
}

static void addEntryToListView(const VideoEntry &e) {
    int row = (int)g_entries.size();
    g_entries.push_back(e);

    LVITEMW it{};
    it.mask     = LVIF_TEXT | LVIF_PARAM;
    it.iItem    = row;
    it.iSubItem = 0;
    std::wstring tag = e.isShort ? L"Short" : L"Video";
    it.pszText = const_cast<LPWSTR>(tag.c_str());
    it.lParam  = (LPARAM)row;
    int inserted = ListView_InsertItem(g_hList, &it);
    if (inserted < 0) return;

    std::wstring dur = formatDuration(e.duration);
    ListView_SetItemText(g_hList, inserted, 1, const_cast<LPWSTR>(dur.c_str()));
    ListView_SetItemText(g_hList, inserted, 2, const_cast<LPWSTR>(e.title.c_str()));
    ListView_SetCheckState(g_hList, inserted, TRUE);
}

// ============================== Layout ======================================
static void doLayout(int W, int H) {
    const int pad = 10;
    const int rowH = 26;
    int x = pad, y = pad;

    // Row 1: URL row
    int labelW = 110, comboW = 130, scanW = 100;
    int urlW = W - pad - x - labelW - 60 - comboW - scanW - pad * 4;
    SetWindowPos(GetDlgItem(g_hWnd, 1001), nullptr, x, y + 4, labelW, rowH - 4, SWP_NOZORDER);
    int xx = x + labelW + 4;
    SetWindowPos(g_hUrl,  nullptr, xx, y, urlW, rowH, SWP_NOZORDER);
    xx += urlW + 8;
    SetWindowPos(GetDlgItem(g_hWnd, 1002), nullptr, xx, y + 4, 50, rowH - 4, SWP_NOZORDER);
    xx += 50 + 4;
    SetWindowPos(g_hType, nullptr, xx, y, comboW, 200, SWP_NOZORDER);
    xx += comboW + 8;
    SetWindowPos(g_hScan, nullptr, xx, y, scanW, rowH, SWP_NOZORDER);
    y += rowH + pad;

    // Row 2: list toolbar
    int btnW = 110;
    SetWindowPos(g_hSelAll,  nullptr, x,                y, btnW, rowH, SWP_NOZORDER);
    SetWindowPos(g_hSelNone, nullptr, x + (btnW + 6),   y, btnW, rowH, SWP_NOZORDER);
    SetWindowPos(g_hInvert,  nullptr, x + (btnW + 6)*2, y, 130,  rowH, SWP_NOZORDER);
    SetWindowPos(g_hCount,   nullptr, W - 200 - pad,    y + 4, 200, rowH - 4, SWP_NOZORDER);
    y += rowH + 4;

    // Calculate dynamic split
    int bottomReserve = 240; // for options + buttons + log
    int listH = H - y - bottomReserve - pad;
    if (listH < 100) listH = 100;
    SetWindowPos(g_hList, nullptr, x, y, W - 2 * pad, listH, SWP_NOZORDER);
    // Auto-size title column
    ListView_SetColumnWidth(g_hList, 2, W - 2 * pad - 60 - 80 - 30);
    y += listH + pad;

    // Options group: mode radios + comboboxes
    SetWindowPos(g_hVideoRadio, nullptr, x,        y + 4, 160, rowH - 4, SWP_NOZORDER);
    SetWindowPos(g_hAudioRadio, nullptr, x + 170,  y + 4, 160, rowH - 4, SWP_NOZORDER);
    y += rowH;

    SetWindowPos(GetDlgItem(g_hWnd, 1003), nullptr, x,        y + 4, 90, rowH - 4, SWP_NOZORDER);
    SetWindowPos(g_hVideoQuality, nullptr, x + 95, y, 170, 200, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(g_hWnd, 1004), nullptr, x + 280, y + 4, 80, rowH - 4, SWP_NOZORDER);
    SetWindowPos(g_hContainer,   nullptr, x + 360,  y, 110, 200, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(g_hWnd, 1005), nullptr, x + 490, y + 4, 90, rowH - 4, SWP_NOZORDER);
    SetWindowPos(g_hAudioFormat, nullptr, x + 580,  y, 110, 200, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(g_hWnd, 1006), nullptr, x + 700, y + 4, 90, rowH - 4, SWP_NOZORDER);
    SetWindowPos(g_hAudioQuality,nullptr, x + 790,  y, 130, 200, SWP_NOZORDER);
    y += rowH + 6;

    // Output folder row
    SetWindowPos(GetDlgItem(g_hWnd, 1007), nullptr, x,       y + 4, 100, rowH - 4, SWP_NOZORDER);
    int browseW = 90;
    int outW = W - 2 * pad - 100 - browseW - 10;
    SetWindowPos(g_hOutput, nullptr, x + 100, y, outW, rowH, SWP_NOZORDER);
    SetWindowPos(g_hBrowse, nullptr, x + 100 + outW + 6, y, browseW, rowH, SWP_NOZORDER);
    y += rowH + 6;

    // Download / cancel / progress
    SetWindowPos(g_hDownload, nullptr, x,           y, 160, rowH, SWP_NOZORDER);
    SetWindowPos(g_hCancel,   nullptr, x + 170,     y, 100, rowH, SWP_NOZORDER);
    SetWindowPos(g_hProgress, nullptr, x + 280,     y + 2, W - x - 280 - pad, rowH - 4, SWP_NOZORDER);
    y += rowH + 6;

    // Log
    int logH = H - y - pad;
    if (logH < 60) logH = 60;
    SetWindowPos(g_hLog, nullptr, x, y, W - 2 * pad, logH, SWP_NOZORDER);
}

// ============================== Window procedure ============================
static void onScanClicked();
static void onDownloadClicked();
static void onBrowseClicked();
static void onCancelClicked();
static void onSelectAll();
static void onSelectNone();
static void onInvert();

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);
        switch (id) {
        case ID_SCAN_BUTTON:    onScanClicked();    break;
        case ID_DOWNLOAD:       onDownloadClicked();break;
        case ID_BROWSE:         onBrowseClicked();  break;
        case ID_CANCEL:         onCancelClicked();  break;
        case ID_SELECT_ALL:     onSelectAll();      break;
        case ID_SELECT_NONE:    onSelectNone();     break;
        case ID_INVERT:         onInvert();         break;
        case ID_VIDEO_RADIO:
        case ID_AUDIO_RADIO:
            if (code == BN_CLICKED) updateModeUi();
            break;
        }
        return 0;
    }
    case WM_NOTIFY: {
        LPNMHDR n = (LPNMHDR)lp;
        if (n->hwndFrom == g_hList && n->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *lv = (NMLISTVIEW*)lp;
            if (lv->uChanged & LVIF_STATE) {
                UINT oldChk = (lv->uOldState & LVIS_STATEIMAGEMASK);
                UINT newChk = (lv->uNewState & LVIS_STATEIMAGEMASK);
                if (oldChk != newChk) updateCountLabel();
            }
        }
        return 0;
    }
    case WM_APP_LOG: {
        LogPayload *p = (LogPayload*)lp;
        appendLogToCtrl(p->text);
        delete p;
        return 0;
    }
    case WM_APP_SCAN_ENTRY: {
        EntryPayload *p = (EntryPayload*)lp;
        addEntryToListView(p->entry);
        updateCountLabel();
        delete p;
        return 0;
    }
    case WM_APP_SCAN_DONE: {
        DonePayload *p = (DonePayload*)lp;
        if (g_workerThread.joinable()) g_workerThread.join();
        appendLogToCtrl(p->cancelled
            ? L"[scan] cancelled."
            : L"[scan] done. " + std::to_wstring((long long)g_entries.size()) + L" video(s) found.");
        setUiBusy(false);
        if (g_entries.empty())
            MessageBoxW(hwnd,
                L"No videos were found.\n\n"
                L"Check the channel URL, your internet connection, "
                L"and that 'yt-dlp' is installed and on PATH.",
                L"No videos", MB_OK | MB_ICONINFORMATION);
        delete p;
        return 0;
    }
    case WM_APP_DL_DONE: {
        DonePayload *p = (DonePayload*)lp;
        if (g_workerThread.joinable()) g_workerThread.join();
        appendLogToCtrl(p->cancelled
            ? L"[download] cancelled."
            : L"[download] all done.");
        setUiBusy(false);
        delete p;
        return 0;
    }
    case WM_APP_PROGRESS_SET: {
        SendMessage(g_hProgress, PBM_SETPOS, (WPARAM)wp, 0);
        return 0;
    }
    case WM_SIZE: {
        doLayout(LOWORD(lp), HIWORD(lp));
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = 980;
        mm->ptMinTrackSize.y = 700;
        return 0;
    }
    case WM_CLOSE:
        if (g_busy.load()) {
            int r = MessageBoxW(hwnd,
                L"A scan or download is in progress. Cancel and quit?",
                L"Quit", MB_YESNO | MB_ICONQUESTION);
            if (r != IDYES) return 0;
            g_cancelRequested.store(true);
            std::lock_guard<std::mutex> g(g_processMutex);
            if (g_currentProcess) TerminateProcess(g_currentProcess, 1);
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_workerThread.joinable()) g_workerThread.join();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================== Button handlers =============================
static void onScanClicked() {
    if (g_busy.load()) return;
    wchar_t buf[2048];
    GetWindowTextW(g_hUrl, buf, 2048);
    std::wstring url = trim(buf);
    if (url.empty()) {
        MessageBoxW(g_hWnd, L"Please enter a YouTube channel URL.", L"Missing URL",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    std::wstring base = normalizeChannelUrl(url);

    int sel = (int)SendMessage(g_hType, CB_GETCURSEL, 0, 0);
    bool wantVideos = (sel == 0 || sel == 1);
    bool wantShorts = (sel == 0 || sel == 2);

    clearVideoList();
    appendLogToCtrl(L"[scan] base URL: " + base);
    SendMessage(g_hProgress, PBM_SETPOS, 0, 0);

    g_cancelRequested.store(false);
    setUiBusy(true);
    if (g_workerThread.joinable()) g_workerThread.join();
    g_workerThread = std::thread(scanWorker, base, wantVideos, wantShorts);
}

static void onDownloadClicked() {
    if (g_busy.load()) return;

    std::vector<int> queue;
    int n = ListView_GetItemCount(g_hList);
    for (int i = 0; i < n; ++i) {
        if (ListView_GetCheckState(g_hList, i)) queue.push_back(i);
    }
    if (queue.empty()) {
        MessageBoxW(g_hWnd, L"Tick at least one video.", L"Nothing selected",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    wchar_t buf[1024];
    GetWindowTextW(g_hOutput, buf, 1024);
    std::wstring outDir = trim(buf);
    if (outDir.empty()) {
        MessageBoxW(g_hWnd, L"Please choose an output folder.", L"Missing folder",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    SHCreateDirectoryExW(nullptr, outDir.c_str(), nullptr);

    DownloadOpts opts;
    opts.audioOnly = SendMessage(g_hAudioRadio, BM_GETCHECK, 0, 0) == BST_CHECKED;
    opts.outputDir = outDir;

    int vqIdx = (int)SendMessage(g_hVideoQuality, CB_GETCURSEL, 0, 0);
    static const int heights[] = { -1, 2160, 1440, 1080, 720, 480, 360, 240 };
    opts.maxHeight = heights[vqIdx < 0 ? 0 : vqIdx];

    int contIdx = (int)SendMessage(g_hContainer, CB_GETCURSEL, 0, 0);
    static const wchar_t *containers[] = { L"", L"mp4", L"mkv", L"webm" };
    opts.container = containers[contIdx < 0 ? 0 : contIdx];

    int afIdx = (int)SendMessage(g_hAudioFormat, CB_GETCURSEL, 0, 0);
    static const wchar_t *afmts[] = { L"best", L"mp3", L"m4a", L"opus", L"vorbis", L"flac", L"wav", L"aac" };
    opts.audioFormat = afmts[afIdx < 0 ? 0 : afIdx];

    int aqIdx = (int)SendMessage(g_hAudioQuality, CB_GETCURSEL, 0, 0);
    static const wchar_t *aqs[] = { L"0", L"320K", L"256K", L"192K", L"160K", L"128K", L"96K", L"64K" };
    opts.audioQuality = aqs[aqIdx < 0 ? 0 : aqIdx];

    g_total = (int)queue.size();
    g_doneCount = 0;
    SendMessage(g_hProgress, PBM_SETRANGE32, 0, g_total);
    SendMessage(g_hProgress, PBM_SETPOS, 0, 0);

    appendLogToCtrl(L"[download] starting " + std::to_wstring((long long)g_total) + L" item(s)");

    g_cancelRequested.store(false);
    setUiBusy(true);
    if (g_workerThread.joinable()) g_workerThread.join();
    g_workerThread = std::thread(downloadWorker, std::move(queue), g_entries, opts);
}

static void onCancelClicked() {
    if (!g_busy.load()) return;
    g_cancelRequested.store(true);
    std::lock_guard<std::mutex> g(g_processMutex);
    if (g_currentProcess) TerminateProcess(g_currentProcess, 1);
    appendLogToCtrl(L"[cancel] requested");
}

static void onBrowseClicked() {
    BROWSEINFOW bi{};
    bi.hwndOwner = g_hWnd;
    bi.lpszTitle = L"Select output folder";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) SetWindowTextW(g_hOutput, path);
        CoTaskMemFree(pidl);
    }
}

static void onSelectAll() {
    int n = ListView_GetItemCount(g_hList);
    for (int i = 0; i < n; ++i) ListView_SetCheckState(g_hList, i, TRUE);
    updateCountLabel();
}
static void onSelectNone() {
    int n = ListView_GetItemCount(g_hList);
    for (int i = 0; i < n; ++i) ListView_SetCheckState(g_hList, i, FALSE);
    updateCountLabel();
}
static void onInvert() {
    int n = ListView_GetItemCount(g_hList);
    for (int i = 0; i < n; ++i)
        ListView_SetCheckState(g_hList, i, !ListView_GetCheckState(g_hList, i));
    updateCountLabel();
}

// ============================== Entry point =================================
static HWND mkLabel(HWND parent, int id, const wchar_t *text) {
    return CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 50, 20, parent, (HMENU)(INT_PTR)id, g_hInstance, nullptr);
}

static HWND createMainWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = g_hInstance;
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"YTDL_MainWnd";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"YTDL_MainWnd", L"YouTube Bulk Downloader",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 800,
        nullptr, nullptr, g_hInstance, nullptr);
    g_hWnd = hwnd;

    NONCLIENTMETRICS ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_hFont = CreateFontIndirectW(&ncm.lfMessageFont);

    auto setFont = [](HWND h){ SendMessageW(h, WM_SETFONT, (WPARAM)g_hFont, TRUE); };

    // Row 1
    HWND lblUrl = mkLabel(hwnd, 1001, L"YouTube channel URL:");
    g_hUrl  = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 100, 20, hwnd, (HMENU)ID_URL_EDIT, g_hInstance, nullptr);
    HWND lblType = mkLabel(hwnd, 1002, L"Type:");
    g_hType = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 100, 200, hwnd, (HMENU)ID_TYPE_COMBO, g_hInstance, nullptr);
    SendMessageW(g_hType, CB_ADDSTRING, 0, (LPARAM)L"All");
    SendMessageW(g_hType, CB_ADDSTRING, 0, (LPARAM)L"Videos");
    SendMessageW(g_hType, CB_ADDSTRING, 0, (LPARAM)L"Shorts");
    SendMessageW(g_hType, CB_SETCURSEL, 0, 0);
    g_hScan = CreateWindowExW(0, L"BUTTON", L"Scan",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 100, 20, hwnd, (HMENU)ID_SCAN_BUTTON, g_hInstance, nullptr);

    // Row 2: list toolbar
    g_hSelAll = CreateWindowExW(0, L"BUTTON", L"Select all",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 100, 20, hwnd,
        (HMENU)ID_SELECT_ALL,  g_hInstance, nullptr);
    g_hSelNone = CreateWindowExW(0, L"BUTTON", L"Select none",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 100, 20, hwnd,
        (HMENU)ID_SELECT_NONE, g_hInstance, nullptr);
    g_hInvert = CreateWindowExW(0, L"BUTTON", L"Invert selection",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 100, 20, hwnd,
        (HMENU)ID_INVERT,      g_hInstance, nullptr);
    g_hCount = CreateWindowExW(0, L"STATIC", L"0 / 0 selected",
        WS_CHILD | WS_VISIBLE | SS_RIGHT, 0, 0, 100, 20, hwnd,
        (HMENU)(INT_PTR)ID_COUNT_LABEL, g_hInstance, nullptr);

    // List view
    g_hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
        0, 0, 100, 100, hwnd, (HMENU)ID_LIST, g_hInstance, nullptr);
    ListView_SetExtendedListViewStyle(g_hList,
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH;
    c.pszText = (LPWSTR)L"Type";     c.cx = 60;  ListView_InsertColumn(g_hList, 0, &c);
    c.pszText = (LPWSTR)L"Duration"; c.cx = 80;  ListView_InsertColumn(g_hList, 1, &c);
    c.pszText = (LPWSTR)L"Title";    c.cx = 600; ListView_InsertColumn(g_hList, 2, &c);

    // Mode radios
    g_hVideoRadio = CreateWindowExW(0, L"BUTTON", L"Video (with audio)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
        0, 0, 100, 20, hwnd, (HMENU)ID_VIDEO_RADIO, g_hInstance, nullptr);
    g_hAudioRadio = CreateWindowExW(0, L"BUTTON", L"Audio only",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
        0, 0, 100, 20, hwnd, (HMENU)ID_AUDIO_RADIO, g_hInstance, nullptr);
    SendMessage(g_hVideoRadio, BM_SETCHECK, BST_CHECKED, 0);

    // Quality / format combos
    mkLabel(hwnd, 1003, L"Video quality:");
    g_hVideoQuality = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 100, 200, hwnd, (HMENU)ID_VIDEO_QUALITY, g_hInstance, nullptr);
    static const wchar_t *vqLabels[] = {
        L"Highest available", L"2160p (4K)", L"1440p (2K)",
        L"1080p", L"720p", L"480p", L"360p", L"240p"
    };
    for (auto s : vqLabels) SendMessageW(g_hVideoQuality, CB_ADDSTRING, 0, (LPARAM)s);
    SendMessage(g_hVideoQuality, CB_SETCURSEL, 0, 0);

    mkLabel(hwnd, 1004, L"Container:");
    g_hContainer = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 100, 200, hwnd, (HMENU)ID_CONTAINER, g_hInstance, nullptr);
    static const wchar_t *contLabels[] = { L"Auto (best)", L"mp4", L"mkv", L"webm" };
    for (auto s : contLabels) SendMessageW(g_hContainer, CB_ADDSTRING, 0, (LPARAM)s);
    SendMessage(g_hContainer, CB_SETCURSEL, 0, 0);

    mkLabel(hwnd, 1005, L"Audio format:");
    g_hAudioFormat = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 100, 200, hwnd, (HMENU)ID_AUDIO_FORMAT, g_hInstance, nullptr);
    static const wchar_t *afLabels[] = {
        L"Best (no re-encode)", L"mp3", L"m4a", L"opus",
        L"vorbis", L"flac", L"wav", L"aac"
    };
    for (auto s : afLabels) SendMessageW(g_hAudioFormat, CB_ADDSTRING, 0, (LPARAM)s);
    SendMessage(g_hAudioFormat, CB_SETCURSEL, 0, 0);

    mkLabel(hwnd, 1006, L"Audio quality:");
    g_hAudioQuality = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 100, 200, hwnd, (HMENU)ID_AUDIO_QUALITY, g_hInstance, nullptr);
    static const wchar_t *aqLabels[] = {
        L"Highest", L"320 kbps", L"256 kbps", L"192 kbps",
        L"160 kbps", L"128 kbps", L"96 kbps", L"64 kbps"
    };
    for (auto s : aqLabels) SendMessageW(g_hAudioQuality, CB_ADDSTRING, 0, (LPARAM)s);
    SendMessage(g_hAudioQuality, CB_SETCURSEL, 0, 0);

    // Output folder
    mkLabel(hwnd, 1007, L"Output folder:");
    g_hOutput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 100, 20, hwnd, (HMENU)ID_OUTPUT_EDIT, g_hInstance, nullptr);
    {
        wchar_t path[MAX_PATH];
        if (SHGetFolderPathW(nullptr, CSIDL_MYVIDEO, nullptr, 0, path) == S_OK) {
            std::wstring dl = std::wstring(path) + L"\\YouTubeDownloader";
            SetWindowTextW(g_hOutput, dl.c_str());
        }
    }
    g_hBrowse = CreateWindowExW(0, L"BUTTON", L"Browse...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 100, 20, hwnd,
        (HMENU)ID_BROWSE, g_hInstance, nullptr);

    // Bottom row
    g_hDownload = CreateWindowExW(0, L"BUTTON", L"Download selected",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 100, 20, hwnd,
        (HMENU)ID_DOWNLOAD, g_hInstance, nullptr);
    g_hCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 100, 20, hwnd,
        (HMENU)ID_CANCEL, g_hInstance, nullptr);
    g_hProgress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 100, 20, hwnd,
        (HMENU)ID_PROGRESS, g_hInstance, nullptr);

    // Log
    g_hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
        ES_AUTOVSCROLL | ES_LEFT,
        0, 0, 100, 100, hwnd, (HMENU)ID_LOG, g_hInstance, nullptr);

    // Apply font to all child controls.
    HWND children[] = {
        lblUrl, g_hUrl, lblType, g_hType, g_hScan,
        g_hSelAll, g_hSelNone, g_hInvert, g_hCount,
        g_hList,
        g_hVideoRadio, g_hAudioRadio,
        GetDlgItem(hwnd, 1003), g_hVideoQuality,
        GetDlgItem(hwnd, 1004), g_hContainer,
        GetDlgItem(hwnd, 1005), g_hAudioFormat,
        GetDlgItem(hwnd, 1006), g_hAudioQuality,
        GetDlgItem(hwnd, 1007), g_hOutput, g_hBrowse,
        g_hDownload, g_hCancel, g_hProgress, g_hLog,
    };
    for (HWND h : children) if (h) setFont(h);

    updateModeUi();
    updateCountLabel();

    return hwnd;
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    g_hInstance = hInst;
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    OleInitialize(nullptr);

    HWND hwnd = createMainWindow();
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    appendLogToCtrl(L"YouTube Bulk Downloader.");
    appendLogToCtrl(L"This is a GUI front-end for yt-dlp.");
    appendLogToCtrl(L"Make sure 'yt-dlp' is installed and on PATH:");
    appendLogToCtrl(L"  https://github.com/yt-dlp/yt-dlp");
    appendLogToCtrl(L"For media merging/conversion, also install ffmpeg:");
    appendLogToCtrl(L"  https://www.ffmpeg.org/download.html");
    appendLogToCtrl(L"");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    OleUninitialize();
    return (int)msg.wParam;
}

// MinGW links wWinMain via -municode if needed, but linking the standard
// `WinMain` symbol works without that flag. Provide an ANSI bridge:
extern "C" int WINAPI WinMain(HINSTANCE h, HINSTANCE, LPSTR, int n) {
    return wWinMain(h, nullptr, GetCommandLineW(), n);
}
