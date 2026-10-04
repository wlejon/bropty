#pragma once

#include "bropty/cell.h"
#include <vector>
#include <cstddef>
#include <functional>

namespace bropty {

enum class CursorShape : uint8_t {
    Block = 0,
    Beam = 1,
    Underline = 2
};

struct Cursor {
    int row{0};
    int col{0};
    bool visible{true};
    bool blinking{true};
    CursorShape shape{CursorShape::Block};

    // Stored attributes for character emission
    Cell pen{};
};

class Grid {
public:
    using EvictLineCallback = std::function<void(std::vector<Cell> cells, bool wrapped)>;

    Grid(int cols, int rows);

    [[nodiscard]] int cols() const noexcept { return cols_; }
    [[nodiscard]] int rows() const noexcept { return rows_; }

    // Cell access
    [[nodiscard]] const Cell& get_cell(int row, int col) const;
    [[nodiscard]] Cell& get_cell(int row, int col);
    void set_cell(int row, int col, const Cell& cell);

    // Line access
    [[nodiscard]] std::vector<Cell> get_line(int row) const;
    [[nodiscard]] bool is_line_wrapped(int row) const;
    void set_line_wrapped(int row, bool wrapped);

    // Cursor
    [[nodiscard]] const Cursor& cursor() const noexcept { return cursor_; }
    [[nodiscard]] Cursor& cursor() noexcept { return cursor_; }
    void set_cursor_pos(int row, int col);
    void move_cursor_rel(int drow, int dcol);
    void save_cursor();
    void restore_cursor();

    // Scrolling margins (0-indexed, inclusive)
    [[nodiscard]] int top_margin() const noexcept { return top_margin_; }
    [[nodiscard]] int bottom_margin() const noexcept { return bottom_margin_; }
    void set_margins(int top, int bottom);
    void reset_margins();

    // Damage / Dirty line tracking
    [[nodiscard]] bool has_damage() const noexcept { return damage_count_ > 0; }
    [[nodiscard]] bool is_line_dirty(int row) const;
    void mark_dirty(int row);
    void mark_range_dirty(int start_row, int end_row);
    void mark_all_dirty();
    void clear_damage() noexcept;

    // Output writing
    // Writes a codepoint with pen attributes at cursor position.
    // Handles wide character (width 1 or 2) and auto-wrap.
    void write_char(uint32_t codepoint, uint8_t width, EvictLineCallback evict_cb = nullptr);

    // Erasing operations
    void erase_in_line(int mode);    // 0: cursor to end, 1: start to cursor, 2: whole line
    void erase_in_display(int mode, EvictLineCallback evict_cb = nullptr); // 0: below, 1: above, 2: all
    void erase_chars(int count);     // Erase count chars starting from cursor without shifting

    // Line / Character insertion and deletion
    void insert_lines(int count);
    void delete_lines(int count);
    void insert_chars(int count);
    void delete_chars(int count);

    // Scrolling
    void scroll_up(int count, EvictLineCallback evict_cb = nullptr);
    void scroll_down(int count);

    // Resize grid
    void resize(int new_cols, int new_rows);

    // Modes
    [[nodiscard]] bool auto_wrap() const noexcept { return auto_wrap_; }
    void set_auto_wrap(bool enabled) noexcept { auto_wrap_ = enabled; }

    [[nodiscard]] bool origin_mode() const noexcept { return origin_mode_; }
    void set_origin_mode(bool enabled) noexcept;

private:
    int cols_{80};
    int rows_{24};
    int top_margin_{0};
    int bottom_margin_{23};

    std::vector<Cell> cells_;
    std::vector<bool> wrapped_lines_;
    std::vector<bool> dirty_lines_;
    size_t damage_count_{0};

    Cursor cursor_{};
    Cursor saved_cursor_{};
    bool cursor_saved_{false};

    bool auto_wrap_{true};
    bool wrap_next_{false}; // Pending wrap flag on next printable character
    bool origin_mode_{false};

    [[nodiscard]] size_t cell_index(int row, int col) const noexcept {
        return static_cast<size_t>(row) * static_cast<size_t>(cols_) + static_cast<size_t>(col);
    }

    void clamp_cursor();
};

} // namespace bropty
