#pragma once
// Internal: the active buffer (history + screen, or the alternate screen) as
// a sequence of logical lines with absolute numbers, and the mapping between
// a line's cells and absolute (row, col) positions.
//
// Two implementations behind one interface:
//
//  * Over a Terminal (buffer_lines_term.cpp), lines come straight from the
//    compact history store and the screen. Line numbering: history line k is
//    line dropped_lines() + k; the screen's lines follow. When the newest
//    history line soft-wraps onto the screen the two halves are one logical
//    line (numbered as the history line). Reflow keeps this sequence (it
//    pops from and pushes to the back of history in order, and drops only
//    from the front), so a line's number survives a resize: that is what
//    lets positions be carried through one (to_line_pos / from_line_pos).
//
//  * Over any other RowSource, lines are assembled from rows by their wrap
//    flags, and a line is numbered by its first row. line(n) accepts any row
//    of the line. Numbers do not survive a resize (can_carry() is false):
//    the source's reflow happened elsewhere.
//
// Either way, walk lines with next_line() / prev_line(), never by +-1.

#include "bropty/position.h"
#include "bropty/row_source.h"
#include "bropty/style.h"
#include "logical_line.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bropty {
class Terminal;
}

namespace bropty::detail {

struct Line {
    int64_t number{-1};
    int64_t first_row{0};
    LogicalLine cells;                // cells (SpacerHeads dropped), cluster tails, semantic flags
    std::vector<Style> palette;       // the styles cells index when the line was remapped
    const Style* styles{nullptr};     // palette.data() or the terminal's StyleTable
    std::vector<uint32_t> row_start;  // first cell of each row, then size()
    bool in_history{false};           // entirely in history (immutable)
    bool missing{false};              // a row the source does not hold reads as empty
    // Hyperlinks: through the source (Style::link is the source's id), or,
    // when local_links, Style::link is a 1-based index into `uris`.
    const RowSource* src{nullptr};
    bool local_links{false};
    std::vector<std::string> uris;

    [[nodiscard]] size_t size() const noexcept { return cells.cells.size(); }
    [[nodiscard]] int rows() const noexcept { return int(row_start.size()) - 1; }
    [[nodiscard]] int64_t end_row() const noexcept { return first_row + rows(); }
    [[nodiscard]] const Cell& cell(size_t i) const noexcept { return cells.cells[i]; }
    [[nodiscard]] const Style& style(size_t i) const noexcept { return styles[cells.cells[i].style]; }
    [[nodiscard]] Zone zone(size_t i) const noexcept { return style(i).zone; }
    // Cluster tail of cell i (empty if none).
    [[nodiscard]] std::u32string_view tail(size_t i) const;
    // URI of a style's link id, or nullptr.
    [[nodiscard]] const std::string* link_uri(uint32_t id) const noexcept;

    // Cell index of a position (a cell, or a boundary: the cell after it),
    // clamped into [0, size()].
    [[nodiscard]] size_t offset_of(RowPos p) const noexcept;
    // Position of cell i (i == size(): just past the content).
    [[nodiscard]] RowPos pos_of(size_t i) const noexcept;
    // The boundary after cell i - 1: the end of a range [.., i).
    [[nodiscard]] RowPos end_pos(size_t i) const noexcept;
    // Widen [a, b) so it does not split a wide character.
    void widen(size_t& a, size_t& b) const noexcept;
    // One past the last cell that is neither empty nor a space.
    [[nodiscard]] size_t content_end() const noexcept;
};

// Whether a cell is an image placeholder (U+10EEEE, kitty placeholders and
// the cells sixel / iTerm2 images are drawn in): part of the picture, not
// of the text.
[[nodiscard]] inline bool is_image_cell(const Cell& c) noexcept { return c.cp() == 0x10EEEE; }

// UTF-8 text of a line with a map back to cells. Empty cells read as spaces;
// trailing empty cells are dropped; spacers contribute nothing.
struct LineText {
    std::string text;
    std::vector<uint32_t> cell_byte;  // byte where cell i starts; size() + 1 entries

    void build(const Line& line);
    // The cell holding byte b.
    [[nodiscard]] size_t cell_at(size_t b) const noexcept;
    // Cells covering bytes [b0, b1), widened over wide characters.
    void cells_of(const Line& line, size_t b0, size_t b1, size_t& c0, size_t& c1) const noexcept;
};

struct LinePos {
    int64_t line{-1};
    size_t offset{0};
};

// Selects BufferLines over a Terminal's primary screen and its history,
// also while the alternate screen is showing (what reflows on a resize).
struct PrimaryTag {};

class BufferLines {
public:
    explicit BufferLines(const RowSource& s) noexcept : s_(s), t_(s.terminal()) {}
    BufferLines(const Terminal& t, PrimaryTag) noexcept;

    // Whether line numbers (and so LinePos) survive a resize of the source.
    [[nodiscard]] bool can_carry() const noexcept { return t_ != nullptr; }

    [[nodiscard]] int64_t first_line() const noexcept;
    [[nodiscard]] int64_t end_line() const;
    // The first line with any row on the screen (the "live" lines start here;
    // every line before it is immutable history).
    [[nodiscard]] int64_t screen_first_line() const;
    [[nodiscard]] int64_t line_number_at_row(int64_t row) const;
    // Semantic flags and first row of a line without decoding it.
    [[nodiscard]] uint32_t line_flags(int64_t number) const;
    [[nodiscard]] int64_t line_first_row(int64_t number) const;
    // The line after / before line `number` (prev of the first line is below
    // first_line(); next of the last is end_line() or beyond).
    [[nodiscard]] int64_t next_line(int64_t number) const;
    [[nodiscard]] int64_t prev_line(int64_t number) const;
    // The same, from a line already read (no lookup).
    [[nodiscard]] int64_t next_line(const Line& l) const noexcept { return t_ ? l.number + 1 : l.end_row(); }
    [[nodiscard]] int64_t prev_line(const Line& l) const noexcept { return t_ ? l.number - 1 : l.first_row - 1; }

    bool line_at_row(int64_t row, Line& out) const;
    bool line(int64_t number, Line& out) const;

    // Positions <-> (line, offset), for carrying positions through a resize.
    [[nodiscard]] LinePos to_line_pos(RowPos p) const;
    [[nodiscard]] RowPos from_line_pos(const LinePos& lp) const;
    // The same for positions that may lie past their line's content (a
    // column beyond its last cell keeps its distance from it), clamped to a
    // cell of `cols` columns on the way back in. For anchors that must stay
    // put through a reflow: images, command marks.
    [[nodiscard]] LinePos carry_out(RowPos p) const;
    [[nodiscard]] RowPos carry_in(const LinePos& lp, int cols) const;

private:
    // The oldest row held (primary mode: history's, whichever screen shows).
    [[nodiscard]] int64_t first_row() const noexcept;
    // Over a Terminal: screen row y of the screen read, and whether that is
    // the alternate screen.
    [[nodiscard]] RowView trow(int y) const noexcept;
    [[nodiscard]] bool alt() const noexcept;
    [[nodiscard]] int64_t tfirst_row() const noexcept;
    // Over a Terminal (buffer_lines_term.cpp).
    [[nodiscard]] bool joint() const noexcept;  // newest history line continues onto the screen
    [[nodiscard]] int64_t screen_base_line() const noexcept;
    [[nodiscard]] int screen_line_start(int y) const noexcept;
    // Screen row where the idx-th screen line starts, or -1.
    [[nodiscard]] int screen_row_of_line(int64_t idx) const noexcept;
    void history_line(size_t k, Line& out) const;
    void screen_line(int y0, Line& out) const;
    void append_screen_rows(int y0, Line& out, bool remap) const;
    [[nodiscard]] int64_t term_end_line() const;
    [[nodiscard]] int64_t term_line_number_at_row(int64_t row) const;
    [[nodiscard]] uint32_t term_line_flags(int64_t number) const;
    [[nodiscard]] int64_t term_line_first_row(int64_t number) const;
    bool term_line_at_row(int64_t row, Line& out) const;
    bool term_line(int64_t number, Line& out) const;

    // Over any other source (buffer_lines.cpp).
    [[nodiscard]] int64_t rows_line_start(int64_t row) const;
    [[nodiscard]] int64_t rows_line_end(int64_t start) const;
    bool rows_line_at_row(int64_t row, Line& out) const;

    const RowSource& s_;
    const Terminal* t_;
    bool primary_{false};
};

// Hash of a row's text and wrap flag (not its styles): equal hashes mean the
// selection over it still reads the same.
uint64_t row_hash(const RowView& v) noexcept;

} // namespace bropty::detail
