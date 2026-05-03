#include "util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cwchar>
#include <cwctype>

std::wstring s2w(const std::string &s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string w2s(const std::wstring &w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring trim(std::wstring s) {
    auto issp = [](wchar_t c){ return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    while (!s.empty() && issp(s.front())) s.erase(s.begin());
    while (!s.empty() && issp(s.back()))  s.pop_back();
    return s;
}

std::wstring formatDuration(int seconds) {
    if (seconds <= 0) return L"--:--";
    int h = seconds / 3600;
    int m = (seconds % 3600) / 60;
    int s = seconds % 60;
    wchar_t buf[32];
    if (h > 0) swprintf(buf, 32, L"%d:%02d:%02d", h, m, s);
    else       swprintf(buf, 32, L"%02d:%02d", m, s);
    return buf;
}

std::wstring normalizeChannelUrl(std::wstring u) {
    u = trim(u);
    while (!u.empty() && u.back() == L'/') u.pop_back();
    static const wchar_t *suffixes[] = {
        L"/videos", L"/shorts", L"/streams", L"/live",
        L"/featured", L"/playlists", L"/community", L"/about", L"/channels"
    };
    for (auto sfx : suffixes) {
        size_t L = wcslen(sfx);
        if (u.size() >= L) {
            std::wstring tail = u.substr(u.size() - L);
            for (auto &c : tail) c = towlower(c);
            if (tail == sfx) { u.resize(u.size() - L); break; }
        }
    }
    return u;
}

std::wstring quoteArg(const std::wstring &arg) {
    if (!arg.empty() &&
        arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out;
    out.push_back(L'"');
    for (size_t i = 0; i < arg.size(); ) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') { ++backslashes; ++i; }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        } else if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(arg[i]);
        }
        ++i;
    }
    out.push_back(L'"');
    return out;
}

std::wstring buildCmdLine(const std::vector<std::wstring> &args) {
    std::wstring out;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) out.push_back(L' ');
        out += quoteArg(args[i]);
    }
    return out;
}
