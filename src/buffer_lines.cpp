// BufferLines: Line / LineText helpers, the dispatch between the Terminal
// path (buffer_lines_term.cpp) and the generic one, and the generic one:
// logical lines assembled from any RowSource's rows by their wrap flags.
#include "buffer_lines.h"

#include "bropty/terminal.h"

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

const std::string* Line::link_uri(uint32_t id) const noexcept {
    if (id == 0) return nullptr;
    if (local_links) return id <= uris.size() ? &uris[id - 1] : nullptr;
    return src ? src->hyperlink_uri(first_row, id) : nullptr;
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
// BufferLines: dispatch

int64_t BufferLines::first_line() const noexcept {
    if (!t_) return s_.first_row();
    return t_->alt_screen_active() ? screen_base_line() : int64_t(t_->scrollback().dropped_lines());
}

int64_t BufferLines::end_line() const { return t_ ? term_end_line() : s_.end_row(); }

int64_t BufferLines::screen_first_line() const {
    return t_ ? screen_base_line() : rows_line_start(s_.screen_top_row());
}

int64_t BufferLines::line_number_at_row(int64_t row) const {
    if (t_) return term_line_number_at_row(row);
    if (row < s_.first_row()) return first_line() - 1;
    return rows_line_start(std::min(row, s_.end_row() - 1));
}

uint32_t BufferLines::line_flags(int64_t number) const {
    if (t_) return term_line_flags(number);
    if (number < s_.first_row() || number >= s_.end_row()) return 0;
    uint32_t flags = 0;
    for (int64_t r = rows_line_start(number); r < s_.end_row(); ++r) {
        const RowView v = s_.row_at(r);
        flags |= v.flags & Row_SemanticMask;
        if (!v.cells || !v.wrapped()) break;
    }
    return flags;
}

int64_t BufferLines::line_first_row(int64_t number) const {
    if (t_) return term_line_first_row(number);
    if (number < s_.first_row()) return s_.first_row();
    if (number >= s_.end_row()) return s_.end_row();
    return rows_line_start(number);
}

int64_t BufferLines::next_line(int64_t number) const {
    if (t_) return number + 1;
    if (number < s_.first_row()) return s_.first_row();
    if (number >= s_.end_row()) return number + 1;
    return rows_line_end(rows_line_start(number));
}

int64_t BufferLines::prev_line(int64_t number) const {
    if (t_) return number - 1;
    const int64_t first = s_.first_row();
    if (number <= first) return first - 1;
    if (number >= s_.end_row()) return rows_line_start(s_.end_row() - 1);
    const int64_t start = rows_line_start(number);
    return start <= first ? first - 1 : rows_line_start(start - 1);
}

bool BufferLines::line_at_row(int64_t row, Line& out) const {
    return t_ ? term_line_at_row(row, out) : rows_line_at_row(row, out);
}

bool BufferLines::line(int64_t number, Line& out) const {
    if (t_) return term_line(number, out);
    return rows_line_at_row(number, out);
}

LinePos BufferLines::to_line_pos(RowPos p) const {
    if (p.row < s_.first_row()) return LinePos{first_line() - 1, 0};
    if (p.row >= s_.end_row()) return LinePos{end_line(), 0};
    Line l;
    line_at_row(p.row, l);
    return LinePos{l.number, l.offset_of(p)};
}

RowPos BufferLines::from_line_pos(const LinePos& lp) const {
    if (lp.line < first_line()) return RowPos{s_.first_row() - 1, 0};
    Line l;
    if (!line(lp.line, l)) return RowPos{s_.end_row(), 0};
    return l.pos_of(std::min(lp.offset, l.size()));
}

// ---------------------------------------------------------------------------
// BufferLines over any RowSource

int64_t BufferLines::rows_line_start(int64_t row) const {
    const int64_t first = s_.first_row();
    while (row > first && s_.row_at(row - 1).wrapped()) --row;
    return row;
}

int64_t BufferLines::rows_line_end(int64_t start) const {
    const int64_t end = s_.end_row();
    int64_t r = start;
    while (r < end) {
        const RowView v = s_.row_at(r++);
        if (!v.cells || !v.wrapped()) break;
    }
    return r;
}

namespace {

// Append a row of a source, its styles interned into the line's palette
// and its hyperlinks into the line's own URI table (rows of one line may
// come from different id spaces).
void append_source_row(Line& out, const RowView& v, int64_t row, const RowSource& s) {
    const size_t first = out.size();
    out.row_start.push_back(uint32_t(first));
    if (!v.cells) {
        out.missing = true;
        return;
    }
    out.cells.append_row(v.cells, v.cols, v.clusters);
    out.cells.flags |= v.flags & Row_SemanticMask;
    std::vector<std::pair<uint32_t, uint32_t>> map;  // row style id -> palette index
    for (size_t i = first; i < out.size(); ++i) {
        Cell& c = out.cells.cells[i];
        auto it = std::find_if(map.begin(), map.end(), [&](const auto& e) { return e.first == c.style; });
        if (it == map.end()) {
            Style st = v.styles[c.style];
            if (st.link) {
                const std::string* uri = s.hyperlink_uri(row, st.link);
                uint32_t k = 0;
                if (uri) {
                    while (k < out.uris.size() && out.uris[k] != *uri) ++k;
                    if (k == out.uris.size()) out.uris.push_back(*uri);
                }
                st.link = uri ? k + 1 : 0;
            }
            uint32_t idx = 0;
            while (idx < out.palette.size() && !(out.palette[idx] == st)) ++idx;
            if (idx == out.palette.size()) out.palette.push_back(st);
            it = map.insert(map.end(), {c.style, idx});
        }
        c.style = it->second;
    }
}

} // namespace

bool BufferLines::rows_line_at_row(int64_t row, Line& out) const {
    if (row < s_.first_row() || row >= s_.end_row()) return false;
    const int64_t start = rows_line_start(row);
    out.cells.clear();
    out.palette.clear();
    out.row_start.clear();
    out.uris.clear();
    out.src = &s_;
    out.local_links = true;
    out.missing = false;
    out.number = start;
    out.first_row = start;
    bool wrapped_last = false;
    for (int64_t r = start; r < s_.end_row(); ++r) {
        const RowView v = s_.row_at(r);
        append_source_row(out, v, r, s_);
        wrapped_last = v.cells && v.wrapped();
        if (!wrapped_last) break;
    }
    const int64_t end = start + int64_t(out.row_start.size());  // the sentinel is not in yet
    out.in_history = end <= s_.screen_top_row() && !wrapped_last;
    if (out.in_history) {
        // As history keeps a line (Scrollback::push_row): without its
        // trailing blanks, so a word or zone at its end reads the same.
        auto& cells = out.cells.cells;
        size_t n = cells.size();
        while (n > 0 && cells[n - 1].is_empty() && cells[n - 1].wide() == Wide::Narrow &&
               out.palette[cells[n - 1].style] == Style{})
            --n;
        cells.resize(n);
        auto& cl = out.cells.clusters;
        while (!cl.empty() && cl.back().first >= n) cl.pop_back();
        for (uint32_t& s : out.row_start) s = std::min(s, uint32_t(n));
    }
    out.row_start.push_back(uint32_t(out.size()));
    out.styles = out.palette.data();
    return true;
}

// ---------------------------------------------------------------------------

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
