// BufferLines over a Terminal: logical lines straight from the compact
// history store and the screen, numbered so that they survive a reflow.
// In primary mode (BufferLines(t, PrimaryTag{})) the screen is the primary
// one even while the alternate screen shows.
#include "buffer_lines.h"

#include "bropty/terminal.h"

#include <algorithm>

namespace bropty::detail {

BufferLines::BufferLines(const Terminal& t, PrimaryTag) noexcept : s_(t), t_(&t), primary_(true) {}

RowView BufferLines::trow(int y) const noexcept { return primary_ ? t_->primary_row(y) : t_->row(y); }

bool BufferLines::alt() const noexcept { return !primary_ && t_->alt_screen_active(); }

int64_t BufferLines::tfirst_row() const noexcept { return primary_ ? t_->history_first_row() : t_->first_row(); }

bool BufferLines::joint() const noexcept {
    return !alt() && t_->scrollback().last_continued();
}

int64_t BufferLines::screen_base_line() const noexcept {
    const Scrollback& sb = t_->scrollback();
    return int64_t(sb.dropped_lines() + sb.lines()) - (joint() ? 1 : 0);
}

int64_t BufferLines::term_end_line() const {
    int64_t n = 1;
    for (int y = 1; y < t_->rows(); ++y)
        if (!trow(y - 1).wrapped()) ++n;
    return screen_base_line() + n;
}

int BufferLines::screen_line_start(int y) const noexcept {
    while (y > 0 && trow(y - 1).wrapped()) --y;
    return y;
}

int64_t BufferLines::term_line_number_at_row(int64_t row) const {
    const Terminal& t = *t_;
    const int64_t top = t.screen_top_row();
    if (row < top) {
        if (alt() || row < tfirst_row()) return first_line() - 1;
        const Scrollback& sb = t.scrollback();
        return int64_t(sb.dropped_lines() + sb.line_of_row(size_t(row - tfirst_row())));
    }
    int y = int(std::min<int64_t>(row - top, t.rows() - 1));
    int64_t n = 0;
    for (int k = 1; k <= y; ++k)
        if (!trow(k - 1).wrapped()) ++n;
    return screen_base_line() + n;
}

int64_t BufferLines::term_line_first_row(int64_t number) const {
    const Terminal& t = *t_;
    const Scrollback& sb = t.scrollback();
    if (!alt()) {
        const int64_t k = number - int64_t(sb.dropped_lines());
        if (k < 0) return tfirst_row();
        if (k < int64_t(sb.lines())) return int64_t(sb.dropped_rows() + sb.line_first_row(size_t(k)));
    }
    const int y = screen_row_of_line(number - screen_base_line());
    return y < 0 ? (number < screen_base_line() ? tfirst_row() : t.end_row()) : t.screen_top_row() + y;
}

int BufferLines::screen_row_of_line(int64_t idx) const noexcept {
    if (idx < 0) return -1;
    int64_t cur = 0;
    for (int y = 0; y < t_->rows(); ++y) {
        if (y > 0 && !trow(y - 1).wrapped()) ++cur;
        if (cur == idx) return y;
    }
    return -1;
}

uint32_t BufferLines::term_line_flags(int64_t number) const {
    const Terminal& t = *t_;
    const Scrollback& sb = t.scrollback();
    if (!alt()) {
        const int64_t k = number - int64_t(sb.dropped_lines());
        if (k < 0) return 0;
        if (k < int64_t(sb.lines()) && !(joint() && k + 1 == int64_t(sb.lines()))) return sb.line_flags(size_t(k));
    }
    const int64_t row = term_line_first_row(number);
    uint32_t flags = 0;
    if (row < t.screen_top_row()) {
        flags = sb.line_flags(sb.lines() - 1);
        for (int y = 0; y < t.rows(); ++y) {
            flags |= trow(y).flags & Row_SemanticMask;
            if (!trow(y).wrapped()) break;
        }
        return flags;
    }
    for (int y = int(row - t.screen_top_row()); y < t.rows(); ++y) {
        flags |= trow(y).flags & Row_SemanticMask;
        if (!trow(y).wrapped()) break;
    }
    return flags;
}

void BufferLines::history_line(size_t k, Line& out) const {
    const Scrollback& sb = t_->scrollback();
    sb.decode_line(k, out.cells, out.palette);
    out.styles = out.palette.data();
    out.number = int64_t(sb.dropped_lines() + k);
    out.first_row = int64_t(sb.dropped_rows() + sb.line_first_row(k));
    out.in_history = !sb.line_continued(k);
    std::vector<WrapSpan> spans;
    wrap_cells(out.cells.cells.data(), out.cells.cells.size(), sb.cols(), spans);
    const size_t nrows = std::max<size_t>(1, sb.line_rows(k));
    out.row_start.clear();
    for (size_t r = 0; r < nrows; ++r) out.row_start.push_back(r < spans.size() ? spans[r].begin : uint32_t(out.size()));
    out.row_start.push_back(uint32_t(out.size()));
}

void BufferLines::append_screen_rows(int y0, Line& out, bool remap) const {
    const Terminal& t = *t_;
    if (!out.row_start.empty()) out.row_start.pop_back();  // the end sentinel
    std::vector<std::pair<uint32_t, uint32_t>> map;        // terminal style id -> palette index
    for (int y = y0; y < t.rows(); ++y) {
        const RowView v = trow(y);
        const size_t first = out.size();
        out.row_start.push_back(uint32_t(first));
        out.cells.append_row(v.cells, v.cols, v.clusters);
        out.cells.flags |= v.flags & Row_SemanticMask;
        if (remap) {
            for (size_t i = first; i < out.size(); ++i) {
                Cell& c = out.cells.cells[i];
                auto it = std::find_if(map.begin(), map.end(), [&](const auto& e) { return e.first == c.style; });
                if (it == map.end()) {
                    const Style& s = t.style(c.style);
                    uint32_t idx = 0;
                    while (idx < out.palette.size() && !(out.palette[idx] == s)) ++idx;
                    if (idx == out.palette.size()) out.palette.push_back(s);
                    it = map.insert(map.end(), {c.style, idx});
                }
                c.style = it->second;
            }
        }
        if (!v.wrapped()) break;
    }
    out.row_start.push_back(uint32_t(out.size()));
    if (remap) out.styles = out.palette.data();
}

void BufferLines::screen_line(int y0, Line& out) const {
    const Terminal& t = *t_;
    out.src = &s_;
    out.local_links = false;
    out.missing = false;
    if (y0 == 0 && joint()) {
        history_line(t.scrollback().lines() - 1, out);
        out.in_history = false;
        out.cells.continued = false;
        append_screen_rows(0, out, true);
        return;
    }
    out.cells.clear();
    out.palette.clear();
    out.row_start.clear();
    out.number = term_line_number_at_row(t.screen_top_row() + y0);
    out.first_row = t.screen_top_row() + y0;
    out.styles = t.styles().data();
    out.in_history = false;
    append_screen_rows(y0, out, false);
}

bool BufferLines::term_line_at_row(int64_t row, Line& out) const {
    const Terminal& t = *t_;
    if (row < tfirst_row() || row >= t.end_row()) return false;
    out.src = &s_;
    out.local_links = false;
    out.missing = false;
    const int64_t top = t.screen_top_row();
    if (row < top) {
        const Scrollback& sb = t.scrollback();
        size_t k = sb.line_of_row(size_t(row - tfirst_row()));
        if (k + 1 == sb.lines() && joint()) screen_line(0, out);
        else history_line(k, out);
        return true;
    }
    screen_line(screen_line_start(int(row - top)), out);
    return true;
}

bool BufferLines::term_line(int64_t number, Line& out) const {
    if (number < first_line()) return false;
    out.src = &s_;
    out.local_links = false;
    out.missing = false;
    const Scrollback& sb = t_->scrollback();
    if (!alt()) {
        const int64_t k = number - int64_t(sb.dropped_lines());
        if (k < int64_t(sb.lines())) {
            if (k + 1 == int64_t(sb.lines()) && joint()) screen_line(0, out);
            else history_line(size_t(k), out);
            return true;
        }
    }
    const int y = screen_row_of_line(number - screen_base_line());
    if (y < 0) return false;
    screen_line(y, out);
    return true;
}

} // namespace bropty::detail
