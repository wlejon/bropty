// pty_child's "test child" personality: a program for an APPLICATION's tests
// (a terminal app launching profiles, tabs, commands) to run, reporting
// exactly what it was started with. Chosen by the executable's name: a copy
// of pty_child whose file name contains "testchild" (helmterm-testchild,
// say) is this program, because its arguments are the thing under test and
// cannot carry a mode. What it does is chosen by environment variables:
//
//   TESTCHILD_REPORT=<path>  write {"argv":[...],"cwd":"...","env":{...}} as
//                            JSON to <path> (written to <path>.tmp and
//                            renamed, so a reader never sees half of it)
//   TESTCHILD_TITLE=<text>   set the window title (OSC 2)
//   TESTCHILD_BELL=1         ring the bell (BEL)
//   TESTCHILD_EXIT=<n>       exit with status n right after reporting
//
// Otherwise it prints "TESTCHILD READY argc=<n>" and ARG<i>=<arg> lines, then
// reads lines: one starting with "q" (or end of input) ends it with status 0,
// "x<n>" with status n, and any other line is echoed back as
// "TESTCHILD GOT <line>" with control characters shown as \xHH.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
extern char** environ;
#endif

namespace testchild {

#if defined(_WIN32)
inline std::string narrow(const wchar_t* w, int n = -1) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(len > 0 ? len : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), len, nullptr, nullptr);
    if (n < 0 && !s.empty()) s.pop_back();
    return s;
}
#endif

/// Whether argv[0] names this personality.
inline bool selected(const char* argv0) {
    if (!argv0) return false;
    const char* base = argv0;
    for (const char* p = argv0; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    return std::strstr(base, "testchild") != nullptr;
}

inline std::vector<std::string> arguments(int argc, char* argv[]) {
    std::vector<std::string> out;
#if defined(_WIN32)
    (void)argc; (void)argv;
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 1; w && i < n; ++i) out.push_back(narrow(w[i]));
    if (w) LocalFree(w);
#else
    for (int i = 1; i < argc; ++i) out.emplace_back(argv[i]);
#endif
    return out;
}

inline std::string cwd() {
#if defined(_WIN32)
    static wchar_t buf[32768];
    DWORD n = GetCurrentDirectoryW(32768, buf);
    return narrow(buf, int(n));
#else
    char buf[8192];
    return getcwd(buf, sizeof buf) ? std::string(buf) : std::string();
#endif
}

inline std::map<std::string, std::string> environment() {
    std::map<std::string, std::string> out;
#if defined(_WIN32)
    LPWCH block = GetEnvironmentStringsW();
    for (LPWCH p = block; p && *p; p += wcslen(p) + 1) {
        std::string kv = narrow(p);
        size_t eq = kv.find('=', 1);  // "=C:=C:\..." entries start with '='
        if (eq != std::string::npos && kv[0] != '=') out[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    if (block) FreeEnvironmentStringsW(block);
#else
    for (char** e = environ; e && *e; ++e) {
        std::string kv(*e);
        size_t eq = kv.find('=');
        if (eq != std::string::npos) out[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
#endif
    return out;
}

inline std::string var(const std::map<std::string, std::string>& all, const char* name) {
    auto it = all.find(name);
    return it == all.end() ? std::string() : it->second;
}

inline std::string json(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", c);
                out += b;
            } else {
                out += char(c);
            }
        }
    }
    return out + "\"";
}

inline bool writeReport(const std::string& path, const std::vector<std::string>& args, const std::string& dir,
                        const std::map<std::string, std::string>& vars) {
    std::string body = "{\"argv\":[";
    for (size_t i = 0; i < args.size(); ++i) body += (i ? "," : "") + json(args[i]);
    body += "],\"cwd\":" + json(dir) + ",\"env\":{";
    bool first = true;
    for (const auto& [k, v] : vars) {
        body += (first ? "" : ",") + json(k) + ":" + json(v);
        first = false;
    }
    body += "}}\n";
    const std::string tmp = path + ".tmp";
#if defined(_WIN32)
    int wl = MultiByteToWideChar(CP_UTF8, 0, tmp.c_str(), -1, nullptr, 0);
    std::wstring wtmp(size_t(wl > 0 ? wl : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, tmp.c_str(), -1, wtmp.data(), wl);
    int pl = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(size_t(pl > 0 ? pl : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), pl);
    FILE* f = _wfopen(wtmp.c_str(), L"wb");
#else
    FILE* f = std::fopen(tmp.c_str(), "wb");
#endif
    if (!f) return false;
    std::fwrite(body.data(), 1, body.size(), f);
    std::fclose(f);
#if defined(_WIN32)
    return MoveFileExW(wtmp.c_str(), wpath.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return std::rename(tmp.c_str(), path.c_str()) == 0;
#endif
}

inline int run(int argc, char* argv[]) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    // VT input: ConPTY then hands a terminal's replies (an OSC 4 colour, an
    // OSC 52 clipboard answer) through as characters instead of eating them.
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(in, &mode)) SetConsoleMode(in, mode | ENABLE_VIRTUAL_TERMINAL_INPUT);
#endif
    const std::vector<std::string> args = arguments(argc, argv);
    const std::string dir = cwd();
    const std::map<std::string, std::string> vars = environment();

    const std::string report = var(vars, "TESTCHILD_REPORT");
    if (!report.empty() && !writeReport(report, args, dir, vars))
        std::printf("TESTCHILD cannot write %s\r\n", report.c_str());

    const std::string title = var(vars, "TESTCHILD_TITLE");
    if (!title.empty()) std::printf("\x1b]2;%s\x07", title.c_str());
    if (var(vars, "TESTCHILD_BELL") == "1") std::printf("\x07");

    std::printf("TESTCHILD READY argc=%zu\r\n", args.size());
    for (size_t i = 0; i < args.size(); ++i) std::printf("ARG%zu=%s\r\n", i, args[i].c_str());
    std::fflush(stdout);

    const std::string exitWith = var(vars, "TESTCHILD_EXIT");
    if (!exitWith.empty()) return std::atoi(exitWith.c_str());

    char line[4096];
    while (std::fgets(line, sizeof line, stdin)) {
        if (line[0] == 'q') return 0;
        if (line[0] == 'x') return std::atoi(line + 1);
        // Control characters (an escape sequence a terminal sent as input,
        // an OSC 52 reply) are shown as \xHH, so echoing them back cannot
        // make the terminal act on them.
        std::string shown;
        for (const char* p = line; *p; ++p) {
            const auto c = static_cast<unsigned char>(*p);
            if (c == '\n' || c == '\r') continue;
            if (c < 0x20 || c == 0x7f) {
                char b[8];
                std::snprintf(b, sizeof b, "\\x%02x", c);
                shown += b;
            } else {
                shown.push_back(char(c));
            }
        }
        std::printf("TESTCHILD GOT %s\r\n", shown.c_str());
        std::fflush(stdout);
    }
    return 0;
}

}  // namespace testchild
