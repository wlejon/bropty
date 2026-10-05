#pragma once
// Internal: logical (unwrapped) lines, their soft-wrapping into rows, and the
// compact byte encoding scrollback stores them in.

#include "bropty/cell.h"
#include "bropty/scrollback.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
struct EncodeStats {
    uint64_t columns{0};  // wide cells count 2
    bool has_wide{false};
};
template <class ClusterAt>
EncodeStats encode_cells(ByteBuf& out, const Cell* cells, size_t n, const Style* styles, ClusterAt&& cluster_at);

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

inline char* put_varint(char* p, uint64_t v) {
    while (v >= 0x80) {
        *p++ = char(uint8_t(v) | 0x80);
        v >>= 7;
    }
    *p++ = char(uint8_t(v));
    return p;
}

// UTF-8 of a code point (callers pass valid scalar values), as append_utf8.
inline char* put_utf8(char* p, char32_t cp) {
    if (cp < 0x80) {
        *p++ = char(cp);
    } else if (cp < 0x800) {
        *p++ = char(0xC0 | (cp >> 6));
        *p++ = char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *p++ = char(0xE0 | (cp >> 12));
        *p++ = char(0x80 | ((cp >> 6) & 0x3F));
        *p++ = char(0x80 | (cp & 0x3F));
    } else {
        *p++ = char(0xF0 | (cp >> 18));
        *p++ = char(0x80 | ((cp >> 12) & 0x3F));
        *p++ = char(0x80 | ((cp >> 6) & 0x3F));
        *p++ = char(0x80 | (cp & 0x3F));
    }
    return p;
}

Style read_style(const uint8_t*& p, const uint8_t* end);
// A style record (see encode_cells); at most kMaxStyleBytes.
constexpr size_t kMaxStyleBytes = 1 + 3 * 4 + 3 + 1 + 5 + 1;
char* put_style(char* p, const Style& s);

template <class ClusterAt>
EncodeStats encode_cells(ByteBuf& out, const Cell* cells, size_t n, const Style* styles, ClusterAt&& cluster_at) {
    EncodeStats stats;
    // Consecutive runs often repeat a style (alternating colours): keep the
    // last record's bytes instead of re-encoding the Style.
    uint32_t cached_sid = 0;
    char cached[kMaxStyleBytes];
    size_t cached_len = 0;
    // One pass per run: the header's column count is only known at the end of
    // the run, so kHeaderSlot bytes are left for it and the run is shifted
    // down over the unused part afterwards (a few bytes; runs are short).
    constexpr size_t kHeaderSlot = 4;  // a varint up to 2^28: run columns < 2^27
    size_t i = 0;
    while (i < n) {
        const Wide w0 = cells[i].wide();
        if (w0 == Wide::SpacerHead || w0 == Wide::SpacerTail) {
            ++i;
            continue;
        }
        const uint32_t sid = cells[i].style;
        // Room for the rest of the row: at most 5 bytes per cell (wide prefix
        // + 4-byte UTF-8). Only a cluster tail can need more, and it re-reserves.
        size_t room = kHeaderSlot + kMaxStyleBytes + (n - i) * 5;
        char* p = out.reserve(room);
        char* limit = p + room;
        const size_t hdr = out.size();
        p += kHeaderSlot;
        const bool has_style = sid != 0 && !styles[sid].is_default();
        if (has_style) {
            if (sid != cached_sid || cached_len == 0) {
                cached_len = size_t(put_style(cached, styles[sid]) - cached);
                cached_sid = sid;
            }
            std::memcpy(p, cached, cached_len);
            p += cached_len;
        }
        uint64_t columns = 0;
        size_t j = i;
        // A cell as one 64-bit word {bits, style}: it is a plain narrow ASCII
        // (or empty) cell of this run exactly when (word & kPlainMask) == key.
        constexpr uint64_t kPlainMask = 0xFFFFFFFF00000000ull | uint64_t(0xFFFFFFFFu & ~(Cell::kProtectedBit | 0x7Fu));
        static_assert(sizeof(Cell) == 8 && offsetof(Cell, style) == 4 && std::endian::native == std::endian::little);
        const uint64_t key = uint64_t(sid) << 32;
        for (; j < n; ++j) {
            // Four plain cells at a time: the bulk of text rows.
            while (j + 4 <= n) {
                uint64_t w4[4];
                std::memcpy(w4, cells + j, sizeof w4);
                if ((((w4[0] & kPlainMask) ^ key) | ((w4[1] & kPlainMask) ^ key) | ((w4[2] & kPlainMask) ^ key) |
                     ((w4[3] & kPlainMask) ^ key)) != 0)
                    break;
                p[0] = char(w4[0] & 0x7F);
                p[1] = char(w4[1] & 0x7F);
                p[2] = char(w4[2] & 0x7F);
                p[3] = char(w4[3] & 0x7F);
                p += 4;
                j += 4;
                columns += 4;
            }
            if (j >= n) break;
            const Cell& c = cells[j];
            const uint32_t plain = c.bits & ~Cell::kProtectedBit;
            if (plain < 0x80) {  // narrow, no cluster, ASCII or empty: one byte
                if (c.style != sid) break;
                *p++ = char(plain);
                ++columns;
                continue;
            }
            const Wide w = c.wide();
            if (w == Wide::SpacerHead || w == Wide::SpacerTail) continue;  // never end a run
            if (c.style != sid) break;
            if (w == Wide::Narrow) {
                ++columns;
            } else {
                stats.has_wide = true;
                columns += (j + 1 < n && cells[j + 1].wide() == Wide::SpacerTail) ? 2 : 1;
            }
            if (c.is_empty()) {
                *p++ = '\0';
                continue;
            }
            std::u32string_view tail;
            if (c.has_cluster()) tail = cluster_at(j);
            if (!tail.empty()) {
                size_t need = 1 + 10 + 5 + 4 * tail.size() + 5 * (n - j);
                if (size_t(limit - p) < need) {
                    out.set_end(p);
                    p = out.reserve(need);
                    limit = p + need;
                }
                *p++ = '\x02';
                p = put_varint(p, tail.size());
            }
            if (w == Wide::Lead && j + 1 < n && cells[j + 1].wide() == Wide::SpacerTail) *p++ = '\x01';
            p = put_utf8(p, c.cp());
            for (char32_t t : tail) p = put_utf8(p, t);
        }
        stats.columns += columns;
        char head[10];
        const size_t hl = size_t(put_varint(head, (columns << 1) | (has_style ? 1u : 0u)) - head);
        out.set_end(p);
        char* base = out.mdata() + hdr;
        const size_t body = size_t(p - (base + kHeaderSlot));
        if (hl > kHeaderSlot) {  // a run of 2^27+ columns: make room instead
            out.reserve(hl - kHeaderSlot);
            base = out.mdata() + hdr;
            std::memmove(base + hl, base + kHeaderSlot, body);
        } else if (hl < kHeaderSlot) {
            std::memmove(base + hl, base + kHeaderSlot, body);
        }
        std::memcpy(base, head, hl);
        out.set_end(base + hl + body);
        i = j;
    }
    return stats;
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
