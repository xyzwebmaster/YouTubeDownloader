#include "app.h"

#include "download.h"
#include "oauth.h"
#include "settings.h"
#include "tiktok_dialog.h"
#include "upload_page.h"
#include "util.h"

#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>

#include <cwchar>
#include <functional>

// ----- Comctl32 v6 manifest (modern controls + visual styles) -----
// MSVC embeds this directly; MinGW ignores the pragma and would need a
// separate .manifest file at link time to get visual styles.
#ifdef _MSC_VER
#pragma comment(linker,"/manifestdependency:\"type='win32' "                     \
                       "name='Microsoft.Windows.Common-Controls' "               \
                       "version='6.0.0.0' processorArchitecture='*' "            \
                       "publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

// ============================== IDs =========================================
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
    ID_TAB_CTRL,
};

// Menu IDs (kept high enough to never collide with control IDs above).
enum : int {
    IDM_FILE_EXIT      = 40001,
    IDM_TOOLS_TIKTOK   = 40010,
    IDM_HELP_ABOUT     = 40020,
};

// ============================== Globals =====================================
HWND                    g_hWnd = nullptr;
std::atomic<bool>       g_busy{false};
std::atomic<bool>       g_cancelRequested{false};
std::mutex              g_processMutex;
HANDLE                  g_currentProcess = nullptr;
std::vector<VideoEntry> g_entries;
std::thread             g_workerThread;

static HINSTANCE g_hInstance = nullptr;

static HWND g_hUrl, g_hType, g_hScan;
static HWND g_hSelAll, g_hSelNone, g_hInvert, g_hCount;
static HWND g_hList;
static HWND g_hVideoRadio, g_hAudioRadio;
static HWND g_hVideoQuality, g_hContainer, g_hAudioFormat, g_hAudioQuality;
static HWND g_hOutput, g_hBrowse;
static HWND g_hDownload, g_hCancel, g_hProgress;
static HWND g_hLog;
static HFONT g_hFont = nullptr;

// Tab control + per-tab control vectors. The tab control takes the entire
// client area (below the menu); each tab's controls are children of the
// main window and are show/hidden when the active tab changes.
static HWND               g_hTabs       = nullptr;
static int                g_currentTab  = 0;
static std::vector<HWND>  g_dlControls;
static std::vector<HWND>  g_upControls;

static std::vector<int> g_queue;
static int              g_doneCount = 0;
static int              g_total     = 0;

// Suppresses per-item updateCountLabel() calls during programmatic bulk
// check-state changes (Select all / Select none / Invert). LVN_ITEMCHANGED
// fires once per item; recomputing the label N times for an N-item list is
// quadratic in user-visible time. Bulk handlers set this, run the loop,
// clear it, then call updateCountLabel() once.
static bool g_suppressCountUpdate = false;

// ============================== Log helpers =================================
void postLog(const std::wstring &line) {
    auto *p = new LogPayload{line};
    PostMessageW(g_hWnd, WM_APP_LOG, 0, (LPARAM)p);
}
void postLogA(const std::string &line) { postLog(s2w(line)); }

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

// Drop any heap-allocated WM_APP_* payloads still sitting in the message
// queue. Called from WM_DESTROY after the worker thread has been joined,
// so no new payloads can arrive. Without this, anything posted between
// "worker decides to exit" and "we tear down the queue" leaks.
static void drainQueuedAppPayloads(HWND hwnd) {
    MSG msg;
    while (PeekMessageW(&msg, hwnd, WM_APP, WM_APP + 32, PM_REMOVE)) {
        switch (msg.message) {
        case WM_APP_LOG:        delete (LogPayload*)msg.lParam;   break;
        case WM_APP_SCAN_ENTRY: delete (EntryPayload*)msg.lParam; break;
        case WM_APP_SCAN_DONE:  delete (DonePayload*)msg.lParam;  break;
        case WM_APP_DL_DONE:    delete (DonePayload*)msg.lParam;  break;
        // WM_APP_PROGRESS_SET has no payload.
        default: break;
        }
    }
    UploadPage::drainQueuedPayloads(hwnd);
}

// Switch the visible tab and re-layout. Called from WM_NOTIFY/TCN_SELCHANGE
// and once at startup to position whichever tab opens by default.
static void doLayout(int W, int H);
static void showTab(int t) {
    int dlMode = (t == 0) ? SW_SHOW : SW_HIDE;
    int upMode = (t == 1) ? SW_SHOW : SW_HIDE;
    for (HWND h : g_dlControls) ShowWindow(h, dlMode);
    for (HWND h : g_upControls) ShowWindow(h, upMode);
    g_currentTab = t;
    if (t == 1) UploadPage::onShow();
    RECT rc;
    GetClientRect(g_hWnd, &rc);
    doLayout(rc.right, rc.bottom);
}

// ============================== Layout ======================================
// Position all controls of the Download tab inside `page` (the inner
// rect of the tab control returned by TabCtrl_AdjustRect). Coordinates
// are absolute in the main window's client area, so we offset by
// page.left / page.top via the `pos` lambda.
static void layoutDownloadTab(const RECT& page) {
    const int pad  = 10;
    const int rowH = 26;
    const int pageW = page.right - page.left;
    const int pageH = page.bottom - page.top;

    auto pos = [&](HWND h, int x, int y, int w, int hh) {
        SetWindowPos(h, nullptr, page.left + x, page.top + y, w, hh, SWP_NOZORDER);
    };

    int x = pad, y = pad;

    int labelW = 110, comboW = 130, scanW = 100;
    int urlW = pageW - pad - x - labelW - 60 - comboW - scanW - pad * 4;
    pos(GetDlgItem(g_hWnd, 1001), x, y + 4, labelW, rowH - 4);
    int xx = x + labelW + 4;
    pos(g_hUrl, xx, y, urlW, rowH);
    xx += urlW + 8;
    pos(GetDlgItem(g_hWnd, 1002), xx, y + 4, 50, rowH - 4);
    xx += 50 + 4;
    pos(g_hType, xx, y, comboW, 200);
    xx += comboW + 8;
    pos(g_hScan, xx, y, scanW, rowH);
    y += rowH + pad;

    int btnW = 110;
    pos(g_hSelAll,  x,                y, btnW, rowH);
    pos(g_hSelNone, x + (btnW + 6),   y, btnW, rowH);
    pos(g_hInvert,  x + (btnW + 6)*2, y, 130,  rowH);
    pos(g_hCount,   pageW - 200 - pad, y + 4, 200, rowH - 4);
    y += rowH + 4;

    int bottomReserve = 240;
    int listH = pageH - y - bottomReserve - pad;
    if (listH < 100) listH = 100;
    pos(g_hList, x, y, pageW - 2 * pad, listH);
    ListView_SetColumnWidth(g_hList, 2, pageW - 2 * pad - 60 - 80 - 30);
    y += listH + pad;

    pos(g_hVideoRadio, x,        y + 4, 160, rowH - 4);
    pos(g_hAudioRadio, x + 170,  y + 4, 160, rowH - 4);
    y += rowH;

    pos(GetDlgItem(g_hWnd, 1003), x,        y + 4, 90, rowH - 4);
    pos(g_hVideoQuality, x + 95, y, 170, 200);
    pos(GetDlgItem(g_hWnd, 1004), x + 280, y + 4, 80, rowH - 4);
    pos(g_hContainer,    x + 360, y, 110, 200);
    pos(GetDlgItem(g_hWnd, 1005), x + 490, y + 4, 90, rowH - 4);
    pos(g_hAudioFormat,  x + 580, y, 110, 200);
    pos(GetDlgItem(g_hWnd, 1006), x + 700, y + 4, 90, rowH - 4);
    pos(g_hAudioQuality, x + 790, y, 130, 200);
    y += rowH + 6;

    pos(GetDlgItem(g_hWnd, 1007), x, y + 4, 100, rowH - 4);
    int browseW = 90;
    int outW = pageW - 2 * pad - 100 - browseW - 10;
    pos(g_hOutput, x + 100, y, outW, rowH);
    pos(g_hBrowse, x + 100 + outW + 6, y, browseW, rowH);
    y += rowH + 6;

    pos(g_hDownload, x,       y, 160, rowH);
    pos(g_hCancel,   x + 170, y, 100, rowH);
    pos(g_hProgress, x + 280, y + 2, pageW - x - 280 - pad, rowH - 4);
    y += rowH + 6;

    int logH = pageH - y - pad;
    if (logH < 60) logH = 60;
    pos(g_hLog, x, y, pageW - 2 * pad, logH);
}

static void doLayout(int W, int H) {
    if (!g_hTabs) return;
    SetWindowPos(g_hTabs, nullptr, 0, 0, W, H, SWP_NOZORDER);
    RECT page = { 0, 0, W, H };
    TabCtrl_AdjustRect(g_hTabs, FALSE, &page);
    if (g_currentTab == 0) layoutDownloadTab(page);
    else                   UploadPage::layout(page);
}

// ============================== Button handlers =============================
static void onScanClicked();
static void onDownloadClicked();
static void onBrowseClicked();
static void onCancelClicked();
static void onSelectAll();
static void onSelectNone();
static void onInvert();

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Forward upload-tab WM_APP messages to the upload page module.
    if (msg >= UploadPage::WM_APP_BEGIN && msg < UploadPage::WM_APP_END) {
        if (UploadPage::onAppMessage(msg, wp, lp)) return 0;
    }

    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);
        if (UploadPage::onCommand(hwnd, id, code)) return 0;
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
        case IDM_FILE_EXIT:
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            break;
        case IDM_TOOLS_TIKTOK:
            if (!g_busy.load()) {
                showTikTokSettingsDialog(hwnd);
                UploadPage::refreshAccountStatus();
            }
            break;
        case IDM_HELP_ABOUT:
            MessageBoxW(hwnd,
                L"YouTube Bulk Downloader\n\n"
                L"Win32 GUI front-end for yt-dlp.\n"
                L"With TikTok upload (sandbox / Content Posting API).\n\n"
                L"https://github.com/xyzwebmaster/YouTubeDownloader",
                L"About", MB_OK | MB_ICONINFORMATION);
            break;
        }
        return 0;
    }
    case WM_NOTIFY: {
        LPNMHDR n = (LPNMHDR)lp;
        if (n->hwndFrom == g_hTabs && n->code == TCN_SELCHANGE) {
            showTab(TabCtrl_GetCurSel(g_hTabs));
            return 0;
        }
        if (n->hwndFrom == g_hList && n->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *lv = (NMLISTVIEW*)lp;
            if (lv->uChanged & LVIF_STATE) {
                UINT oldChk = (lv->uOldState & LVIS_STATEIMAGEMASK);
                UINT newChk = (lv->uNewState & LVIS_STATEIMAGEMASK);
                if (oldChk != newChk && !g_suppressCountUpdate)
                    updateCountLabel();
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
        UploadPage::onShutdown();
        drainQueuedAppPayloads(hwnd);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

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

// Helper for the bulk check-state buttons. Suppresses the per-item count
// recompute, freezes redraw to avoid flicker on large lists, runs `op` on
// every row, then restores everything and updates the count once.
static void bulkSetChecks(const std::function<BOOL(int /*row*/)> &op) {
    int n = ListView_GetItemCount(g_hList);
    g_suppressCountUpdate = true;
    SendMessage(g_hList, WM_SETREDRAW, FALSE, 0);
    for (int i = 0; i < n; ++i) ListView_SetCheckState(g_hList, i, op(i));
    SendMessage(g_hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hList, nullptr, TRUE);
    g_suppressCountUpdate = false;
    updateCountLabel();
}

static void onSelectAll()  { bulkSetChecks([](int){ return TRUE; }); }
static void onSelectNone() { bulkSetChecks([](int){ return FALSE; }); }
static void onInvert() {
    bulkSetChecks([](int i){ return ListView_GetCheckState(g_hList, i) ? FALSE : TRUE; });
}

// ============================== Window construction =========================
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

    // Menu bar.
    HMENU hMenu  = CreateMenu();
    HMENU hFile  = CreatePopupMenu();
    AppendMenuW(hFile, MF_STRING, IDM_FILE_EXIT, L"E&xit");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hFile, L"&File");
    HMENU hTools = CreatePopupMenu();
    AppendMenuW(hTools, MF_STRING, IDM_TOOLS_TIKTOK, L"&TikTok ayarlari...");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hTools, L"&Tools");
    HMENU hHelp  = CreatePopupMenu();
    AppendMenuW(hHelp, MF_STRING, IDM_HELP_ABOUT, L"&About");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hHelp, L"&Help");
    SetMenu(hwnd, hMenu);

    NONCLIENTMETRICS ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_hFont = CreateFontIndirectW(&ncm.lfMessageFont);

    auto setFont = [](HWND h){ SendMessageW(h, WM_SETFONT, (WPARAM)g_hFont, TRUE); };

    // Tab control owns the rest of the client area. Download tab holds
    // the existing scan/queue/options/log; Upload tab holds the TikTok
    // file list and uploader. Both sets of controls are children of the
    // main window — we just toggle visibility on TCN_SELCHANGE rather
    // than reparenting.
    g_hTabs = CreateWindowExW(0, WC_TABCONTROLW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
        0, 0, 100, 30, hwnd, (HMENU)(INT_PTR)ID_TAB_CTRL, g_hInstance, nullptr);
    setFont(g_hTabs);
    {
        TCITEMW ti{};
        ti.mask = TCIF_TEXT;
        ti.pszText = (LPWSTR)L"Download";
        TabCtrl_InsertItem(g_hTabs, 0, &ti);
        ti.pszText = (LPWSTR)L"Upload";
        TabCtrl_InsertItem(g_hTabs, 1, &ti);
    }

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

    g_hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
        0, 0, 100, 100, hwnd, (HMENU)ID_LIST, g_hInstance, nullptr);
    ListView_SetExtendedListViewStyle(g_hList,
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH;
    c.pszText = (LPWSTR)L"Type";     c.cx = 60;  ListView_InsertColumn(g_hList, 0, &c);
    c.pszText = (LPWSTR)L"Duration"; c.cx = 80;  ListView_InsertColumn(g_hList, 1, &c);
    c.pszText = (LPWSTR)L"Title";    c.cx = 600; ListView_InsertColumn(g_hList, 2, &c);

    g_hVideoRadio = CreateWindowExW(0, L"BUTTON", L"Video (with audio)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
        0, 0, 100, 20, hwnd, (HMENU)ID_VIDEO_RADIO, g_hInstance, nullptr);
    g_hAudioRadio = CreateWindowExW(0, L"BUTTON", L"Audio only",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
        0, 0, 100, 20, hwnd, (HMENU)ID_AUDIO_RADIO, g_hInstance, nullptr);
    SendMessage(g_hVideoRadio, BM_SETCHECK, BST_CHECKED, 0);

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

    g_hDownload = CreateWindowExW(0, L"BUTTON", L"Download selected",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 100, 20, hwnd,
        (HMENU)ID_DOWNLOAD, g_hInstance, nullptr);
    g_hCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 100, 20, hwnd,
        (HMENU)ID_CANCEL, g_hInstance, nullptr);
    g_hProgress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 100, 20, hwnd,
        (HMENU)ID_PROGRESS, g_hInstance, nullptr);

    g_hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
        ES_AUTOVSCROLL | ES_LEFT,
        0, 0, 100, 100, hwnd, (HMENU)ID_LOG, g_hInstance, nullptr);

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

    // Track all download-tab controls so showTab() can hide them when
    // the user switches to the Upload tab.
    for (HWND h : children) if (h) g_dlControls.push_back(h);

    // Build the Upload tab's controls (initially hidden — they're
    // created without WS_VISIBLE). We track them in g_upControls so
    // showTab() can show them.
    g_upControls = UploadPage::createControls(hwnd, g_hFont);

    updateModeUi();
    updateCountLabel();

    return hwnd;
}

// ============================== Entry =======================================
int runApp(HINSTANCE hInst, int nCmdShow) {
    g_hInstance = hInst;

    // Force yt-dlp (Python) to emit UTF-8 on its piped stdout/stderr, so
    // non-ASCII characters in titles (Turkish, accented Latin, etc.) survive
    // the round-trip through the pipe instead of being mangled by the system
    // code page.
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    SetEnvironmentVariableW(L"PYTHONUTF8", L"1");

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS |
                 ICC_STANDARD_CLASSES | ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);
    OleInitialize(nullptr);

    // Load persisted user settings (TikTok credentials, tokens, etc.)
    // before any UI element reads them. A missing file is treated as
    // "no settings yet" — see settings.cpp.
    Settings::load();

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
