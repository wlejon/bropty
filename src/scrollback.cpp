#include "bropty/scrollback.h"
#include <algorithm>
#include <stdexcept>

namespace bropty {

namespace {

const Cell kDefaultBlankCell{};

bool is_cell_empty(const Cell& c) {
    return c.is_empty();
}

} // namespace

ScrollbackBuffer::ScrollbackBuffer(size_t max_lines)
    : max_lines_(max_lines) {}

void ScrollbackBuffer::push_line(std::vector<Cell> cells, bool wrapped) {
    if (max_lines_ == 0) return;

    if (lines_.size() >= max_lines_) {
        lines_.pop_front();
    }
    lines_.emplace_back(std::move(cells), wrapped);
}

bool ScrollbackBuffer::pop_line(std::vector<Cell>& out_cells, bool& out_wrapped) {
    if (lines_.empty()) return false;
    out_cells = std::move(lines_.back().cells);
    out_wrapped = lines_.back().wrapped;
    lines_.pop_back();
    return true;
}

void ScrollbackBuffer::set_max_lines(size_t max_lines) {
    max_lines_ = max_lines;
    while (lines_.size() > max_lines_) {
        lines_.pop_front();
    }
}

void ScrollbackBuffer::clear() noexcept {
    lines_.clear();
}

const ScrollbackLine& ScrollbackBuffer::get_line(size_t index) const {
    if (index >= lines_.size()) {
        throw std::out_of_range("Scrollback line index out of range");
    }
    return lines_[index];
}

ScrollbackLine& ScrollbackBuffer::get_line(size_t index) {
    if (index >= lines_.size()) {
        throw std::out_of_range("Scrollback line index out of range");
    }
    return lines_[index];
}

const Cell& ScrollbackBuffer::get_cell(size_t line_index, size_t col) const {
    if (line_index >= lines_.size()) {
        return kDefaultBlankCell;
    }
    const auto& line = lines_[line_index].cells;
    if (col >= line.size()) {
        return kDefaultBlankCell;
    }
    return line[col];
}

void ScrollbackBuffer::reflow(int new_cols) {
    if (new_cols <= 0 || lines_.empty()) {
        return;
    }

    std::deque<ScrollbackLine> new_lines;
    size_t i = 0;
    while (i < lines_.size()) {
        // Collect one logical line (all consecutive wrapped lines until unwrapped line)
        std::vector<Cell> logical_cells;
        bool ends_with_wrap = false;

        while (i < lines_.size()) {
            const auto& line = lines_[i];
            bool line_wrapped = line.wrapped;

            if (line_wrapped) {
                // Whole line is part of logical line
                logical_cells.insert(logical_cells.end(), line.cells.begin(), line.cells.end());
                ++i;
            } else {
                // Last physical line of this logical line
                // Strip trailing empty cells
                auto it_end = line.cells.end();
                while (it_end != line.cells.begin() && is_cell_empty(*(it_end - 1))) {
                    --it_end;
                }
                logical_cells.insert(logical_cells.end(), line.cells.begin(), it_end);
                ends_with_wrap = false;
                ++i;
                break;
            }
        }

        // Now re-slice logical_cells into new_cols chunks
        if (logical_cells.empty()) {
            new_lines.emplace_back(std::vector<Cell>{}, false);
            continue;
        }

        size_t offset = 0;
        while (offset < logical_cells.size()) {
            size_t count = std::min<size_t>(new_cols, logical_cells.size() - offset);
            
            // Check if wide character is cut in half at the end of the line
            if (count == static_cast<size_t>(new_cols) && offset + count < logical_cells.size()) {
                const auto& last_cell = logical_cells[offset + count - 1];
                if (last_cell.has_flag(CellFlag_WideLead)) {
                    // Don't split wide char across lines: leave spacer on next line
                    --count;
                }
            }

            std::vector<Cell> chunk(logical_cells.begin() + offset, logical_cells.begin() + offset + count);
            offset += count;

            bool is_chunk_wrapped = (offset < logical_cells.size()) || ends_with_wrap;
            new_lines.emplace_back(std::move(chunk), is_chunk_wrapped);
        }
    }

    // Enforce max_lines limit
    while (new_lines.size() > max_lines_) {
        new_lines.pop_front();
    }

    lines_ = std::move(new_lines);
}

} // namespace bropty
