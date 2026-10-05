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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
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

int read_byte() {
    char c;
    DWORD n = 0;
    if (!ReadFile(in_handle(), &c, 1, &n, nullptr) || n == 0) return -1;
    return static_cast<unsigned char>(c);
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

int main(int argc, char** argv) {
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
