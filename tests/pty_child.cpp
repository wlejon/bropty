// A small program for the PTY tests to run as the child, so that behaviour
// under test (argument and environment passing, raw key input, signals that
// are ignored, output floods, window size) does not depend on which shells a
// machine has. Everything it reports goes to stdout, through the pty.
//
//   pty_child args <a>...      print each argument as [arg]
//   pty_child env <NAME>...    print NAME=value or NAME!unset
//   pty_child exit <n>         exit with status n
//   pty_child print <text>     write text with \e (ESC), \n (CR LF) and \uXXXX
//                              (BMP code point, as UTF-8) expanded
//   pty_child stubborn         ignore SIGHUP/SIGTERM/SIGINT (Windows: block in
//                              the console control handler), print READY, sleep
//   pty_child flood <bytes>    write that many bytes of numbered lines (0 =
//                              forever), then FLOOD-DONE
//   pty_child keys <n>         raw mode, print READY, read n bytes, print them
//                              as hex (KEYS:1b5b41...), exit
//   pty_child size             print SIZE <rows>x<cols> now and after every
//                              input byte, until 'q'
//   pty_child tty              (POSIX) print TTY lead=<0|1> fg=<0|1>
//                              devtty=<0|1>: session leader, the foreground
//                              process group of its controlling terminal,
//                              /dev/tty opens; then VIA-DEVTTY written to it
//   pty_child cooked           (POSIX) print READY, then block reading stdin
//                              in the default (cooked, ISIG) mode
//   pty_child count <n>        raw mode, print READY, read n bytes, print
//                              COUNT <n> (a sink for large pastes)
//   pty_child pixels           (POSIX) print PIXELS <w>x<h> (TIOCGWINSZ pixel
//                              size) now and after every input byte, until 'q'
//   pty_child sleep            sleep forever, silently
//   pty_child grandchild <how> start `pty_child sleep` detached from the
//                              console / session and print GRANDCHILD <pid>,
//                              then sleep. how = detached (Windows:
//                              DETACHED_PROCESS; POSIX: setsid), breakaway
//                              (Windows: plus CREATE_BREAKAWAY_FROM_JOB)
#include "test_common.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

void out(const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
}

#if defined(_WIN32)
HANDLE in_handle() { return GetStdHandle(STD_INPUT_HANDLE); }

void raw_mode() {
    HANDLE in = in_handle();
    SetConsoleMode(in, ENABLE_VIRTUAL_TERMINAL_INPUT);
    SetConsoleCP(CP_UTF8);  // byte reads are otherwise in the OEM code page
    SetConsoleOutputCP(CP_UTF8);
    HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD m = 0;
    GetConsoleMode(o, &m);
    SetConsoleMode(o, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

// The input as UTF-8 bytes, read as UTF-16 through ReadConsoleW. Byte reads
// (ReadFile) under input code page 65001 are broken in older console hosts:
// Windows Server 2022's conhost returns NUL for every non-ASCII character. The
// UTF-16 read is how console programs get Unicode input on every version, and
// ConPTY delivers what bropty wrote either way.
int read_byte() {
    static std::string pending;
    static size_t at = 0;
    static wchar_t carry = 0;  // a high surrogate whose pair is still to come
    while (at >= pending.size()) {
        wchar_t w[65];
        DWORD n = 0;
        size_t have = 0;
        if (carry) w[have++] = carry;
        carry = 0;
        if (!ReadConsoleW(in_handle(), w + have, DWORD(64), &n, nullptr) || n == 0) {
            DWORD b = 0;
            char c;  // not a console (redirected): plain bytes
            if (have || !ReadFile(in_handle(), &c, 1, &b, nullptr) || b == 0) return -1;
            return static_cast<unsigned char>(c);
        }
        have += n;
        if (w[have - 1] >= 0xD800 && w[have - 1] <= 0xDBFF) carry = w[--have];
        pending.assign(size_t(WideCharToMultiByte(CP_UTF8, 0, w, int(have), nullptr, 0, nullptr, nullptr)), '\0');
        if (!pending.empty())
            WideCharToMultiByte(CP_UTF8, 0, w, int(have), pending.data(), int(pending.size()), nullptr, nullptr);
        at = 0;
    }
    return static_cast<unsigned char>(pending[at++]);
}

BOOL WINAPI stubborn_handler(DWORD) {
    Sleep(INFINITE);
    return TRUE;
}

std::string size_text() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info);
    int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    int cols = info.srWindow.Right - info.srWindow.Left + 1;
    return "SIZE " + std::to_string(rows) + "x" + std::to_string(cols);
}
#else
void raw_mode() {
    struct termios t {};
    tcgetattr(0, &t);
    cfmakeraw(&t);
    t.c_oflag |= OPOST | ONLCR;  // keep "\n" -> "\r\n" for readable output
    tcsetattr(0, TCSANOW, &t);
}

int read_byte() {
    unsigned char c;
    ssize_t n = ::read(0, &c, 1);
    return n == 1 ? c : -1;
}

std::string size_text() {
    struct winsize ws {};
    ioctl(1, TIOCGWINSZ, &ws);
    return "SIZE " + std::to_string(ws.ws_row) + "x" + std::to_string(ws.ws_col);
}
#endif

} // namespace

#if defined(_WIN32)
static UINT g_inherited_error_mode = 0;
#endif

int main(int argc, char** argv) {
#if defined(_WIN32)
    g_inherited_error_mode = GetErrorMode();  // what the parent handed down, before init_test()
#endif
    init_test();  // its own children (grandchild mode) inherit the error mode too
    if (argc < 2) return 2;
    std::string mode = argv[1];
    if (mode == "args") {
        for (int i = 2; i < argc; ++i) out(std::string("[") + argv[i] + "]\n");
        return 0;
    }
    if (mode == "env") {
        for (int i = 2; i < argc; ++i) {
            const char* v = std::getenv(argv[i]);
            out(std::string(argv[i]) + (v ? std::string("=") + v : std::string("!unset")) + "\n");
        }
        return 0;
    }
    if (mode == "exit") return argc > 2 ? std::atoi(argv[2]) : 0;
#if defined(_WIN32)
    // The error mode this process inherited.
    if (mode == "errormode") {
        out("ERRORMODE " + std::to_string(g_inherited_error_mode) + "\n");
        return 0;
    }
    if (mode == "crash") {
        // Must end the process with an NTSTATUS, not a "has stopped working" box.
        volatile int* p = nullptr;
        *p = 1;
        return 0;
    }
#endif
    if (mode == "print") {
#if defined(_WIN32)
        HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD m = 0;
        GetConsoleMode(o, &m);
        SetConsoleMode(o, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        SetConsoleOutputCP(CP_UTF8);
#endif
        std::string s;
        for (const char* p = argc > 2 ? argv[2] : ""; *p; ++p) {
            if (p[0] == '\\' && p[1] == 'e') {
                s.push_back('\x1b');
                ++p;
            } else if (p[0] == '\\' && p[1] == 'n') {
                s += "\r\n";
                ++p;
            } else if (p[0] == '\\' && p[1] == 'u' && std::strlen(p) >= 6) {
                unsigned cp = unsigned(std::strtoul(std::string(p + 2, 4).c_str(), nullptr, 16));
                s.push_back(char(0xE0 | (cp >> 12)));
                s.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
                s.push_back(char(0x80 | (cp & 0x3F)));
                p += 5;
            } else {
                s.push_back(*p);
            }
        }
        out(s);
        return 0;
    }
    if (mode == "stubborn") {
#if defined(_WIN32)
        SetConsoleCtrlHandler(stubborn_handler, TRUE);
#else
        std::signal(SIGHUP, SIG_IGN);
        std::signal(SIGTERM, SIG_IGN);
        std::signal(SIGINT, SIG_IGN);
#endif
        out("READY\n");
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (mode == "flood") {
        long long limit = argc > 2 ? std::atoll(argv[2]) : 0;
        long long sent = 0;
        std::string line;
        for (long long n = 0; limit == 0 || sent < limit; ++n) {
            line = "flood line " + std::to_string(n) + " ................................................\n";
            std::fwrite(line.data(), 1, line.size(), stdout);
            sent += static_cast<long long>(line.size());
        }
        out("FLOOD-DONE\n");
        return 0;
    }
    if (mode == "keys") {
        int want = argc > 2 ? std::atoi(argv[2]) : 1;
        raw_mode();
        out("READY\n");
        std::string hex = "KEYS:";
        static const char* digits = "0123456789abcdef";
        for (int i = 0; i < want; ++i) {
            int b = read_byte();
            if (b < 0) break;
            hex.push_back(digits[b >> 4]);
            hex.push_back(digits[b & 15]);
        }
        out(hex + "\n");
        return 0;
    }
#if !defined(_WIN32)
    if (mode == "tty") {
        const bool lead = getsid(0) == getpid();
        const bool fg = tcgetpgrp(0) == getpgrp();
        // Darwin's /dev/tty is its own device node (fstat does not resolve to
        // the slave), so prove it is the pty by writing through it.
        int fd = ::open("/dev/tty", O_RDWR | O_NOCTTY);
        out(std::string("TTY lead=") + (lead ? "1" : "0") + " fg=" + (fg ? "1" : "0") +
            " devtty=" + (fd >= 0 ? "1" : "0") + "\n");
        if (fd >= 0) {
            const char msg[] = "VIA-DEVTTY\n";
            ssize_t n = ::write(fd, msg, sizeof msg - 1);
            (void)n;
            ::close(fd);
        }
        return 0;
    }
    if (mode == "cooked") {
        out("READY\n");
        for (;;)
            if (read_byte() < 0) return 0;
    }
#endif
    if (mode == "count") {
        long long want = argc > 2 ? std::atoll(argv[2]) : 1;
        raw_mode();
        out("READY\n");
        long long got = 0;
        static char buf[64 * 1024];
        while (got < want) {
            size_t ask = size_t(std::min<long long>(want - got, sizeof buf));
#if defined(_WIN32)
            DWORD n = 0;
            if (!ReadFile(in_handle(), buf, DWORD(ask), &n, nullptr) || n == 0) break;
#else
            ssize_t n = ::read(0, buf, ask);
            if (n <= 0) break;
#endif
            got += static_cast<long long>(n);
        }
        out("COUNT " + std::to_string(got) + "\n");
        return 0;
    }
    if (mode == "sleep") {
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (mode == "grandchild") {
        std::string how = argc > 2 ? argv[2] : "detached";
#if defined(_WIN32)
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring line = L"\"" + std::wstring(self) + L"\" sleep";
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        DWORD flags = DETACHED_PROCESS;
        if (how == "breakaway") flags |= CREATE_BREAKAWAY_FROM_JOB;
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi)) {
            out("GRANDCHILD-FAILED " + std::to_string(GetLastError()) + "\n");
            return 3;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        out("GRANDCHILD " + std::to_string(pi.dwProcessId) + "\n");
#else
        pid_t p = fork();
        if (p == 0) {
            setsid();
            execl(argv[0], argv[0], "sleep", static_cast<char*>(nullptr));
            _exit(127);
        }
        out("GRANDCHILD " + std::to_string(p) + "\n");
#endif
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
#if !defined(_WIN32)
    if (mode == "pixels") {
        raw_mode();
        auto px = [] {
            struct winsize ws {};
            ioctl(1, TIOCGWINSZ, &ws);
            return "PIXELS " + std::to_string(ws.ws_xpixel) + "x" + std::to_string(ws.ws_ypixel) + "\n";
        };
        out(px());
        for (;;) {
            int b = read_byte();
            if (b < 0 || b == 'q') return 0;
            out(px());
        }
    }
#endif
    if (mode == "size") {
        raw_mode();
        out(size_text() + "\n");
        for (;;) {
            int b = read_byte();
            if (b < 0 || b == 'q') return 0;
            out(size_text() + "\n");
        }
    }
    return 2;
}
