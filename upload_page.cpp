#include "upload_page.h"

#include "app.h"
#include "oauth.h"
#include "settings.h"
#include "tiktok_dialog.h"
#include "upload_browser.h"
#include "upload_tiktok.h"
#include "util.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <cwchar>
#include <cwctype>
#include <thread>
#include <vector>

namespace UploadPage {

namespace {

enum : int {
    ID_UP_STATUS = 5100,
    ID_UP_SETTINGS,
    ID_UP_BROWSER_SETUP,
    ID_UP_MODE_API,
    ID_UP_MODE_BROWSER,
    ID_UP_MODE_INSTAGRAM,
    ID_UP_CAPTION_LBL,
    ID_UP_CAPTION,
    ID_UP_LIST,
    ID_UP_ADD,
    ID_UP_REMOVE,
    ID_UP_CLEAR,
    ID_UP_ADD_FOLDER,
    ID_UP_INFO,
    ID_UP_UPLOAD,
    ID_UP_CANCEL,
    ID_UP_PROGRESS,
    ID_UP_LOG,
};

const UINT MSG_LOG         = WM_APP_BEGIN + 0;
const UINT MSG_FILE_STATUS = WM_APP_BEGIN + 1;
const UINT MSG_PROGRESS    = WM_APP_BEGIN + 2;
const UINT MSG_DONE        = WM_APP_BEGIN + 3;

struct LogPayload        { std::wstring text; };
struct FileStatusPayload { int row; int status; std::wstring err; std::string publish_id; };
struct ProgressPayload   { int row; long long sent; long long total; };
struct DonePayload       { bool cancelled = false; int total = 0; int succeeded = 0; };

struct UploadFile {
    std::wstring path;
    long long    size = 0;
    int          status = 0;   // 0=pending 1=uploading 2=done 3=failed
    std::wstring err;
    std::string  publish_id;
};

HWND hStatus = nullptr, hSettings = nullptr, hBrowserSetup = nullptr;
HWND hModeApi = nullptr, hModeBrowser = nullptr, hModeInstagram = nullptr;
HWND hCaptionLbl = nullptr, hCaption = nullptr;
HWND hList = nullptr;
HWND hAdd = nullptr, hRemove = nullptr, hClear = nullptr, hAddFolder = nullptr;
HWND hInfo = nullptr, hUpload = nullptr, hCancel = nullptr;
HWND hProgress = nullptr, hLog = nullptr;

std::vector<UploadFile> g_files;
std::thread             g_thread;

enum class UploadMode { Api, TikTokBrowser, InstagramBrowser };
UploadMode currentMode() {
    std::string mode = Settings::get("upload.mode");
    if (mode == "instagram") return UploadMode::InstagramBrowser;
    if (mode == "tiktok_browser") return UploadMode::TikTokBrowser;
    if (mode == "tiktok_api") return UploadMode::Api;

    return Settings::get("tiktok.upload_mode") == "browser"
        ? UploadMode::TikTokBrowser : UploadMode::Api;
}
void saveMode(UploadMode m) {
    const char* mode = "tiktok_api";
    if (m == UploadMode::TikTokBrowser) mode = "tiktok_browser";
    if (m == UploadMode::InstagramBrowser) mode = "instagram";
    Settings::set("upload.mode", mode);
    Settings::set("tiktok.upload_mode", m == UploadMode::TikTokBrowser ? "browser" : "api");
    Settings::save();
}

std::wstring formatSize(long long n) {
    static const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v = (double)n;
    int u = 0;
    while (v >= 1024 && u + 1 < 5) { v /= 1024; ++u; }
    wchar_t buf[32];
    if (u == 0) std::swprintf(buf, 32, L"%lld B", n);
    else        std::swprintf(buf, 32, L"%.1f %ls", v, units[u]);
    return buf;
}

std::wstring fileTitle(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = (slash == std::wstring::npos)
        ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    return name;
}

std::wstring composeInstagramCaption(const std::wstring& path,
                                     const std::wstring& description) {
    std::wstring title = trim(fileTitle(path));
    std::wstring desc = trim(description);
    if (title.empty()) return desc;
    if (desc.empty()) return title;
    return title + L"\r\n\r\n" + desc;
}

const wchar_t* statusName(int s) {
    switch (s) {
    case 0: return L"Pending";
    case 1: return L"Uploading";
    case 2: return L"Done";
    case 3: return L"Failed";
    default: return L"";
    }
}

void appendLog(const std::wstring& line) {
    int len = GetWindowTextLengthW(hLog);
    SendMessageW(hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    std::wstring withNl = line + L"\r\n";
    SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)withNl.c_str());
    SendMessageW(hLog, EM_SCROLLCARET, 0, 0);
}

void refreshRow(int i) {
    if (i < 0 || i >= (int)g_files.size()) return;
    const UploadFile& f = g_files[i];
    ListView_SetItemText(hList, i, 0, (LPWSTR)statusName(f.status));
    std::wstring sz = formatSize(f.size);
    ListView_SetItemText(hList, i, 1, (LPWSTR)sz.c_str());
    ListView_SetItemText(hList, i, 2, (LPWSTR)f.path.c_str());
}

void addFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    long long size = 0;
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER s{};
        if (GetFileSizeEx(h, &s)) size = s.QuadPart;
        CloseHandle(h);
    }
    UploadFile f;
    f.path = path;
    f.size = size;
    f.status = 0;
    int row = (int)g_files.size();
    g_files.push_back(f);

    LVITEMW it{};
    it.mask = LVIF_TEXT;
    it.iItem = row;
    it.iSubItem = 0;
    it.pszText = (LPWSTR)L"";
    int inserted = ListView_InsertItem(hList, &it);
    if (inserted < 0) return;
    ListView_SetCheckState(hList, inserted, TRUE);
    refreshRow(inserted);
}

void onAdd() {
    OPENFILENAMEW ofn{};
    wchar_t buf[8192] = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_hWnd;
    ofn.lpstrFilter = L"Video files\0*.mp4;*.mov;*.webm;*.mkv;*.m4v\0All files\0*.*\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = sizeof(buf) / sizeof(wchar_t);
    ofn.Flags       = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST |
                      OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle  = L"Yuklenecek videolari sec";
    if (!GetOpenFileNameW(&ofn)) return;

    // OPENFILENAME with multi-select returns either a single full path
    // (no embedded null) or directory + null-separated filenames + double null.
    const wchar_t* p = buf;
    std::wstring dir = p;
    p += dir.size() + 1;
    if (*p == L'\0') {
        addFile(dir);
    } else {
        while (*p) {
            std::wstring name = p;
            p += name.size() + 1;
            std::wstring full = dir + L"\\" + name;
            addFile(full);
        }
    }
}

void onRemove() {
    for (int i = (int)g_files.size() - 1; i >= 0; --i) {
        if (ListView_GetCheckState(hList, i)) {
            ListView_DeleteItem(hList, i);
            g_files.erase(g_files.begin() + i);
        }
    }
}

void onClear() {
    ListView_DeleteAllItems(hList);
    g_files.clear();
}

bool isVideoExt(const std::wstring& path) {
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
    return endsWith(L".mp4") || endsWith(L".mov") || endsWith(L".mkv") ||
           endsWith(L".m4v") || endsWith(L".webm");
}

void enumerateVideos(const std::wstring& root, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd{};
    std::wstring spec = root + L"\\*";
    HANDLE h = FindFirstFileW(spec.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        std::wstring child = root + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            enumerateVideos(child, out);
        } else if (isVideoExt(child)) {
            out.push_back(child);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void onAddFolder() {
    BROWSEINFOW bi{};
    bi.hwndOwner = g_hWnd;
    bi.lpszTitle = L"Klasor sec";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH];
    BOOL ok = SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    if (!ok) return;

    std::vector<std::wstring> found;
    enumerateVideos(path, found);
    for (const auto& f : found) addFile(f);
    appendLog(L"[upload] " + std::to_wstring(found.size()) + L" file(s) added from folder");
}

void onCancelClicked() {
    if (!g_busy.load()) return;
    g_cancelRequested.store(true);
    appendLog(L"[upload] cancel requested");
}

void apiUploadWorker(std::vector<int> queue, std::string accessToken) {
    int total = (int)queue.size();
    int ok = 0;
    for (int i = 0; i < total; ++i) {
        if (g_cancelRequested.load()) break;
        int row = queue[i];
        if (row < 0 || row >= (int)g_files.size()) continue;

        {
            auto* p = new FileStatusPayload{row, 1, L"", ""};
            PostMessageW(g_hWnd, MSG_FILE_STATUS, 0, (LPARAM)p);
        }

        UploadTikTok::UploadResult r = UploadTikTok::uploadInbox(
            g_files[row].path,
            accessToken,
            [](const std::wstring& line) {
                auto* p = new LogPayload{line};
                PostMessageW(g_hWnd, MSG_LOG, 0, (LPARAM)p);
            },
            [row](long long sent, long long t) {
                auto* p = new ProgressPayload{row, sent, t};
                PostMessageW(g_hWnd, MSG_PROGRESS, 0, (LPARAM)p);
            },
            g_cancelRequested);

        auto* fs = new FileStatusPayload{
            row, r.success ? 2 : 3, r.error, r.publish_id
        };
        PostMessageW(g_hWnd, MSG_FILE_STATUS, 0, (LPARAM)fs);
        if (r.success) ++ok;
    }
    auto* d = new DonePayload{g_cancelRequested.load(), total, ok};
    PostMessageW(g_hWnd, MSG_DONE, 0, (LPARAM)d);
}

void browserUploadWorker(std::vector<int> queue, std::wstring caption) {
    int total = (int)queue.size();
    int ok = 0;
    for (int i = 0; i < total; ++i) {
        if (g_cancelRequested.load()) break;
        int row = queue[i];
        if (row < 0 || row >= (int)g_files.size()) continue;

        {
            auto* p = new FileStatusPayload{row, 1, L"", ""};
            PostMessageW(g_hWnd, MSG_FILE_STATUS, 0, (LPARAM)p);
        }

        UploadBrowser::UploadResult r = UploadBrowser::uploadOne(
            g_files[row].path,
            caption,
            [](const std::wstring& line) {
                auto* p = new LogPayload{line};
                PostMessageW(g_hWnd, MSG_LOG, 0, (LPARAM)p);
            },
            [](const std::wstring& /*stage*/) {
                // Each stage transition flips the row's "Uploading"
                // indicator on; we don't have a byte-wise progress
                // signal from the browser path so the bar just stays
                // at indeterminate 50%.
            },
            g_cancelRequested);

        auto* fs = new FileStatusPayload{
            row, r.success ? 2 : 3, r.error, ""
        };
        PostMessageW(g_hWnd, MSG_FILE_STATUS, 0, (LPARAM)fs);
        if (r.success) ++ok;
    }
    auto* d = new DonePayload{g_cancelRequested.load(), total, ok};
    PostMessageW(g_hWnd, MSG_DONE, 0, (LPARAM)d);
}

void instagramUploadWorker(std::vector<int> queue, std::wstring description) {
    int total = (int)queue.size();
    int ok = 0;
    for (int i = 0; i < total; ++i) {
        if (g_cancelRequested.load()) break;
        int row = queue[i];
        if (row < 0 || row >= (int)g_files.size()) continue;

        {
            auto* p = new FileStatusPayload{row, 1, L"", ""};
            PostMessageW(g_hWnd, MSG_FILE_STATUS, 0, (LPARAM)p);
        }

        std::wstring caption = composeInstagramCaption(g_files[row].path, description);
        UploadBrowser::UploadResult r = UploadBrowser::uploadInstagramOne(
            g_files[row].path,
            caption,
            [](const std::wstring& line) {
                auto* p = new LogPayload{line};
                PostMessageW(g_hWnd, MSG_LOG, 0, (LPARAM)p);
            },
            [](const std::wstring& /*stage*/) {},
            g_cancelRequested);

        auto* fs = new FileStatusPayload{
            row, r.success ? 2 : 3, r.error, ""
        };
        PostMessageW(g_hWnd, MSG_FILE_STATUS, 0, (LPARAM)fs);
        if (r.success) ++ok;
    }
    auto* d = new DonePayload{g_cancelRequested.load(), total, ok};
    PostMessageW(g_hWnd, MSG_DONE, 0, (LPARAM)d);
}

void setBusyUi(bool busy) {
    UploadMode mode = currentMode();
    bool browserMode = mode != UploadMode::Api;
    EnableWindow(hUpload,       !busy);
    EnableWindow(hAdd,          !busy);
    EnableWindow(hRemove,       !busy);
    EnableWindow(hClear,        !busy);
    EnableWindow(hAddFolder,    !busy);
    EnableWindow(hSettings,     !busy && mode == UploadMode::Api);
    EnableWindow(hBrowserSetup, !busy && browserMode);
    EnableWindow(hModeApi,      !busy);
    EnableWindow(hModeBrowser,  !busy);
    EnableWindow(hModeInstagram, !busy);
    EnableWindow(hCaption,      !busy && browserMode);
    EnableWindow(hCaptionLbl,   !busy && browserMode);
    EnableWindow(hCancel,        busy);
}

std::vector<int> collectQueue() {
    std::vector<int> queue;
    int n = (int)g_files.size();
    for (int i = 0; i < n; ++i) {
        if (ListView_GetCheckState(hList, i)) {
            g_files[i].status = 0;
            g_files[i].err.clear();
            g_files[i].publish_id.clear();
            refreshRow(i);
            queue.push_back(i);
        }
    }
    return queue;
}

void onUpload() {
    if (g_busy.load()) {
        MessageBoxW(g_hWnd, L"Bir is zaten calisiyor (download veya upload).",
                    L"Mesgul", MB_OK | MB_ICONINFORMATION);
        return;
    }

    UploadMode mode = currentMode();

    if (mode == UploadMode::Api) {
        OAuth::Tokens t;
        if (!OAuth::loadTokens(t) || t.access_token.empty()) {
            MessageBoxW(g_hWnd,
                L"API modunda yuklemek icin Tools -> TikTok ayarlari'ndan "
                L"baglanmalisin.",
                L"TikTok bagli degil", MB_OK | MB_ICONWARNING);
            return;
        }
        if (!OAuth::isTokenValid(t)) {
            std::string key    = OAuth::effectiveClientKey();
            std::string secret = OAuth::effectiveClientSecret();
            appendLog(L"[upload] access_token expired, refreshing...");
            OAuth::AuthResult ar = OAuth::tiktokRefresh(key, secret, t.refresh_token);
            if (!ar.success) {
                MessageBoxW(g_hWnd,
                    (L"Token yenilemesi basarisiz: " + ar.error).c_str(),
                    L"TikTok", MB_OK | MB_ICONERROR);
                return;
            }
            OAuth::saveTokens(ar.tokens);
            t = ar.tokens;
            refreshAccountStatus();
        }

        std::vector<int> queue = collectQueue();
        if (queue.empty()) {
            MessageBoxW(g_hWnd, L"En az bir dosya isaretle.",
                        L"Hicbir dosya yok", MB_OK | MB_ICONINFORMATION);
            return;
        }

        SendMessage(hProgress, PBM_SETRANGE32, 0, 1000);
        SendMessage(hProgress, PBM_SETPOS,     0, 0);
        appendLog(L"[upload] API modu, " + std::to_wstring(queue.size()) + L" dosya");

        g_cancelRequested.store(false);
        g_busy.store(true);
        setBusyUi(true);
        if (g_thread.joinable()) g_thread.join();
        g_thread = std::thread(apiUploadWorker, std::move(queue), t.access_token);
        return;
    }

    bool instagramMode = mode == UploadMode::InstagramBrowser;
    bool helperReady = instagramMode
        ? UploadBrowser::isInstagramAvailable()
        : UploadBrowser::isPythonAvailable();
    if (!helperReady) {
        MessageBoxW(g_hWnd,
            L"Browser modu Python + Playwright gerektirir.\n\n"
            L"Bir terminalde:\n"
            L"  pip install playwright\n"
            L"  python -m playwright install chromium",
            L"Python yok", MB_OK | MB_ICONWARNING);
        return;
    }

    std::vector<int> queue = collectQueue();
    if (queue.empty()) {
        MessageBoxW(g_hWnd, L"En az bir dosya isaretle.",
                    L"Hicbir dosya yok", MB_OK | MB_ICONINFORMATION);
        return;
    }

    int capLen = GetWindowTextLengthW(hCaption);
    std::wstring caption;
    if (capLen > 0) {
        std::vector<wchar_t> capBuf((size_t)capLen + 1, L'\0');
        GetWindowTextW(hCaption, capBuf.data(), capLen + 1);
        caption.assign(capBuf.data());
    }

    SendMessage(hProgress, PBM_SETRANGE32, 0, 1000);
    SendMessage(hProgress, PBM_SETPOS,     0, 500);   // indeterminate-ish
    appendLog((instagramMode ? L"[upload] Instagram Reels Browser modu, "
                              : L"[upload] TikTok Browser modu, ")
              + std::to_wstring(queue.size()) + L" dosya");
    appendLog(instagramMode
        ? L"[upload] Instagram Reels caption = dosya basligi + aciklama alani."
        : L"[upload] DIKKAT: TikTok otomasyonu hesap banina yol acabilir.");

    g_cancelRequested.store(false);
    g_busy.store(true);
    setBusyUi(true);
    if (g_thread.joinable()) g_thread.join();
    if (instagramMode) {
        g_thread = std::thread(instagramUploadWorker, std::move(queue), caption);
    } else {
        g_thread = std::thread(browserUploadWorker, std::move(queue), caption);
    }
}

void onBrowserSetup() {
    if (g_busy.load()) return;
    UploadMode mode = currentMode();
    bool instagramMode = mode == UploadMode::InstagramBrowser;
    if (mode == UploadMode::Api) return;

    bool helperReady = instagramMode
        ? UploadBrowser::isInstagramAvailable()
        : UploadBrowser::isPythonAvailable();
    if (!helperReady) {
        MessageBoxW(g_hWnd,
            L"Python + Playwright gerekli.\n\n"
            L"Bir terminalde:\n"
            L"  pip install playwright\n"
            L"  python -m playwright install chromium",
            L"Python yok", MB_OK | MB_ICONWARNING);
        return;
    }
    appendLog(instagramMode
        ? L"[instagram] setup baslatildi - acilan pencereye giris yap"
        : L"[browser] setup baslatildi - acilan pencereye giris yap");
    g_cancelRequested.store(false);
    g_busy.store(true);
    setBusyUi(true);
    if (g_thread.joinable()) g_thread.join();
    g_thread = std::thread([instagramMode]() {
        auto logFn = [](const std::wstring& line) {
            auto* p = new LogPayload{line};
            PostMessageW(g_hWnd, MSG_LOG, 0, (LPARAM)p);
        };
        UploadBrowser::UploadResult r = instagramMode
            ? UploadBrowser::runInstagramSetup(logFn, g_cancelRequested)
            : UploadBrowser::runSetup(logFn, g_cancelRequested);
        if (r.success) {
            Settings::set(instagramMode ? "instagram.browser_setup"
                                        : "tiktok.browser_setup",
                          "done");
            Settings::save();
        }
        auto* d = new DonePayload{g_cancelRequested.load(), 1, r.success ? 1 : 0};
        if (!r.success && !r.error.empty()) {
            auto* lp = new LogPayload{
                (instagramMode ? L"[instagram] setup failed: "
                               : L"[browser] setup failed: ") + r.error
            };
            PostMessageW(g_hWnd, MSG_LOG, 0, (LPARAM)lp);
        }
        PostMessageW(g_hWnd, MSG_DONE, 0, (LPARAM)d);
    });
}

void applyModeUi() {
    UploadMode m = currentMode();
    bool busy = g_busy.load();
    bool browser = (m != UploadMode::Api);
    SendMessage(hModeApi,       BM_SETCHECK, m == UploadMode::Api              ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessage(hModeBrowser,   BM_SETCHECK, m == UploadMode::TikTokBrowser    ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessage(hModeInstagram, BM_SETCHECK, m == UploadMode::InstagramBrowser ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(hCaption,      !busy && browser);
    EnableWindow(hCaptionLbl,   !busy && browser);
    EnableWindow(hBrowserSetup, !busy && browser);
    EnableWindow(hSettings,     !busy && m == UploadMode::Api);
    SetWindowTextW(hBrowserSetup,
        m == UploadMode::InstagramBrowser
            ? L"Instagram setup..."
            : L"TikTok browser setup...");
    SetWindowTextW(hCaptionLbl,
        m == UploadMode::InstagramBrowser
            ? L"Description:"
            : L"Caption:");
    SetWindowTextW(hInfo,
        m == UploadMode::InstagramBrowser
            ? L"Instagram Reels Browser modu: caption dosya basligi + aciklama alanindan olusur. "
              L"Coklu secimde her video sirayla paylasilir."
        : m == UploadMode::TikTokBrowser
            ? L"Browser modu: TikTok'a tarayici uzerinden dogrudan post atilir. "
              L"Hesap banı riski var, test hesabiyla dene."
            : L"API / Inbox modu: video TikTok uygulamasinda Drafts'a duser. "
              L"Caption ve privacy'i orada belirleyip Post tusuna basacaksin.");
}

}  // namespace

std::vector<HWND> createControls(HWND parent, HFONT font) {
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    auto setFont = [font](HWND h) { SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE); };

    hStatus = CreateWindowExW(0, L"STATIC", L"TikTok: Bagli degil",
        WS_CHILD | SS_LEFT, 0, 0, 100, 24,
        parent, (HMENU)(INT_PTR)ID_UP_STATUS, hInst, nullptr);
    setFont(hStatus);

    hSettings = CreateWindowExW(0, L"BUTTON", L"TikTok ayarlari...",
        WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 100, 24,
        parent, (HMENU)(INT_PTR)ID_UP_SETTINGS, hInst, nullptr);
    setFont(hSettings);

    hBrowserSetup = CreateWindowExW(0, L"BUTTON", L"Browser setup...",
        WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 100, 24,
        parent, (HMENU)(INT_PTR)ID_UP_BROWSER_SETUP, hInst, nullptr);
    setFont(hBrowserSetup);

    hModeApi = CreateWindowExW(0, L"BUTTON", L"API (Inbox draft)",
        WS_CHILD | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
        0, 0, 100, 22, parent, (HMENU)(INT_PTR)ID_UP_MODE_API, hInst, nullptr);
    setFont(hModeApi);
    hModeBrowser = CreateWindowExW(0, L"BUTTON", L"TikTok Browser",
        WS_CHILD | WS_TABSTOP | BS_AUTORADIOBUTTON,
        0, 0, 100, 22, parent, (HMENU)(INT_PTR)ID_UP_MODE_BROWSER, hInst, nullptr);
    setFont(hModeBrowser);
    hModeInstagram = CreateWindowExW(0, L"BUTTON", L"Instagram Reels",
        WS_CHILD | WS_TABSTOP | BS_AUTORADIOBUTTON,
        0, 0, 100, 22, parent, (HMENU)(INT_PTR)ID_UP_MODE_INSTAGRAM, hInst, nullptr);
    setFont(hModeInstagram);

    hCaptionLbl = CreateWindowExW(0, L"STATIC", L"Caption:",
        WS_CHILD | SS_LEFT, 0, 0, 100, 22,
        parent, (HMENU)(INT_PTR)ID_UP_CAPTION_LBL, hInst, nullptr);
    setFont(hCaptionLbl);
    hCaption = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE |
        ES_AUTOVSCROLL | ES_WANTRETURN,
        0, 0, 100, 22, parent, (HMENU)(INT_PTR)ID_UP_CAPTION, hInst, nullptr);
    setFont(hCaption);

    hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
        0, 0, 100, 100, parent, (HMENU)(INT_PTR)ID_UP_LIST, hInst, nullptr);
    ListView_SetExtendedListViewStyle(hList,
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH;
    c.pszText = (LPWSTR)L"Status"; c.cx = 110; ListView_InsertColumn(hList, 0, &c);
    c.pszText = (LPWSTR)L"Size";   c.cx = 100; ListView_InsertColumn(hList, 1, &c);
    c.pszText = (LPWSTR)L"Path";   c.cx = 600; ListView_InsertColumn(hList, 2, &c);

    hAdd = CreateWindowExW(0, L"BUTTON", L"Add files...",
        WS_CHILD | WS_TABSTOP, 0, 0, 100, 24,
        parent, (HMENU)(INT_PTR)ID_UP_ADD, hInst, nullptr);
    setFont(hAdd);
    hRemove = CreateWindowExW(0, L"BUTTON", L"Remove",
        WS_CHILD | WS_TABSTOP, 0, 0, 100, 24,
        parent, (HMENU)(INT_PTR)ID_UP_REMOVE, hInst, nullptr);
    setFont(hRemove);
    hClear = CreateWindowExW(0, L"BUTTON", L"Clear",
        WS_CHILD | WS_TABSTOP, 0, 0, 100, 24,
        parent, (HMENU)(INT_PTR)ID_UP_CLEAR, hInst, nullptr);
    setFont(hClear);
    hAddFolder = CreateWindowExW(0, L"BUTTON", L"Add folder...",
        WS_CHILD | WS_TABSTOP, 0, 0, 100, 24,
        parent, (HMENU)(INT_PTR)ID_UP_ADD_FOLDER, hInst, nullptr);
    setFont(hAddFolder);

    hInfo = CreateWindowExW(0, L"STATIC",
        L"Sandbox / Inbox modu: video TikTok uygulamasinda Drafts'a duser. "
        L"Caption ve privacy'i orada belirleyip Post tusuna basacaksin.",
        WS_CHILD | SS_LEFT, 0, 0, 100, 36,
        parent, (HMENU)(INT_PTR)ID_UP_INFO, hInst, nullptr);
    setFont(hInfo);

    hUpload = CreateWindowExW(0, L"BUTTON", L"Upload selected",
        WS_CHILD | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 100, 26,
        parent, (HMENU)(INT_PTR)ID_UP_UPLOAD, hInst, nullptr);
    setFont(hUpload);
    hCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_TABSTOP, 0, 0, 100, 26,
        parent, (HMENU)(INT_PTR)ID_UP_CANCEL, hInst, nullptr);
    setFont(hCancel);
    EnableWindow(hCancel, FALSE);

    hProgress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
        WS_CHILD | PBS_SMOOTH, 0, 0, 100, 22,
        parent, (HMENU)(INT_PTR)ID_UP_PROGRESS, hInst, nullptr);

    hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 100, 100,
        parent, (HMENU)(INT_PTR)ID_UP_LOG, hInst, nullptr);
    setFont(hLog);

    refreshAccountStatus();
    applyModeUi();

    return { hStatus, hSettings, hBrowserSetup, hModeApi, hModeBrowser, hModeInstagram,
             hCaptionLbl, hCaption, hList,
             hAdd, hRemove, hClear, hAddFolder,
             hInfo, hUpload, hCancel, hProgress, hLog };
}

void layout(const RECT& r) {
    int x = r.left + 8, y = r.top + 8;
    int W = r.right  - r.left - 16;
    int H = r.bottom - r.top  - 16;
    int rowH = 26;

    // Top row: status (left) + Settings + Browser-setup buttons (right).
    int btnSettingsW = 160, btnBrowserW = 190, btnGap = 6;
    int rightBlock = btnSettingsW + btnGap + btnBrowserW;
    SetWindowPos(hStatus,       nullptr, x, y + 4, W - rightBlock - 8, rowH - 4, SWP_NOZORDER);
    SetWindowPos(hSettings,     nullptr, x + W - rightBlock,                  y, btnSettingsW, rowH, SWP_NOZORDER);
    SetWindowPos(hBrowserSetup, nullptr, x + W - btnBrowserW,                 y, btnBrowserW,  rowH, SWP_NOZORDER);
    y += rowH + 6;

    // Mode row + description row (mode pinned left, text fills the rest)
    SetWindowPos(hModeApi,       nullptr, x,       y + 2, 150, rowH - 4, SWP_NOZORDER);
    SetWindowPos(hModeBrowser,   nullptr, x + 160, y + 2, 170, rowH - 4, SWP_NOZORDER);
    SetWindowPos(hModeInstagram, nullptr, x + 340, y + 2, 200, rowH - 4, SWP_NOZORDER);
    y += rowH + 4;
    int captionH = rowH * 2 + 8;
    SetWindowPos(hCaptionLbl,    nullptr, x,       y + 4, 130, rowH - 4, SWP_NOZORDER);
    SetWindowPos(hCaption,       nullptr, x + 135, y, W - 135, captionH, SWP_NOZORDER);
    y += captionH + 6;

    int btnRow    = rowH + 6;
    int infoRow   = 36   + 6;
    int actionRow = rowH + 6;
    int progRow   = 22   + 6;
    int logH      = 120;
    int usedTop   = y - (r.top + 8);
    int listH     = H - usedTop - btnRow - infoRow - actionRow - progRow - logH;
    if (listH < 80) listH = 80;
    SetWindowPos(hList, nullptr, x, y, W, listH, SWP_NOZORDER);
    ListView_SetColumnWidth(hList, 2, W - 110 - 100 - 30);
    y += listH + 6;

    int bw = 110, bg = 6;
    SetWindowPos(hAdd,       nullptr, x,                y, bw,  rowH, SWP_NOZORDER);
    SetWindowPos(hRemove,    nullptr, x + (bw + bg),    y, bw,  rowH, SWP_NOZORDER);
    SetWindowPos(hClear,     nullptr, x + (bw + bg)*2,  y, bw,  rowH, SWP_NOZORDER);
    SetWindowPos(hAddFolder, nullptr, x + (bw + bg)*3,  y, 130, rowH, SWP_NOZORDER);
    y += rowH + 6;

    SetWindowPos(hInfo, nullptr, x, y, W, 36, SWP_NOZORDER);
    y += 36 + 6;

    SetWindowPos(hUpload,   nullptr, x,         y, 160, rowH, SWP_NOZORDER);
    SetWindowPos(hCancel,   nullptr, x + 170,   y, 100, rowH, SWP_NOZORDER);
    SetWindowPos(hProgress, nullptr, x + 280,   y + 2, W - 280 - 8, rowH - 4, SWP_NOZORDER);
    y += rowH + 6;

    int logHActual = r.bottom - 8 - y;
    if (logHActual < 60) logHActual = 60;
    SetWindowPos(hLog, nullptr, x, y, W, logHActual, SWP_NOZORDER);
}

bool onCommand(HWND mainWnd, int id, int /*code*/) {
    switch (id) {
    case ID_UP_SETTINGS:
        if (g_busy.load()) return true;
        showTikTokSettingsDialog(mainWnd);
        refreshAccountStatus();
        return true;
    case ID_UP_BROWSER_SETUP: onBrowserSetup(); return true;
    case ID_UP_MODE_API:
        saveMode(UploadMode::Api);
        applyModeUi();
        refreshAccountStatus();
        return true;
    case ID_UP_MODE_BROWSER:
        saveMode(UploadMode::TikTokBrowser);
        applyModeUi();
        refreshAccountStatus();
        return true;
    case ID_UP_MODE_INSTAGRAM:
        saveMode(UploadMode::InstagramBrowser);
        applyModeUi();
        refreshAccountStatus();
        return true;
    case ID_UP_ADD:        onAdd();           return true;
    case ID_UP_REMOVE:     onRemove();        return true;
    case ID_UP_CLEAR:      onClear();         return true;
    case ID_UP_ADD_FOLDER: onAddFolder();     return true;
    case ID_UP_UPLOAD:     onUpload();        return true;
    case ID_UP_CANCEL:     onCancelClicked(); return true;
    }
    return false;
}

bool onAppMessage(UINT msg, WPARAM /*wp*/, LPARAM lp) {
    if (msg == MSG_LOG) {
        LogPayload* p = (LogPayload*)lp;
        appendLog(p->text);
        delete p;
        return true;
    }
    if (msg == MSG_FILE_STATUS) {
        FileStatusPayload* p = (FileStatusPayload*)lp;
        if (p->row >= 0 && p->row < (int)g_files.size()) {
            g_files[p->row].status     = p->status;
            g_files[p->row].err        = p->err;
            g_files[p->row].publish_id = p->publish_id;
            refreshRow(p->row);
            if (p->status == 3) appendLog(L"[upload] failed: " + p->err);
            if (p->status == 2 && !p->publish_id.empty())
                appendLog(L"[upload] draft created. publish_id=" + s2w(p->publish_id));
        }
        delete p;
        return true;
    }
    if (msg == MSG_PROGRESS) {
        ProgressPayload* p = (ProgressPayload*)lp;
        if (p->total > 0) {
            int promille = (int)((p->sent * 1000) / p->total);
            if (promille < 0)    promille = 0;
            if (promille > 1000) promille = 1000;
            SendMessage(hProgress, PBM_SETPOS, (WPARAM)promille, 0);
        }
        delete p;
        return true;
    }
    if (msg == MSG_DONE) {
        DonePayload* p = (DonePayload*)lp;
        if (g_thread.joinable()) g_thread.join();
        g_busy.store(false);
        if (p->cancelled)
            appendLog(L"[upload] cancelled. " +
                      std::to_wstring(p->succeeded) + L"/" +
                      std::to_wstring(p->total) + L" basarili.");
        else
            appendLog(L"[upload] done. " +
                      std::to_wstring(p->succeeded) + L"/" +
                      std::to_wstring(p->total) + L" basarili.");
        setBusyUi(false);
        refreshAccountStatus();
        delete p;
        return true;
    }
    return false;
}

void onShow() {
    refreshAccountStatus();
}

void onShutdown() {
    if (g_thread.joinable()) g_thread.join();
}

void drainQueuedPayloads(HWND hwnd) {
    MSG msg;
    while (PeekMessageW(&msg, hwnd, WM_APP_BEGIN, WM_APP_END, PM_REMOVE)) {
        if      (msg.message == MSG_LOG)         delete (LogPayload*)msg.lParam;
        else if (msg.message == MSG_FILE_STATUS) delete (FileStatusPayload*)msg.lParam;
        else if (msg.message == MSG_PROGRESS)    delete (ProgressPayload*)msg.lParam;
        else if (msg.message == MSG_DONE)        delete (DonePayload*)msg.lParam;
    }
}

void refreshAccountStatus() {
    if (!hStatus) return;
    UploadMode mode = currentMode();
    if (mode == UploadMode::InstagramBrowser) {
        SetWindowTextW(hStatus,
            Settings::get("instagram.browser_setup") == "done"
                ? L"Instagram Reels: giris kaydedildi"
                : L"Instagram Reels: Browser setup ile giris yap");
        return;
    }
    if (mode == UploadMode::TikTokBrowser) {
        SetWindowTextW(hStatus,
            Settings::get("tiktok.browser_setup") == "done"
                ? L"TikTok Browser: giris kaydedildi"
                : L"TikTok Browser: Browser setup ile giris yap");
        return;
    }

    OAuth::Tokens t;
    std::wstring text;
    if (OAuth::loadTokens(t) && OAuth::isTokenValid(t)) {
        text = L"TikTok: Bagli (open_id: " + s2w(t.open_id) + L")";
    } else if (!t.access_token.empty()) {
        text = L"TikTok: Token expired — Settings'ten yenile";
    } else {
        text = L"TikTok: Bagli degil — Settings'ten ayarla";
    }
    SetWindowTextW(hStatus, text.c_str());
}

}  // namespace UploadPage
