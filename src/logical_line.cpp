#include "logical_line.h"

namespace bropty::detail {

void write_varint(std::string& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(char(uint8_t(v) | 0x80));
        v >>= 7;
    }
    out.push_back(char(uint8_t(v)));
}

void write_utf8(std::string& out, char32_t cp) { append_utf8(out, cp); }

namespace {
void write_color(std::string& out, Color c) {
    out.push_back(char(c.kind()));
    if (c.is_indexed()) {
        out.push_back(char(c.index()));
    } else if (c.is_rgb()) {
        Rgb v = c.rgb_value();
        out.push_back(char(v.r));
        out.push_back(char(v.g));
        out.push_back(char(v.b));
    }
}

Color read_color(const uint8_t*& p, const uint8_t* end) {
    if (p >= end) return Color{};
    auto kind = Color::Kind(*p++ & 3);
    if (kind == Color::Kind::Indexed && p < end) return Color::indexed(*p++);
    if (kind == Color::Kind::Rgb && end - p >= 3) {
        Color c = Color::rgb(p[0], p[1], p[2]);
        p += 3;
        return c;
    }
    return Color{};
}
} // namespace

void write_style(std::string& out, const Style& s) {
    uint8_t mask = 0;
    if (!s.fg.is_default()) mask |= 1;
    if (!s.bg.is_default()) mask |= 2;
    if (!s.underline_color.is_default()) mask |= 4;
    if (s.attrs) mask |= 8;
    if (s.underline != Underline::None) mask |= 16;
    if (s.link) mask |= 32;
    out.push_back(char(mask));
    if (mask & 1) write_color(out, s.fg);
    if (mask & 2) write_color(out, s.bg);
    if (mask & 4) write_color(out, s.underline_color);
    if (mask & 8) write_varint(out, s.attrs);
    if (mask & 16) out.push_back(char(s.underline));
    if (mask & 32) write_varint(out, s.link);
}

Style read_style(const uint8_t*& p, const uint8_t* end) {
    Style s;
    if (p >= end) return s;
    uint8_t mask = *p++;
    if (mask & 1) s.fg = read_color(p, end);
    if (mask & 2) s.bg = read_color(p, end);
    if (mask & 4) s.underline_color = read_color(p, end);
    if (mask & 8) s.attrs = uint16_t(read_varint(p, end));
    if ((mask & 16) && p < end) s.underline = Underline(*p++);
    if (mask & 32) s.link = uint32_t(read_varint(p, end));
    return s;
}

void LogicalLine::trim_trailing_blanks() {
    size_t n = cells.size();
    while (n > 0 && cells[n - 1].is_empty() && cells[n - 1].style == 0 && cells[n - 1].wide() == Wide::Narrow) --n;
    cells.resize(n);
    while (!clusters.empty() && clusters.back().first >= n) clusters.pop_back();
}

void LogicalLine::append_row(const Cell* row, int n, const ClusterMap* row_clusters) {
    for (int x = 0; x < n; ++x) {
        const Cell& c = row[x];
        if (c.wide() == Wide::SpacerHead) continue;
        if (c.has_cluster() && row_clusters) {
            auto tail = row_clusters->find(x);
            if (!tail.empty()) clusters.emplace_back(uint32_t(cells.size()), std::u32string(tail));
        }
        cells.push_back(c);
    }
}

void wrap_cells(const Cell* cells, size_t n, int cols, std::vector<WrapSpan>& out) {
    out.clear();
    uint32_t begin = 0;
    int col = 0;
    size_t i = 0;
    while (i < n) {
        int w = (cells[i].wide() == Wide::Lead && i + 1 < n && cells[i + 1].wide() == Wide::SpacerTail) ? 2 : 1;
        if (col + w > cols && col > 0) {
            bool head = (w == 2 && col == cols - 1);
            out.push_back({begin, uint32_t(i), head});
            begin = uint32_t(i);
            col = 0;
            continue;
        }
        col += w;
        i += size_t(w);
    }
    out.push_back({begin, uint32_t(n), false});
}

size_t count_wrapped_rows(const Cell* cells, size_t n, int cols) {
    size_t rows = 1;
    int col = 0;
    size_t i = 0;
    while (i < n) {
        int w = (cells[i].wide() == Wide::Lead && i + 1 < n && cells[i + 1].wide() == Wide::SpacerTail) ? 2 : 1;
        if (col + w > cols && col > 0) {
            ++rows;
            col = 0;
            continue;
        }
        col += w;
        i += size_t(w);
    }
    return rows;
}

} // namespace bropty::detail
