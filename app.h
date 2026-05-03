#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "download.h"

// Custom WM_APP messages used to marshal data from worker threads to the UI
// thread. Each one with a payload posts a heap-allocated struct via lParam;
// the receiving handler is responsible for delete-ing it.
#define WM_APP_LOG          (WM_APP + 1)
#define WM_APP_SCAN_ENTRY   (WM_APP + 2)
#define WM_APP_SCAN_DONE    (WM_APP + 3)
#define WM_APP_DL_DONE      (WM_APP + 4)
#define WM_APP_PROGRESS_SET (WM_APP + 5)

struct EntryPayload { VideoEntry entry; };
struct LogPayload   { std::wstring text; };
struct DonePayload  { int code = 0; bool cancelled = false; };

// Globals shared with worker threads (defined in app.cpp).
extern HWND                    g_hWnd;
extern std::atomic<bool>       g_busy;
extern std::atomic<bool>       g_cancelRequested;
extern std::mutex              g_processMutex;
extern HANDLE                  g_currentProcess;
extern std::vector<VideoEntry> g_entries;
extern std::thread             g_workerThread;

// Thread-safe log helpers used by background workers.
void postLog(const std::wstring &line);
void postLogA(const std::string &line);

// Application entry point — creates the main window and runs the message
// loop. Returns the WM_QUIT exit code.
int runApp(HINSTANCE hInst, int nCmdShow);
