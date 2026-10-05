// Images living with the text: kitty placements and image cells through
// scrolling (full screen, regions, bottom margins), history eviction, erase
// (ED 2 / ED 3), the alternate screen, RIS, reflow on resize, and what
// frames show while scrolled back.
#include "image_helpers.h"

#include "bropty/view.h"

using namespace bropty;
using ih::IT;
using ih::kitty;

namespace {

// A 1 x 2 cell kitty image (10 x 40 px).
const std::string kImg = kitty("a=t,i=1,f=24,s=10,v=40,q=2", std::string(10 * 40 * 3, '\x7f'));

const Placement* placement(const Terminal& t, uint32_t pid = 0) {
    for (const Placement& p : t.images().placements())
        if (!pid || p.placement_id == pid) return &p;
    return nullptr;
}

void scrolling_and_eviction() {
    IT t(20, 5, 10);  // 10 rows of history
    t << kImg << "\x1b[2;3H" << kitty("a=p,i=1,p=1,q=2,C=1");
    const Placement* p = placement(t.t);
    CHECK(p && p->row == 1 && p->col == 2);
    // Full-screen scrolling: the absolute row stays, the text and image go
    // into history together.
    t << "\x1b[5;1H\n\n\n";
    p = placement(t.t);
    CHECK(p && p->row == 1);
    CHECK_EQ(t.t.screen_top_row(), int64_t(3));
    TerminalView view(t.t);
    auto f = view.snapshot();
    CHECK(f->images.empty());  // rows 1..2, above the viewport
    view.scroll_to_row(1);
    f = view.snapshot();
    CHECK(!f->images.empty() && f->images[0].y == 0.0f);
    view.scroll_to_row(2);
    f = view.snapshot();
    CHECK(!f->images.empty() && f->images[0].y == -1.0f && f->images[0].h == 2.0f);  // half in view
    view.scroll_to_bottom();
    // Evicted with its rows: kept while either of its rows (1, 2) is held.
    for (int guard = 0; guard < 100 && t.t.first_row() < 3; ++guard) {
        CHECK(placement(t.t) != nullptr);
        t << "\n";
    }
    CHECK(t.t.first_row() >= 3);
    CHECK(placement(t.t) == nullptr);
    CHECK(t.t.images().find(1) != nullptr);  // the image itself stays (kitty)
}

void regions() {
    IT t(20, 10);
    t << kImg;
    // In a region rows 3..7 (0-based 2..6): one placement inside, one outside.
    t << "\x1b[4;5H" << kitty("a=p,i=1,p=1,q=2,C=1") << "\x1b[9;5H" << kitty("a=p,i=1,p=2,q=2,C=1");
    t << "\x1b[3;7r\x1b[7;1H\n";  // scroll the region up by one
    CHECK(placement(t.t, 1) && placement(t.t, 1)->row == 2);
    CHECK(placement(t.t, 2) && placement(t.t, 2)->row == 8);
    // Scrolling it past the region's top clips the top row off, then removes it.
    t << "\n";
    const Placement* p = placement(t.t, 1);
    CHECK(p && p->row == 2 && p->src_y == 20 && p->src_h == 20);
    t << "\n";
    CHECK(placement(t.t, 1) == nullptr);
    CHECK(placement(t.t, 2) != nullptr);
    // Reverse index at the region's top scrolls down.
    t << "\x1b[5;5H" << kitty("a=p,i=1,p=3,q=2,C=1") << "\x1b[3;1H\x1bM";
    CHECK(placement(t.t, 3) && placement(t.t, 3)->row == 5);
    // Insert / delete lines scroll the region below the cursor.
    t << "\x1b[4;1H\x1b[L";
    CHECK(placement(t.t, 3) && placement(t.t, 3)->row == 6);
    t << "\x1b[4;1H\x1b[2M";
    CHECK(placement(t.t, 3) && placement(t.t, 3)->row == 4);
    t << "\x1b[r";
    // A region from the top with a bottom margin: scrolled rows go into
    // history, rows below the margin stay where they are on the screen.
    IT u(20, 10);
    u << kImg << "\x1b[9;1H" << kitty("a=p,i=1,p=1,q=2,C=1") << "\x1b[1;1H" << kitty("a=p,i=1,p=2,q=2,C=1");
    u << "\x1b[1;5r\x1b[5;1H\n";
    CHECK_EQ(u.t.history_rows(), size_t(1));
    CHECK(placement(u.t, 1) && placement(u.t, 1)->row == u.t.screen_top_row() + 8);
    CHECK(placement(u.t, 2) && placement(u.t, 2)->row == 0);  // now in history
}

void erase_and_screens() {
    IT t(20, 5, 100);
    t << kImg << kitty("a=p,i=1,p=1,q=2,C=1") << "\x1b[5;1H\n\n\n\n\n";
    t << "\x1b[1;1H" << kitty("a=p,i=1,p=2,q=2,C=1");
    CHECK_EQ(t.t.images().placements().size(), size_t(2));
    // ED 2 removes what reaches the screen; history keeps its own.
    t << "\x1b[2J";
    CHECK(placement(t.t, 1) != nullptr);
    CHECK(placement(t.t, 2) == nullptr);
    // ED 3 clears history: its placements go.
    t << "\x1b[3J";
    CHECK(placement(t.t, 1) == nullptr);
    // The alternate screen has its own images, cleared when it is.
    t << kitty("a=p,i=1,p=3,q=2,C=1") << "\x1b[?1049h";
    CHECK_EQ(t.t.images().placements().size(), size_t(0));
    CHECK(t.t.images().find(1) == nullptr);
    t << kImg << kitty("a=p,i=1,p=4,q=2,C=1");
    CHECK_EQ(t.t.images(true).placements().size(), size_t(1));
    CHECK_EQ(t.t.images(false).placements().size(), size_t(1));
    t << "\x1b[?1049l";
    CHECK(placement(t.t, 3) != nullptr);
    // Entering it again clears it (with its text), sixel cells included.
    t << "\x1b[?1049h";
    CHECK_EQ(t.t.images(true).image_count(), size_t(0));
    t << "\x1bPq#1;2;100;0;0~~\x1b\\";
    CHECK_EQ(t.t.images(true).cell_image_count(), size_t(1));
    t << "\x1b[?1049l\x1b[?1049h";
    CHECK_EQ(t.t.images(true).cell_image_count(), size_t(0));
    t << "\x1b[?1049l";
    CHECK(placement(t.t, 3) != nullptr);
    // RIS clears everything.
    t << "\x1bPq~\x1b\\" << "\x1b" "c";
    CHECK_EQ(t.t.images().image_count(), size_t(0));
    CHECK_EQ(t.t.image_bytes(), size_t(0));
    CHECK_EQ(t.t.images().placements().size(), size_t(0));
}

void cell_image_eviction() {
    IT t(20, 4, 5);
    t << "\x1bPq#1;2;100;0;0~\x1b\\";  // 1 x 1 cell
    CHECK_EQ(t.t.images().cell_image_count(), size_t(1));
    for (int i = 0; i < 8; ++i) t << "\n";
    CHECK_EQ(t.t.images().cell_image_count(), size_t(1));  // in history
    for (int i = 0; i < 6; ++i) t << "\n";
    CHECK_EQ(t.t.images().cell_image_count(), size_t(0));  // evicted with its row
    CHECK_EQ(t.t.image_bytes(), size_t(0));
}

void reflow() {
    // A kitty placement rides on the character under its top-left cell.
    IT t(20, 6);
    t << kImg << "0123456789abcdefghij" << "\x1b[1;13H" << kitty("a=p,i=1,p=1,q=2,C=1");  // over 'c'
    t.t.resize(10, 6);  // "0123456789" / "abcdefghij": 'c' at row 1, col 2
    const Placement* p = placement(t.t, 1);
    CHECK(p && p->row == t.t.screen_top_row() + 1 && p->col == 2);
    t.t.resize(20, 6);
    p = placement(t.t, 1);
    CHECK(p && p->row == t.t.screen_top_row() && p->col == 12);
    // Past the end of a line's text: the offset beyond it is kept.
    IT u(20, 6);
    u << kImg << "abc" << "\x1b[1;8H" << kitty("a=p,i=1,p=1,q=2,C=1");
    u.t.resize(30, 6);
    p = placement(u.t, 1);
    CHECK(p && p->col == 7);
    // Image cells reflow like text: a row of them wraps and splits.
    IT s(10, 6);
    s << "abcdef\x1bPq#1;2;100;0;0!40~\x1b\\";  // 40 px = 4 cells at columns 6..9
    TerminalView view(s.t);
    auto f = view.snapshot();
    CHECK_EQ(f->images.size(), size_t(1));
    s.t.resize(8, 6);  // the line wraps: columns 6..7 stay, 8..9 move to the next row
    f = view.snapshot();
    CHECK_EQ(f->images.size(), size_t(2));
    if (f->images.size() == 2) {
        CHECK_EQ(f->images[0].x, 6.0f);
        CHECK_EQ(f->images[0].w, 2.0f);
        CHECK_EQ(f->images[0].src_x, 0.0f);
        CHECK_EQ(f->images[1].x, 0.0f);
        CHECK_EQ(f->images[1].src_x, 20.0f);
    }
    s.t.resize(10, 6);
    f = view.snapshot();
    CHECK_EQ(f->images.size(), size_t(1));
    // Resized while the alternate screen shows: primary placements keep
    // their place relative to the primary's top.
    IT a(20, 6);
    a << kImg << "\x1b[3;1H" << kitty("a=p,i=1,p=1,q=2,C=1") << "\x1b[?1049h";
    a.t.resize(15, 6);
    a << "\x1b[?1049l";
    p = placement(a.t, 1);
    CHECK(p && p->row == a.t.screen_top_row() + 2);
}

void frames_follow() {
    IT t(20, 6);
    t << kImg << "\x1b[3;3H" << kitty("a=p,i=1,p=1,q=2,C=1");
    TerminalView view(t.t);
    auto f0 = view.snapshot();
    CHECK_EQ(f0->images.size(), size_t(1));
    // Moving a placement damages the rows it left and entered.
    t << "\x1b[5;3H" << kitty("a=p,i=1,p=1,q=2,C=1");
    auto f1 = view.snapshot();
    CHECK(f1->damage[2] == 1 && f1->damage[3] == 1 && f1->damage[4] == 1 && f1->damage[5] == 1);
    CHECK(f1->damage[0] == 0);
    // Unchanged: no damage, same pixels pointer (upload once).
    t << "x";
    auto f2 = view.snapshot();
    CHECK(f2->images[0].pixels == f1->images[0].pixels);
    CHECK(f2->damage[2] == 0 && f2->damage[5] == 0);
    // Frames hold their pixels after the terminal drops the image.
    const ImagePixelsPtr held = f2->images[0].pixels;
    t << kitty("a=d,d=I,i=1");
    auto f3 = view.snapshot();
    CHECK(f3->images.empty());
    CHECK(held->width == 10 && held->rgba.size() == 10 * 40 * 4);
    CHECK(f3->damage[4] == 1);
}

} // namespace

int main() {
    init_test();
    scrolling_and_eviction();
    regions();
    erase_and_screens();
    cell_image_eviction();
    reflow();
    frames_follow();
    return check::finish("test_image_anchor");
}
