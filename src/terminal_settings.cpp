// Host settings changed on a running Terminal: the history capacity and the
// default cursor style.
#include "bropty/terminal.h"

namespace bropty {

void Terminal::set_scrollback_rows(size_t rows) {
    ++change_count_;
    opts_.scrollback_rows = rows;
    scrollback_.set_max_rows(rows);
    // What eviction by output does after a feed: images anchored to the
    // dropped rows go, command records are trimmed to what is left, and
    // hyperlinks no row refers to any more are collected.
    graphics_after_feed();
    if (history_first_row() != commands_first_row_) commands_trim();
    maybe_collect_garbage();
    ++change_count_;
}

void Terminal::set_default_cursor_style(CursorShape shape, bool blink) {
    ++change_count_;
    default_cursor_shape_ = shape;
    default_cursor_blink_ = blink;
    cursor_shape_ = shape;
    cursor_shape_blink_ = blink;
}

} // namespace bropty
