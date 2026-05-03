#pragma once
#include <string>
#include <vector>

std::wstring s2w(const std::string &s);
std::string  w2s(const std::wstring &w);
std::wstring trim(std::wstring s);
std::wstring formatDuration(int seconds);
std::wstring normalizeChannelUrl(std::wstring u);
std::wstring quoteArg(const std::wstring &arg);
std::wstring buildCmdLine(const std::vector<std::wstring> &args);
