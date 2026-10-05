// Selection, search, link detection and TerminalView over a RowSource that
// is not a Terminal (row_mirror.h: a client-style mirror with its own style
// tables, per-row hyperlink id spaces and history it may not hold), checked
// against the same operations over the Terminal it mirrors. Random content
// and gestures; then the parts only a mirror has: rows that arrive later
// (requests, a search that waits, gaps) and resizes it cannot carry.
#include "bropty/view.h"
#include "row_mirror.h"
#include "term_helpers.h"

#include <random>

using namespace bropty;

namespace {

struct Rng {
    std::mt19937 g;
    explicit Rng(uint32_t seed) : g(seed) {}
    int below(int n) { return int(g() % uint32_t(n)); }
};

const char* kWords[] = {"alpha", "beta", "gamma", "delta", "http://ex.am/ple?q=1", "/usr/lib/x.so:12",
                        "src/main.cpp:4:2", "www.bro.dev", "naïve", "中文字", "e\xCC\x81t\xC3\xA9", "x",
                        "foo-bar_baz", "~/notes.txt", "(see https://a.b/c).", "ALPHA"};

std::string random_output(Rng& r, int lines) {
    std::string s;
    for (int i = 0; i < lines; ++i) {
        const int k = r.below(12);
        if (k == 0) s += "\x1b]133;A\x07$ \x1b]133;B\x07" "cmd\r\n\x1b]133;C\x07";
        if (k == 1) s += "\x1b]8;id=" + std::to_string(r.below(3)) + ";https://link/" + std::to_string(r.below(4)) +
                         "\x1b\\linked text\x1b]8;;\x1b\\ ";
        if (k == 2) s += "\x1b[3" + std::to_string(r.below(8)) + "m";
        if (k == 3) s += "\x1b[0m";
        const int n = 1 + r.below(k == 4 ? 14 : 6);  // sometimes long enough to wrap
        for (int w = 0; w < n; ++w) {
            s += kWords[r.below(int(sizeof kWords / sizeof *kWords))];
            s += ' ';
        }
        if (k == 5) s += "\x1b]133;D\x07";
        s += "\r\n";
    }
    return s;
}

// What a frame row shows, independent of style-table layout and link ids.
std::string row_key(const FrameRow& r) {
    std::string s = r.view().text(false) + "|" + std::to_string(r.flags) + "|";
    for (const Cell& c : r.cells) {
        Style st = r.styles[c.style];
        const std::string* uri = st.link ? r.link_uri(st.link) : nullptr;
        st.link = 0;
        s += std::to_string(st.fg.packed()) + "," + std::to_string(st.attrs) + "," + (uri ? *uri : "") + ";";
    }
    return s;
}

void check_frames(TerminalView& a, TerminalView& b) {
    auto fa = a.snapshot();
    auto fb = b.snapshot();
    CHECK_EQ(fa->rows, fb->rows);
    CHECK_EQ(fa->cols, fb->cols);
    CHECK_EQ(fa->top_row, fb->top_row);
    CHECK_EQ(fa->first_row, fb->first_row);
    CHECK_EQ(fa->cursor_y, fb->cursor_y);
    CHECK(fa->highlights == fb->highlights);
    CHECK_EQ(fa->match_count, fb->match_count);
    if (fa->rows != fb->rows) return;
    for (int y = 0; y < fa->rows; ++y) CHECK_EQ(row_key(*fa->lines[size_t(y)]), row_key(*fb->lines[size_t(y)]));
}

bool same_link(const std::optional<LinkHit>& a, const std::optional<LinkHit>& b) {
    if (a.has_value() != b.has_value()) return false;
    return !a || (a->range == b->range && a->kind == b->kind && a->target == b->target);
}

void run_search(Search& s, int limit = 100000) {
    while (s.step() && limit-- > 0) {
    }
}

std::vector<RowRange> matches(const Search& s) {
    std::vector<RowRange> v;
    for (size_t i = 0; i < s.size(); ++i) v.push_back(s.at(i));
    return v;
}

// Random content, gestures and searches: the mirror answers exactly as the
// terminal does.
void same_answers(uint32_t seed) {
    Rng r(seed);
    th::T t(24 + r.below(20), 6 + r.below(6), 60 + size_t(r.below(80)));
    mirror::Mirror m;
    TerminalView tv(t.t);
    TerminalView mv(m);
    for (int round = 0; round < 12; ++round) {
        t << random_output(r, 2 + r.below(15));
        if (r.below(9) == 0) {
            t << "\x1b[?1049h" + random_output(r, 3);
            if (r.below(2)) t << "\x1b[?1049l";
        }
        m.pull(t.t);
        tv.sync();
        mv.sync();
        const int64_t lo = t.t.first_row(), hi = t.t.end_row();
        CHECK_EQ(m.first_row(), lo);
        CHECK_EQ(m.end_row(), hi);
        auto cell = [&] { return RowPos{lo + r.below(int(hi - lo)), r.below(t.t.cols())}; };

        // Selection, every mode.
        for (int g = 0; g < 8; ++g) {
            const auto mode = SelectionMode(r.below(5));
            const RowPos a = cell(), b = cell();
            const bool half = r.below(2) != 0;
            tv.selection().start(a, mode, half);
            mv.selection().start(a, mode, half);
            tv.selection().extend(b, !half);
            mv.selection().extend(b, !half);
            CHECK_EQ(mv.selection().active(), tv.selection().active());
            CHECK(mv.selection().range() == tv.selection().range());
            if (!(mv.selection().range() == tv.selection().range()))
                std::printf("  seed %u mode %d from %lld:%d to %lld:%d\n", seed, int(mode), (long long)a.row, a.col,
                            (long long)b.row, b.col);
            CHECK_EQ(mv.selection().text(), tv.selection().text());
            CHECK_EQ(mv.selection().html(t.t.palette()), tv.selection().html(t.t.palette()));
        }
        const RowPos z = cell();
        CHECK_EQ(mv.selection().select_output(z), tv.selection().select_output(z));
        CHECK_EQ(mv.selection().text(), tv.selection().text());
        CHECK_EQ(mv.selection().select_last_output(), tv.selection().select_last_output());
        CHECK_EQ(mv.selection().text(), tv.selection().text());
        tv.selection().select_all();
        mv.selection().select_all();
        CHECK_EQ(mv.selection().text(), tv.selection().text());

        // Links under random cells, and all links in the buffer.
        for (int k = 0; k < 40; ++k) {
            const RowPos c = cell();
            CHECK(same_link(link_at(m, c), link_at(t.t, c)));
        }
        std::vector<LinkHit> la, lb;
        links_in_rows(t.t, lo, hi, la);
        links_in_rows(m, lo, hi, lb);
        CHECK_EQ(lb.size(), la.size());
        for (size_t i = 0; i < std::min(la.size(), lb.size()); ++i) CHECK(same_link(la[i], lb[i]));

        // Search to completion.
        const char* needle = kWords[r.below(int(sizeof kWords / sizeof *kWords))];
        const bool cs = r.below(2) != 0;
        tv.search().start(std::make_shared<LiteralMatcher>(needle, cs));
        mv.search().start(std::make_shared<LiteralMatcher>(needle, cs));
        run_search(tv.search());
        run_search(mv.search());
        CHECK(mv.search().complete());
        CHECK(matches(mv.search()) == matches(tv.search()));
        const bool sb = r.below(2) != 0;
        CHECK(mv.search_next(sb) == tv.search_next(sb));  // moves the viewport alike
        CHECK_EQ(mv.top_row(), tv.top_row());
        mv.search().next(false, RowPos{lo, 0});
        tv.search().next(false, RowPos{lo, 0});
        CHECK(mv.search().current() == tv.search().current());

        // Viewport, prompts, hover; then whole frames.
        const int64_t back = -r.below(40);
        tv.scroll_to_bottom();
        mv.scroll_to_bottom();
        tv.scroll_by(back);
        mv.scroll_by(back);
        CHECK_EQ(mv.top_row(), tv.top_row());
        const bool bw = r.below(2) != 0;
        CHECK_EQ(mv.scroll_to_prompt(bw), tv.scroll_to_prompt(bw));
        CHECK_EQ(mv.top_row(), tv.top_row());
        const RowPos h = RowPos{tv.top_row() + r.below(t.t.rows()), r.below(t.t.cols())};
        tv.set_hover(h);
        mv.set_hover(h);
        CHECK(same_link(mv.hover(), tv.hover()));
        tv.selection().start(cell(), SelectionMode::Word);
        tv.selection().extend(cell());
        mv.selection().start(tv.selection().range().start, SelectionMode::Character);
        mv.selection().select_range(tv.selection().range());
        tv.selection().select_range(tv.selection().range());
        check_frames(tv, mv);
    }
}

// Frames reuse rows by serial: an unchanged screen costs nothing, a scroll
// only the new row, and a source without serials still renders right.
void frame_row_reuse() {
    th::T t(20, 5, 100);
    mirror::Mirror m;
    t << "one\r\ntwo\r\nthree\r\nfour";
    m.pull(t.t);
    TerminalView v(m);
    auto f1 = v.snapshot();
    auto f2 = v.snapshot();
    for (int y = 0; y < 5; ++y) CHECK(f1->lines[size_t(y)] == f2->lines[size_t(y)]);
    t << "\r\nfive\r\nsix";
    m.pull(t.t);
    auto f3 = v.snapshot();
    // "two" .. "four" scrolled up by one: the same snapshots.
    CHECK_EQ(f3->lines[0]->view().text(), std::string("two"));
    CHECK(f3->lines[0] == f1->lines[1]);
    CHECK(f3->lines[1] == f1->lines[2]);
    CHECK(f3->lines[2] == f1->lines[3]);
    CHECK_EQ(f3->lines[4]->view().text(), std::string("six"));
    mirror::Mirror plain;
    plain.pull(t.t, false);
    TerminalView pv(plain);
    auto p = pv.snapshot();
    for (int y = 0; y < 5; ++y) CHECK_EQ(p->lines[size_t(y)]->view().text(), f3->lines[size_t(y)]->view().text());
    CHECK(p->images.empty());
}

// History the mirror does not hold yet: frames show it blank and ask for
// it; a search waits for it; lines that scroll off while missing are
// matched once they arrive.
void rows_arrive_later() {
    th::T t(30, 6, 500);
    std::string out;
    for (int i = 0; i < 80; ++i) out += "line " + std::to_string(i) + (i % 10 == 3 ? " needle" : "") + "\r\n";
    t << out;
    mirror::Mirror m;
    m.pull(t.t);
    m.hold_history_from(INT64_MAX);  // a client that fetched nothing yet
    TerminalView tv(t.t), mv(m);

    // A scrolled-back frame: blank rows, and one request covering them.
    tv.scroll_by(-20);
    mv.scroll_by(-20);
    m.clear_requests();
    auto blank = mv.snapshot();
    CHECK_EQ(blank->lines[0]->view().text(), std::string(""));
    CHECK_EQ(blank->lines[0]->cols, 30);
    CHECK_EQ(m.requests().size(), size_t(1));
    if (!m.requests().empty()) {
        CHECK(m.requests()[0].first <= mv.top_row());
        CHECK(m.requests()[0].second >= mv.top_row() + 6);
    }

    // The search matches the screen, then waits at history.
    auto needle = std::make_shared<LiteralMatcher>("needle");
    mv.search().start(needle);
    tv.search().start(needle);
    m.clear_requests();
    CHECK(mv.search().step());
    CHECK(mv.search().waiting());
    CHECK(!mv.search().complete());
    CHECK(!m.requests().empty());
    CHECK(mv.search().step());  // still waiting, still asking
    CHECK(mv.search().waiting());

    // More output while waiting: rows scroll off the screen into history the
    // mirror does not hold either.
    std::string more;
    for (int i = 80; i < 95; ++i) more += "line " + std::to_string(i) + (i == 90 ? " needle" : "") + "\r\n";
    t << more;
    m.pull(t.t);
    m.hold_history_from(INT64_MAX);
    mv.sync();
    tv.sync();
    CHECK(!mv.search().complete());

    // The rows arrive: the search completes with exactly the terminal's matches.
    m.hold_history_from(m.first_row());
    run_search(mv.search());
    run_search(tv.search());
    CHECK(!mv.search().waiting());
    CHECK(mv.search().complete());
    CHECK(matches(mv.search()) == matches(tv.search()));
    CHECK_EQ(mv.search().size(), size_t(9));
    auto full = mv.snapshot();
    auto want = tv.snapshot();
    for (int y = 0; y < 6; ++y) CHECK_EQ(row_key(*full->lines[size_t(y)]), row_key(*want->lines[size_t(y)]));
}

// A resize of a mirror cannot carry positions: the selection is cleared,
// the search starts over (and finds the reflowed matches), the viewport
// returns to the bottom. Screen switches clear the selection as on a
// Terminal.
void resize_and_switch() {
    th::T t(30, 6, 200);
    for (int i = 0; i < 40; ++i) t << "row " + std::to_string(i) + " word\r\n";
    mirror::Mirror m;
    m.pull(t.t);
    TerminalView mv(m);
    mv.selection().start(RowPos{m.screen_top_row(), 0}, SelectionMode::Line);
    CHECK(mv.selection().active());
    mv.search().start(std::make_shared<LiteralMatcher>("word"));
    run_search(mv.search());
    CHECK_EQ(mv.search().size(), size_t(40));
    mv.scroll_by(-10);
    CHECK(!mv.at_bottom());
    t.t.resize(12, 5);
    m.pull(t.t);
    mv.sync();
    CHECK(!mv.selection().active());
    CHECK(mv.at_bottom());
    CHECK(mv.search().active());
    run_search(mv.search());
    CHECK_EQ(mv.search().size(), size_t(40));
    TerminalView tv(t.t);
    tv.search().start(std::make_shared<LiteralMatcher>("word"));
    run_search(tv.search());
    CHECK(matches(mv.search()) == matches(tv.search()));

    mv.selection().select_all();
    t << "\x1b[?1049h";
    m.pull(t.t);
    mv.sync();
    CHECK(!mv.selection().active());
    CHECK(mv.source().alt_screen_active());
    CHECK(mv.terminal_or_null() == nullptr);
    CHECK(tv.terminal_or_null() == &t.t);
}

} // namespace

int main() {
    init_test();
    for (uint32_t seed = 1; seed <= 12; ++seed) same_answers(seed);
    frame_row_reuse();
    rows_arrive_later();
    resize_and_switch();
    return check::finish("test_row_source");
}
