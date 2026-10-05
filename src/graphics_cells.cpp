// Images as cells (sixel, iTerm2), and the hooks that keep every image
// anchored to the text: scrolling, history eviction, resize.
#include "bropty/terminal.h"

#include "buffer_lines.h"
#include "graphics_state.h"

#include <algorithm>
#include <cmath>

namespace bropty {

const ImageLayer& Terminal::images(bool alternate) const noexcept { return gfx_->layer(alternate); }
size_t Terminal::image_bytes() const noexcept { return gfx_->total_bytes(); }
uint64_t Terminal::images_version() const noexcept { return gfx_->version(); }

uint64_t Terminal::advance_animations(uint64_t now_ms) {
    bool changed = false;
    const uint64_t next = gfx_->advance(now_ms, changed);
    if (changed) ++change_count_;
    return next;
}

namespace {

// The image id a cell-image cell carries, or 0.
uint32_t cell_image_id(const RowView& v, int x) {
    const Cell& c = v.cells[x];
    if (c.cp() != kImagePlaceholder || !c.has_cluster() || !v.clusters) return 0;
    const std::u32string_view tail = v.clusters->find(x);
    if (tail.size() != 2 || tail[0] < kCellImageRowBase || tail[0] >= kCellImageColBase) return 0;
    const Color fg = v.styles[c.style].fg;
    return fg.is_rgb() ? fg.rgb_value().to_u32() : 0;
}

} // namespace

void Terminal::sweep_cell_images() {
    for (bool alt : {false, true}) {
        if (gfx_->layer(alt).cell_image_count() == 0) continue;
        Screen& s = alt ? alt_ : primary_;
        std::vector<uint32_t> ids;
        for (int y = 0; y < rows_; ++y) {
            const RowView v = s.grid.view(y, styles_.data());
            if (!v.clusters || v.clusters->empty()) continue;
            for (int x = 0; x < v.cols; ++x)
                if (const uint32_t id = cell_image_id(v, x)) ids.push_back(id);
        }
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        gfx_->retain_cell_images(alt, ids, screen_top_row(), end_row());
    }
}

void Terminal::place_cell_image(std::unique_ptr<Image> img, CellCursor mode, bool at_origin) {
    detail::Graphics& g = *gfx_;
    const bool alt = alt_screen_active();
    sweep_cell_images();
    const int box_cols = std::max(1, int(std::ceil(img->cell_width - 1e-4f)));
    const int box_rows = std::max(1, int(std::ceil(img->cell_height - 1e-4f)));
    img->box_cols = box_cols;
    img->box_rows = box_rows;
    std::string error;
    Image* stored = g.store_cell_image(alt, std::move(img), error);
    if (!stored) return;
    const uint32_t id = stored->id;
    image_cells_ = true;

    Style st;
    st.fg = Color::rgb(uint8_t(id >> 16), uint8_t(id >> 8), uint8_t(id));
    st.zone = zone_;
    const uint32_t sid = styles_.intern(st);

    Cursor& c = cur();
    invalidate_print();
    c.pending_wrap = false;
    int r0 = at_origin ? 0 : c.row;
    const int c0 = at_origin ? 0 : c.col;
    const int ncols = std::min(box_cols, cols_ - c0);
    // Scrolling images (sixel without DECSDM, iTerm2) scroll the region as
    // rows are drawn below its bottom; others are clipped at the screen edge.
    const bool scrolls = !at_origin && mode != CellCursor::Stay && r0 >= top_ && r0 <= bottom_;
    const int limit = scrolls ? bottom_ : rows_ - 1;
    int written = 0;
    for (int i = 0; i < box_rows; ++i) {
        int y = r0 + i;
        if (y > limit) {
            if (!scrolls) break;
            scroll_up(y - limit);
            r0 -= y - limit;
            y = limit;
        }
        Grid& gr = grid();
        for (int x = c0; x < c0 + ncols; ++x) clear_wide_at(y, x);
        ClusterMap& cm = gr.clusters_mut(y);
        cm.erase_range(c0, c0 + ncols);
        Cell* row = gr.row(y);
        for (int j = 0; j < ncols; ++j) {
            Cell& cell = row[c0 + j];
            cell = Cell::make(kImagePlaceholder, sid);
            cell.set_cluster(true);
            const char32_t tail[2] = {kCellImageRowBase + char32_t(i), kCellImageColBase + char32_t(j)};
            cm.set(c0 + j, std::u32string_view(tail, 2));
        }
        sanitize_row(y, c0, c0 + ncols);
        gr.mark_dirty(y);
        ++written;
    }
    // The rows drawn (after any scrolling), for eviction and liveness.
    g.add_anchor(alt, id, screen_top_row() + r0, std::max(1, written));
    if (mode == CellCursor::Stay || at_origin) return;
    const int last = r0 + std::max(1, written) - 1;
    int y = last, x;
    if (mode == CellCursor::Sixel) {
        x = c0;
    } else {
        x = c0 + box_cols;
    }
    if (mode == CellCursor::Iterm) {
        if (x >= cols_) {
            x = 0;
            ++y;
        }
    } else if (x > right_) {  // xterm: past the right margin, to the next line's left margin
        x = left_;
        ++y;
    }
    if (scrolls && y > bottom_) {
        scroll_up(y - bottom_);
        y = bottom_;
    }
    c.row = std::clamp(y, 0, rows_ - 1);
    c.col = std::clamp(x, 0, cols_ - 1);
}

void Terminal::graphics_scrolled(int top, int bottom, int left, int right, int n, bool to_history) {
    detail::Graphics& g = *gfx_;
    const bool alt = alt_screen_active();
    const int64_t s = screen_top_row();
    if (to_history) {
        // Rows [0, bottom] keep their numbers (they move up the screen as the
        // numbering moves down); rows below the region stay put on screen, so
        // their numbers grow.
        if (bottom < rows_ - 1) g.shift_rows(alt, s + bottom + 1, n);
        return;
    }
    g.scroll_region(alt, s + top, s + bottom, left, right, n, image_cell_width(), image_cell_height());
}

void Terminal::graphics_after_feed() {
    detail::Graphics& g = *gfx_;
    const int64_t first = int64_t(scrollback_.dropped_rows());
    if (first != g.evicted_before) {
        g.evicted_before = first;
        if (g.anchored(false)) g.evict_before(false, first, image_cell_width(), image_cell_height());
    }
}

namespace {

// A position as (line, cell offset) that may lie past the line's content.
detail::LinePos carry_out(const detail::BufferLines& bl, int64_t row, int col) {
    detail::Line l;
    if (!bl.line_at_row(row, l)) return bl.to_line_pos(RowPos{row, col});
    size_t off = l.offset_of(RowPos{row, col});
    if (off >= l.size()) {
        const RowPos e = l.pos_of(l.size());
        if (e.row == row && col > e.col) off = l.size() + size_t(col - e.col);
    }
    return detail::LinePos{l.number, off};
}

RowPos carry_in(const detail::BufferLines& bl, const detail::LinePos& lp, int cols) {
    detail::Line l;
    if (lp.line < bl.first_line() || !bl.line(lp.line, l)) return bl.from_line_pos(lp);
    RowPos p = l.pos_of(std::min(lp.offset, l.size()));
    if (lp.offset > l.size()) p.col += int(lp.offset - l.size());
    p.col = std::clamp(p.col, 0, cols - 1);
    return p;
}

} // namespace

void Terminal::graphics_resize_begin() {
    carried_anchors_.clear();
    if (alt_screen_active() || !gfx_->anchored(false)) return;
    detail::BufferLines bl(*this);
    gfx_->for_each_anchor(false, [&](int64_t& row, int& col, int* rows) {
        const detail::LinePos a = carry_out(bl, row, col);
        carried_anchors_.push_back(CarriedAnchor{a.line, a.offset, true});
        if (rows) {
            const detail::LinePos b = carry_out(bl, row + std::max(1, *rows) - 1, 0);
            carried_anchors_.push_back(CarriedAnchor{b.line, b.offset, true});
        }
    });
}

void Terminal::graphics_resize_end(int64_t old_screen_top) {
    detail::Graphics& g = *gfx_;
    const int64_t delta = screen_top_row() - old_screen_top;
    // The alternate screen is cropped, not reflowed: its rows keep their
    // screen positions, so their numbers follow the screen's top.
    if (g.anchored(true)) g.shift_rows(true, INT64_MIN, delta);
    if (g.anchored(false)) {
        if (carried_anchors_.empty()) {
            // Resized while the alternate screen was showing: the primary's
            // images keep their place relative to its top.
            g.shift_rows(false, INT64_MIN, delta);
        } else {
            detail::BufferLines bl(*this);
            size_t i = 0;
            g.for_each_anchor(false, [&](int64_t& row, int& col, int* rows) {
                if (i >= carried_anchors_.size()) return;
                const CarriedAnchor a = carried_anchors_[i++];
                const RowPos p = carry_in(bl, detail::LinePos{a.line, a.offset}, cols_);
                row = p.row;
                if (!rows) {
                    col = p.col;
                    return;
                }
                if (i >= carried_anchors_.size()) return;
                const CarriedAnchor b = carried_anchors_[i++];
                const RowPos q = carry_in(bl, detail::LinePos{b.line, b.offset}, cols_);
                *rows = int(std::max<int64_t>(1, q.row - p.row + 1));
            });
        }
        g.evicted_before = int64_t(scrollback_.dropped_rows());
        g.evict_before(false, g.evicted_before, image_cell_width(), image_cell_height());
    }
    carried_anchors_.clear();
}

} // namespace bropty
