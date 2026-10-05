// TerminalView: viewport, maintenance of selection / search / hover as the
// terminal changes, and the resize hooks. Frame building is in frame.cpp.
#include "bropty/view.h"

#include "buffer_lines.h"

#include <algorithm>

namespace bropty {

using detail::BufferLines;

TerminalView::TerminalView(Terminal& t) : t_(t), selection_(t), search_(t) { t_.add_observer(this); }

TerminalView::~TerminalView() { t_.remove_observer(this); }

int64_t TerminalView::top_row() const noexcept {
    if (follow_ || t_.alt_screen_active()) return t_.screen_top_row();
    return std::clamp(top_, t_.first_row(), t_.screen_top_row());
}

void TerminalView::clamp_viewport() noexcept {
    if (follow_) return;
    if (top_ < t_.first_row()) top_ = t_.first_row();
    if (top_ >= t_.screen_top_row()) follow_ = true;
}

void TerminalView::scroll_by(int64_t rows) { scroll_to_row(top_row() + rows); }

void TerminalView::scroll_to_row(int64_t row) {
    if (t_.alt_screen_active()) return;
    row = std::clamp(row, t_.first_row(), t_.screen_top_row());
    follow_ = row >= t_.screen_top_row();
    top_ = row;
    ++version_;
}

void TerminalView::reveal(RowRange r) {
    if (t_.alt_screen_active()) return;
    const int64_t top = top_row();
    if (r.start.row >= top && r.end.row < top + t_.rows()) return;
    scroll_to_row(r.start.row - t_.rows() / 3);
}

bool TerminalView::scroll_to_prompt(bool backward) {
    if (t_.alt_screen_active()) return false;
    BufferLines bl(t_);
    const int64_t top = top_row();
    const int64_t n = bl.line_number_at_row(top);
    if (backward) {
        int64_t m = bl.line_first_row(n) < top ? n : n - 1;
        for (; m >= bl.first_line(); --m) {
            if (bl.line_flags(m) & Row_Prompt) {
                scroll_to_row(bl.line_first_row(m));
                return true;
            }
        }
        return false;
    }
    const int64_t end = bl.end_line();
    for (int64_t m = n + 1; m < end; ++m) {
        if (bl.line_flags(m) & Row_Prompt) {
            scroll_to_row(bl.line_first_row(m));
            return true;
        }
    }
    return false;
}

std::optional<RowRange> TerminalView::search_next(bool backward) {
    sync();
    const int64_t top = top_row();
    const RowPos from = backward ? RowPos{top + t_.rows(), 0} : RowPos{top, 0};
    std::optional<RowRange> r = search_.next(backward, from);
    if (r) reveal(*r);
    return r;
}

void TerminalView::set_hover(std::optional<RowPos> cell) {
    hover_pos_ = cell;
    hover_ = cell ? link_at(t_, *cell) : std::nullopt;
    ++version_;
}

void TerminalView::sync() {
    if (t_.change_count() == seen_change_) return;
    seen_change_ = t_.change_count();
    clamp_viewport();
    selection_.verify();
    search_.sync();
    if (hover_pos_) {
        std::optional<LinkHit> h = link_at(t_, *hover_pos_);
        const bool same = h.has_value() == hover_.has_value() &&
                          (!h || (h->range == hover_->range && h->target == hover_->target));
        if (!same) {
            hover_ = std::move(h);
            ++version_;
        }
    }
}

void TerminalView::before_resize() {
    selection_.before_resize();
    search_.before_resize();
    if (!follow_ && !t_.alt_screen_active()) {
        BufferLines bl(t_);
        detail::LinePos lp = bl.to_line_pos(RowPos{top_row(), 0});
        carried_top_line_ = lp.line;
        carried_top_offset_ = lp.offset;
    } else {
        follow_ = true;
    }
}

void TerminalView::after_resize() {
    selection_.after_resize();
    search_.after_resize();
    if (!follow_) {
        BufferLines bl(t_);
        top_ = bl.from_line_pos(detail::LinePos{carried_top_line_, carried_top_offset_}).row;
        clamp_viewport();
    }
    hover_pos_.reset();
    hover_.reset();
    history_cache_.clear();
    seen_change_ = ~0ull;
    ++version_;
}

void TerminalView::screen_switched() {
    selection_.clear();
    search_.screen_switched();
    hover_pos_.reset();
    hover_.reset();
    history_cache_.clear();
    ++version_;
}

} // namespace bropty
