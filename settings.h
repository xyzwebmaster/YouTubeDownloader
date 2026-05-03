#pragma once
#include <string>

// Process-wide settings store. Persisted as a single DPAPI-encrypted JSON
// blob at %APPDATA%\YouTubeDownloader\settings.dat. DPAPI binds the
// ciphertext to the current Windows user account, so the file is useless
// if copied to a different machine or user — appropriate for storing OAuth
// client secrets and refresh tokens.
//
// Keys are dotted strings (e.g. "tiktok.client_key", "tiktok.access_token")
// so callers don't need to know the underlying nested structure.
namespace Settings {

// Reads + decrypts settings.dat into the in-memory map. Returns false if
// the file exists but couldn't be decrypted/parsed (in which case the
// in-memory map is left empty so the user can re-enter values). A missing
// file is treated as success with an empty map.
bool load();

// Encrypts + writes the in-memory map to settings.dat, atomically via a
// .tmp file. Creates %APPDATA%\YouTubeDownloader if missing.
bool save();

bool        has(const std::string& key);
std::string get(const std::string& key, const std::string& def = "");
void        set(const std::string& key, const std::string& val);
void        erase(const std::string& key);

// UTF-16 convenience overloads — the Win32 GUI hands us wstrings.
std::wstring getW(const std::string& key, const std::wstring& def = L"");
void         setW(const std::string& key, const std::wstring& val);

// Path used for diagnostics / "open settings folder".
std::wstring filePath();

}
