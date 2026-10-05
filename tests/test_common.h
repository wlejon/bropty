#pragma once
// Process setup shared by every test binary (and pty_child): a test must fail,
// never stop on a modal error dialog waiting for a click. That covers a crash,
// a child that cannot start (0xC0000142, DLL initialisation failed), a Debug
// CRT assert and abort(). The error mode is inherited by child processes
// (bropty never spawns with CREATE_DEFAULT_ERROR_MODE), so programs started
// under a test -- through a ConPTY too -- inherit it as well.

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <crtdbg.h>
#include <cstdlib>

inline void init_test() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // Debug CRT reports (assert, _ASSERTE, heap checks) go to stderr.
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
}
#else
inline void init_test() {}
#endif

// Exit statuses that mean a Windows process crashed or never started
// (NTSTATUS errors: 0xC0000142 DLL initialisation failed, 0xC0000005 access
// violation, ...). 0xC000013A (closed by Ctrl+C / console close) is not one.
inline bool is_crash_status(long long code) {
    const auto u = static_cast<unsigned long>(static_cast<long>(code));
    return code != -1 && (u & 0xF0000000ul) == 0xC0000000ul && u != 0xC000013Aul;
}
