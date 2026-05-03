// YouTube Bulk Downloader - Win32 GUI front-end for yt-dlp.
//
// Build (MinGW):
//   g++ -O2 -static -mwindows -DUNICODE -D_UNICODE -o YouTubeDownloader.exe
//       main.cpp app.cpp download.cpp process.cpp util.cpp
//       http.cpp json.cpp settings.cpp oauth.cpp tiktok_dialog.cpp
//       upload_tiktok.cpp upload_page.cpp
//       -lcomctl32 -lcomdlg32 -lshell32 -lole32 -luuid
//       -lwinhttp -lcrypt32 -lbcrypt -lws2_32

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

#include "app.h"

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    return runApp(hInst, nCmdShow);
}

// MinGW links wWinMain via -municode if needed, but linking the standard
// `WinMain` symbol works without that flag. Provide an ANSI bridge:
extern "C" int WINAPI WinMain(HINSTANCE h, HINSTANCE, LPSTR, int n) {
    return wWinMain(h, nullptr, GetCommandLineW(), n);
}
