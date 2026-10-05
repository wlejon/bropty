// Printing: the hot path. ASCII runs are written in bulk (print_ascii);
// everything else goes through print(), which segments grapheme clusters
// (mode 2027) and applies cluster widths, including VS16 widening and VS15
// narrowing of an emoji already on screen.
#include "bropty/terminal.h"

#include <algorithm>

namespace bropty {

namespace {

// DEC Special Graphics for 0x5F..0x7E.
constexpr char16_t kDecGraphics[32] = {
    0x00A0, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1, 0x2424, 0x240B,
    0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C,
    0x2524, 0x2534, 0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,
};

// Does overwriting this cell need clear_wide_at()? Only half of a wide pair
// or a cell carrying a cluster tail does.
inline bool needs_clear(const Cell& c) noexcept { return (c.bits & ((3u << 21) | Cell::kClusterBit)) != 0; }

} // namespace

// Prepare cell (y, x) to be overwritten: break up any wide pair it belongs to.
void Terminal::clear_wide_at(int y, int x) {
    Grid& g = grid();
    Cell* r = g.row(y);
    Wide w = r[x].wide();
    if (w == Wide::SpacerTail && x > 0 && r[x - 1].wide() == Wide::Lead) {
        r[x - 1] = Cell::blank(r[x - 1].style);
        if (const ClusterMap* m = g.clusters(y); m && !m->empty()) g.clusters_mut(y).erase(x - 1);
    } else if (w == Wide::Lead && x + 1 < cols_ && r[x + 1].wide() == Wide::SpacerTail) {
        r[x + 1] = Cell::blank(r[x + 1].style);
    }
    if (r[x].has_cluster()) g.clusters_mut(y).erase(x);
}

void Terminal::write_cell(int y, int x, char32_t cp, Wide w) {
    const Cursor& c = cur();
    Cell& dst = grid().row(y)[x];
    // Field stores (see print_ascii): no Cell temporary for MSVC to bounce through the stack.
    dst.bits = (uint32_t(cp) & Cell::kCpMask) | (uint32_t(w) << 21) | (c.protect ? Cell::kProtectedBit : 0u);
    dst.style = c.pen_id;
}

void Terminal::wrap_line() {
    Cursor& c = cur();
    if (left_ == 0 && right_ == cols_ - 1) grid().set_flag(c.row, Row_Wrapped, true);
    c.pending_wrap = false;
    c.col = left_edge();
    index();
}

void Terminal::print_ascii(const char* s, size_t n) {
    Cursor& c = cur();
    // The bulk path applies to untranslated ASCII outside insert mode.
    bool plain = c.cs.g[c.cs.gl] == 'B' && c.cs.single_shift < 0 && !modes_.insert;
    if (!plain) {
        for (size_t i = 0; i < n; ++i) print(char32_t(uint8_t(s[i])));
        return;
    }
    Grid& g = grid();
    size_t i = 0;
    while (i < n) {
        if (c.pending_wrap) {
            if (modes_.autowrap) wrap_line();
            else c.pending_wrap = false;
        }
        int re = right_edge();
        int room = re - c.col + 1;
        int k = int(std::min<size_t>(size_t(std::max(room, 1)), n - i));
        Cell* r = g.row(c.row);
        int first = c.col;
        int last = first + k - 1;
        if (r[first].wide() == Wide::SpacerTail) clear_wide_at(c.row, first);
        if (r[last].wide() == Wide::Lead) clear_wide_at(c.row, last);
        if (const ClusterMap* m = g.clusters(c.row); m && !m->empty()) g.clusters_mut(c.row).erase_range(first, last + 1);
        const uint32_t base = c.protect ? Cell::kProtectedBit : 0u;
        const uint32_t pen = c.pen_id;  // a local: stores to cells must not force reloads
        Cell* dst = r + first;
        const unsigned char* src = reinterpret_cast<const unsigned char*>(s + i);
        // Field stores, not `dst[j] = Cell{...}`: MSVC builds the temporary on
        // the stack and reloads it as one 64-bit value, a store-forwarding
        // stall on every cell.
        for (int j = 0; j < k; ++j) {
            dst[j].bits = uint32_t(src[j]) | base;
            dst[j].style = pen;
        }
        g.mark_dirty(c.row);
        i += size_t(k);
        c.col += k;
        last_.row = c.row;
        last_.col = last;
        last_.cp = char32_t(uint8_t(s[i - 1]));
        if (c.col > re) {
            c.col = re;
            if (modes_.autowrap) {
                c.pending_wrap = true;
            } else if (i < n) {
                // No wrap: the remaining characters all land on the last column.
                r[re] = Cell{uint32_t(uint8_t(s[n - 1])) | base, c.pen_id};
                last_.col = re;
                last_.cp = char32_t(uint8_t(s[n - 1]));
                i = n;
            }
        }
    }
    last_.valid = true;
    last_.epoch = epoch_;
    last_.cur_row = c.row;
    last_.cur_col = c.col;
    last_.cur_pending = c.pending_wrap;
    last_.seg.reset_after_ascii();
}

void Terminal::print(char32_t cp) {
    if (cp >= 0x80 && cp < 0xA0) return;  // C1 code points are not printable
    Cursor& c = cur();
    if (cp >= 0x20 && cp < 0x7F) {
        int slot = c.cs.single_shift >= 0 ? c.cs.single_shift : c.cs.gl;
        c.cs.single_shift = -1;
        char set = c.cs.g[slot];
        if (set == '0' && cp >= 0x5F) cp = kDecGraphics[cp - 0x5F];
        else if (set == 'A' && cp == '#') cp = 0x00A3;
    }
    // One table walk for width, segmentation and the emoji checks.
    const unicode::Props p = unicode::properties(cp);
    int w = (cp >= 0x20 && cp < 0x7F) ? 1 : unicode::width_of(p, opts_.ambiguous_wide);
    // After a boundary the segmenter is in exactly the state a fresh one
    // reaches on cp, so a new cluster can start from it without re-running it.
    unicode::GraphemeSegmenter seg;
    bool seg_valid = false;
    if (modes_.grapheme_clustering && try_extend_cluster(cp, p, seg, seg_valid)) return;
    if (w == 0) {
        attach_zero_width(cp);
        return;
    }
    if (cp == kImagePlaceholder) image_cells_ = true;
    print_cluster_start(cp, w, seg_valid ? &seg : nullptr, p);
}

bool Terminal::try_extend_cluster(char32_t cp, unicode::Props p, unicode::GraphemeSegmenter& seg, bool& seg_valid) {
    const Cursor& c = cur();
    if (!last_.valid || last_.epoch != epoch_ || c.row != last_.cur_row || c.col != last_.cur_col ||
        c.pending_wrap != last_.cur_pending)
        return false;
    seg = last_.seg;
    if (seg.next_props(p)) {
        seg_valid = true;
        return false;
    }
    last_.seg = seg;
    Grid& g = grid();
    Cell& cell = g.at(last_.row, last_.col);
    if (cell.is_empty()) return false;
    cell.set_cluster(true);
    g.clusters_mut(last_.row).append(last_.col, cp);
    g.mark_dirty(last_.row);
    if (cell.wide() == Wide::Narrow &&
        unicode::cluster_width(g.cluster(last_.row, last_.col), opts_.ambiguous_wide) == 2) {
        widen_last_cluster();
    } else if (cp == 0xFE0E && cell.wide() == Wide::Lead &&
               unicode::cluster_width(g.cluster(last_.row, last_.col), opts_.ambiguous_wide) == 1) {
        narrow_last_cluster();
    }
    return true;
}

void Terminal::attach_zero_width(char32_t cp) {
    Cursor& c = cur();
    int y = c.row;
    int x = c.pending_wrap ? c.col : c.col - 1;
    if (x < 0) return;
    Grid& g = grid();
    if (g.at(y, x).wide() == Wide::SpacerTail && x > 0) --x;
    Cell& cell = g.at(y, x);
    if (cell.is_empty() || cell.is_spacer()) return;
    cell.set_cluster(true);
    g.clusters_mut(y).append(x, cp);
    g.mark_dirty(y);
}

// VS15 after an emoji that is wide by default (U+231A WATCH, U+1F600 ...):
// text presentation, one column. The spacer is released and the cursor
// steps back onto it, as in Ghostty.
void Terminal::narrow_last_cluster() {
    Cursor& c = cur();
    Grid& g = grid();
    const int y = last_.row;
    const int x = last_.col;
    Cell& lead = g.at(y, x);
    lead.set_wide(Wide::Narrow);
    if (x + 1 < cols_ && g.at(y, x + 1).wide() == Wide::SpacerTail) g.at(y, x + 1) = Cell::blank(lead.style);
    g.mark_dirty(y);
    c.row = y;
    c.col = x + 1;
    c.pending_wrap = false;
    if (c.col > right_edge()) {  // cannot happen for a pair that fitted, but stay safe
        c.col = right_edge();
        c.pending_wrap = modes_.autowrap;
    }
    last_.epoch = epoch_;
    last_.cur_row = c.row;
    last_.cur_col = c.col;
    last_.cur_pending = c.pending_wrap;
}

void Terminal::widen_last_cluster() {
    Cursor& c = cur();
    Grid& g = grid();
    int y = last_.row;
    int x = last_.col;
    int re = right_edge();
    if (x + 1 <= re) {
        clear_wide_at(y, x + 1);
        Cell& lead = g.at(y, x);
        lead.set_wide(Wide::Lead);
        g.at(y, x + 1) = Cell::make(0, lead.style, Wide::SpacerTail);
        c.pending_wrap = false;
        c.col = x + 2;
        if (c.col > re) {
            c.col = re;
            c.pending_wrap = modes_.autowrap;
        }
    } else if (modes_.autowrap) {
        // The cluster no longer fits on this row: move it to the next one.
        std::u32string cl = g.cluster(y, x);
        uint32_t style = g.at(y, x).style;
        bool prot = g.at(y, x).is_protected();
        g.at(y, x) = Cell::make(0, c.bce_id, Wide::SpacerHead);
        g.clusters_mut(y).erase(x);
        c.pending_wrap = false;
        c.col = x;
        wrap_line();
        int ny = c.row;
        int nx = c.col;
        clear_wide_at(ny, nx);
        if (nx + 1 < cols_) clear_wide_at(ny, nx + 1);
        Cell lead = Cell::make(cl[0], style, Wide::Lead);
        lead.set_protected(prot);
        if (cl.size() > 1) {
            lead.set_cluster(true);
            g.clusters_mut(ny).set(nx, std::u32string_view(cl).substr(1));
        }
        g.at(ny, nx) = lead;
        if (nx + 1 < cols_) g.at(ny, nx + 1) = Cell::make(0, style, Wide::SpacerTail);
        g.mark_dirty(ny);
        last_.row = ny;
        last_.col = nx;
        c.col = nx + 2;
        int nre = right_edge();
        if (c.col > nre) {
            c.col = nre;
            c.pending_wrap = true;
        }
    } else {
        return;
    }
    last_.epoch = epoch_;
    last_.cur_row = c.row;
    last_.cur_col = c.col;
    last_.cur_pending = c.pending_wrap;
}

void Terminal::print_cluster_start(char32_t cp, int w, const unicode::GraphemeSegmenter* seg, unicode::Props p) {
    Cursor& c = cur();
    if (c.pending_wrap) {
        if (modes_.autowrap) wrap_line();
        else c.pending_wrap = false;
    }
    int re = right_edge();
    Grid& g = grid();
    if (w == 2 && c.col + 1 > re) {
        if (modes_.autowrap && re - left_edge() >= 1) {
            clear_wide_at(c.row, c.col);
            g.at(c.row, c.col) = Cell::make(0, c.bce_id, Wide::SpacerHead);  // background-erased
            g.mark_dirty(c.row);
            wrap_line();
            re = right_edge();
        } else if (re >= 1) {
            c.col = re - 1;
        } else {
            w = 1;  // a one-column screen cannot hold a wide cell
        }
    }
    if (modes_.insert) insert_chars(w);
    const int x = c.col;
    Cell* r = g.row(c.row);
    if (needs_clear(r[x])) clear_wide_at(c.row, x);
    const uint32_t prot = c.protect ? Cell::kProtectedBit : 0u;
    if (w == 2) {
        if (needs_clear(r[x + 1])) clear_wide_at(c.row, x + 1);
        r[x].bits = (uint32_t(cp) & Cell::kCpMask) | (uint32_t(Wide::Lead) << 21) | prot;
        r[x].style = c.pen_id;
        r[x + 1].bits = (uint32_t(Wide::SpacerTail) << 21) | prot;
        r[x + 1].style = c.pen_id;
    } else {
        r[x].bits = (uint32_t(cp) & Cell::kCpMask) | prot;
        r[x].style = c.pen_id;
    }
    g.mark_dirty(c.row);
    c.col += w;
    if (c.col > re) {
        c.col = re;
        if (modes_.autowrap) c.pending_wrap = true;
    }
    last_.valid = true;
    last_.row = c.row;
    last_.col = x;
    last_.cp = cp;
    last_.epoch = epoch_;
    last_.cur_row = c.row;
    last_.cur_col = c.col;
    last_.cur_pending = c.pending_wrap;
    if (seg) {
        last_.seg = *seg;
    } else {
        last_.seg.reset();
        last_.seg.next_props(p);
    }
}

void Terminal::repeat_last(int n) {
    if (!last_.valid || last_.cp == 0) return;
    char32_t cp = last_.cp;
    n = std::min(n, cols_ * rows_);
    for (int i = 0; i < n; ++i) {
        if (cp >= 0x20 && cp < 0x7F) {
            char ch = char(cp);
            // REP repeats the graphic character as displayed: bypass charset mapping.
            Cursor& c = cur();
            int saved_ss = c.cs.single_shift;
            char saved = c.cs.g[c.cs.gl];
            c.cs.g[c.cs.gl] = 'B';
            c.cs.single_shift = -1;
            print_ascii(&ch, 1);
            c.cs.g[c.cs.gl] = saved;
            c.cs.single_shift = int8_t(saved_ss);
        } else {
            const unicode::Props p = unicode::properties(cp);
            int w = unicode::width_of(p, opts_.ambiguous_wide);
            if (w > 0) print_cluster_start(cp, w, nullptr, p);
        }
    }
}

} // namespace bropty
