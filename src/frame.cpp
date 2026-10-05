// Frames: the lock-free channel, frame helpers, and TerminalView's frame
// building (copy-on-write row snapshots, highlights, damage).
#include "bropty/frame.h"

#include "bropty/view.h"

#include <algorithm>

namespace bropty {

// ---------------------------------------------------------------------------
// FrameChannel

void FrameChannel::publish(std::shared_ptr<const Frame> frame) noexcept {
    slots_[back_] = std::move(frame);  // the frame it replaces is released here, on the writer
    back_ = middle_.exchange(back_ | kFresh, std::memory_order_acq_rel) & 3u;
}

std::shared_ptr<const Frame> FrameChannel::acquire() noexcept {
    if (middle_.load(std::memory_order_acquire) & kFresh)
        front_ = middle_.exchange(front_, std::memory_order_acq_rel) & 3u;
    return slots_[front_];
}

// ---------------------------------------------------------------------------
// Frame

std::pair<size_t, size_t> Frame::highlights_of(int y) const noexcept {
    auto lo = std::lower_bound(highlights.begin(), highlights.end(), y,
                               [](const Highlight& h, int v) { return h.y < v; });
    auto hi = std::upper_bound(lo, highlights.end(), y, [](int v, const Highlight& h) { return v < h.y; });
    return {size_t(lo - highlights.begin()), size_t(hi - highlights.begin())};
}

bool Frame::row_differs(const Frame& drawn, int y) const noexcept {
    if (drawn.rows != rows || drawn.cols != cols || y < 0 || y >= rows) return true;
    if (drawn.palette != palette || drawn.modes.reverse_video != modes.reverse_video) return true;
    if (lines[size_t(y)] != drawn.lines[size_t(y)]) return true;
    auto [a0, a1] = highlights_of(y);
    auto [b0, b1] = drawn.highlights_of(y);
    if (a1 - a0 != b1 - b0 || !std::equal(highlights.begin() + long(a0), highlights.begin() + long(a1),
                                          drawn.highlights.begin() + long(b0)))
        return true;
    if (cursor_y == y || drawn.cursor_y == y) {
        if (cursor_y != drawn.cursor_y || cursor.col != drawn.cursor.col || cursor.visible != drawn.cursor.visible ||
            cursor.shape != drawn.cursor.shape || cursor.blink != drawn.cursor.blink)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// TerminalView frame building

std::shared_ptr<FrameRow> TerminalView::snapshot_row(const RowView& v) {
    auto r = std::make_shared<FrameRow>();
    r->serial = ++serial_;
    if (!v.cells) return r;
    r->cols = v.cols;
    r->flags = v.flags;
    r->cells.assign(v.cells, v.cells + v.cols);
    // Row-local style table. Rows hold a handful of styles, often in runs:
    // map terminal ids to local ones in a stack table (a row has at most
    // `cols` styles), then copy the styles in one allocation.
    constexpr int kStack = 256;
    uint32_t stack_ids[kStack];
    std::vector<uint32_t> heap_ids;
    uint32_t* ids = stack_ids;
    if (v.cols > kStack) {
        heap_ids.resize(size_t(v.cols));
        ids = heap_ids.data();
    }
    uint32_t n = 0, last_id = ~0u, last_local = 0;
    for (Cell& c : r->cells) {
        if (c.style != last_id) {
            last_id = c.style;
            uint32_t k = 0;
            while (k < n && ids[k] != last_id) ++k;
            if (k == n) ids[n++] = last_id;
            last_local = k;
        }
        c.style = last_local;
    }
    r->styles.resize(n);
    for (uint32_t k = 0; k < n; ++k) r->styles[k] = v.styles[ids[k]];
    if (v.clusters) r->clusters = *v.clusters;
    for (const Style& s : r->styles) {
        if (!s.link || r->link_uri(s.link)) continue;
        if (const Hyperlink* h = t_.hyperlink(s.link)) r->links.emplace_back(s.link, h->uri);
    }
    return r;
}

std::shared_ptr<const FrameRow> TerminalView::screen_row(int y) {
    ScreenEntry& e = screen_cache_[t_.row_storage(y)];
    const uint64_t stamp = t_.row_stamp(y);
    if (!e.row || e.stamp != stamp) {
        e.row = snapshot_row(t_.row(y));
        e.stamp = stamp;
    }
    return e.row;
}

std::shared_ptr<const FrameRow> TerminalView::history_row(int64_t row) {
    auto it = history_cache_.find(row);
    if (it != history_cache_.end()) return it->second;
    const RowView v = t_.row_at(row);
    auto r = snapshot_row(v);
    // History rows are immutable except one: the last row of history, when
    // its line continues onto the screen, stops being soft-wrapped if the row
    // that scrolls up after it turns out not to continue it. Keep that one
    // out of the cache.
    if (!(row + 1 == t_.screen_top_row() && v.wrapped())) history_cache_.emplace(row, r);
    return r;
}

void TerminalView::build_highlights(Frame& f) const {
    const int64_t top = f.top_row;
    const int64_t bottom = top + f.rows;  // exclusive
    auto push = [&](const RowRange& r, HighlightKind k) {
        for (int64_t row = std::max(r.start.row, top); row <= r.end.row && row < bottom; ++row) {
            const int c0 = row == r.start.row ? r.start.col : 0;
            const int c1 = std::min(row == r.end.row ? r.end.col : f.cols, f.cols);
            if (c1 > c0) f.highlights.push_back(Highlight{int(row - top), c0, c1, k});
        }
    };
    if (selection_.active()) {
        for (int64_t row = top; row < bottom; ++row) {
            int c0, c1;
            if (selection_.row_span(row, c0, c1))
                f.highlights.push_back(Highlight{int(row - top), c0, std::min(c1, f.cols), HighlightKind::Selection});
        }
    }
    if (search_.active()) {
        const std::optional<RowRange> cur = search_.current();
        for (size_t i = search_.lower_bound_row(top); i < search_.size(); ++i) {
            const RowRange& m = search_.at(i);
            if (m.start.row >= bottom) break;
            push(m, cur && *cur == m ? HighlightKind::CurrentMatch : HighlightKind::Match);
        }
    }
    if (hover_) push(hover_->range, HighlightKind::Hover);
    std::stable_sort(f.highlights.begin(), f.highlights.end(), [](const Highlight& a, const Highlight& b) {
        return a.y != b.y ? a.y < b.y : a.col0 < b.col0;
    });
}

void TerminalView::build_damage(Frame& f) const {
    f.damage.assign(size_t(f.rows), 1);
    if (!last_) return;
    for (int y = 0; y < f.rows; ++y) f.damage[size_t(y)] = f.row_differs(*last_, y) ? 1 : 0;
}

std::shared_ptr<Frame> TerminalView::build() {
    auto f = std::make_shared<Frame>();
    f->seq = ++seq_;
    f->cols = t_.cols();
    f->rows = t_.rows();
    f->top_row = top_row();
    f->first_row = t_.first_row();
    f->screen_top_row = t_.screen_top_row();
    if (t_.grid_id() != screen_grid_ || screen_cache_.size() != size_t(f->rows)) {
        screen_cache_.assign(size_t(f->rows), ScreenEntry{});
        screen_grid_ = t_.grid_id();
    }
    f->lines.resize(size_t(f->rows));
    for (int y = 0; y < f->rows; ++y) {
        const int64_t abs = f->top_row + y;
        f->lines[size_t(y)] = abs >= f->screen_top_row ? screen_row(int(abs - f->screen_top_row)) : history_row(abs);
    }
    // History rows are kept only while in view.
    for (auto it = history_cache_.begin(); it != history_cache_.end();) {
        if (it->first < f->top_row || it->first >= f->top_row + f->rows) it = history_cache_.erase(it);
        else ++it;
    }
    f->cursor = t_.cursor();
    const int64_t crow = f->screen_top_row + f->cursor.row;
    f->cursor_y = (crow >= f->top_row && crow < f->top_row + f->rows) ? int(crow - f->top_row) : -1;
    f->modes = t_.modes();
    f->alt_screen = t_.alt_screen_active();
    const Palette& p = t_.palette();
    if (!palette_ || !(palette_->colors == p.colors && palette_->foreground == p.foreground &&
                       palette_->background == p.background && palette_->cursor == p.cursor))
        palette_ = std::make_shared<const Palette>(p);
    f->palette = palette_;
    build_highlights(*f);
    f->search_active = search_.active();
    f->search_complete = search_.complete();
    f->match_count = search_.size();
    f->current_match = search_.current_index();
    f->selection_active = selection_.active();
    build_damage(*f);
    t_.advance_generation();  // later writes get stamps the cache has not seen
    return f;
}

std::shared_ptr<const Frame> TerminalView::snapshot() {
    sync();
    auto f = build();
    last_ = f;
    last_sig_ = Signature{t_.change_count(), version_, selection_.version(), search_.version()};
    return f;
}

bool TerminalView::publish(FrameChannel& channel, bool only_if_consumed) {
    sync();
    const Signature sig{t_.change_count(), version_, selection_.version(), search_.version()};
    if (last_ && sig == last_sig_) return false;
    if (only_if_consumed && !channel.consumed()) return false;
    auto f = build();
    last_ = f;
    last_sig_ = sig;
    channel.publish(std::move(f));
    return true;
}

} // namespace bropty
