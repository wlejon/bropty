// IPtyProcess::foreground_process() on Windows: the child's process tree,
// walked down through the youngest live console program at each level (see
// the comment on foreground_process in bropty/pty.h for why the tree).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

#include "process_info.h"

#include <cwctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bropty::pty_detail {

namespace {

struct Handle {
    HANDLE h = nullptr;
    explicit Handle(HANDLE v) : h(v == INVALID_HANDLE_VALUE ? nullptr : v) {}
    ~Handle() {
        if (h) CloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

std::string narrow(const wchar_t* s, size_t n) {
    if (n == 0) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, s, int(n), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, int(n), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = wchar_t(std::towlower(c));
    return s;
}

uint64_t ticks(const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; }

std::wstring image_path(HANDLE process) {
    std::wstring path(32768, L'\0');
    DWORD n = DWORD(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &n)) return {};
    path.resize(n);
    return path;
}

// The command line from the process's own parameters
// (ProcessCommandLineInformation, Windows 8.1 and later, needing only
// PROCESS_QUERY_LIMITED_INFORMATION).
std::string command_line_of(HANDLE process) {
    using NtQip = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const NtQip query = [] {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        return nt ? reinterpret_cast<NtQip>(reinterpret_cast<void*>(GetProcAddress(nt, "NtQueryInformationProcess")))
                  : nullptr;
    }();
    if (!query) return {};
    constexpr ULONG kProcessCommandLineInformation = 60;
    struct UnicodeString {
        USHORT Length;
        USHORT MaximumLength;
        PWSTR Buffer;
    };
    std::vector<unsigned char> buf(4096);
    for (int attempt = 0; attempt < 4; ++attempt) {
        ULONG need = 0;
        const LONG st = query(process, kProcessCommandLineInformation, buf.data(), ULONG(buf.size()), &need);
        if (st >= 0) {
            const auto* us = reinterpret_cast<const UnicodeString*>(buf.data());
            return narrow(us->Buffer, us->Length / sizeof(wchar_t));
        }
        if (need <= buf.size()) return {};
        buf.resize(need);
    }
    return {};
}

// Whether the executable at `path` is a console program (its PE header's
// subsystem), cached: the answer for a path does not change while it runs.
bool is_console_program(const std::wstring& path) {
    static std::mutex mu;
    static std::unordered_map<std::wstring, bool> cache;
    const std::wstring key = lower(path);
    {
        std::lock_guard<std::mutex> g(mu);
        if (auto it = cache.find(key); it != cache.end()) return it->second;
    }
    bool console = true;  // unreadable: assume it may hold the console
    Handle f(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, 0, nullptr));
    if (f.h) {
        IMAGE_DOS_HEADER dos{};
        DWORD got = 0;
        if (ReadFile(f.h, &dos, sizeof dos, &got, nullptr) && got == sizeof dos && dos.e_magic == IMAGE_DOS_SIGNATURE) {
            // The optional header follows the signature and the file header;
            // Subsystem sits at the same offset in PE32 and PE32+.
            const LONG at = dos.e_lfanew + LONG(sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER)) +
                            LONG(offsetof(IMAGE_OPTIONAL_HEADER32, Subsystem));
            WORD subsystem = 0;
            LARGE_INTEGER pos;
            pos.QuadPart = at;
            if (SetFilePointerEx(f.h, pos, nullptr, FILE_BEGIN) && ReadFile(f.h, &subsystem, sizeof subsystem, &got, nullptr) &&
                got == sizeof subsystem)
                console = subsystem != IMAGE_SUBSYSTEM_WINDOWS_GUI;
        }
    }
    std::lock_guard<std::mutex> g(mu);
    if (cache.size() > 4096) cache.clear();
    cache.emplace(key, console);
    return console;
}

} // namespace

bool describe_process(int64_t pid, ProcessInfo& out) {
    if (pid <= 0) return false;
    Handle p(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid)));
    if (!p.h) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(p.h, &code) || code != STILL_ACTIVE) return false;
    out = ProcessInfo{};
    out.pid = pid;
    const std::wstring path = image_path(p.h);
    out.path = narrow(path.data(), path.size());
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring base = slash == std::wstring::npos ? path : path.substr(slash + 1);
    out.name = narrow(base.data(), base.size());
    out.command_line = command_line_of(p.h);
    return true;
}

std::optional<ProcessInfo> foreground_of_tree(int64_t root_pid, uint64_t root_created) {
    if (root_pid <= 0) return std::nullopt;
    struct Entry {
        DWORD pid, parent;
        std::wstring exe;
    };
    std::vector<Entry> all;
    {
        Handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snap.h) return std::nullopt;
        PROCESSENTRY32W e{};
        e.dwSize = sizeof e;
        for (BOOL ok = Process32FirstW(snap.h, &e); ok; ok = Process32NextW(snap.h, &e))
            all.push_back({e.th32ProcessID, e.th32ParentProcessID, lower(e.szExeFile)});
    }
    DWORD current = DWORD(root_pid);
    uint64_t current_created = root_created;
    for (int depth = 0; depth < 64; ++depth) {
        DWORD best = 0;
        uint64_t best_created = 0;
        for (const Entry& e : all) {
            if (e.parent != current || e.pid == current) continue;
            // Console hosts serve the console; they are not the user's.
            if (e.exe == L"conhost.exe" || e.exe == L"openconsole.exe") continue;
            Handle p(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.pid));
            if (!p.h) continue;
            FILETIME created, exited, kernel, user;
            DWORD code = 0;
            if (!GetProcessTimes(p.h, &created, &exited, &kernel, &user) || !GetExitCodeProcess(p.h, &code) ||
                code != STILL_ACTIVE)
                continue;
            const uint64_t c = ticks(created);
            if (c < current_created) continue;  // a recycled parent pid
            if (best != 0 && c <= best_created) continue;
            const std::wstring path = image_path(p.h);
            if (!path.empty() && !is_console_program(path)) continue;
            best = e.pid;
            best_created = c;
        }
        if (best == 0) break;
        current = best;
        current_created = best_created;
    }
    ProcessInfo info;
    if (describe_process(current, info)) return info;
    return std::nullopt;
}

} // namespace bropty::pty_detail
