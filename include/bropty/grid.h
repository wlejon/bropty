#pragma once
// The visible screen: rows x cols cells in one contiguous allocation, with a
// row indirection table so scrolling rotates row indices instead of moving
// cells. Per-row metadata (flags, cluster tails) travels with its storage row.
// Dirty bits are per *screen position*: a renderer re-reads exactly the rows
// whose bit is set and clears them with clear_dirty().

#include "bropty/cell.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace bropty {

class Grid {
public:
    Grid(int cols, int rows);

    [[nodiscard]] int cols() const noexcept { return cols_; }
    [[nodiscard]] int rows() const noexcept { return rows_; }

    [[nodiscard]] Cell* row(int y) noexcept { return &cells_[size_t(map_[size_t(y)]) * size_t(cols_)]; }
    [[nodiscard]] const Cell* row(int y) const noexcept { return &cells_[size_t(map_[size_t(y)]) * size_t(cols_)]; }
    [[nodiscard]] Cell& at(int y, int x) noexcept { return row(y)[x]; }
    [[nodiscard]] const Cell& at(int y, int x) const noexcept { return row(y)[x]; }

    [[nodiscard]] uint32_t flags(int y) const noexcept { return meta_[map_[size_t(y)]].flags; }
    void set_flags(int y, uint32_t f) noexcept {
        meta_[map_[size_t(y)]].flags = f;
        changed_[size_t(y)] = 1;
    }
    void set_flag(int y, uint32_t f, bool on) noexcept {
        uint32_t& v = meta_[map_[size_t(y)]].flags;
        v = on ? (v | f) : (v & ~f);
        changed_[size_t(y)] = 1;
    }
    [[nodiscard]] bool wrapped(int y) const noexcept { return (flags(y) & Row_Wrapped) != 0; }

    [[nodiscard]] const ClusterMap* clusters(int y) const noexcept { return meta_[map_[size_t(y)]].clusters.get(); }
    ClusterMap& clusters_mut(int y);
    // The cluster text at (y, x): base code point plus any tail.
    [[nodiscard]] std::u32string cluster(int y, int x) const;

    // Fill [x0, x1) of row y with `fill`, dropping any cluster tails there.
    void fill(int y, int x0, int x1, Cell fill);
    // Reset row y entirely (cells, flags, clusters).
    void clear_row(int y, Cell fill);

    // Rotate rows [top, bottom] up by n: rows top..top+n-1 move (as storage)
    // to the bottom of the range. The caller clears the n recycled rows.
    void rotate_up(int top, int bottom, int n);
    void rotate_down(int top, int bottom, int n);

    // Copy one row's cells/clusters [x0, x1) from (sy) to (dy) at dx (used for
    // horizontal-margin scrolling, where whole-row rotation is not possible).
    void copy_span(int sy, int sx0, int sx1, int dy, int dx);

    // Resize without reflow: crop or pad on the right and bottom.
    void resize_crop(int cols, int rows);

    [[nodiscard]] RowView view(int y, const Style* styles) const noexcept;

    // Damage tracking. Dirty bits are per screen position (a scroll dirties
    // every row it moves). Separately, each row has a content stamp that
    // follows it as it scrolls and changes only when its content does: a
    // write sets a per-position "changed" byte (rotated with the rows), and
    // set_generation() folds those into per-storage-row stamps. Writes cost
    // two byte stores; a scroll one small memmove.
    void mark_dirty(int y) noexcept {
        dirty_[size_t(y)] = 1;
        changed_[size_t(y)] = 1;
    }
    void mark_dirty(int y0, int y1) noexcept {  // [y0, y1]
        for (int y = y0; y <= y1; ++y) mark_dirty(y);
    }
    void mark_all_dirty() noexcept;
    [[nodiscard]] bool dirty(int y) const noexcept { return dirty_[size_t(y)] != 0; }
    void clear_dirty() noexcept;
    // Content stamp of row y. A generation owns the stamps [base, base +
    // rows()): rows changed since the last set_generation() read as
    // base + their storage index, so every stamp handed out is unique as
    // long as the owner never gives two generations overlapping ranges.
    [[nodiscard]] uint64_t stamp(int y) const noexcept {
        const uint32_t s = map_[size_t(y)];
        return changed_[size_t(y)] ? base_ + s : stamp_[s];
    }
    // The storage row shown at y (stable while the row scrolls).
    [[nodiscard]] uint32_t storage(int y) const noexcept { return map_[size_t(y)]; }
    // Fold pending changes into the stamps (the current generation's), then
    // start a generation owning [base, base + rows()).
    void set_generation(uint64_t base) noexcept;
    // Start over at `base`: every row reads as changed (after a rebuild).
    void rebase(uint64_t base) noexcept;
    // Unique per storage allocation: changes when the grid is rebuilt.
    [[nodiscard]] uint64_t id() const noexcept { return id_; }

    // Visit every cell (for style mark-and-sweep).
    template <class F>
    void for_each_cell(F&& f) const {
        for (const Cell& c : cells_) f(c);
    }

private:
    struct Meta {
        uint32_t flags{0};
        std::unique_ptr<ClusterMap> clusters;
    };

    int cols_;
    int rows_;
    std::vector<Cell> cells_;
    std::vector<uint32_t> map_;
    std::vector<Meta> meta_;
    std::vector<uint8_t> dirty_;
    std::vector<uint8_t> changed_;  // per screen position, rotated with map_ (so it follows the row)
    std::vector<uint64_t> stamp_;   // per storage row
    uint64_t base_{1};
    uint64_t id_{0};

    void moved(int y0, int y1) noexcept {  // positions [y0, y1] show other rows now
        for (int y = y0; y <= y1; ++y) dirty_[size_t(y)] = 1;
    }
    static uint64_t next_id() noexcept;
};

} // namespace bropty
