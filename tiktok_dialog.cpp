#include "tiktok_dialog.h"

#include "oauth.h"
#include "settings.h"
#include "util.h"

#include <shellapi.h>

#include <atomic>
#include <cstdlib>
#include <thread>

namespace {

enum : int {
    ID_TT_KEY = 4001,
    ID_TT_SECRET,
    ID_TT_REDIRECT,
    ID_TT_PORT,
    ID_TT_SCOPE,
    ID_TT_CONNECT,
    ID_TT_DISCONNECT,
    ID_TT_STATUS,
    ID_TT_OK,
    ID_TT_CANCEL,
    ID_TT_OPEN_PORTAL,
    ID_TT_LOG,
};

#define WM_APP_OAUTH_LOG  (WM_APP + 20)
#define WM_APP_OAUTH_DONE (WM_APP + 21)

struct LogPayload  { std::wstring text; };
struct DonePayload { OAuth::AuthResult ar; };

struct DlgState {
    HWND hKey       = nullptr;
    HWND hSecret    = nullptr;
    HWND hRedirect  = nullptr;
    HWND hPort      = nullptr;
    HWND hScope     = nullptr;
    HWND hConnect   = nullptr;
    HWND hDisconnect= nullptr;
    HWND hStatus    = nullptr;
    HWND hOK        = nullptr;
    HWND hCancel    = nullptr;
    HWND hPortal    = nullptr;
    HWND hLog       = nullptr;
    HFONT hFont     = nullptr;

    bool result = false;
    bool done   = false;

    std::thread       oauthThread;
    std::atomic<bool> oauthCancel{false};
    std::atomic<bool> oauthBusy{false};
};

void appendLog(HWND log, const std::wstring& line) {
    int len = GetWindowTextLengthW(log);
    SendMessageW(log, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    std::wstring withNl = line + L"\r\n";
    SendMessageW(log, EM_REPLACESEL, FALSE, (LPARAM)withNl.c_str());
    SendMessageW(log, EM_SCROLLCARET, 0, 0);
}

void updateStatusLine(DlgState* s) {
    OAuth::Tokens t;
    if (OAuth::loadTokens(t) && OAuth::isTokenValid(t)) {
        std::wstring msg = L"Bagli (open_id: " + s2w(t.open_id) + L")";
        SetWindowTextW(s->hStatus, msg.c_str());
        EnableWindow(s->hDisconnect, TRUE);
    } else if (!t.access_token.empty()) {
        SetWindowTextW(s->hStatus, L"Token süresi doldu — Connect ile yenile");
        EnableWindow(s->hDisconnect, TRUE);
    } else {
        SetWindowTextW(s->hStatus, L"Bagli degil");
        EnableWindow(s->hDisconnect, FALSE);
    }
}

void setInputsEnabled(DlgState* s, bool en) {
    EnableWindow(s->hKey,        en);
    EnableWindow(s->hSecret,     en);
    EnableWindow(s->hRedirect,   en);
    EnableWindow(s->hPort,       en);
    EnableWindow(s->hConnect,    en);
    EnableWindow(s->hOK,         en);
    EnableWindow(s->hCancel,     en);
}

void onConnect(HWND hwnd, DlgState* s) {
    if (s->oauthBusy.load()) return;

    wchar_t kBuf[1024], sBuf[1024], rBuf[1024], pBuf[64];
    GetWindowTextW(s->hKey,      kBuf, 1024);
    GetWindowTextW(s->hSecret,   sBuf, 1024);
    GetWindowTextW(s->hRedirect, rBuf, 1024);
    GetWindowTextW(s->hPort,     pBuf, 64);
    std::string key      = w2s(kBuf);
    std::string secret   = w2s(sBuf);
    std::string redirect = w2s(rBuf);
    int port = _wtoi(pBuf);
    if (port <= 0 || port > 65535) port = 53682;
    if (key.empty() || secret.empty()) {
        MessageBoxW(hwnd, L"Client Key ve Client Secret gerekli.",
                    L"TikTok", MB_OK | MB_ICONWARNING);
        return;
    }

    s->oauthCancel.store(false);
    s->oauthBusy.store(true);
    setInputsEnabled(s, false);
    EnableWindow(s->hDisconnect, FALSE);
    appendLog(s->hLog, L"[oauth] starting...");

    if (s->oauthThread.joinable()) s->oauthThread.join();
    s->oauthThread = std::thread([hwnd, s, key, secret, redirect, port]() {
        OAuth::AuthResult ar = OAuth::tiktokAuthorize(
            key, secret, redirect, port,
            [hwnd](const std::wstring& line) {
                auto* p = new LogPayload{line};
                PostMessageW(hwnd, WM_APP_OAUTH_LOG, 0, (LPARAM)p);
            },
            s->oauthCancel);
        auto* d = new DonePayload{std::move(ar)};
        PostMessageW(hwnd, WM_APP_OAUTH_DONE, 0, (LPARAM)d);
    });
}

LRESULT CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DlgState* s = (DlgState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == ID_TT_OK) {
            wchar_t buf[1024];
            GetWindowTextW(s->hKey,      buf, 1024); Settings::setW("tiktok.client_key",     buf);
            GetWindowTextW(s->hSecret,   buf, 1024); Settings::setW("tiktok.client_secret",  buf);
            GetWindowTextW(s->hRedirect, buf, 1024); Settings::setW("tiktok.redirect_uri",   buf);
            GetWindowTextW(s->hPort,     buf, 1024); Settings::setW("tiktok.listener_port",  buf);
            Settings::save();
            s->result = true;
            DestroyWindow(hwnd);
        } else if (id == ID_TT_CANCEL) {
            DestroyWindow(hwnd);
        } else if (id == ID_TT_OPEN_PORTAL) {
            ShellExecuteW(nullptr, L"open",
                          L"https://developers.tiktok.com/",
                          nullptr, nullptr, SW_SHOWNORMAL);
        } else if (id == ID_TT_DISCONNECT) {
            OAuth::clearTokens();
            updateStatusLine(s);
            appendLog(s->hLog, L"[oauth] tokens cleared");
        } else if (id == ID_TT_CONNECT) {
            onConnect(hwnd, s);
        }
        return 0;
    }
    case WM_APP_OAUTH_LOG: {
        LogPayload* p = (LogPayload*)lp;
        appendLog(s->hLog, p->text);
        delete p;
        return 0;
    }
    case WM_APP_OAUTH_DONE: {
        DonePayload* p = (DonePayload*)lp;
        if (s->oauthThread.joinable()) s->oauthThread.join();
        s->oauthBusy.store(false);
        if (p->ar.success) {
            OAuth::saveTokens(p->ar.tokens);
            appendLog(s->hLog,
                L"[oauth] connected. open_id=" + s2w(p->ar.tokens.open_id));
        } else {
            appendLog(s->hLog, L"[oauth] failed: " + p->ar.error);
        }
        setInputsEnabled(s, true);
        updateStatusLine(s);
        delete p;
        return 0;
    }
    case WM_CLOSE:
        if (s->oauthBusy.load()) {
            s->oauthCancel.store(true);
            // Drop the close request — the OAuth thread will land on
            // WM_APP_OAUTH_DONE soon and the user can close then. This
            // avoids racing the worker thread against window destruction.
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (s->oauthThread.joinable()) s->oauthThread.join();
        s->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND mkLabel(HWND parent, HINSTANCE hInst, HFONT font,
             int x, int y, int w, int h, const wchar_t* text) {
    HWND lbl = CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, h, parent, nullptr, hInst, nullptr);
    SendMessageW(lbl, WM_SETFONT, (WPARAM)font, TRUE);
    return lbl;
}

}  // namespace

bool showTikTokSettingsDialog(HWND parent) {
    static const wchar_t* kClass = L"YTDL_TikTokSettingsDlg";
    static bool registered = false;

    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = DlgProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    DlgState st{};

    NONCLIENTMETRICS ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    st.hFont = CreateFontIndirectW(&ncm.lfMessageFont);

    const int W = 640, H = 540;
    RECT pr;
    GetWindowRect(parent, &pr);
    int x = pr.left + ((pr.right  - pr.left)   - W) / 2;
    int y = pr.top  + ((pr.bottom - pr.top)    - H) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, L"TikTok ayarlari",
        WS_POPUPWINDOW | WS_CAPTION,
        x, y, W, H, parent, nullptr, hInst, nullptr);
    if (!dlg) {
        DeleteObject(st.hFont);
        return false;
    }
    SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)&st);

    auto setFont = [&](HWND h) { SendMessageW(h, WM_SETFONT, (WPARAM)st.hFont, TRUE); };

    int padX = 12, lblW = 120, ctrlX = padX + lblW + 8;
    int row = 12, rowH = 26;

    mkLabel(dlg, hInst, st.hFont, padX, row + 4, lblW, rowH - 4, L"Client Key:");
    st.hKey = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        Settings::getW("tiktok.client_key").c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        ctrlX, row, W - ctrlX - padX, rowH, dlg, (HMENU)ID_TT_KEY, hInst, nullptr);
    setFont(st.hKey);
    row += rowH + 4;

    mkLabel(dlg, hInst, st.hFont, padX, row + 4, lblW, rowH - 4, L"Client Secret:");
    st.hSecret = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        Settings::getW("tiktok.client_secret").c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD,
        ctrlX, row, W - ctrlX - padX, rowH, dlg, (HMENU)ID_TT_SECRET, hInst, nullptr);
    setFont(st.hSecret);
    row += rowH + 4;

    mkLabel(dlg, hInst, st.hFont, padX, row + 4, lblW, rowH - 4, L"Redirect URI:");
    std::wstring redirDef = Settings::getW("tiktok.redirect_uri",
                                           L"http://localhost:53682/callback");
    st.hRedirect = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", redirDef.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        ctrlX, row, W - ctrlX - padX - 110, rowH,
        dlg, (HMENU)ID_TT_REDIRECT, hInst, nullptr);
    setFont(st.hRedirect);
    st.hPortal = CreateWindowExW(0, L"BUTTON", L"Dev portal...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        W - padX - 100, row, 100, rowH,
        dlg, (HMENU)ID_TT_OPEN_PORTAL, hInst, nullptr);
    setFont(st.hPortal);
    row += rowH + 4;

    mkLabel(dlg, hInst, st.hFont, padX, row + 4, lblW, rowH - 4, L"Listener port:");
    std::wstring portDef = Settings::getW("tiktok.listener_port", L"53682");
    st.hPort = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", portDef.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
        ctrlX, row, 100, rowH, dlg, (HMENU)ID_TT_PORT, hInst, nullptr);
    setFont(st.hPort);
    row += rowH + 4;

    mkLabel(dlg, hInst, st.hFont, padX, row + 4, lblW, rowH - 4, L"Scope:");
    st.hScope = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        L"user.info.basic,video.upload",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
        ctrlX, row, W - ctrlX - padX, rowH,
        dlg, (HMENU)ID_TT_SCOPE, hInst, nullptr);
    setFont(st.hScope);
    row += rowH + 12;

    mkLabel(dlg, hInst, st.hFont, padX, row + 4, lblW, rowH - 4, L"Durum:");
    st.hStatus = CreateWindowExW(0, L"STATIC", L"...",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_NOPREFIX,
        ctrlX, row + 4, W - ctrlX - padX, rowH - 4,
        dlg, (HMENU)ID_TT_STATUS, hInst, nullptr);
    setFont(st.hStatus);
    row += rowH + 4;

    st.hConnect = CreateWindowExW(0, L"BUTTON", L"Connect TikTok...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        padX, row, 160, rowH, dlg, (HMENU)ID_TT_CONNECT, hInst, nullptr);
    setFont(st.hConnect);
    st.hDisconnect = CreateWindowExW(0, L"BUTTON", L"Disconnect",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        padX + 170, row, 120, rowH, dlg, (HMENU)ID_TT_DISCONNECT, hInst, nullptr);
    setFont(st.hDisconnect);
    row += rowH + 8;

    int logH = H - row - rowH - 24 - padX;
    if (logH < 60) logH = 60;
    st.hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
        ES_AUTOVSCROLL,
        padX, row, W - 2 * padX, logH,
        dlg, (HMENU)ID_TT_LOG, hInst, nullptr);
    setFont(st.hLog);

    st.hOK = CreateWindowExW(0, L"BUTTON", L"OK",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        W - padX - 200, H - rowH - 28, 90, rowH,
        dlg, (HMENU)ID_TT_OK, hInst, nullptr);
    setFont(st.hOK);
    st.hCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        W - padX - 100, H - rowH - 28, 90, rowH,
        dlg, (HMENU)ID_TT_CANCEL, hInst, nullptr);
    setFont(st.hCancel);

    updateStatusLine(&st);
    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    SetFocus(st.hKey);

    // Modal loop. We can't use PostQuitMessage to break out (that would
    // also terminate the main app's outer loop), so we wait on a flag set
    // in WM_DESTROY. MsgWaitForMultipleObjects gives us a 100ms tick to
    // re-check IsWindow() — there's no message that reliably arrives
    // *after* the queue tears down, so we have to poll.
    while (!st.done) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) {
                // Re-post for the outer loop and bail.
                PostQuitMessage((int)m.wParam);
                EnableWindow(parent, TRUE);
                if (st.hFont) DeleteObject(st.hFont);
                return st.result;
            }
            if (!IsDialogMessageW(dlg, &m)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
    }

    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    if (st.hFont) DeleteObject(st.hFont);
    return st.result;
}
