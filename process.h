#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <functional>
#include <string>

struct PipedProcess {
    HANDLE process    = nullptr;
    HANDLE thread     = nullptr;
    HANDLE stdoutRead = nullptr;
};

PipedProcess startProcess(std::wstring cmdline);

// Drain stdout (calling onLine per \n-terminated line), then wait for the
// process to exit and return its exit code. Does NOT close any handles —
// the caller must call closePipedProcess() afterwards. This split is
// intentional: it lets the caller null any external pointer to p.process
// (under a mutex) before the handle is closed, eliminating a window where
// another thread could call TerminateProcess() on a closed/recycled handle.
int readAllLinesAndWait(PipedProcess &p,
                        const std::function<void(const std::string&)> &onLine);

void closePipedProcess(PipedProcess &p);
