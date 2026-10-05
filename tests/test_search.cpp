// Scrollback search: the literal matcher, incremental matching across screen
// and history, navigation, wrapped / wide matches, host matchers, eviction,
// reflow and cancellation. Incremental results are always compared with a
// search started from scratch on the final state.
#include "bropty/view.h"
#include "term_helpers.h"

#include <thread>

using namespace bropty;

namespace {

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

std::string text_of(const Terminal& t, const RowRange& r) {
    Selection sel(t);
    sel.select_range(r);
    return sel.text();
}

void literal_matcher() {
    std::vector<std::pair<size_t, size_t>> out;
    LiteralMatcher ci("Needle");
    ci.find("a NEEDLE and a needle", out);
    CHECK_EQ(out.size(), size_t(2));
    CHECK_EQ(out[0].first, size_t(2));
    CHECK_EQ(out[1].first, size_t(15));
    out.clear();
    LiteralMatcher cs("Needle", true);
    cs.find("a NEEDLE and a Needle", out);
    CHECK_EQ(out.size(), size_t(1));
    out.clear();
    LiteralMatcher aa("aa");
    aa.find("aaaaa", out);  // non-overlapping
    CHECK_EQ(out.size(), size_t(2));
    out.clear();
    LiteralMatcher fr("\xc3\x89" "COLE");  // ÉCOLE
    fr.find("une \xc3\xa9" "cole", out);  // école
    CHECK_EQ(out.size(), size_t(1));
    CHECK_EQ(out[0].first, size_t(4));
    out.clear();
    LiteralMatcher gr("\xce\xa3\xce\x9f\xce\xa6");  // ΣΟΦ
    gr.find("\xcf\x83\xce\xbf\xcf\x86\xce\xb9\xce\xb1", out);  // σοφια
    CHECK_EQ(out.size(), size_t(1));
    out.clear();
    LiteralMatcher ru("\xd0\x9c\xd0\x98\xd0\xa0");  // МИР
    ru.find("\xd0\xbc\xd0\xb8\xd1\x80", out);  // мир
    CHECK_EQ(out.size(), size_t(1));
    out.clear();
    LiteralMatcher empty("");
    empty.find("anything", out);
    CHECK(out.empty());
}

void across_history() {
    th::T t(40, 10, 1000);
    for (int i = 0; i < 200; ++i) {
        t << "line " + std::to_string(i) + (i % 10 == 3 ? " has a needle" : "") + "\r\n";
    }
    Search s(t.t);
    s.start(std::make_shared<LiteralMatcher>("NEEDLE"));
    CHECK(!s.complete());
    int steps = 0;
    while (s.step(std::chrono::microseconds(0))) ++steps;  // a tiny budget: 16 lines a step
    CHECK(steps > 3);
    CHECK(s.complete());
    CHECK_EQ(s.size(), size_t(20));
    for (size_t i = 0; i < s.size(); ++i) {
        CHECK_EQ(text_of(t.t, s.at(i)), std::string("needle"));
        if (i) CHECK(s.at(i - 1).start < s.at(i).start);
    }
    CHECK(all(s) == scratch(t.t, std::make_shared<LiteralMatcher>("needle")));

    // Navigation: backward from the bottom finds the newest, then older ones; wraps.
    TerminalView view(t.t);
    view.search().start(std::make_shared<LiteralMatcher>("needle"));
    while (view.search().step()) {}
    auto m = view.search_next(true);
    CHECK(m.has_value());
    if (m) CHECK(*m == s.at(19));
    CHECK(view.search().current_index() == std::optional<size_t>(19));
    m = view.search_next(true);
    if (m) CHECK(*m == s.at(18));
    // The view scrolled so the current match is visible.
    CHECK(m && m->start.row >= view.top_row() && m->start.row < view.top_row() + t.t.rows());
    m = view.search_next(false);
    m = view.search_next(false);
    if (m) CHECK(*m == s.at(0));  // wrapped around past the newest
    auto f = view.snapshot();
    bool current = false;
    for (const Highlight& h : f->highlights) current |= h.kind == HighlightKind::CurrentMatch;
    CHECK(current);
    CHECK_EQ(f->match_count, size_t(20));
}

void wrapped_and_wide() {
    th::T t(10, 4);
    t << "xxxxxxxneedlexx\r\n\xe4\xb8\xad" "needle";
    Search s(t.t);
    s.start(std::make_shared<LiteralMatcher>("needle"));
    while (s.step()) {}
    CHECK_EQ(s.size(), size_t(2));
    const int64_t top = t.t.screen_top_row();
    CHECK(s.at(0).start == (RowPos{top, 7}));
    CHECK(s.at(0).end == (RowPos{top + 1, 3}));  // spans the soft wrap
    CHECK(s.at(1).start == (RowPos{top + 2, 2}));  // after the wide 中
    CHECK(s.at(1).end == (RowPos{top + 2, 8}));
    CHECK_EQ(text_of(t.t, s.at(0)), std::string("needle"));
}

// A host matcher: runs of digits.
struct Digits final : SearchMatcher {
    void find(std::string_view line, std::vector<std::pair<size_t, size_t>>& out) override {
        for (size_t i = 0; i < line.size();) {
            if (line[i] >= '0' && line[i] <= '9') {
                size_t j = i;
                while (j < line.size() && line[j] >= '0' && line[j] <= '9') ++j;
                out.emplace_back(i, j);
                i = j;
            } else {
                ++i;
            }
        }
    }
};

void host_matcher_and_live_output() {
    th::T t(30, 5, 50);
    TerminalView view(t.t);
    auto digits = std::make_shared<Digits>();
    t << "a1 b22\r\nc333";
    view.search().start(digits);
    CHECK_EQ(view.search().size(), size_t(3));
    // Output arrives (and scrolls lines into history) while the search runs.
    for (int i = 0; i < 40; ++i) {
        t << "\r\nrow " + std::to_string(i);
        if (i % 7 == 0) {
            view.sync();
            view.search().step(std::chrono::microseconds(0));
        }
    }
    view.sync();
    while (view.search().step()) {}
    CHECK(all(view.search()) == scratch(t.t, digits));
    // Rewriting a screen line updates its matches.
    t << "\x1b[1;1H\x1b[2Kzz 9";
    view.sync();
    CHECK(all(view.search()) == scratch(t.t, digits));
    // Eviction: a scrollback of 50 rows drops the oldest matches.
    for (int i = 0; i < 100; ++i) t << "\r\nmore " + std::to_string(i);
    view.sync();
    while (view.search().step()) {}
    CHECK(all(view.search()) == scratch(t.t, digits));
    CHECK(view.search().at(0).start.row >= t.t.first_row());
}

void reflow() {
    th::T t(20, 6, 500);
    TerminalView view(t.t);
    for (int i = 0; i < 60; ++i) t << "item " + std::to_string(i) + " target_word end of line\r\n";
    auto m = std::make_shared<LiteralMatcher>("target_word");
    view.search().start(m);
    while (view.search().step()) {}
    CHECK_EQ(view.search().size(), size_t(60));
    view.search_next(true);
    const auto before = view.search().current_index();
    for (int w : {7, 33, 13, 20}) {
        t.t.resize(w, 6);
        view.sync();
        while (view.search().step()) {}
        CHECK(all(view.search()) == scratch(t.t, m));
        for (size_t i = 0; i < view.search().size(); ++i) CHECK_EQ(text_of(t.t, view.search().at(i)), std::string("target_word"));
        CHECK(view.search().current_index() == before);  // the current match stays current
    }
}

void cancel_and_switch() {
    th::T t(80, 10, 100000);
    std::string block;
    for (int i = 0; i < 20000; ++i) block += "some text with no hits at all, line " + std::to_string(i) + "\r\n";
    t << block;
    Search s(t.t);
    s.start(std::make_shared<LiteralMatcher>("zzz"));
    CHECK(s.step(std::chrono::microseconds(0)));
    std::thread other([&] { s.cancel(); });
    other.join();
    CHECK(!s.step());
    CHECK(!s.active());
    CHECK_EQ(s.size(), size_t(0));

    // Entering the alternate screen restarts the search on it.
    TerminalView view(t.t);
    view.search().start(std::make_shared<LiteralMatcher>("line 1999"));
    while (view.search().step()) {}
    CHECK(view.search().size() > 0);
    t << "\x1b[?1049h\x1b[Hline 1999 on alt";
    view.sync();
    while (view.search().step()) {}
    CHECK_EQ(view.search().size(), size_t(1));
    CHECK_EQ(view.search().at(0).start.row, t.t.screen_top_row());
}

} // namespace

int main() {
    init_test();
    literal_matcher();
    across_history();
    wrapped_and_wide();
    host_matcher_and_live_output();
    reflow();
    cancel_and_switch();
    return check::finish("test_search");
}
