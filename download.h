#pragma once
#include <string>
#include <vector>

struct VideoEntry {
    std::wstring id;
    std::wstring title;
    std::wstring url;
    int          duration = 0;
    bool         isShort  = false;
};

struct DownloadOpts {
    bool         audioOnly    = false;
    int          maxHeight    = -1;        // -1 = best
    std::wstring container;                 // "" = auto
    std::wstring audioFormat;               // "best" or specific
    std::wstring audioQuality;              // e.g. "0", "320K"
    std::wstring outputDir;
};

// Background workers. Both run on a dedicated thread (g_workerThread) and
// communicate with the UI by PostMessageW(g_hWnd, WM_APP_*, ...).
void scanWorker(std::wstring channelBase, bool wantVideos, bool wantShorts);
void downloadWorker(std::vector<int> queueCopy,
                    std::vector<VideoEntry> entriesCopy,
                    DownloadOpts opts);
