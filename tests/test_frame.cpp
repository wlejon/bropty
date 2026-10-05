// Frame snapshots: contents, copy-on-write row reuse (a scroll re-snapshots
// only the new rows), damage, highlights, a scrolled-back viewport that stays
// on its text, palette sharing, immutability, and FrameChannel's handoff.
#include "bropty/view.h"
#include "term_helpers.h"

using namespace bropty;

namespace {

std::string row_text(const Frame& f, int y) { return f.lines[size_t(y)]->view().text(); }

void contents() {
    th::T t(20, 5, 100);
    TerminalView view(t.t);
    t << "hello\r\n\x1b[31;1mred\x1b[0m \xe4\xb8\xad e\xcc\x81\x1b[?25l";
    auto f = view.snapshot();
    CHECK_EQ(f->rows, 5);
    CHECK_EQ(f->cols, 20);
    CHECK_EQ(row_text(*f, 0), std::string("hello"));
    CHECK_EQ(row_text(*f, 1), std::string("red \xe4\xb8\xad e\xcc\x81"));
    const FrameRow& r1 = *f->lines[1];
    CHECK(r1.styles[r1.cells[0].style].fg == Color::indexed(1));
    CHECK(r1.styles[r1.cells[0].style].has(Attr_Bold));
    CHECK(r1.styles[r1.cells[3].style].is_default());
    CHECK(r1.cells[4].wide() == Wide::Lead);
    CHECK(r1.view().cluster(7) == U"é");
    CHECK_EQ(f->cursor.row, 1);
    CHECK_EQ(f->cursor_y, 1);
    CHECK(!f->cursor.visible);
    CHECK(f->at_bottom());
    CHECK(f->palette != nullptr);
    CHECK_EQ(f->damage.size(), size_t(5));
}

void copy_on_write() {
    th::T t(20, 5, 100);
    TerminalView view(t.t);
    t << "a\r\nb\r\nc\r\nd\r\ne";
    auto f1 = view.snapshot();
    FrameChannel ch;
    CHECK(!view.publish(ch));  // nothing changed since the snapshot
    // One cell changes: only that row is re-snapshotted.
    t << "\x1b[3;2HX";
    auto f2 = view.snapshot();
    for (int y = 0; y < 5; ++y) {
        if (y == 2) CHECK(f2->lines[2] != f1->lines[2]);
        else CHECK(f2->lines[size_t(y)] == f1->lines[size_t(y)]);
    }
    CHECK_EQ(row_text(*f2, 2), std::string("cX"));
    CHECK_EQ(row_text(*f1, 2), std::string("c"));  // the old frame is immutable
    // Damage: the changed row and the rows the cursor left / entered.
    CHECK(f2->damage[2] == 1);
    CHECK(f2->damage[4] == 1);  // the cursor left row 4
    CHECK(f2->damage[0] == 0 && f2->damage[1] == 0 && f2->damage[3] == 0);
    // A full-screen scroll reuses every row that moved: only the new one is copied.
    t << "\x1b[5;1H\n";
    auto f3 = view.snapshot();
    for (int y = 0; y < 4; ++y) CHECK(f3->lines[size_t(y)] == f2->lines[size_t(y + 1)]);
    CHECK(f3->lines[4] != f2->lines[4]);
    CHECK(f3->lines[4]->serial > f2->lines[4]->serial);
    // Rewriting identical content still makes a new snapshot (stamps track writes).
    t << "\x1b[1;1Hb";
    auto f4 = view.snapshot();
    CHECK(f4->lines[0] != f3->lines[0]);
    CHECK_EQ(row_text(*f4, 0), std::string("b"));
}

void scrolled_back_viewport() {
    th::T t(20, 4, 100);
    TerminalView view(t.t);
    for (int i = 0; i < 20; ++i) t << "line " + std::to_string(i) + "\r\n";
    // Screen: line 17, 18, 19, ""; history: lines 0..16.
    view.scroll_by(-6);
    CHECK(!view.at_bottom());
    auto f1 = view.snapshot();
    CHECK(!f1->at_bottom());
    CHECK_EQ(row_text(*f1, 0), std::string("line 11"));
    CHECK_EQ(row_text(*f1, 3), std::string("line 14"));
    CHECK_EQ(f1->cursor_y, -1);  // the cursor is below the view
    // Output arrives: the view stays on its text, history rows are not re-copied.
    for (int i = 20; i < 30; ++i) t << "line " + std::to_string(i) + "\r\n";
    auto f2 = view.snapshot();
    CHECK_EQ(row_text(*f2, 0), std::string("line 11"));
    CHECK(f2->top_row == f1->top_row);
    for (int y = 0; y < 4; ++y) CHECK(f2->lines[size_t(y)] == f1->lines[size_t(y)]);
    // A viewport partly on the screen.
    view.scroll_to_row(t.t.screen_top_row() - 2);
    auto f3 = view.snapshot();
    CHECK_EQ(row_text(*f3, 0), std::string("line 25"));
    CHECK_EQ(row_text(*f3, 2), std::string("line 27"));
    CHECK_EQ(f3->cursor_y, -1);  // the cursor's row (screen row 3) is below this view
    // Reflow keeps the top line on top.
    view.scroll_to_row(t.t.screen_top_row() - 8);
    const std::string top_before = row_text(*view.snapshot(), 0);
    t.t.resize(5, 4);
    t.t.resize(20, 4);
    CHECK_EQ(row_text(*view.snapshot(), 0), top_before);
    // Eviction under the view clamps it to what is left.
    th::T e(10, 3, 5);
    TerminalView ev(e.t);
    for (int i = 0; i < 8; ++i) e << "r" + std::to_string(i) + "\r\n";
    ev.scroll_to_row(e.t.first_row());
    const int64_t first = e.t.first_row();
    e << "x\r\ny\r\nz\r\n";
    ev.sync();
    CHECK(ev.top_row() == e.t.first_row());
    CHECK(ev.top_row() > first);
    ev.scroll_to_bottom();
    CHECK(ev.at_bottom());
}

void highlights_and_damage() {
    th::T t(20, 5, 100);
    TerminalView view(t.t);
    t << "one two\r\nthree four\r\nfive";
    auto f1 = view.snapshot();
    const int64_t top = t.t.screen_top_row();
    view.selection().start(RowPos{top, 4}, SelectionMode::Character);
    view.selection().extend(RowPos{top + 1, 4}, true);
    auto f2 = view.snapshot();
    CHECK_EQ(f2->highlights.size(), size_t(2));
    CHECK(f2->highlights[0] == (Highlight{0, 4, 20, HighlightKind::Selection}));
    CHECK(f2->highlights[1] == (Highlight{1, 0, 5, HighlightKind::Selection}));
    CHECK(f2->selection_active);
    CHECK(f2->damage[0] == 1 && f2->damage[1] == 1 && f2->damage[2] == 0);
    CHECK(f2->lines[0] == f1->lines[0]);  // only the overlay changed
    CHECK(f2->row_differs(*f1, 0));
    CHECK(!f2->row_differs(*f1, 3));
    auto [a, b] = f2->highlights_of(1);
    CHECK_EQ(b - a, size_t(1));
    // Search matches and the current match.
    view.search().start(std::make_shared<LiteralMatcher>("o"));
    view.search_next(false);
    auto f3 = view.snapshot();
    int matches = 0, current = 0;
    for (const Highlight& h : f3->highlights) {
        matches += h.kind == HighlightKind::Match;
        current += h.kind == HighlightKind::CurrentMatch;
    }
    CHECK_EQ(current, 1);
    CHECK_EQ(matches, 2);  // "two" and "four"; "one" is the current match
    CHECK(f3->search_active);
}

void palette_sharing() {
    th::T t(10, 2);
    TerminalView view(t.t);
    auto f1 = view.snapshot();
    t << "x";
    auto f2 = view.snapshot();
    CHECK(f1->palette == f2->palette);
    t << "\x1b]4;1;rgb:12/34/56\x1b\\";
    auto f3 = view.snapshot();
    CHECK(f3->palette != f2->palette);
    CHECK(f3->palette->colors[1] == (Rgb{0x12, 0x34, 0x56}));
    for (int y = 0; y < 2; ++y) CHECK(f3->row_differs(*f2, y));
}

void channel() {
    FrameChannel ch;
    CHECK(ch.acquire() == nullptr);
    CHECK(ch.consumed());
    auto mk = [](uint64_t seq) {
        auto f = std::make_shared<Frame>();
        f->seq = seq;
        return std::shared_ptr<const Frame>(f);
    };
    ch.publish(mk(1));
    CHECK(!ch.consumed());
    CHECK(ch.has_new());
    auto a = ch.acquire();
    CHECK(a && a->seq == 1);
    CHECK(ch.consumed());
    CHECK(ch.acquire()->seq == 1);  // nothing newer: the same frame again
    ch.publish(mk(2));
    ch.publish(mk(3));
    ch.publish(mk(4));
    CHECK(ch.acquire()->seq == 4);  // the newest wins
    CHECK(a->seq == 1);             // a frame the reader holds stays valid

    th::T t(10, 3);
    TerminalView view(t.t);
    FrameChannel c2;
    CHECK(view.publish(c2));
    CHECK(!view.publish(c2));  // nothing changed
    t << "a";
    CHECK(!view.publish(c2, true));  // the reader has not taken the last one
    CHECK(c2.acquire() != nullptr);
    CHECK(view.publish(c2, true));
    CHECK_EQ(row_text(*c2.acquire(), 0), std::string("a"));
    t << "\r\n\r\n\r\n";
    CHECK(view.publish(c2));
    view.scroll_by(-1);
    CHECK(view.publish(c2));  // a viewport change is a change
    CHECK(!c2.acquire()->at_bottom());
}

} // namespace

int main() {
    init_test();
    contents();
    copy_on_write();
    scrolled_back_viewport();
    highlights_and_damage();
    palette_sharing();
    channel();
    return check::finish("test_frame");
}
