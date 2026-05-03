#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Modal-style "TikTok ayarlari" dialog. Lets the user paste their
// developer-portal credentials, choose a localhost callback port, and
// run the OAuth flow without leaving the app. Blocks the calling thread
// (the main UI thread) until the dialog closes; the parent window is
// disabled in the meantime.
//
// Returns true if the user clicked OK (and credentials may have been
// saved); false if they cancelled or closed it.
bool showTikTokSettingsDialog(HWND parent);
