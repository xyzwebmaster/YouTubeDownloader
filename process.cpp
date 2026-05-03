#include "process.h"

PipedProcess startProcess(std::wstring cmdline) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0)) return {};
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outWrite;
    si.hStdError  = outWrite;
    si.hStdInput  = nullptr;

    PROCESS_INFORMATION pi{};
    std::wstring buf = std::move(cmdline);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr,
                             TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                             &si, &pi);
    CloseHandle(outWrite);
    if (!ok) {
        CloseHandle(outRead);
        return {};
    }
    PipedProcess p;
    p.process    = pi.hProcess;
    p.thread     = pi.hThread;
    p.stdoutRead = outRead;
    return p;
}

int readAllLinesAndWait(PipedProcess &p,
                        const std::function<void(const std::string&)> &onLine) {
    char  buf[4096];
    std::string acc;
    DWORD got = 0;
    while (ReadFile(p.stdoutRead, buf, sizeof(buf), &got, nullptr) && got > 0) {
        acc.append(buf, got);
        for (;;) {
            size_t pos = acc.find_first_of("\r\n");
            if (pos == std::string::npos) break;
            std::string line = acc.substr(0, pos);
            size_t skip = 1;
            if (pos + 1 < acc.size() && acc[pos] == '\r' && acc[pos + 1] == '\n') skip = 2;
            acc.erase(0, pos + skip);
            if (!line.empty()) onLine(line);
        }
    }
    if (!acc.empty()) onLine(acc);

    WaitForSingleObject(p.process, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(p.process, &code);
    return (int)code;
}

void closePipedProcess(PipedProcess &p) {
    if (p.stdoutRead) CloseHandle(p.stdoutRead);
    if (p.thread)     CloseHandle(p.thread);
    if (p.process)    CloseHandle(p.process);
    p = {};
}
