#include "settings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <map>
#include <mutex>
#include <vector>

#include "json.h"
#include "util.h"

#ifdef _MSC_VER
#pragma comment(lib, "crypt32.lib")
#endif

namespace {

std::mutex                          g_mu;
std::map<std::string, std::string>  g_kv;
bool                                g_loaded = false;

std::wstring appDir() {
    wchar_t base[MAX_PATH] = {0};
    if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, base) != S_OK) return L"";
    std::wstring p = std::wstring(base) + L"\\YouTubeDownloader";
    SHCreateDirectoryExW(nullptr, p.c_str(), nullptr);
    return p;
}

bool readFileBytes(const std::wstring& path, std::vector<BYTE>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > (LONGLONG)(64 * 1024 * 1024)) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    BOOL ok = ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr);
    CloseHandle(h);
    if (!ok || got != out.size()) { out.clear(); return false; }
    return true;
}

bool writeFileBytesAtomic(const std::wstring& path, const BYTE* data, size_t n) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, data, (DWORD)n, &wrote, nullptr) && wrote == (DWORD)n;
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

bool dpapiProtect(const BYTE* in, size_t n, std::vector<BYTE>& out) {
    DATA_BLOB src{(DWORD)n, const_cast<BYTE*>(in)};
    DATA_BLOB dst{};
    if (!CryptProtectData(&src, L"YouTubeDownloader settings",
                          nullptr, nullptr, nullptr, 0, &dst)) return false;
    out.assign(dst.pbData, dst.pbData + dst.cbData);
    LocalFree(dst.pbData);
    return true;
}

bool dpapiUnprotect(const BYTE* in, size_t n, std::vector<BYTE>& out) {
    DATA_BLOB src{(DWORD)n, const_cast<BYTE*>(in)};
    DATA_BLOB dst{};
    if (!CryptUnprotectData(&src, nullptr, nullptr, nullptr, nullptr, 0, &dst))
        return false;
    out.assign(dst.pbData, dst.pbData + dst.cbData);
    LocalFree(dst.pbData);
    return true;
}

}  // namespace

namespace Settings {

std::wstring filePath() {
    std::wstring d = appDir();
    if (d.empty()) return L"";
    return d + L"\\settings.dat";
}

bool load() {
    std::lock_guard<std::mutex> g(g_mu);
    g_kv.clear();
    g_loaded = true;

    std::wstring p = filePath();
    if (p.empty()) return false;

    std::vector<BYTE> cipher;
    if (!readFileBytes(p, cipher)) return true;  // file missing == empty settings

    std::vector<BYTE> plain;
    if (!dpapiUnprotect(cipher.data(), cipher.size(), plain)) {
        // File exists but can't be decrypted (different user / corrupt).
        // Leave map empty so the user can re-enter values.
        return false;
    }

    std::string json((const char*)plain.data(), plain.size());
    std::string err;
    Json j = Json::parse(json, &err);
    if (!j.isObject()) return false;
    for (const auto& key : j.keys()) {
        const Json& v = j[key];
        if (v.isString()) g_kv[key] = v.asStr();
        else if (v.isNumber()) {
            char buf[64];
            long long iv = v.asI64();
            if ((double)iv == v.asNum()) std::snprintf(buf, sizeof(buf), "%lld", iv);
            else                          std::snprintf(buf, sizeof(buf), "%.17g", v.asNum());
            g_kv[key] = buf;
        } else if (v.isBool()) {
            g_kv[key] = v.asBool() ? "true" : "false";
        }
    }
    return true;
}

bool save() {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_loaded) g_loaded = true;

    std::wstring p = filePath();
    if (p.empty()) return false;

    Json j = Json::object();
    for (const auto& kv : g_kv) j.set(kv.first, Json(kv.second));
    std::string text = j.dump(0);

    std::vector<BYTE> cipher;
    if (!dpapiProtect((const BYTE*)text.data(), text.size(), cipher)) return false;

    return writeFileBytesAtomic(p, cipher.data(), cipher.size());
}

bool has(const std::string& key) {
    std::lock_guard<std::mutex> g(g_mu);
    return g_kv.find(key) != g_kv.end();
}

std::string get(const std::string& key, const std::string& def) {
    std::lock_guard<std::mutex> g(g_mu);
    auto it = g_kv.find(key);
    return it == g_kv.end() ? def : it->second;
}

void set(const std::string& key, const std::string& val) {
    std::lock_guard<std::mutex> g(g_mu);
    g_kv[key] = val;
}

void erase(const std::string& key) {
    std::lock_guard<std::mutex> g(g_mu);
    g_kv.erase(key);
}

std::wstring getW(const std::string& key, const std::wstring& def) {
    std::string v = get(key);
    if (v.empty()) return def;
    return s2w(v);
}

void setW(const std::string& key, const std::wstring& val) {
    set(key, w2s(val));
}

}  // namespace Settings
