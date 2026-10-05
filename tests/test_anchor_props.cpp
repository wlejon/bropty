// Randomized property tests: anchors (selection, search matches, viewport,
// cached frame rows) against recomputation from scratch, over random
// sequences of output, overwrites, erases, region scrolls, line edits,
// resizes (reflow), history clears and viewport scrolls.
//
// After every step:
//  * A selection that is still active reads exactly the marker it was made
//    on. After a step that cannot have touched the marker (output appended at
//    the bottom, a resize, a viewport scroll), if a fresh search still finds
//    the marker the selection must be active and lie exactly on that match.
//  * Once the incremental search has caught up, its matches equal a search
//    started from scratch on the current buffer.
//  * A scrolled-back viewport still shows the same logical line at its top
//    after benign steps (unless that line was evicted).
//  * Every row of a frame equals the terminal row it says it shows (the
//    copy-on-write row cache never serves stale content).
//   test_anchor_props [iterations-per-seed]
#include "bropty/view.h"
#include "term_helpers.h"

#include <cstdlib>

using namespace bropty;

namespace {

struct Rng {
    uint64_t s;
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return uint32_t(s >> 11);
    }
    int below(int n) { return int(next() % uint32_t(n)); }
    bool chance(int pct) { return below(100) < pct; }
};

std::vector<RowRange> all(const Search& s) {
    std::vector<RowRange> v;
    for (size_t i = 0; i < s.size(); ++i) v.push_back(s.at(i));
    return v;
}

std::vector<RowRange> scratch(const Terminal& t, std::shared_ptr<SearchMatcher> m) {
    Search s(t);
    s.start(std::move(m));
    while (s.step()) {}
    return all(s);
}

std::string text_of(const Terminal& t, RowRange r) {
    Selection sel(t);
    sel.select_range(r);
    return sel.text();
}

std::string line_text_at(const Terminal& t, int64_t row) {
    Selection sel(t);
    sel.start(RowPos{row, 0}, SelectionMode::Line);
    return sel.text();
}

// (r, cols) on a soft-wrapped row and (r + 1, 0) are the same boundary.
RowPos norm(const Terminal& t, RowPos p) {
    if (p.col >= t.cols()) {
        RowView v = t.row_at(p.row);
        if (v.cells && v.wrapped()) return RowPos{p.row + 1, 0};
    }
    return p;
}

const char* kWords[] = {"alpha", "beta", "needle", "NeEdLe", "x", "gamma-delta", "\xe4\xb8\xad\xe6\x96\x87",
                        "e\xcc\x81", "\xf0\x9f\x98\x80", "zz", "needleneedle", "/path/to"};

std::string random_text(Rng& r, int max_len) {
    std::string s;
    int n = 1 + r.below(max_len);
    for (int i = 0; i < n; ++i) {
        if (i) s += r.chance(80) ? " " : "";
        if (r.chance(10)) s += "\x1b[3" + std::to_string(r.below(8)) + "m";
        s += kWords[r.below(int(sizeof kWords / sizeof *kWords))];
        if (r.chance(10)) s += "\x1b[0m";
    }
    return s;
}

struct Run {
    Rng rng;
    th::T t;
    TerminalView view;
    std::shared_ptr<LiteralMatcher> needle = std::make_shared<LiteralMatcher>("needle");
    std::string marker;  // the text the selection was made on
    int next_marker = 0;
    std::string top_line;  // logical line at the top of a scrolled-back view
    int checks_strict = 0;

    Run(uint64_t seed, int cols, int rows, size_t sb)
        : rng{seed}, t(cols, rows, sb), view(t.t) {
        view.search().start(needle);
    }

    void feed(const std::string& s) {
        t << s;
        view.sync();
    }

    void append_line(int max_words) { feed("\x1b[999;1H\r\n" + random_text(rng, max_words)); }

    void new_marker() {
        marker = "Q" + std::to_string(next_marker++) + "Q";
        feed("\x1b[999;1H\r\nbefore " + marker + " after");
        auto m = scratch(t.t, std::make_shared<LiteralMatcher>(marker, true));
        CHECK_EQ(m.size(), size_t(1));
        if (m.size() == 1) view.selection().select_range(m[0]);
    }

    // One random step; returns whether it was benign (cannot touch existing text).
    int last_op = -1;
    int iter = 0;
    void dump() {
        std::printf("  state: %dx%d first %lld screen %lld cursor %d,%d\n", t.t.cols(), t.t.rows(),
                    (long long)t.t.first_row(), (long long)t.t.screen_top_row(), t.t.cursor().row, t.t.cursor().col);
        for (int64_t r = std::max(t.t.first_row(), t.t.screen_top_row() - 8); r < t.t.end_row(); ++r) {
            RowView v = t.t.row_at(r);
            std::printf("  %lld%s |%s|%s\n", (long long)r, r >= t.t.screen_top_row() ? "*" : " ", v.text(false).c_str(),
                        v.wrapped() ? "~" : "");
        }
        const RowRange s = view.selection().range();
        std::printf("  selection %d: %lld,%d .. %lld,%d\n", int(view.selection().active()), (long long)s.start.row,
                    s.start.col, (long long)s.end.row, s.end.col);
    }
    bool step() {
        const int op = rng.below(100);
        last_op = op;
        const int cols = t.t.cols(), rows = t.t.rows();
        if (op < 40) {
            append_line(rng.chance(20) ? 40 : 6);
            return true;
        }
        // Random draws are sequenced one per statement (compilers evaluate
        // operands in different orders; a seed must replay the same run).
        auto cup = [&] {
            const int y = 1 + rng.below(rows);
            const int x = 1 + rng.below(cols);
            return "\x1b[" + std::to_string(y) + ";" + std::to_string(x) + "H";
        };
        if (op < 46) {
            const std::string at = cup();
            feed(at + random_text(rng, 3));
            return false;
        }
        if (op < 50) {
            const char* e[] = {"\x1b[K", "\x1b[1K", "\x1b[2K", "\x1b[J", "\x1b[1J", "\x1b[2J"};
            const std::string at = cup();
            feed(at + e[rng.below(6)]);
            return false;
        }
        if (op < 53) {
            const int top = 1 + rng.below(rows);
            const int bot = top + rng.below(rows - top + 1);
            const std::string text = random_text(rng, 2);
            feed("\x1b[" + std::to_string(top) + ";" + std::to_string(bot) + "r\x1b[" + std::to_string(bot) + ";1H\n" +
                 text + "\x1b[r");
            return false;
        }
        if (op < 56) {
            const char* e[] = {"@", "P", "L", "M", "X"};
            const std::string at = cup();
            const int n = 1 + rng.below(3);
            feed(at + "\x1b[" + std::to_string(n) + e[rng.below(5)]);
            return false;
        }
        if (op < 66) {
            const int c = 4 + rng.below(40);
            const int r = 2 + rng.below(12);
            t.t.resize(c, r);
            view.sync();
            return true;
        }
        if (op < 76) {
            view.scroll_by(rng.below(21) - 10);
            top_line = view.at_bottom() ? std::string() : line_text_at(t.t, view.top_row());
            return true;
        }
        if (op < 78) {
            feed("\x1b[3J");
            return false;
        }
        if (op < 84) {
            new_marker();
            return true;
        }
        if (op < 86) {
            view.search().start(needle);
            return true;
        }
        view.search().step(std::chrono::microseconds(rng.below(3) == 0 ? 0 : 200));
        return true;
    }

    void check_frame() {
        auto f = view.snapshot();
        CHECK_EQ(f->rows, t.t.rows());
        for (int y = 0; y < f->rows; ++y) {
            const RowView want = t.t.row_at(f->top_row + y);
            const RowView got = f->lines[size_t(y)]->view();
            bool same = want.cols == got.cols && want.flags == got.flags;
            for (int x = 0; same && x < want.cols; ++x) {
                same = (want.cells[x].bits & (Cell::kCpMask | (3u << 21) | Cell::kClusterBit)) ==
                           (got.cells[x].bits & (Cell::kCpMask | (3u << 21) | Cell::kClusterBit)) &&
                       want.style(x) == got.style(x);
            }
            if (same && want.text(false) != got.text(false)) same = false;
            CHECK(same);
            if (!same) {
                std::printf("  iteration %d after op %d: %dx%d, frame row %d (abs %lld, screen %lld)\n"
                            "    want |%s| flags %u\n    got  |%s| flags %u\n",
                            iter, last_op, t.t.cols(), t.t.rows(), y, (long long)(f->top_row + y),
                            (long long)t.t.screen_top_row(), want.text(false).c_str(), want.flags,
                            got.text(false).c_str(), got.flags);
                return;
            }
        }
    }

    void check(bool benign) {
        Selection& sel = view.selection();
        if (sel.active() && !marker.empty()) {
            const std::string got = sel.text();
            // History evicted or cleared under the start of the selection
            // trims it to what is left (as alacritty does): a suffix remains.
            const bool trimmed = sel.range().start == RowPos{t.t.first_row(), 0} && got.size() < marker.size() &&
                                 marker.compare(marker.size() - got.size(), got.size(), got) == 0;
            if (trimmed) {
                sel.clear();
            } else if (got != marker) {
                CHECK_EQ(got, marker);
                std::printf("  iteration %d after op %d: %dx%d, selection rows %lld..%lld cols %d..%d, first %lld screen %lld\n",
                            iter, last_op, t.t.cols(), t.t.rows(), (long long)sel.range().start.row,
                            (long long)sel.range().end.row, sel.range().start.col, sel.range().end.col,
                            (long long)t.t.first_row(), (long long)t.t.screen_top_row());
                sel.clear();
            }
        }
        if (benign && !marker.empty()) {
            auto m = scratch(t.t, std::make_shared<LiteralMatcher>(marker, true));
            if (m.size() == 1 && sel.active()) {
                ++checks_strict;
                CHECK(norm(t.t, sel.range().start) == norm(t.t, m[0].start));
                CHECK(norm(t.t, sel.range().end) == norm(t.t, m[0].end));
            } else if (m.size() == 1 && !sel.active()) {
                // It may have been cleared by an earlier non-benign step; then
                // there is nothing to keep. Re-select to keep the property alive.
                sel.select_range(m[0]);
            }
        }
        if (!view.search().active()) view.search().start(needle);
        if (view.search().complete()) {
            const auto want = scratch(t.t, needle);
            const auto got = all(view.search());
            CHECK(got == want);
            if (got != want) {
                std::printf("  search: incremental %zu matches, scratch %zu\n", got.size(), want.size());
                view.search().start(needle);
            }
        }
        if (benign && !view.at_bottom() && !top_line.empty()) {
            if (view.top_row() > t.t.first_row()) {
                std::string now = line_text_at(t.t, view.top_row());
                // History splits a runaway line (one longer than half its
                // capacity) where it scrolls off: the view keeps the start.
                if (now.size() < top_line.size() && top_line.compare(0, now.size(), now) == 0) now = top_line;
                CHECK_EQ(now, top_line);
                if (now != top_line)
                    std::printf("  iteration %d after op %d: %dx%d, top %lld\n", iter, last_op, t.t.cols(), t.t.rows(),
                                (long long)view.top_row());
            }
            top_line = line_text_at(t.t, view.top_row());
        } else {
            top_line = view.at_bottom() ? std::string() : line_text_at(t.t, view.top_row());
        }
        check_frame();
    }
};

} // namespace

int main(int argc, char** argv) {
    init_test();
#if defined(NDEBUG)
    int iters = 3000;
#else
    int iters = 600;
#endif
    if (argc > 1) iters = std::atoi(argv[1]);
    int strict = 0;
    for (uint64_t seed = 1; seed <= 8 && check::g_failures == 0; ++seed) {
        Run run(seed * 0x9E3779B97F4A7C15ull, 12 + int(seed * 5 % 30), 3 + int(seed % 8), 20 + seed * 40);
        run.new_marker();
        const char* dbg = std::getenv("ANCHOR_DEBUG");  // "seed:iteration": dump the state before that step
        for (int i = 0; i < iters && check::g_failures == 0; ++i) {
            if (dbg && std::to_string(seed) + ":" + std::to_string(i) == dbg) run.dump();
            run.iter = i;
            const bool benign = run.step();
            run.check(benign);
            if (i % 50 == 0) {
                while (run.view.search().step()) {}
                run.check(true);
            }
        }
        strict += run.checks_strict;
        if (check::g_failures) std::printf("  failed with seed %llu\n", (unsigned long long)seed);
    }
    std::printf("  %d strict selection checks\n", strict);
    CHECK(strict > 100);
    return check::finish("test_anchor_props");
}
