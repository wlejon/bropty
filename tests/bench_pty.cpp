// PTY transport throughput: how fast a child's output reaches a reader that
// does nothing with it, through bropty's PTY (its reader thread, ring and
// wakeups) and, on Windows, through a bare ConPTY driven with nothing but
// the Win32 calls, so the two can be compared on one machine. Prints MB/s of
// the child's own bytes (ConPTY re-renders, so the bytes read are not the
// bytes written) for several write patterns of the child:
//
//   line   one stdio write per ~64-byte line (stdout to a console is
//          unbuffered), what pty_child's `flood` does
//   4k     the same lines in 4 KiB writes
//   64k    in 64 KiB writes
//
//   bench_pty <pty_child> [megabytes = 16] [pattern...]
//
// Not a pass / fail test; it checks only that every run finished.
#include "bropty/pty.h"
#include "check.h"
#include "test_common.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace bropty;
using Clock = std::chrono::steady_clock;

namespace {

std::string g_child;

std::vector<std::string> child_args(long long bytes, const std::string& pattern) {
    std::vector<std::string> a = {"flood", std::to_string(bytes)};
    if (pattern == "4k") a.push_back("4096");
    else if (pattern == "64k") a.push_back("65536");
    return a;
}

// bropty's PTY, read as a host's parser thread would (woken, then draining
// what is buffered), the bytes discarded.
double run_bropty(long long bytes, const std::string& pattern, size_t& received) {
    std::shared_ptr<IPtyProcess> pty(create_pty());
    std::atomic<int> wakes{0};
    pty->set_wakeup([&] { wakes.fetch_add(1, std::memory_order_relaxed); });
    PtyConfig c;
    c.command = g_child;
    c.args = child_args(bytes, pattern);
    c.size = PtySize{120, 40, 0, 0};
    const auto t0 = Clock::now();
    if (!pty->spawn(c)) {
        std::printf("  spawn failed: %s\n", pty->last_error().c_str());
        return 0;
    }
    std::vector<char> buf(64 * 1024);
    received = 0;
    for (;;) {
        const size_t n = pty->read_timeout(buf.data(), buf.size(), std::chrono::milliseconds(100));
        received += n;
        if (n == 0 && pty->eof()) break;
        if (Clock::now() - t0 > std::chrono::seconds(300)) break;
    }
    const double s = std::chrono::duration<double>(Clock::now() - t0).count();
    pty->terminate();
    return s;
}

#if defined(_WIN32)
std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// The pseudo-console functions: the system's, or with BENCH_CONPTY_DLL set,
// those of a conpty.dll (Windows Terminal's redistributable, which runs its
// own OpenConsole.exe from beside the dll instead of the system conhost).
using CreatePc = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
using ClosePc = void(WINAPI*)(HPCON);
CreatePc g_create = &CreatePseudoConsole;
ClosePc g_close = &ClosePseudoConsole;

// A bare ConPTY: two pipes (`pipe_size` for the output one, 0 = the
// default), CreatePseudoConsole, the child, and one thread doing
// ReadFile(64 KiB) until EOF. Nothing else.
double run_raw_conpty(long long bytes, const std::string& pattern, DWORD pipe_size, size_t& received) {
    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr;
    CreatePipe(&in_r, &in_w, nullptr, 0);
    CreatePipe(&out_r, &out_w, nullptr, pipe_size);
    HPCON hpc = nullptr;
    if (FAILED(g_create(COORD{120, 40}, in_r, out_w, 0, &hpc))) {
        std::printf("  CreatePseudoConsole failed\n");
        return 0;
    }
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc, sizeof(HPCON), nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;  // the console's, not this process's stdio
    si.lpAttributeList = attrs;
    std::string line = "\"" + g_child + "\"";
    for (const std::string& a : child_args(bytes, pattern)) line += " " + a;
    std::wstring wline = widen(line);
    PROCESS_INFORMATION pi{};
    const auto t0 = Clock::now();
    if (!CreateProcessW(nullptr, wline.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT, nullptr,
                        nullptr, &si.StartupInfo, &pi)) {
        std::printf("  CreateProcess failed\n");
        return 0;
    }
    CloseHandle(in_r);
    CloseHandle(out_w);
    received = 0;
    std::thread reader([&] {
        std::vector<char> buf(64 * 1024);
        DWORD n = 0;
        while (ReadFile(out_r, buf.data(), DWORD(buf.size()), &n, nullptr) && n) received += n;
    });
    WaitForSingleObject(pi.hProcess, INFINITE);
    g_close(hpc);  // flushes, then ends the output pipe
    reader.join();
    const double s = std::chrono::duration<double>(Clock::now() - t0).count();
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(in_w);
    CloseHandle(out_r);
    DeleteProcThreadAttributeList(attrs);
    return s;
}
#endif

void report(const char* path, const std::string& pattern, long long bytes, double secs, size_t received) {
    const double mb = double(bytes) / (1024.0 * 1024.0);
    std::printf("  %-22s %-5s %8.1f MB/s  (%.2f s; %.1f MB read)\n", path, pattern.c_str(), secs > 0 ? mb / secs : 0.0,
                secs, double(received) / (1024.0 * 1024.0));
    CHECK(secs > 0 && received > 0);
}

} // namespace

int main(int argc, char** argv) {
    init_test();
    if (argc < 2) {
        std::printf("usage: bench_pty <pty_child> [megabytes] [line|4k|64k...]\n");
        return 2;
    }
    g_child = argv[1];
    const long long mb = argc > 2 ? std::atoll(argv[2]) : 16;
    std::vector<std::string> patterns;
    for (int i = 3; i < argc; ++i) patterns.push_back(argv[i]);
    if (patterns.empty()) patterns = {"line", "4k", "64k"};
    const long long bytes = mb * 1024 * 1024;
    std::printf("PTY throughput, %lld MB per run\n", mb);
#if defined(_WIN32)
    if (const char* dll = std::getenv("BENCH_CONPTY_DLL"); dll && *dll) {
        HMODULE m = LoadLibraryW(widen(dll).c_str());
        auto c = m ? reinterpret_cast<CreatePc>(reinterpret_cast<void*>(GetProcAddress(m, "CreatePseudoConsole"))) : nullptr;
        auto d = m ? reinterpret_cast<ClosePc>(reinterpret_cast<void*>(GetProcAddress(m, "ClosePseudoConsole"))) : nullptr;
        if (!c || !d) {
            std::printf("BENCH_CONPTY_DLL=%s: no CreatePseudoConsole there\n", dll);
            return 2;
        }
        g_create = c;
        g_close = d;
        std::printf("raw ConPTY runs use %s\n", dll);
    }
#endif
    for (const std::string& p : patterns) {
        size_t got = 0;
#if defined(_WIN32)
        double s = run_raw_conpty(bytes, p, 0, got);
        report("raw ConPTY", p, bytes, s, got);
        s = run_raw_conpty(bytes, p, 1 << 20, got);
        report("raw ConPTY, 1 MB pipe", p, bytes, s, got);
#endif
        const double b = run_bropty(bytes, p, got);
        report("bropty", p, bytes, b, got);
    }
    return check::finish("bench_pty");
}
