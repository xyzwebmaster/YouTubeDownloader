#include "upload_browser.h"

#include "json.h"
#include "process.h"
#include "util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <mutex>

// We want each line of stdout from the helper to be processed as it
// comes (so the UI can stream "open / uploading / posted" stages),
// but we also want CTRL+C-style cancellation to terminate the helper
// mid-flight. Reuse the same g_currentProcess + mutex the download
// path uses for that.
//
// app.h defines g_processMutex and g_currentProcess; we link against
// them at runtime. Using their types here avoids a build-time header
// dependency cycle.
extern std::mutex g_processMutex;
extern HANDLE     g_currentProcess;

namespace UploadBrowser {

namespace {

// Locate browser-helper scripts next to the running .exe. Build/install
// scripts must place the .py files alongside the .exe.
std::wstring exeDir() {
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return L"";
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"" : p.substr(0, slash);
}

std::wstring scriptPathFor(const wchar_t* fileName) {
    std::wstring d = exeDir();
    if (d.empty()) return L"";
    std::vector<std::wstring> candidates = {
        d + L"\\helpers\\" + fileName,
        d + L"\\" + fileName,
    };
    for (const auto& p : candidates) {
        DWORD attr = GetFileAttributesW(p.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES &&
            !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            return p;
        }
    }
    return L"";
}

// Try `python.exe` first, then `py.exe -3`. Both common on Windows.
// Returns the bare command (no args) — the caller appends the script
// path and subcommand.
std::wstring findPython() {
    wchar_t path[MAX_PATH] = {0};
    DWORD n = SearchPathW(nullptr, L"python.exe", nullptr,
                          MAX_PATH, path, nullptr);
    if (n > 0 && n < MAX_PATH) return path;
    n = SearchPathW(nullptr, L"py.exe", nullptr, MAX_PATH, path, nullptr);
    if (n > 0 && n < MAX_PATH) return path;
    return L"";
}

// Run the helper with the given subcommand args, dispatching each
// stdout line to onLine. Returns process exit code, or -1 if the
// helper couldn't be spawned.
int runHelper(const std::wstring& script,
              const wchar_t* scriptName,
              const std::vector<std::wstring>& extraArgs,
              const std::function<void(const std::wstring&)>& onLine,
              const std::atomic<bool>& cancel) {
    std::wstring py = findPython();
    if (py.empty()) {
        onLine(L"{\"ok\":false,\"error\":\"Python (python.exe or py.exe) "
               L"was not found on PATH.\"}");
        return -1;
    }
    if (script.empty()) {
        onLine(L"{\"ok\":false,\"error\":\"" + std::wstring(scriptName) +
               L" was not found next to the .exe.\"}");
        return -1;
    }

    std::vector<std::wstring> args;
    args.push_back(py);
    // py.exe needs `-3` to pick the latest Python 3 launcher; harmless
    // for python.exe (it'll treat it as a script arg, but our script
    // doesn't accept positional args before the subcommand). Skip if
    // we found python.exe.
    bool isPy = (py.size() >= 6 &&
                 _wcsicmp(py.c_str() + py.size() - 6, L"py.exe") == 0);
    if (isPy) args.push_back(L"-3");
    args.push_back(script);
    for (const auto& a : extraArgs) args.push_back(a);

    PipedProcess p = startProcess(buildCmdLine(args));
    if (!p.process) {
        onLine(L"{\"ok\":false,\"error\":\"Python helper process could not be started.\"}");
        return -1;
    }
    {
        std::lock_guard<std::mutex> g(g_processMutex);
        g_currentProcess = p.process;
    }
    int code = readAllLinesAndWait(p, [&](const std::string& line) {
        if (cancel.load()) return;
        onLine(s2w(line));
    });
    {
        std::lock_guard<std::mutex> g(g_processMutex);
        g_currentProcess = nullptr;
    }
    closePipedProcess(p);
    return code;
}

// Try to parse one line from the helper as JSON. Returns null Json on
// failure (the caller should treat the line as plain log text).
Json parseLine(const std::wstring& line) {
    std::string s = w2s(line);
    if (s.empty() || s[0] != '{') return Json();
    return Json::parse(s);
}

}  // namespace

bool isPythonAvailable() {
    return !findPython().empty() && !scriptPath().empty();
}

std::wstring scriptPath() {
    return scriptPathFor(L"tiktok_uploader.py");
}

bool isInstagramAvailable() {
    return !findPython().empty() && !instagramScriptPath().empty();
}

std::wstring instagramScriptPath() {
    return scriptPathFor(L"instagram_uploader.py");
}

UploadResult runSetup(const LogFn& log, const std::atomic<bool>& cancel) {
    UploadResult r;
    bool gotDone = false;
    std::wstring lastError;

    int code = runHelper(scriptPath(), L"tiktok_uploader.py", {L"setup"}, [&](const std::wstring& line) {
        Json j = parseLine(line);
        if (j.isNull()) { log(line); return; }
        if (j.has("error") && !j["error"].asStr().empty()) {
            lastError = s2w(j["error"].asStr());
        }
        if (j["stage"].asStr() == "login_open") {
            log(L"[browser] " + s2w(j["msg"].asStr()));
        } else if (j["stage"].asStr() == "done" && j["ok"].asBool()) {
            gotDone = true;
            log(L"[browser] setup done; session saved");
        } else {
            log(line);
        }
    }, cancel);

    r.success = (code == 0) && gotDone;
    if (!r.success && r.error.empty()) {
        r.error = lastError.empty()
            ? L"setup beklenmedik sekilde sonlandi"
            : lastError;
    }
    return r;
}

UploadResult runInstagramSetup(const LogFn& log, const std::atomic<bool>& cancel) {
    UploadResult r;
    bool gotDone = false;
    std::wstring lastError;

    int code = runHelper(instagramScriptPath(), L"instagram_uploader.py", {L"setup"}, [&](const std::wstring& line) {
        Json j = parseLine(line);
        if (j.isNull()) { log(line); return; }
        if (j.has("error") && !j["error"].asStr().empty()) {
            lastError = s2w(j["error"].asStr());
        }
        if (j["stage"].asStr() == "login_open") {
            log(L"[instagram] " + s2w(j["msg"].asStr()));
        } else if (j["stage"].asStr() == "done" && j["ok"].asBool()) {
            gotDone = true;
            log(L"[instagram] setup done; session saved");
        } else {
            log(line);
        }
    }, cancel);

    r.success = (code == 0) && gotDone;
    if (!r.success && r.error.empty()) {
        r.error = lastError.empty()
            ? L"Instagram setup beklenmedik sekilde sonlandi"
            : lastError;
    }
    return r;
}

LoginStatus getStatus(const LogFn& log, const std::atomic<bool>& cancel) {
    LoginStatus st;
    int code = runHelper(scriptPath(), L"tiktok_uploader.py", {L"status"}, [&](const std::wstring& line) {
        Json j = parseLine(line);
        if (j.isNull()) { log(line); return; }
        if (j.has("logged_in")) {
            st.logged_in = j["logged_in"].asBool();
            st.username  = s2w(j["username"].asStr());
            st.success   = true;
        } else if (j.has("error")) {
            st.error = s2w(j["error"].asStr());
        } else {
            log(line);
        }
    }, cancel);
    if (code != 0 && st.error.empty()) {
        st.error = L"status helper exit " + std::to_wstring(code);
    }
    return st;
}

UploadResult uploadOne(const std::wstring& filePath,
                       const std::wstring& caption,
                       const LogFn& log,
                       const StageFn& stage,
                       const std::atomic<bool>& cancel) {
    UploadResult r;
    std::vector<std::wstring> args = {
        L"upload",
        L"--file",    filePath,
        L"--caption", caption,
    };
    bool seenPosted = false;
    std::wstring lastError;

    int code = runHelper(scriptPath(), L"tiktok_uploader.py", args, [&](const std::wstring& line) {
        Json j = parseLine(line);
        if (j.isNull()) { log(line); return; }
        std::wstring s = s2w(j["stage"].asStr());
        if (!s.empty()) stage(s);
        if (j.has("error") && !j["error"].asStr().empty()) {
            lastError = s2w(j["error"].asStr());
        }
        if (j["stage"].asStr() == "posted" && j["ok"].asBool()) {
            seenPosted = true;
        }
        if (!s.empty()) {
            std::wstring detail;
            if (j.has("selector") && !j["selector"].asStr().empty()) {
                detail = L" (" + s2w(j["selector"].asStr()) + L")";
            }
            log(L"[browser] " + s + detail);
        } else {
            log(line);
        }
    }, cancel);

    r.success = (code == 0) && seenPosted;
    if (!r.success && r.error.empty()) {
        r.error = lastError.empty()
            ? (L"upload helper exit " + std::to_wstring(code))
            : lastError;
    }
    return r;
}

UploadResult uploadInstagramOne(const std::wstring& filePath,
                                const std::wstring& caption,
                                const LogFn& log,
                                const StageFn& stage,
                                const std::atomic<bool>& cancel) {
    UploadResult r;
    std::vector<std::wstring> args = {
        L"upload",
        L"--file",    filePath,
        L"--caption", caption,
    };
    bool seenPosted = false;
    std::wstring lastError;

    int code = runHelper(instagramScriptPath(), L"instagram_uploader.py", args, [&](const std::wstring& line) {
        Json j = parseLine(line);
        if (j.isNull()) { log(line); return; }
        std::wstring s = s2w(j["stage"].asStr());
        if (!s.empty()) stage(s);
        if (j.has("error") && !j["error"].asStr().empty()) {
            lastError = s2w(j["error"].asStr());
        }
        if (j["stage"].asStr() == "posted" && j["ok"].asBool()) {
            seenPosted = true;
        }
        if (!s.empty()) {
            std::wstring detail;
            if (j.has("selector") && !j["selector"].asStr().empty()) {
                detail = L" (" + s2w(j["selector"].asStr()) + L")";
            }
            log(L"[instagram] " + s + detail);
        } else {
            log(line);
        }
    }, cancel);

    r.success = (code == 0) && seenPosted;
    if (!r.success && r.error.empty()) {
        r.error = lastError.empty()
            ? (L"Instagram upload helper exit " + std::to_wstring(code))
            : lastError;
    }
    return r;
}

}  // namespace UploadBrowser
