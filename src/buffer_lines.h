#pragma once
// Internal: the active buffer (history + screen, or the alternate screen) as
// a sequence of logical lines with absolute numbers, and the mapping between
// a line's cells and absolute (row, col) positions.
//
// Line numbering: history line k is line dropped_lines() + k; the screen's
// lines follow. When the newest history line soft-wraps onto the screen the
// two halves are one logical line (numbered as the history line). Reflow
// keeps this sequence (it pops from and pushes to the back of history in
// order, and drops only from the front), so a line's number survives a
// resize: that is what lets positions be carried through one (to_line_pos /
// from_line_pos). Lines that are entirely in history are immutable.

#include "bropty/position.h"
#include "bropty/terminal.h"
#include "logical_line.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bropty::detail {

struct Line {
    int64_t number{-1};
    int64_t first_row{0};
    LogicalLine cells;                // cells (SpacerHeads dropped), cluster tails, semantic flags
    std::vector<Style> palette;       // the styles cells index when the line came from history
    const Style* styles{nullptr};     // palette.data() or the terminal's StyleTable
    std::vector<uint32_t> row_start;  // first cell of each row, then size()
    bool in_history{false};           // entirely in history (immutable)

    [[nodiscard]] size_t size() const noexcept { return cells.cells.size(); }
    [[nodiscard]] int rows() const noexcept { return int(row_start.size()) - 1; }
    [[nodiscard]] int64_t end_row() const noexcept { return first_row + rows(); }
    [[nodiscard]] const Cell& cell(size_t i) const noexcept { return cells.cells[i]; }
    [[nodiscard]] const Style& style(size_t i) const noexcept { return styles[cells.cells[i].style]; }
    [[nodiscard]] Zone zone(size_t i) const noexcept { return style(i).zone; }
    // Cluster tail of cell i (empty if none).
    [[nodiscard]] std::u32string_view tail(size_t i) const;

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

class BufferLines {
public:
    explicit BufferLines(const Terminal& t) : t_(t) {}

    [[nodiscard]] int64_t first_line() const noexcept;
    [[nodiscard]] int64_t end_line() const;
    // The first line with any row on the screen (the "live" lines start here;
    // every line before it is immutable history).
    [[nodiscard]] int64_t screen_first_line() const noexcept;
    [[nodiscard]] int64_t line_number_at_row(int64_t row) const;
    // Semantic flags and first row of a line without decoding it.
    [[nodiscard]] uint32_t line_flags(int64_t number) const;
    [[nodiscard]] int64_t line_first_row(int64_t number) const;

    bool line_at_row(int64_t row, Line& out) const;
    bool line(int64_t number, Line& out) const;

    // Positions <-> (line, offset), for carrying positions through a resize.
    [[nodiscard]] LinePos to_line_pos(RowPos p) const;
    [[nodiscard]] RowPos from_line_pos(const LinePos& lp) const;

private:
    [[nodiscard]] bool joint() const noexcept;  // newest history line continues onto the screen
    [[nodiscard]] int64_t screen_base_line() const noexcept;
    [[nodiscard]] int screen_line_start(int y) const noexcept;
    // Screen row where the idx-th screen line starts, or -1.
    [[nodiscard]] int screen_row_of_line(int64_t idx) const noexcept;
    void history_line(size_t k, Line& out) const;
    void screen_line(int y0, Line& out) const;
    void append_screen_rows(int y0, Line& out, bool remap) const;

    const Terminal& t_;
};

// Hash of a row's text and wrap flag (not its styles): equal hashes mean the
// selection over it still reads the same.
uint64_t row_hash(const RowView& v) noexcept;

} // namespace bropty::detail
