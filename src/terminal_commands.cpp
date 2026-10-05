// Shell-integration command records: the commands OSC 133 marks delimit on
// the primary screen, with their exit codes, kept on their text through
// scrolling, history eviction and reflow (Terminal::commands()).
#include "bropty/terminal.h"

#include "buffer_lines.h"

#include <algorithm>

namespace bropty {

namespace {

// The value of `key` among ';'-separated key=value params, if present.
std::optional<std::string_view> param(std::string_view params, std::string_view key) {
    while (!params.empty()) {
        const size_t semi = params.find(';');
        const std::string_view kv = params.substr(0, semi);
        params = semi == std::string_view::npos ? std::string_view() : params.substr(semi + 1);
        if (kv.size() > key.size() && kv.substr(0, key.size()) == key && kv[key.size()] == '=')
            return kv.substr(key.size() + 1);
    }
    return std::nullopt;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// %HH escapes (kitty's cmdline_url).
std::string percent_decode(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hex_digit(s[i + 1]) >= 0 && hex_digit(s[i + 2]) >= 0) {
            out.push_back(char(hex_digit(s[i + 1]) * 16 + hex_digit(s[i + 2])));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// VS Code's OSC 633 escaping: "\\" is a backslash, "\xHH" a byte.
std::string unescape_633(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '\\') {
            out.push_back('\\');
            ++i;
        } else if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] == 'x' && hex_digit(s[i + 2]) >= 0 &&
                   hex_digit(s[i + 3]) >= 0) {
            out.push_back(char(hex_digit(s[i + 2]) * 16 + hex_digit(s[i + 3])));
            i += 3;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// 'D;<code>': the first parameter as an integer, if it is one.
std::optional<int> exit_code(std::string_view params) {
    std::string_view f = params.substr(0, params.find(';'));
    bool neg = false;
    if (!f.empty() && f[0] == '-') {
        neg = true;
        f.remove_prefix(1);
    }
    if (f.empty() || f.size() > 9) return std::nullopt;
    int v = 0;
    for (char c : f) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + (c - '0');
    }
    return neg ? -v : v;
}

RowPos last_pos(const CommandRecord& r) {
    RowPos p = r.prompt;
    for (const std::optional<RowPos>* o : {&r.input, &r.output, &r.end})
        if (*o) p = std::max(p, **o);
    return p;
}

template <class F>
void for_each_pos(CommandRecord& r, F&& f) {
    f(r.prompt);
    for (std::optional<RowPos>* o : {&r.input, &r.output, &r.end})
        if (*o) f(**o);
}

} // namespace

void Terminal::command_mark(char kind, std::string_view params) {
    if (alt_screen_active()) return;  // shells mark the primary screen only
    const RowPos at = cursor_pos();
    CommandRecord* open = !commands_.empty() && !commands_.back().finished ? &commands_.back() : nullptr;
    switch (kind) {
    case 'A': {
        // kitty: k=s (secondary), k=c (continuation), k=r (right) prompts
        // belong to the command line being typed, not to a new command.
        if (const auto k = param(params, "k"); k && (*k == "s" || *k == "c" || *k == "r")) return;
        if (open && !open->output) {
            // The shell gave up on this prompt before running anything (an
            // empty Enter, Ctrl+C, a redraw): it moves to the new prompt.
            *open = CommandRecord{};
            open->prompt = at;
            break;
        }
        if (open) open->finished = true;  // no 'D': finished without an exit code
        if (commands_.size() >= kMaxCommands) commands_.erase(commands_.begin());
        CommandRecord r;
        r.prompt = at;
        commands_.push_back(std::move(r));
        break;
    }
    case 'B':
        if (!open) return;
        open->input = at;
        break;
    case 'C':
        if (!open) return;
        open->output = at;
        if (const auto c = param(params, "cmdline_url")) open->command_line = percent_decode(*c);
        else if (const auto c2 = param(params, "cmdline")) open->command_line.assign(*c2);
        break;
    case 'D':
        if (!open) return;
        open->end = at;
        open->exit_code = exit_code(params);
        open->finished = true;
        break;
    default: return;
    }
    ++commands_version_;
}

void Terminal::command_line_633(std::string_view rest) {
    if (rest.size() < 2 || rest[0] != 'E' || rest[1] != ';' || alt_screen_active()) return;
    if (commands_.empty() || commands_.back().finished) return;
    rest.remove_prefix(2);
    commands_.back().command_line = unescape_633(rest.substr(0, rest.find(';')));  // then an optional nonce
    ++commands_version_;
}

void Terminal::commands_trim() {
    const int64_t first = history_first_row();
    commands_first_row_ = first;
    if (commands_.empty()) return;
    bool changed = false;
    const auto gone = std::remove_if(commands_.begin(), commands_.end(), [&](const CommandRecord& r) {
        return r.finished && last_pos(r).row < first;
    });
    if (gone != commands_.end()) {
        commands_.erase(gone, commands_.end());
        changed = true;
    }
    for (CommandRecord& r : commands_) {
        for_each_pos(r, [&](RowPos& p) {
            if (p.row >= first) return;
            p = RowPos{first, 0};
            r.trimmed = true;
            changed = true;
        });
    }
    if (changed) ++commands_version_;
}

void Terminal::commands_screen_erased() {
    const int64_t top = screen_top_row();
    const auto gone = std::remove_if(commands_.begin(), commands_.end(),
                                     [&](const CommandRecord& r) { return r.finished && r.prompt.row >= top; });
    if (gone == commands_.end()) return;
    commands_.erase(gone, commands_.end());
    ++commands_version_;
}

// A resize reflows the primary screen and its history (also while the
// alternate screen shows): every position goes out as (logical line, cell
// offset) and comes back on the same character.
void Terminal::commands_resize_begin() {
    carried_commands_.clear();
    if (commands_.empty()) return;
    detail::BufferLines bl(*this, detail::PrimaryTag{});
    for (CommandRecord& r : commands_) {
        for_each_pos(r, [&](RowPos& p) {
            const detail::LinePos lp = bl.carry_out(p);
            carried_commands_.push_back(CarriedAnchor{lp.line, lp.offset, true});
        });
    }
}

void Terminal::commands_resize_end() {
    if (carried_commands_.empty()) return;
    detail::BufferLines bl(*this, detail::PrimaryTag{});
    size_t i = 0;
    for (CommandRecord& r : commands_) {
        for_each_pos(r, [&](RowPos& p) {
            if (i >= carried_commands_.size()) return;
            const CarriedAnchor& a = carried_commands_[i++];
            p = bl.carry_in(detail::LinePos{a.line, a.offset}, cols_);
            if (p.row >= end_row()) p = RowPos{end_row() - 1, 0};
        });
    }
    carried_commands_.clear();
    ++commands_version_;
    commands_trim();
}

} // namespace bropty
