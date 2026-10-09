// A small program for the PTY tests to run as the child, so that behaviour
// under test (argument and environment passing, raw key input, signals that
// are ignored, output floods, window size) does not depend on which shells a
// machine has. Everything it reports goes to stdout, through the pty.
//
//   pty_child args <a>...      print each argument as [arg]
//   pty_child env <NAME>...    print NAME=value or NAME!unset
//   pty_child exit <n>         exit with status n
//   pty_child print <text>     write text with \e (ESC), \a (BEL), \n (CR LF)
//                              and \uXXXX (BMP code point, as UTF-8) expanded
//   pty_child stubborn         ignore SIGHUP/SIGTERM/SIGINT (Windows: block in
//                              the console control handler), print READY, sleep
//   pty_child flood <bytes> [chunk]
//                              write that many bytes of numbered lines (0 =
//                              forever), then FLOOD-DONE. Without `chunk`, one
//                              stdio write per line (stdout to a console is
//                              unbuffered); with it, the lines in blocks of
//                              `chunk` bytes, one OS write each (WriteFile on
//                              the console handle / write(1)), lines ending
//                              in CR LF as stdio's text mode writes them
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
//   pty_child nest <n>         run `pty_child nest <n-1>` and wait for it; at
//                              0 print NEST-READY pid=<pid> (POSIX: and
//                              pgid=<process group>), then read until 'q'
//   pty_child replies <reader> [query]
//                              (Windows) what a console program receives of
//                              the terminal's replies. Set the input mode,
//                              write `query` (escapes as for print), print
//                              READY, read stdin until a 'q' and print
//                              GOT <input> END, the input printable: ESC as
//                              <1b>, other controls and non-ASCII as <hh> /
//                              <hhhh>, a key record with no character as {vk}.
//                              reader: file / file-vt (ReadFile), consolew-vt
//                              (ReadConsoleW), records / records-vt
//                              (ReadConsoleInputW, key-down records); -vt sets
//                              ENABLE_VIRTUAL_TERMINAL_INPUT, otherwise the
//                              input mode is 0 (no line input, no echo)
#include "test_common.h"
#include "pty_child_reporter.h"

#include <algorithm>
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
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

void out(const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
}

// \e (ESC), \a (BEL), \n (CR LF) and \uXXXX (BMP code point, as UTF-8).
std::string expand(const char* p) {
    std::string s;
    for (; *p; ++p) {
        if (p[0] == '\\' && p[1] == 'e') {
            s.push_back('\x1b');
            ++p;
        } else if (p[0] == '\\' && p[1] == 'a') {
            s.push_back('\x07');
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
    return s;
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

void printable(std::string& o, unsigned u) {
    static const char* digits = "0123456789abcdef";
    if (u >= 0x20 && u < 0x7f && u != '<' && u != '{') {
        o.push_back(char(u));
        return;
    }
    o.push_back('<');
    for (int shift = u > 0xff ? 12 : 4; shift >= 0; shift -= 4) o.push_back(digits[(u >> shift) & 15]);
    o.push_back('>');
}

// See `replies` in the header comment.
int replies(const std::string& reader, const char* query) {
    HANDLE in = in_handle();
    const bool vt = reader.size() > 3 && reader.compare(reader.size() - 3, 3, "-vt") == 0;
    SetConsoleMode(in, vt ? ENABLE_VIRTUAL_TERMINAL_INPUT : 0);
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD m = 0;
    GetConsoleMode(o, &m);
    SetConsoleMode(o, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    out(expand(query));
    out("READY\n");
    std::string got;
    bool done = false;
    while (!done) {
        if (reader == "file" || reader == "file-vt") {
            char buf[256];
            DWORD n = 0;
            if (!ReadFile(in, buf, sizeof buf, &n, nullptr) || n == 0) break;
            for (DWORD i = 0; i < n && !done; ++i) {
                done = buf[i] == 'q';
                if (!done) printable(got, static_cast<unsigned char>(buf[i]));
            }
        } else if (reader == "consolew-vt") {
            wchar_t buf[256];
            DWORD n = 0;
            if (!ReadConsoleW(in, buf, 256, &n, nullptr) || n == 0) break;
            for (DWORD i = 0; i < n && !done; ++i) {
                done = buf[i] == L'q';
                if (!done) printable(got, unsigned(buf[i]));
            }
        } else if (reader == "records" || reader == "records-vt") {
            INPUT_RECORD recs[64];
            DWORD n = 0;
            if (!ReadConsoleInputW(in, recs, 64, &n) || n == 0) break;
            for (DWORD i = 0; i < n && !done; ++i) {
                if (recs[i].EventType != KEY_EVENT || !recs[i].Event.KeyEvent.bKeyDown) continue;
                const KEY_EVENT_RECORD& k = recs[i].Event.KeyEvent;
                for (WORD r = 0; r < std::max<WORD>(k.wRepeatCount, 1) && !done; ++r) {
                    done = k.uChar.UnicodeChar == L'q';
                    if (done) break;
                    if (k.uChar.UnicodeChar) printable(got, unsigned(k.uChar.UnicodeChar));
                    else got += "{" + std::to_string(k.wVirtualKeyCode) + "}";
                }
            }
        } else {
            out("BAD-READER\n");
            return 2;
        }
    }
    out("GOT " + got + " END\n");
    return 0;
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
    // A copy named *testchild*: the application-test personality, whose
    // arguments are reported, never interpreted (pty_child_reporter.h).
    if (testchild::selected(argc > 0 ? argv[0] : nullptr)) return testchild::run(argc, argv);
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
        out(expand(argc > 2 ? argv[2] : ""));
        return 0;
    }
#if defined(_WIN32)
    if (mode == "replies" && argc > 2) return replies(argv[2], argc > 3 ? argv[3] : "");
#endif
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
    if (mode == "flood" && argc > 3) {
        const long long limit = std::atoll(argv[2]);
        const size_t chunk = size_t(std::max(1LL, std::atoll(argv[3])));
        long long sent = 0;
        std::string buf;
        auto flush_buf = [&](size_t upto) {
            size_t off = 0;
            while (off < upto) {
                const size_t n = std::min(chunk, upto - off);
#if defined(_WIN32)
                DWORD w = 0;
                if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf.data() + off, DWORD(n), &w, nullptr) || !w) return;
#else
                const ssize_t w = ::write(1, buf.data() + off, n);
                if (w <= 0) return;
#endif
                off += size_t(w);
            }
            buf.erase(0, upto);
        };
        for (long long n = 0; limit == 0 || sent < limit; ++n) {
            // The bytes the line-at-a-time flood sends through stdio (CR LF
            // on Windows, where text mode adds the CR).
#if defined(_WIN32)
            const char* eol = "\r\n";
#else
            const char* eol = "\n";
#endif
            const std::string line =
                "flood line " + std::to_string(n) + " ................................................" + eol;
            buf += line;
            sent += static_cast<long long>(line.size());
            if (buf.size() >= chunk) flush_buf(buf.size() - buf.size() % chunk);
        }
        flush_buf(buf.size());
        out("FLOOD-DONE\n");
        return 0;
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
    if (mode == "nest") {
        // A chain of n more of these, each waiting for the next, the last of
        // which reports itself and waits for a 'q' on the terminal.
        const int n = argc > 2 ? std::atoi(argv[2]) : 0;
        if (n <= 0) {
#if defined(_WIN32)
            out("NEST-READY pid=" + std::to_string(GetCurrentProcessId()) + "\n");
#else
            out("NEST-READY pid=" + std::to_string(getpid()) + " pgid=" + std::to_string(getpgrp()) + "\n");
#endif
            for (int c; (c = read_byte()) >= 0 && c != 'q';) {
            }
            return 0;
        }
        const std::string next = std::to_string(n - 1);
#if defined(_WIN32)
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring line = L"\"" + std::wstring(self) + L"\" nest " + std::wstring(next.begin(), next.end());
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return 3;
        CloseHandle(pi.hThread);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
#else
        const pid_t p = fork();
        if (p == 0) {
            execl(argv[0], argv[0], "nest", next.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        int st = 0;
        waitpid(p, &st, 0);
#endif
        return 0;
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
