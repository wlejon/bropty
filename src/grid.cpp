#include "bropty/grid.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace bropty {

Grid::Grid(int cols, int rows) : cols_(std::max(1, cols)), rows_(std::max(1, rows)) {
    cells_.assign(size_t(cols_) * size_t(rows_), Cell{});
    map_.resize(size_t(rows_));
    meta_.resize(size_t(rows_));
    for (int i = 0; i < rows_; ++i) map_[size_t(i)] = uint32_t(i);
    dirty_.assign(size_t(rows_), 1);
    changed_.assign(size_t(rows_), 1);
    stamp_.assign(size_t(rows_), 0);
    id_ = next_id();
}

uint64_t Grid::next_id() noexcept {
    static std::atomic<uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

void Grid::set_generation(uint64_t base) noexcept {
    for (int y = 0; y < rows_; ++y) {
        if (changed_[size_t(y)]) {
            const uint32_t s = map_[size_t(y)];
            stamp_[s] = base_ + s;
            changed_[size_t(y)] = 0;
        }
    }
    base_ = base;
}

void Grid::rebase(uint64_t base) noexcept {
    std::fill(changed_.begin(), changed_.end(), uint8_t(1));
    base_ = base;
}

ClusterMap& Grid::clusters_mut(int y) {
    auto& m = meta_[map_[size_t(y)]];
    if (!m.clusters) m.clusters = std::make_unique<ClusterMap>();
    return *m.clusters;
}

std::u32string Grid::cluster(int y, int x) const {
    std::u32string out;
    const Cell& c = at(y, x);
    if (c.is_empty() || c.is_spacer()) return out;
    out.push_back(c.cp());
    if (c.has_cluster()) {
        if (const ClusterMap* m = clusters(y)) out.append(m->find(x));
    }
    return out;
}

void Grid::fill(int y, int x0, int x1, Cell fill) {
    x0 = std::max(0, x0);
    x1 = std::min(cols_, x1);
    if (x0 >= x1) return;
    Cell* r = row(y);
    std::fill(r + x0, r + x1, fill);
    auto& m = meta_[map_[size_t(y)]];
    if (m.clusters) m.clusters->erase_range(x0, x1);
    mark_dirty(y);
}

void Grid::clear_row(int y, Cell fill) {
    Cell* r = row(y);
    std::fill(r, r + cols_, fill);
    auto& m = meta_[map_[size_t(y)]];
    m.flags = 0;
    if (m.clusters) m.clusters->clear();
    mark_dirty(y);
}

void Grid::rotate_up(int top, int bottom, int n) {
    if (n <= 0 || top >= bottom + 1) return;
    n = std::min(n, bottom - top + 1);
    if (n == 1) {  // the common case (one linefeed at the bottom): one memmove
        uint32_t first = map_[size_t(top)];
        uint32_t* m = map_.data();
        std::memmove(m + top, m + top + 1, size_t(bottom - top) * sizeof(uint32_t));
        map_[size_t(bottom)] = first;
        uint8_t first_changed = changed_[size_t(top)];
        uint8_t* c = changed_.data();
        std::memmove(c + top, c + top + 1, size_t(bottom - top));
        changed_[size_t(bottom)] = first_changed;
    } else {
        std::rotate(map_.begin() + top, map_.begin() + top + n, map_.begin() + bottom + 1);
        std::rotate(changed_.begin() + top, changed_.begin() + top + n, changed_.begin() + bottom + 1);
    }
    moved(top, bottom);
}

void Grid::rotate_down(int top, int bottom, int n) {
    if (n <= 0 || top >= bottom + 1) return;
    n = std::min(n, bottom - top + 1);
    std::rotate(map_.begin() + top, map_.begin() + (bottom + 1 - n), map_.begin() + bottom + 1);
    std::rotate(changed_.begin() + top, changed_.begin() + (bottom + 1 - n), changed_.begin() + bottom + 1);
    moved(top, bottom);
}

void Grid::copy_span(int sy, int sx0, int sx1, int dy, int dx) {
    if (sx0 >= sx1) return;
    const Cell* s = row(sy);
    Cell* d = row(dy);
    int n = sx1 - sx0;
    // memmove semantics: overlapping same-row copies are safe.
    if (d + dx < s + sx0) {
        std::copy(s + sx0, s + sx1, d + dx);
    } else {
        std::copy_backward(s + sx0, s + sx1, d + dx + n);
    }

    const ClusterMap* sm = clusters(sy);
    std::vector<std::pair<int, std::u32string>> moved;
    if (sm) {
        for (const auto& e : sm->entries()) {
            if (e.first >= sx0 && e.first < sx1) moved.emplace_back(e.first - sx0 + dx, e.second);
        }
    }
    auto& dm = meta_[map_[size_t(dy)]];
    if (dm.clusters) dm.clusters->erase_range(dx, dx + n);
    if (!moved.empty()) {
        ClusterMap& m = clusters_mut(dy);
        for (auto& [c, t] : moved) m.set(c, t);
    }
    mark_dirty(dy);
}

void Grid::resize_crop(int cols, int rows) {
    cols = std::max(1, cols);
    rows = std::max(1, rows);
    std::vector<Cell> cells(size_t(cols) * size_t(rows), Cell{});
    std::vector<Meta> meta(static_cast<size_t>(rows));
    int ncopy = std::min(cols, cols_);
    for (int y = 0; y < std::min(rows, rows_); ++y) {
        const Cell* src = row(y);
        Cell* dst = &cells[size_t(y) * size_t(cols)];
        std::copy_n(src, ncopy, dst);
        // A wide lead cut in half at the new right edge becomes empty.
        if (ncopy < cols_ && ncopy > 0 && dst[ncopy - 1].wide() == Wide::Lead) dst[ncopy - 1] = Cell::blank(dst[ncopy - 1].style);
        Meta& sm = meta_[map_[size_t(y)]];
        meta[size_t(y)].flags = sm.flags;
        if (cols < cols_) meta[size_t(y)].flags &= ~uint32_t(Row_Wrapped);
        if (sm.clusters && !sm.clusters->empty()) {
            sm.clusters->erase_range(ncopy, cols_);
            meta[size_t(y)].clusters = std::move(sm.clusters);
        }
    }
    cols_ = cols;
    rows_ = rows;
    cells_ = std::move(cells);
    meta_ = std::move(meta);
    map_.resize(size_t(rows_));
    for (int i = 0; i < rows_; ++i) map_[size_t(i)] = uint32_t(i);
    dirty_.assign(size_t(rows_), 1);
    changed_.assign(size_t(rows_), 1);
    stamp_.assign(size_t(rows_), 0);
    id_ = next_id();
}

RowView Grid::view(int y, const Style* styles) const noexcept {
    RowView v;
    v.cells = row(y);
    v.cols = cols_;
    v.flags = flags(y);
    v.styles = styles;
    v.clusters = clusters(y);
    return v;
}

void Grid::mark_all_dirty() noexcept { std::fill(dirty_.begin(), dirty_.end(), uint8_t(1)); }
void Grid::clear_dirty() noexcept { std::fill(dirty_.begin(), dirty_.end(), uint8_t(0)); }

} // namespace bropty
