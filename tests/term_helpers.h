#pragma once
// Shared fixtures for Terminal-level tests.

#include "bropty/terminal.h"
#include "check.h"

#include <string>
#include <vector>

namespace th {

struct Capture : bropty::TerminalHost {
    std::string out;  // replies sent to the application
    int bells = 0;
    std::string title, icon, cwd;
    std::vector<std::pair<std::string, std::string>> clipboard_writes;
    std::optional<std::string> clipboard_content;
    std::vector<std::string> marks;
    std::vector<std::string> notes;
    std::vector<std::string> apcs;
    int palette_changes = 0;

    void write_to_pty(std::string_view b) override { out.append(b); }
    void bell() override { ++bells; }
    void title_changed(std::string_view t) override { title = t; }
    void icon_name_changed(std::string_view n) override { icon = n; }
    void cwd_changed(std::string_view u) override { cwd = u; }
    void clipboard_write(std::string_view s, std::string_view d) override {
        clipboard_writes.emplace_back(std::string(s), std::string(d));
    }
    std::optional<std::string> clipboard_read(std::string_view) override { return clipboard_content; }
    void notification(std::string_view t, std::string_view b) override {
        notes.push_back(std::string(t) + "|" + std::string(b));
    }
    void semantic_mark(char k, std::string_view p) override { marks.push_back(std::string(1, k) + std::string(p)); }
    void palette_changed() override { ++palette_changes; }
    void apc(std::string_view p) override { apcs.emplace_back(p); }
};

inline bropty::TerminalOptions opts(int cols, int rows, size_t sb = 1000, bool clusters = true) {
    bropty::TerminalOptions o;
    o.cols = cols;
    o.rows = rows;
    o.scrollback_rows = sb;
    o.grapheme_clustering = clusters;
    return o;
}

struct T {
    Capture host;
    bropty::Terminal t;
    T(int cols, int rows, size_t sb = 1000, bool clusters = true) : t(opts(cols, rows, sb, clusters)) {
        t.set_host(&host);
    }
    explicit T(const bropty::TerminalOptions& o) : t(o) { t.set_host(&host); }
    T& operator<<(std::string_view s) {
        t.feed(s);
        return *this;
    }
    std::string row(int y) const { return t.row_text(y); }
    const bropty::Cell& cell(int y, int x) const { return t.row(y).cells[x]; }
    const bropty::Style& style(int y, int x) const { return t.style(cell(y, x).style); }
    std::u32string cluster(int y, int x) const { return t.row(y).cluster(x); }
    int crow() const { return t.cursor().row; }
    int ccol() const { return t.cursor().col; }
    std::string reply() {
        std::string r = host.out;
        host.out.clear();
        return r;
    }
    // History then screen; rows joined by '~' when soft-wrapped and '|'
    // otherwise; trailing empty unwrapped rows dropped.
    std::string all() const {
        std::string out;
        for (size_t i = 0; i < t.history_rows(); ++i) {
            bropty::RowView v = t.history_row(i);
            out += v.text();
            out += v.wrapped() ? "~" : "|";
        }
        for (int y = 0; y < t.rows(); ++y) {
            bropty::RowView v = t.row(y);
            out += v.text();
            out += v.wrapped() ? "~" : "|";
        }
        while (!out.empty() && out.back() == '|') out.pop_back();
        return out;
    }
};

inline std::string utf8(std::u32string_view s) {
    std::string out;
    for (char32_t c : s) bropty::append_utf8(out, c);
    return out;
}

} // namespace th
