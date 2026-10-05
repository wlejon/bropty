#include "bropty/pty.h"

#include <algorithm>

namespace bropty {

#if defined(_WIN32)
std::unique_ptr<IPtyProcess> create_pty_win();
#else
std::unique_ptr<IPtyProcess> create_pty_posix();
#endif

std::unique_ptr<IPtyProcess> create_pty() {
#if defined(_WIN32)
    return create_pty_win();
#else
    return create_pty_posix();
#endif
}

namespace pty_detail {

std::string quote_windows_arg(std::string_view arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string_view::npos) return std::string(arg);
    // Backslashes are literal except in front of a quote (or the closing
    // quote we add), where each must be doubled.
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
        } else {
            out.append(backslashes, '\\');
        }
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

std::string windows_command_line(std::string_view command, const std::vector<std::string>& args) {
    // argv[0] is parsed differently by the CRT: quotes delimit, backslashes are
    // never escapes (and file names cannot contain quotes).
    std::string line;
    if (command.empty() || command.find_first_of(" \t") != std::string_view::npos) {
        // Appended, not "\"" + std::string(...): GCC 12 warns -Wrestrict on that (PR 105651).
        line += '"';
        line += command;
        line += '"';
    } else {
        line = std::string(command);
    }
    for (const std::string& a : args) {
        line.push_back(' ');
        line += quote_windows_arg(a);
    }
    return line;
}

namespace {

std::string_view env_name(std::string_view entry) {
    // Windows keeps per-drive cwds as "=C:=C:\dir": the name may begin with '='.
    size_t eq = entry.find('=', entry.empty() || entry[0] != '=' ? 0 : 1);
    return eq == std::string_view::npos ? entry : entry.substr(0, eq);
}

char fold(char c, bool ci) { return (ci && c >= 'a' && c <= 'z') ? char(c - 32) : c; }

bool name_less(std::string_view a, std::string_view b, bool ci) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        unsigned char x = static_cast<unsigned char>(fold(a[i], ci));
        unsigned char y = static_cast<unsigned char>(fold(b[i], ci));
        if (x != y) return x < y;
    }
    return a.size() < b.size();
}

bool name_eq(std::string_view a, std::string_view b, bool ci) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (fold(a[i], ci) != fold(b[i], ci)) return false;
    return true;
}

} // namespace

std::vector<std::string> build_environment(const std::vector<std::string>& base, const PtyConfig& config,
                                           bool ci) {
    std::vector<std::string> env;
    if (config.inherit_env) env = base;
    auto set = [&](std::string_view name, std::string_view value, bool only_if_missing) {
        for (std::string& e : env) {
            if (name_eq(env_name(e), name, ci)) {
                if (!only_if_missing) e = std::string(name) + "=" + std::string(value);
                return;
            }
        }
        env.push_back(std::string(name) + "=" + std::string(value));
    };
    // The child talks to this terminal, not to whatever terminal (if any) the
    // host was started from: replace its identity and stale size variables.
    static constexpr std::string_view kParentTerminal[] = {
        "COLUMNS", "LINES", "TERM_PROGRAM", "TERM_PROGRAM_VERSION", "TERM_SESSION_ID", "WT_SESSION",
        "WT_PROFILE_ID", "KITTY_WINDOW_ID", "KITTY_PID", "VTE_VERSION", "WEZTERM_PANE", "ALACRITTY_WINDOW_ID",
    };
    for (std::string_view n : kParentTerminal) {
        env.erase(std::remove_if(env.begin(), env.end(), [&](const std::string& e) { return name_eq(env_name(e), n, ci); }),
                  env.end());
    }
    set("TERM", "xterm-256color", false);
    set("COLORTERM", "truecolor", false);
    for (const auto& [k, v] : config.env) {
        if (!k.empty()) set(k, v, false);
    }
    for (const std::string& u : config.env_unset) {
        env.erase(std::remove_if(env.begin(), env.end(), [&](const std::string& e) { return name_eq(env_name(e), u, ci); }),
                  env.end());
    }
    std::stable_sort(env.begin(), env.end(),
                     [&](const std::string& a, const std::string& b) { return name_less(env_name(a), env_name(b), ci); });
    return env;
}

} // namespace pty_detail

} // namespace bropty
