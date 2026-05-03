#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>

// "Upload" tab page. The main window owns a tab control with two pages
// (Download / Upload); this module owns everything inside the Upload
// page — its controls, its file list, and its background upload thread.
//
// The main wndproc forwards events to the page via these entry points
// so app.cpp doesn't have to know about TikTok upload internals.
namespace UploadPage {

// WM_APP messages emitted by the upload worker thread. Only the values
// in this range are forwarded to onAppMessage() — everything else is
// download-tab traffic (or core wndproc) and ignored here.
const UINT WM_APP_BEGIN = WM_APP + 100;
const UINT WM_APP_END   = WM_APP + 110;

// Create all upload-tab controls as children of `parent` and return
// their handles so the caller can show/hide them as the active tab
// changes. Controls start hidden — call onShow() when the upload tab
// becomes the active one.
std::vector<HWND> createControls(HWND parent, HFONT font);

// Position the controls inside the given tab-page rect (the inner area
// returned by TabCtrl_AdjustRect on the tab control).
void layout(const RECT& pageArea);

// Forward WM_COMMAND clicks on upload-tab controls. Returns true if
// the id was consumed.
bool onCommand(HWND mainWnd, int id, int code);

// Forward any WM_APP_* message in our reserved range. Returns true if
// consumed.
bool onAppMessage(UINT msg, WPARAM wp, LPARAM lp);

// Called when the tab becomes visible. Refreshes the connection status
// line in case tokens changed since last show.
void onShow();

// Called from the main WM_DESTROY handler so the upload thread can be
// joined cleanly.
void onShutdown();

// Re-read tokens and refresh the status line. Call after the TikTok
// settings dialog closes.
void refreshAccountStatus();

// Drain any heap-allocated payloads still in the message queue. Called
// from the main window's WM_DESTROY drain so we don't leak on shutdown.
void drainQueuedPayloads(HWND hwnd);

}  // namespace UploadPage
