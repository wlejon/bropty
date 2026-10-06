// Host settings on a running Terminal: the history capacity
// (set_scrollback_rows) and the default cursor style DECSCUSR 0 and RIS
// return to (set_default_cursor_style).
#include "term_helpers.h"

#include <string>

using namespace bropty;
using th::T;

namespace {

void scrollback_rows() {
    T t(10, 2, 100);
    CHECK_EQ(t.t.scrollback_rows(), size_t(100));
    for (int i = 0; i < 50; ++i) t << "line" + std::to_string(i) + "\r\n";
    // 51 rows written to a 2-row screen: 49 in history.
    CHECK_EQ(t.t.history_rows(), size_t(49));
    const int64_t first = t.t.history_first_row();
    const uint64_t changes = t.t.change_count();

    // Lower: the oldest go at once.
    t.t.set_scrollback_rows(10);
    CHECK_EQ(t.t.scrollback_rows(), size_t(10));
    CHECK_EQ(t.t.history_rows(), size_t(10));
    CHECK_EQ(t.t.history_text(0), std::string("line39"));
    CHECK_EQ(t.t.history_first_row(), first + 39);
    CHECK(t.t.change_count() != changes);

    // The new capacity holds as output continues.
    for (int i = 50; i < 70; ++i) t << "line" + std::to_string(i) + "\r\n";
    CHECK_EQ(t.t.history_rows(), size_t(10));
    CHECK_EQ(t.t.history_text(9), std::string("line68"));

    // Raise: nothing comes back, but history grows to it again.
    t.t.set_scrollback_rows(1000);
    CHECK_EQ(t.t.history_rows(), size_t(10));
    for (int i = 70; i < 90; ++i) t << "line" + std::to_string(i) + "\r\n";
    CHECK_EQ(t.t.history_rows(), size_t(30));

    // Zero: no history at all.
    t.t.set_scrollback_rows(0);
    CHECK_EQ(t.t.history_rows(), size_t(0));
    t << "more\r\nmore\r\nmore\r\n";
    CHECK_EQ(t.t.history_rows(), size_t(0));
}

void scrollback_trims_commands() {
    T t(20, 2, 100);
    t << "\x1b]133;A\x07$ \x1b]133;B\x07old\r\n\x1b]133;C\x07out\r\n\x1b]133;D;0\x07";
    for (int i = 0; i < 20; ++i) t << "x" + std::to_string(i) + "\r\n";
    t << "\x1b]133;A\x07$ ";
    CHECK_EQ(t.t.commands().size(), size_t(2));
    t.t.set_scrollback_rows(2);
    // The first command's rows are gone; its record is trimmed or dropped,
    // and never points before what history still holds.
    for (const CommandRecord& c : t.t.commands()) CHECK(c.prompt.row >= t.t.history_first_row());
}

void default_cursor_style() {
    T t(10, 2);
    CHECK(t.t.cursor().shape == CursorShape::Block);
    CHECK(t.t.cursor().blink);

    // Setting the default applies it now.
    t.t.set_default_cursor_style(CursorShape::Bar, false);
    CHECK(t.t.cursor().shape == CursorShape::Bar);
    CHECK(!t.t.cursor().blink);

    // A program's own style wins until it asks for the default back.
    t << "\x1b[3 q";
    CHECK(t.t.cursor().shape == CursorShape::Underline);
    CHECK(t.t.cursor().blink);
    t << "\x1b[0 q";
    CHECK(t.t.cursor().shape == CursorShape::Bar);
    CHECK(!t.t.cursor().blink);
    t << "\x1b[1 q";
    CHECK(t.t.cursor().shape == CursorShape::Block);
    t << "\x1b[ q";  // no parameter: 0
    CHECK(t.t.cursor().shape == CursorShape::Bar);

    // RIS returns to it too.
    t << "\x1b[4 q\x1b" "c";
    CHECK(t.t.cursor().shape == CursorShape::Bar);
    CHECK(!t.t.cursor().blink);

    // DECRQSS reports the style in effect.
    t.reply();
    t << "\x1bP$q q\x1b\\";
    CHECK_EQ(t.reply(), std::string("\x1bP1$r6 q\x1b\\"));
}

} // namespace

int main() {
    init_test();
    scrollback_rows();
    scrollback_trims_commands();
    default_cursor_style();
    return check::finish("test_terminal_settings");
}
