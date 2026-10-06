// IPtyProcess::foreground_process() on POSIX: the terminal's foreground
// process group, described from /proc (Linux) or libproc + sysctl (macOS).
#include "process_info.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/sysctl.h>
#else
#include <dirent.h>
#endif

namespace bropty::pty_detail {

namespace {

std::string base_name(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// argv[0] as a name: its base name, without a login shell's leading '-'.
std::string name_of_argv0(std::string a) {
    // A program that rewrote its argv into one string ("sshd: user@pts/0")
    // keeps it whole; only a path is cut to its base name.
    if (a.find(' ') == std::string::npos) a = base_name(a);
    if (!a.empty() && a[0] == '-') a.erase(0, 1);
    return a;
}

#if !defined(__APPLE__)

bool read_file(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

// The state letter and process group from /proc/<pid>/stat ("pid (comm) S
// ppid pgrp ..."; comm may hold spaces and parentheses, so parse after the
// last ')').
bool read_stat(long pid, char& state, long& pgrp) {
    std::string s;
    if (!read_file("/proc/" + std::to_string(pid) + "/stat", s)) return false;
    const size_t close = s.rfind(')');
    if (close == std::string::npos) return false;
    long ppid = 0;
    return std::sscanf(s.c_str() + close + 1, " %c %ld %ld", &state, &ppid, &pgrp) == 3;
}

#endif

} // namespace

bool describe_process(int64_t pid, ProcessInfo& out) {
    if (pid <= 0) return false;
    out = ProcessInfo{};
    out.pid = pid;
    std::vector<std::string> argv;
    std::string comm;
#if defined(__APPLE__)
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(int(pid), path, sizeof path) > 0) out.path = path;
    char name[2 * MAXCOMLEN + 1] = {};
    if (proc_name(int(pid), name, sizeof name) > 0) comm = name;
    // KERN_PROCARGS2: int argc, the exec path, NUL padding, then argv.
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, int(pid)};
    size_t size = 0;
    if (sysctl(mib, 3, nullptr, &size, nullptr, 0) == 0 && size > sizeof(int)) {
        std::vector<char> buf(size);
        if (sysctl(mib, 3, buf.data(), &size, nullptr, 0) == 0 && size > sizeof(int)) {
            int argc = 0;
            std::memcpy(&argc, buf.data(), sizeof argc);
            size_t i = sizeof argc;
            while (i < size && buf[i] != '\0') ++i;  // the exec path
            while (i < size && buf[i] == '\0') ++i;  // its padding
            while (i < size && int(argv.size()) < argc) {
                const size_t start = i;
                while (i < size && buf[i] != '\0') ++i;
                argv.emplace_back(buf.data() + start, i - start);
                ++i;
            }
        }
    }
    if (out.path.empty() && comm.empty() && argv.empty()) {
        // Nothing readable: gone, or not ours to read.
        if (kill(pid_t(pid), 0) != 0 && errno == ESRCH) return false;
    }
#else
    const std::string dir = "/proc/" + std::to_string(pid);
    char state = 0;
    long pgrp = 0;
    if (!read_stat(long(pid), state, pgrp) || state == 'Z') return false;
    char path[PATH_MAX];
    const ssize_t n = readlink((dir + "/exe").c_str(), path, sizeof path - 1);
    if (n > 0) {
        out.path.assign(path, size_t(n));
        // A replaced executable reads "/usr/bin/x (deleted)".
        static constexpr std::string_view kDeleted = " (deleted)";
        if (out.path.size() > kDeleted.size() &&
            out.path.compare(out.path.size() - kDeleted.size(), kDeleted.size(), kDeleted) == 0)
            out.path.resize(out.path.size() - kDeleted.size());
    }
    if (read_file(dir + "/comm", comm) && !comm.empty() && comm.back() == '\n') comm.pop_back();
    std::string raw;
    if (read_file(dir + "/cmdline", raw)) {
        size_t start = 0;
        for (size_t i = 0; i <= raw.size(); ++i) {
            if (i == raw.size() || raw[i] == '\0') {
                if (i > start || i < raw.size()) argv.emplace_back(raw, start, i - start);
                start = i + 1;
            }
        }
        // A program that shortened its argv in place leaves NUL padding.
        while (!argv.empty() && argv.back().empty()) argv.pop_back();
    }
#endif
    out.command_line = posix_command_line(argv);
    if (!argv.empty()) out.name = name_of_argv0(argv[0]);
    if (out.name.empty()) out.name = comm;
    if (out.name.empty()) out.name = base_name(out.path);
    return true;
}

std::optional<ProcessInfo> foreground_of_tty(int master_fd) {
    if (master_fd < 0) return std::nullopt;
    const pid_t pgid = tcgetpgrp(master_fd);
    if (pgid <= 0) return std::nullopt;
    ProcessInfo info;
    if (describe_process(pgid, info)) return info;
    // The leader has gone (the first stage of a pipeline that finished):
    // the group's lowest live pid speaks for it.
    long best = 0;
#if defined(__APPLE__)
    std::vector<pid_t> pids(256);
    for (;;) {
        const int bytes = proc_listpids(PROC_PGRP_ONLY, uint32_t(pgid), pids.data(), int(pids.size() * sizeof(pid_t)));
        if (bytes <= 0) return std::nullopt;
        if (size_t(bytes) < pids.size() * sizeof(pid_t)) {
            pids.resize(size_t(bytes) / sizeof(pid_t));
            break;
        }
        pids.resize(pids.size() * 2);
    }
    for (pid_t p : pids)
        if (p > 0 && (best == 0 || p < best)) best = p;
#else
    DIR* d = opendir("/proc");
    if (!d) return std::nullopt;
    while (dirent* e = readdir(d)) {
        char* end = nullptr;
        const long p = std::strtol(e->d_name, &end, 10);
        if (p <= 0 || *end != '\0' || (best != 0 && p >= best)) continue;
        char state = 0;
        long pgrp = 0;
        if (read_stat(p, state, pgrp) && pgrp == long(pgid) && state != 'Z') best = p;
    }
    closedir(d);
#endif
    if (best != 0 && describe_process(best, info)) return info;
    return std::nullopt;
}

} // namespace bropty::pty_detail
