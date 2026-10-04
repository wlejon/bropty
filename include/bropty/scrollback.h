#pragma once

#include "bropty/cell.h"
#include <deque>
#include <vector>
#include <cstddef>

namespace bropty {

struct ScrollbackLine {
    std::vector<Cell> cells;
    bool wrapped{false}; // True if automatically wrapped to the next line

    ScrollbackLine() = default;
    ScrollbackLine(std::vector<Cell> c, bool w) : cells(std::move(c)), wrapped(w) {}
};

class ScrollbackBuffer {
public:
    explicit ScrollbackBuffer(size_t max_lines = 10000);

    // Push a line evicted from the top of the grid
    void push_line(std::vector<Cell> cells, bool wrapped);

    // Pop the most recent line (used when scrolling back down or moving lines back to grid during resize)
    bool pop_line(std::vector<Cell>& out_cells, bool& out_wrapped);

    [[nodiscard]] size_t size() const noexcept { return lines_.size(); }
    [[nodiscard]] size_t max_lines() const noexcept { return max_lines_; }
    void set_max_lines(size_t max_lines);

    [[nodiscard]] bool empty() const noexcept { return lines_.empty(); }
    void clear() noexcept;

    // Direct access (0 = oldest, size() - 1 = most recent)
    [[nodiscard]] const ScrollbackLine& get_line(size_t index) const;
    [[nodiscard]] ScrollbackLine& get_line(size_t index);

    // Safe cell access with default blank fallback
    [[nodiscard]] const Cell& get_cell(size_t line_index, size_t col) const;

    // Reflow all lines when terminal column count changes
    void reflow(int new_cols);

private:
    size_t max_lines_{10000};
    std::deque<ScrollbackLine> lines_;
};

} // namespace bropty
