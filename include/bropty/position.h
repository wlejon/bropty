#pragma once
// Positions in a terminal's buffer that stay attached to their text.
//
// Rows are numbered absolutely: history row i of the primary screen is row
// Terminal::first_row() + i and screen row y is Terminal::screen_top_row() + y.
// A row keeps its number while it scrolls from the screen into history and
// for as long as it stays there; numbers are never reused for other text
// except across a resize, where everything that holds a RowPos (selection,
// search matches, the viewport) is carried to the same characters by the
// terminal's reflow (see TerminalObserver). Rows evicted from the front of
// history fall below first_row().
//
// A column is either a cell (0 <= col < cols) or, where a type says so, a
// boundary between cells (0 <= col <= cols, boundary b lies before cell b).

#include <compare>
#include <cstdint>

namespace bropty {

struct RowPos {
    int64_t row{0};
    int col{0};

    constexpr auto operator<=>(const RowPos&) const noexcept = default;
};

// A stream range between two boundaries, [start, end) in reading order.
struct RowRange {
    RowPos start;
    RowPos end;

    [[nodiscard]] constexpr bool empty() const noexcept { return !(start < end); }
    // Whether cell (row, col) lies inside.
    [[nodiscard]] constexpr bool contains(int64_t row, int col) const noexcept {
        RowPos p{row, col};
        return !(p < start) && p < end;
    }
    constexpr bool operator==(const RowRange&) const noexcept = default;
};

} // namespace bropty
