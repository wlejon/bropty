#include "buffer_lines.h"

#include <algorithm>

namespace bropty::detail {

// ---------------------------------------------------------------------------
// Line

std::u32string_view Line::tail(size_t i) const {
    const auto& cl = cells.clusters;
    auto it = std::lower_bound(cl.begin(), cl.end(), uint32_t(i),
                               [](const auto& e, uint32_t k) { return e.first < k; });
    return (it != cl.end() && it->first == i) ? std::u32string_view(it->second) : std::u32string_view();
}

size_t Line::offset_of(RowPos p) const noexcept {
    if (rows() <= 0 || p.row < first_row) return 0;
    if (p.row >= end_row()) return size();
    const size_t r = size_t(p.row - first_row);
    const size_t begin = row_start[r];
    const size_t len = row_start[r + 1] - begin;
    return begin + std::min<size_t>(size_t(std::max(p.col, 0)), len);
}

RowPos Line::pos_of(size_t i) const noexcept {
    if (rows() <= 0) return RowPos{first_row, 0};
    auto last = row_start.begin() + rows();
    auto it = std::upper_bound(row_start.begin(), last, uint32_t(i));
    size_t r = it == row_start.begin() ? 0 : size_t(it - row_start.begin()) - 1;
    return RowPos{first_row + int64_t(r), int(i - row_start[r])};
}

RowPos Line::end_pos(size_t i) const noexcept {
    if (i == 0) return pos_of(0);
    RowPos p = pos_of(i - 1);
    ++p.col;
    return p;
}

void Line::widen(size_t& a, size_t& b) const noexcept {
    const size_t n = size();
    if (a > 0 && a < n && cell(a).wide() == Wide::SpacerTail) --a;
    if (b > 0 && b < n && cell(b).wide() == Wide::SpacerTail) ++b;
}

size_t Line::content_end() const noexcept {
    size_t i = size();
    while (i > 0) {
        const Cell& c = cell(i - 1);
        if (c.wide() == Wide::SpacerTail) break;
        if (c.is_empty() || (c.cp() == U' ' && !c.has_cluster())) {
            --i;
            continue;
        }
        break;
    }
    return i;
}

// ---------------------------------------------------------------------------
// LineText

void LineText::build(const Line& line) {
    const size_t n = line.size();
    text.clear();
    cell_byte.assign(n + 1, 0);
    size_t ci = 0;
    const auto& cl = line.cells.clusters;
    size_t content = 0;  // cells up to the last non-empty one
    for (size_t i = 0; i < n; ++i) {
        cell_byte[i] = uint32_t(text.size());
        const Cell& c = line.cell(i);
        if (c.is_spacer()) continue;
        if (c.is_empty()) {
            text.push_back(' ');
            continue;
        }
        append_utf8(text, c.cp());
        if (c.has_cluster()) {
            while (ci < cl.size() && cl[ci].first < i) ++ci;
            if (ci < cl.size() && cl[ci].first == i)
                for (char32_t t : cl[ci].second) append_utf8(text, t);
        }
        content = i + 1;
    }
    cell_byte[n] = uint32_t(text.size());
    if (content < n) {
        // Keep a wide character's tail inside the content.
        if (line.cell(content).wide() == Wide::SpacerTail) ++content;
        const uint32_t end = cell_byte[std::min(content, n)];
        text.resize(end);
        for (size_t i = content; i <= n; ++i) cell_byte[i] = end;
    }
}

size_t LineText::cell_at(size_t b) const noexcept {
    const size_t n = cell_byte.size() - 1;
    auto it = std::upper_bound(cell_byte.begin(), cell_byte.begin() + long(n), uint32_t(b));
    return it == cell_byte.begin() ? 0 : size_t(it - cell_byte.begin()) - 1;
}

void LineText::cells_of(const Line& line, size_t b0, size_t b1, size_t& c0, size_t& c1) const noexcept {
    c0 = cell_at(b0);
    c1 = b1 > b0 ? cell_at(b1 - 1) + 1 : c0;
    line.widen(c0, c1);
}

// ---------------------------------------------------------------------------
// BufferLines

bool BufferLines::joint() const noexcept {
    return !t_.alt_screen_active() && t_.scrollback().last_continued();
}

int64_t BufferLines::screen_base_line() const noexcept {
    const Scrollback& sb = t_.scrollback();
    return int64_t(sb.dropped_lines() + sb.lines()) - (joint() ? 1 : 0);
}

int64_t BufferLines::first_line() const noexcept {
    return t_.alt_screen_active() ? screen_base_line() : int64_t(t_.scrollback().dropped_lines());
}

int64_t BufferLines::screen_first_line() const noexcept { return screen_base_line(); }

int64_t BufferLines::end_line() const {
    int64_t n = 1;
    for (int y = 1; y < t_.rows(); ++y)
        if (!t_.row(y - 1).wrapped()) ++n;
    return screen_base_line() + n;
}

int BufferLines::screen_line_start(int y) const noexcept {
    while (y > 0 && t_.row(y - 1).wrapped()) --y;
    return y;
}

int64_t BufferLines::line_number_at_row(int64_t row) const {
    const int64_t top = t_.screen_top_row();
    if (row < top) {
        if (t_.alt_screen_active() || row < t_.first_row()) return first_line() - 1;
        const Scrollback& sb = t_.scrollback();
        return int64_t(sb.dropped_lines() + sb.line_of_row(size_t(row - t_.first_row())));
    }
    int y = int(std::min<int64_t>(row - top, t_.rows() - 1));
    int64_t n = 0;
    for (int k = 1; k <= y; ++k)
        if (!t_.row(k - 1).wrapped()) ++n;
    return screen_base_line() + n;
}

int64_t BufferLines::line_first_row(int64_t number) const {
    const Scrollback& sb = t_.scrollback();
    if (!t_.alt_screen_active()) {
        const int64_t k = number - int64_t(sb.dropped_lines());
        if (k < 0) return t_.first_row();
        if (k < int64_t(sb.lines())) return int64_t(sb.dropped_rows() + sb.line_first_row(size_t(k)));
    }
    const int y = screen_row_of_line(number - screen_base_line());
    return y < 0 ? (number < screen_base_line() ? t_.first_row() : t_.end_row()) : t_.screen_top_row() + y;
}

int BufferLines::screen_row_of_line(int64_t idx) const noexcept {
    if (idx < 0) return -1;
    int64_t cur = 0;
    for (int y = 0; y < t_.rows(); ++y) {
        if (y > 0 && !t_.row(y - 1).wrapped()) ++cur;
        if (cur == idx) return y;
    }
    return -1;
}

uint32_t BufferLines::line_flags(int64_t number) const {
    const Scrollback& sb = t_.scrollback();
    if (!t_.alt_screen_active()) {
        const int64_t k = number - int64_t(sb.dropped_lines());
        if (k < 0) return 0;
        if (k < int64_t(sb.lines()) && !(joint() && k + 1 == int64_t(sb.lines()))) return sb.line_flags(size_t(k));
    }
    const int64_t row = line_first_row(number);
    uint32_t flags = 0;
    if (row < t_.screen_top_row()) {
        flags = sb.line_flags(sb.lines() - 1);
        for (int y = 0; y < t_.rows(); ++y) {
            flags |= t_.row(y).flags & Row_SemanticMask;
            if (!t_.row(y).wrapped()) break;
        }
        return flags;
    }
    for (int y = int(row - t_.screen_top_row()); y < t_.rows(); ++y) {
        flags |= t_.row(y).flags & Row_SemanticMask;
        if (!t_.row(y).wrapped()) break;
    }
    return flags;
}

void BufferLines::history_line(size_t k, Line& out) const {
    const Scrollback& sb = t_.scrollback();
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
    if (!out.row_start.empty()) out.row_start.pop_back();  // the end sentinel
    std::vector<std::pair<uint32_t, uint32_t>> map;        // terminal style id -> palette index
    for (int y = y0; y < t_.rows(); ++y) {
        const RowView v = t_.row(y);
        const size_t first = out.size();
        out.row_start.push_back(uint32_t(first));
        out.cells.append_row(v.cells, v.cols, v.clusters);
        out.cells.flags |= v.flags & Row_SemanticMask;
        if (remap) {
            for (size_t i = first; i < out.size(); ++i) {
                Cell& c = out.cells.cells[i];
                auto it = std::find_if(map.begin(), map.end(), [&](const auto& e) { return e.first == c.style; });
                if (it == map.end()) {
                    const Style& s = t_.style(c.style);
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
    if (y0 == 0 && joint()) {
        history_line(t_.scrollback().lines() - 1, out);
        out.in_history = false;
        out.cells.continued = false;
        append_screen_rows(0, out, true);
        return;
    }
    out.cells.clear();
    out.palette.clear();
    out.row_start.clear();
    out.number = line_number_at_row(t_.screen_top_row() + y0);
    out.first_row = t_.screen_top_row() + y0;
    out.styles = t_.styles().data();
    out.in_history = false;
    append_screen_rows(y0, out, false);
}

bool BufferLines::line_at_row(int64_t row, Line& out) const {
    if (row < t_.first_row() || row >= t_.end_row()) return false;
    const int64_t top = t_.screen_top_row();
    if (row < top) {
        const Scrollback& sb = t_.scrollback();
        size_t k = sb.line_of_row(size_t(row - t_.first_row()));
        if (k + 1 == sb.lines() && joint()) screen_line(0, out);
        else history_line(k, out);
        return true;
    }
    screen_line(screen_line_start(int(row - top)), out);
    return true;
}

bool BufferLines::line(int64_t number, Line& out) const {
    if (number < first_line()) return false;
    const Scrollback& sb = t_.scrollback();
    if (!t_.alt_screen_active()) {
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

LinePos BufferLines::to_line_pos(RowPos p) const {
    if (p.row < t_.first_row()) return LinePos{first_line() - 1, 0};
    if (p.row >= t_.end_row()) return LinePos{end_line(), 0};
    Line l;
    line_at_row(p.row, l);
    return LinePos{l.number, l.offset_of(p)};
}

RowPos BufferLines::from_line_pos(const LinePos& lp) const {
    if (lp.line < first_line()) return RowPos{t_.first_row() - 1, 0};
    Line l;
    if (!line(lp.line, l)) return RowPos{t_.end_row(), 0};
    return l.pos_of(std::min(lp.offset, l.size()));
}

uint64_t row_hash(const RowView& v) noexcept {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t x) {
        h ^= x;
        h *= 1099511628211ull;
    };
    if (!v.cells) return h;
    for (int x = 0; x < v.cols; ++x) {
        const Cell& c = v.cells[x];
        mix(c.bits & (Cell::kCpMask | (3u << 21)));
        if (c.has_cluster() && v.clusters)
            for (char32_t t : v.clusters->find(x)) mix(uint64_t(t) | (1ull << 40));
    }
    mix(v.wrapped() ? 0xA5 : 0x5A);
    return h;
}

} // namespace bropty::detail
