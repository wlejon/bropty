#pragma once
// Internal: logical (unwrapped) lines, their soft-wrapping into rows, and the
// compact byte encoding scrollback stores them in.

#include "bropty/cell.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace bropty::detail {

// One logical line: the cells of all its rows joined, SpacerHeads dropped.
// Each wide Lead is followed by its SpacerTail. Cell::style is whatever id
// space the producer chose (global StyleTable ids or a local palette).
struct LogicalLine {
    std::vector<Cell> cells;
    std::vector<std::pair<uint32_t, std::u32string>> clusters;  // cell index -> tail, sorted
    uint32_t flags{0};       // semantic Row_* flags carried by the line
    bool continued{false};   // last row soft-wraps into rows stored elsewhere
    bool next_wide{false};   // (continued) the continuation starts with a wide cell

    void clear() {
        cells.clear();
        clusters.clear();
        flags = 0;
        continued = false;
        next_wide = false;
    }
    // Drop trailing empty default-styled cells.
    void trim_trailing_blanks();
    // Append a screen row's cells [0, n) (SpacerHead dropped) and its clusters.
    void append_row(const Cell* row, int n, const ClusterMap* clusters);
};

struct WrapSpan {
    uint32_t begin;
    uint32_t end;
    bool spacer_head;  // row ends with a SpacerHead (next wide cell did not fit)
};

// Split cells into rows of width `cols` (>= 2). Always yields at least one span.
void wrap_cells(const Cell* cells, size_t n, int cols, std::vector<WrapSpan>& out);
size_t count_wrapped_rows(const Cell* cells, size_t n, int cols);

// Compact encoding. Runs of equal style; each run is
//   varint(columns << 1 | has_style) [style record] cell*
// A cell is UTF-8 of its code point, 0x00 for an empty cell, 0x01 prefix for
// a wide lead (its SpacerTail is implicit). A multi-code-point cluster is
//   0x02 varint(tail_len) [0x01] base tail_cp*
// so every cell is self-delimiting inside its run and a run header can never
// be mistaken for cell data. Style records store only the non-default fields.
// `cluster_at(i)` returns the cluster tail of cell i (only called for cells
// with the cluster bit). SpacerHeads are skipped; SpacerTails are implicit.
template <class ClusterAt>
void encode_cells(std::string& out, const Cell* cells, size_t n, const Style* styles, ClusterAt&& cluster_at);

// Decode, mapping each run's Style through `intern` (Style -> id).
template <class Intern>
void decode_cells(const uint8_t* p, const uint8_t* end, LogicalLine& out, Intern&& intern);

// Hyperlink ids referenced by an encoded line.
template <class F>
void for_each_link(const uint8_t* p, const uint8_t* end, F&& f);

// --- implementation details used by the templates -------------------------

inline uint64_t read_varint(const uint8_t*& p, const uint8_t* end) {
    uint64_t v = 0;
    int shift = 0;
    while (p < end) {
        uint8_t b = *p++;
        v |= uint64_t(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return v;
}

inline char32_t read_utf8(const uint8_t*& p, const uint8_t* end) {
    uint8_t b = *p++;
    if (b < 0x80) return b;
    int n = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : 1;
    char32_t cp = b & (0x3F >> n);
    while (n-- > 0 && p < end) cp = (cp << 6) | (*p++ & 0x3F);
    return cp;
}

Style read_style(const uint8_t*& p, const uint8_t* end);
void write_style(std::string& out, const Style& s);
void write_varint(std::string& out, uint64_t v);
void write_utf8(std::string& out, char32_t cp);

template <class ClusterAt>
void encode_cells(std::string& out, const Cell* cells, size_t n, const Style* styles, ClusterAt&& cluster_at) {
    size_t i = 0;
    while (i < n) {
        if (cells[i].wide() == Wide::SpacerHead || cells[i].wide() == Wide::SpacerTail) {
            ++i;
            continue;
        }
        // Collect a run of cells sharing a style.
        uint32_t sid = cells[i].style;
        size_t j = i;
        uint64_t columns = 0;
        while (j < n) {
            const Cell& c = cells[j];
            if (c.wide() == Wide::SpacerHead || c.wide() == Wide::SpacerTail) { ++j; continue; }
            if (c.style != sid) break;
            columns += (c.wide() == Wide::Lead && j + 1 < n && cells[j + 1].wide() == Wide::SpacerTail) ? 2 : 1;
            ++j;
        }
        const Style& st = styles[sid];
        bool has_style = !st.is_default();
        write_varint(out, (columns << 1) | (has_style ? 1u : 0u));
        if (has_style) write_style(out, st);
        for (size_t k = i; k < j; ++k) {
            const Cell& c = cells[k];
            if (c.wide() == Wide::SpacerHead || c.wide() == Wide::SpacerTail) continue;
            if (c.is_empty()) {
                out.push_back('\0');
                continue;
            }
            std::u32string_view tail;
            if (c.has_cluster()) tail = cluster_at(k);
            if (!tail.empty()) {
                out.push_back('\x02');
                write_varint(out, tail.size());
            }
            if (c.wide() == Wide::Lead && k + 1 < n && cells[k + 1].wide() == Wide::SpacerTail) out.push_back('\x01');
            write_utf8(out, c.cp());
            for (char32_t t : tail) write_utf8(out, t);
        }
        i = j;
    }
}

template <class Intern>
void decode_cells(const uint8_t* p, const uint8_t* end, LogicalLine& out, Intern&& intern) {
    while (p < end) {
        uint64_t head = read_varint(p, end);
        uint64_t columns = head >> 1;
        uint32_t style = 0;
        if (head & 1) style = intern(read_style(p, end));
        else style = intern(Style{});
        uint64_t done = 0;
        while (done < columns && p < end) {
            if (*p == 0x00) {
                ++p;
                out.cells.push_back(Cell::blank(style));
                ++done;
                continue;
            }
            uint64_t tail_len = 0;
            if (*p == 0x02) {
                ++p;
                tail_len = read_varint(p, end);
                if (p >= end) break;
            }
            bool wide = *p == 0x01;
            if (wide) ++p;
            if (p >= end) break;
            uint32_t idx = uint32_t(out.cells.size());
            out.cells.push_back(Cell::make(read_utf8(p, end), style, wide ? Wide::Lead : Wide::Narrow));
            if (wide) out.cells.push_back(Cell::make(0, style, Wide::SpacerTail));
            done += wide ? 2 : 1;
            if (tail_len) {
                std::u32string tail;
                for (uint64_t t = 0; t < tail_len && p < end; ++t) tail.push_back(read_utf8(p, end));
                out.cells[idx].set_cluster(true);
                out.clusters.emplace_back(idx, std::move(tail));
            }
        }
    }
}

template <class F>
void for_each_link(const uint8_t* p, const uint8_t* end, F&& f) {
    while (p < end) {
        uint64_t head = read_varint(p, end);
        uint64_t columns = head >> 1;
        if (head & 1) {
            Style s = read_style(p, end);
            if (s.link) f(s.link);
        }
        uint64_t done = 0;
        while (done < columns && p < end) {
            if (*p == 0x00) { ++p; ++done; continue; }
            uint64_t tail_len = 0;
            if (*p == 0x02) { ++p; tail_len = read_varint(p, end); }
            bool wide = p < end && *p == 0x01;
            if (wide) ++p;
            if (p >= end) break;
            read_utf8(p, end);
            for (uint64_t t = 0; t < tail_len && p < end; ++t) read_utf8(p, end);
            done += wide ? 2 : 1;
        }
    }
}

} // namespace bropty::detail
