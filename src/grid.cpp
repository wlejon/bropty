#include "bropty/grid.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace bropty {

namespace {
const Cell kBlankCell{};
}

Grid::Grid(int cols, int rows)
    : cols_(std::max(1, cols)),
      rows_(std::max(1, rows)),
      top_margin_(0),
      bottom_margin_(rows_ - 1),
      cells_(static_cast<size_t>(cols_) * static_cast<size_t>(rows_), kBlankCell),
      wrapped_lines_(rows_, false),
      dirty_lines_(rows_, true),
      damage_count_(rows_) {}

const Cell& Grid::get_cell(int row, int col) const {
    if (row < 0 || row >= rows_ || col < 0 || col >= cols_) {
        return kBlankCell;
    }
    return cells_[cell_index(row, col)];
}

Cell& Grid::get_cell(int row, int col) {
    if (row < 0 || row >= rows_ || col < 0 || col >= cols_) {
        throw std::out_of_range("Grid cell coordinate out of range");
    }
    return cells_[cell_index(row, col)];
}

void Grid::set_cell(int row, int col, const Cell& cell) {
    if (row < 0 || row >= rows_ || col < 0 || col >= cols_) return;
    cells_[cell_index(row, col)] = cell;
    mark_dirty(row);
}

std::vector<Cell> Grid::get_line(int row) const {
    if (row < 0 || row >= rows_) return {};
    size_t start = static_cast<size_t>(row) * static_cast<size_t>(cols_);
    return std::vector<Cell>(cells_.begin() + start, cells_.begin() + start + cols_);
}

bool Grid::is_line_wrapped(int row) const {
    if (row < 0 || row >= rows_) return false;
    return wrapped_lines_[row];
}

void Grid::set_line_wrapped(int row, bool wrapped) {
    if (row >= 0 && row < rows_) {
        wrapped_lines_[row] = wrapped;
    }
}

void Grid::set_cursor_pos(int row, int col) {
    wrap_next_ = false;
    if (origin_mode_) {
        cursor_.row = top_margin_ + row;
        cursor_.col = col;
    } else {
        cursor_.row = row;
        cursor_.col = col;
    }
    clamp_cursor();
}

void Grid::move_cursor_rel(int drow, int dcol) {
    wrap_next_ = false;
    cursor_.row += drow;
    cursor_.col += dcol;
    clamp_cursor();
}

void Grid::clamp_cursor() {
    int min_row = origin_mode_ ? top_margin_ : 0;
    int max_row = origin_mode_ ? bottom_margin_ : rows_ - 1;
    cursor_.row = std::clamp(cursor_.row, min_row, max_row);
    cursor_.col = std::clamp(cursor_.col, 0, cols_ - 1);
}

void Grid::save_cursor() {
    saved_cursor_ = cursor_;
    cursor_saved_ = true;
}

void Grid::restore_cursor() {
    if (cursor_saved_) {
        cursor_ = saved_cursor_;
        wrap_next_ = false;
        clamp_cursor();
    }
}

void Grid::set_margins(int top, int bottom) {
    if (top < 0) top = 0;
    if (bottom >= rows_) bottom = rows_ - 1;
    if (top < bottom) {
        top_margin_ = top;
        bottom_margin_ = bottom;
        set_cursor_pos(0, 0);
    }
}

void Grid::reset_margins() {
    top_margin_ = 0;
    bottom_margin_ = rows_ - 1;
}

bool Grid::is_line_dirty(int row) const {
    if (row < 0 || row >= rows_) return false;
    return dirty_lines_[row];
}

void Grid::mark_dirty(int row) {
    if (row >= 0 && row < rows_ && !dirty_lines_[row]) {
        dirty_lines_[row] = true;
        damage_count_++;
    }
}

void Grid::mark_range_dirty(int start_row, int end_row) {
    start_row = std::max(0, start_row);
    end_row = std::min(rows_ - 1, end_row);
    for (int r = start_row; r <= end_row; ++r) {
        mark_dirty(r);
    }
}

void Grid::mark_all_dirty() {
    std::fill(dirty_lines_.begin(), dirty_lines_.end(), true);
    damage_count_ = rows_;
}

void Grid::clear_damage() noexcept {
    std::fill(dirty_lines_.begin(), dirty_lines_.end(), false);
    damage_count_ = 0;
}

void Grid::write_char(uint32_t codepoint, uint8_t width, EvictLineCallback evict_cb) {
    if (width == 0) return;

    if (wrap_next_) {
        if (auto_wrap_) {
            wrapped_lines_[cursor_.row] = true;
            if (cursor_.row == bottom_margin_) {
                scroll_up(1, evict_cb);
            } else if (cursor_.row + 1 < rows_) {
                cursor_.row++;
            }
            cursor_.col = 0;
        }
        wrap_next_ = false;
    }

    // Wide character wrapping if only 1 col left on current line
    if (width == 2 && cursor_.col >= cols_ - 1) {
        if (auto_wrap_) {
            // Clear current cell before wrapping
            Cell empty_cell = cursor_.pen;
            empty_cell.codepoint = ' ';
            cells_[cell_index(cursor_.row, cursor_.col)] = empty_cell;
            mark_dirty(cursor_.row);

            wrapped_lines_[cursor_.row] = true;
            if (cursor_.row == bottom_margin_) {
                scroll_up(1, evict_cb);
            } else if (cursor_.row + 1 < rows_) {
                cursor_.row++;
            }
            cursor_.col = 0;
        }
    }

    Cell lead = cursor_.pen;
    lead.codepoint = codepoint;
    lead.width = width;
    if (width == 2) {
        lead.set_flag(CellFlag_WideLead, true);
    }

    cells_[cell_index(cursor_.row, cursor_.col)] = lead;
    mark_dirty(cursor_.row);

    if (width == 2 && cursor_.col + 1 < cols_) {
        Cell trail = cursor_.pen;
        trail.codepoint = ' ';
        trail.width = 0;
        trail.set_flag(CellFlag_WideTrail, true);
        cells_[cell_index(cursor_.row, cursor_.col + 1)] = trail;
    }

    cursor_.col += width;
    if (cursor_.col >= cols_) {
        cursor_.col = cols_ - 1;
        wrap_next_ = true;
    }
}

void Grid::erase_in_line(int mode) {
    int start_col = 0;
    int end_col = cols_ - 1;

    if (mode == 0) {
        start_col = cursor_.col;
    } else if (mode == 1) {
        end_col = cursor_.col;
    } else if (mode == 2) {
        wrapped_lines_[cursor_.row] = false;
    }

    Cell erase_cell = cursor_.pen;
    erase_cell.codepoint = ' ';
    erase_cell.flags = CellFlag_None;

    for (int c = start_col; c <= end_col; ++c) {
        cells_[cell_index(cursor_.row, c)] = erase_cell;
    }
    mark_dirty(cursor_.row);
}

void Grid::erase_in_display(int mode, EvictLineCallback evict_cb) {
    if (mode == 0) { // Below cursor
        erase_in_line(0);
        for (int r = cursor_.row + 1; r < rows_; ++r) {
            cursor_.row = r;
            erase_in_line(2);
        }
    } else if (mode == 1) { // Above cursor
        for (int r = 0; r < cursor_.row; ++r) {
            cursor_.row = r;
            erase_in_line(2);
        }
        erase_in_line(1);
    } else if (mode == 2 || mode == 3) { // Whole display
        Cell erase_cell = cursor_.pen;
        erase_cell.codepoint = ' ';
        erase_cell.flags = CellFlag_None;
        std::fill(cells_.begin(), cells_.end(), erase_cell);
        std::fill(wrapped_lines_.begin(), wrapped_lines_.end(), false);
        mark_all_dirty();
    }
}

void Grid::erase_chars(int count) {
    int start_col = cursor_.col;
    int end_col = std::min(cols_ - 1, cursor_.col + count - 1);

    Cell erase_cell = cursor_.pen;
    erase_cell.codepoint = ' ';
    erase_cell.flags = CellFlag_None;

    for (int c = start_col; c <= end_col; ++c) {
        cells_[cell_index(cursor_.row, c)] = erase_cell;
    }
    mark_dirty(cursor_.row);
}

void Grid::insert_lines(int count) {
    if (cursor_.row < top_margin_ || cursor_.row > bottom_margin_) return;
    count = std::min(count, bottom_margin_ - cursor_.row + 1);

    for (int r = bottom_margin_; r >= cursor_.row + count; --r) {
        size_t dst = cell_index(r, 0);
        size_t src = cell_index(r - count, 0);
        std::copy_n(cells_.begin() + src, cols_, cells_.begin() + dst);
        wrapped_lines_[r] = wrapped_lines_[r - count];
        mark_dirty(r);
    }

    Cell blank = cursor_.pen;
    blank.codepoint = ' ';
    blank.flags = CellFlag_None;

    for (int r = cursor_.row; r < cursor_.row + count; ++r) {
        size_t dst = cell_index(r, 0);
        std::fill_n(cells_.begin() + dst, cols_, blank);
        wrapped_lines_[r] = false;
        mark_dirty(r);
    }
}

void Grid::delete_lines(int count) {
    if (cursor_.row < top_margin_ || cursor_.row > bottom_margin_) return;
    count = std::min(count, bottom_margin_ - cursor_.row + 1);

    for (int r = cursor_.row; r <= bottom_margin_ - count; ++r) {
        size_t dst = cell_index(r, 0);
        size_t src = cell_index(r + count, 0);
        std::copy_n(cells_.begin() + src, cols_, cells_.begin() + dst);
        wrapped_lines_[r] = wrapped_lines_[r + count];
        mark_dirty(r);
    }

    Cell blank = cursor_.pen;
    blank.codepoint = ' ';
    blank.flags = CellFlag_None;

    for (int r = bottom_margin_ - count + 1; r <= bottom_margin_; ++r) {
        size_t dst = cell_index(r, 0);
        std::fill_n(cells_.begin() + dst, cols_, blank);
        wrapped_lines_[r] = false;
        mark_dirty(r);
    }
}

void Grid::insert_chars(int count) {
    count = std::min(count, cols_ - cursor_.col);
    if (count <= 0) return;

    size_t row_start = cell_index(cursor_.row, 0);
    for (int c = cols_ - 1; c >= cursor_.col + count; --c) {
        cells_[row_start + c] = cells_[row_start + c - count];
    }

    Cell blank = cursor_.pen;
    blank.codepoint = ' ';
    blank.flags = CellFlag_None;

    for (int c = cursor_.col; c < cursor_.col + count; ++c) {
        cells_[row_start + c] = blank;
    }
    mark_dirty(cursor_.row);
}

void Grid::delete_chars(int count) {
    count = std::min(count, cols_ - cursor_.col);
    if (count <= 0) return;

    size_t row_start = cell_index(cursor_.row, 0);
    for (int c = cursor_.col; c < cols_ - count; ++c) {
        cells_[row_start + c] = cells_[row_start + c + count];
    }

    Cell blank = cursor_.pen;
    blank.codepoint = ' ';
    blank.flags = CellFlag_None;

    for (int c = cols_ - count; c < cols_; ++c) {
        cells_[row_start + c] = blank;
    }
    mark_dirty(cursor_.row);
}

void Grid::scroll_up(int count, EvictLineCallback evict_cb) {
    if (count <= 0) return;
    int margin_height = bottom_margin_ - top_margin_ + 1;
    count = std::min(count, margin_height);

    for (int i = 0; i < count; ++i) {
        int r = top_margin_ + i;
        if (top_margin_ == 0 && evict_cb) {
            evict_cb(get_line(r), wrapped_lines_[r]);
        }
    }

    for (int r = top_margin_; r <= bottom_margin_ - count; ++r) {
        size_t dst = cell_index(r, 0);
        size_t src = cell_index(r + count, 0);
        std::copy_n(cells_.begin() + src, cols_, cells_.begin() + dst);
        wrapped_lines_[r] = wrapped_lines_[r + count];
        mark_dirty(r);
    }

    Cell blank = cursor_.pen;
    blank.codepoint = ' ';
    blank.flags = CellFlag_None;

    for (int r = bottom_margin_ - count + 1; r <= bottom_margin_; ++r) {
        size_t dst = cell_index(r, 0);
        std::fill_n(cells_.begin() + dst, cols_, blank);
        wrapped_lines_[r] = false;
        mark_dirty(r);
    }
}

void Grid::scroll_down(int count) {
    if (count <= 0) return;
    int margin_height = bottom_margin_ - top_margin_ + 1;
    count = std::min(count, margin_height);

    for (int r = bottom_margin_; r >= top_margin_ + count; --r) {
        size_t dst = cell_index(r, 0);
        size_t src = cell_index(r - count, 0);
        std::copy_n(cells_.begin() + src, cols_, cells_.begin() + dst);
        wrapped_lines_[r] = wrapped_lines_[r - count];
        mark_dirty(r);
    }

    Cell blank = cursor_.pen;
    blank.codepoint = ' ';
    blank.flags = CellFlag_None;

    for (int r = top_margin_; r < top_margin_ + count; ++r) {
        size_t dst = cell_index(r, 0);
        std::fill_n(cells_.begin() + dst, cols_, blank);
        wrapped_lines_[r] = false;
        mark_dirty(r);
    }
}

void Grid::set_origin_mode(bool enabled) noexcept {
    origin_mode_ = enabled;
    set_cursor_pos(0, 0);
}

void Grid::resize(int new_cols, int new_rows) {
    new_cols = std::max(1, new_cols);
    new_rows = std::max(1, new_rows);

    if (new_cols == cols_ && new_rows == rows_) {
        return;
    }

    std::vector<Cell> new_cells(static_cast<size_t>(new_cols) * static_cast<size_t>(new_rows), kBlankCell);
    std::vector<bool> new_wrapped(new_rows, false);

    int copy_rows = std::min(rows_, new_rows);
    int copy_cols = std::min(cols_, new_cols);

    for (int r = 0; r < copy_rows; ++r) {
        size_t old_start = static_cast<size_t>(r) * static_cast<size_t>(cols_);
        size_t new_start = static_cast<size_t>(r) * static_cast<size_t>(new_cols);
        std::copy_n(cells_.begin() + old_start, copy_cols, new_cells.begin() + new_start);
        new_wrapped[r] = wrapped_lines_[r];
    }

    cols_ = new_cols;
    rows_ = new_rows;
    cells_ = std::move(new_cells);
    wrapped_lines_ = std::move(new_wrapped);

    top_margin_ = 0;
    bottom_margin_ = rows_ - 1;

    dirty_lines_.assign(rows_, true);
    damage_count_ = rows_;

    clamp_cursor();
}

} // namespace bropty
