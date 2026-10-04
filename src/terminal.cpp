// Terminal core: construction, printing (the hot path), C0 controls, cursor
// movement and the screen-editing primitives the CSI dispatcher builds on.
#include "bropty/terminal.h"

#include <algorithm>
#include <cstring>

namespace bropty {

namespace {

TerminalOptions normalized(TerminalOptions o) {
    o.cols = std::max(2, o.cols);
    o.rows = std::max(1, o.rows);
    return o;
}

TerminalOptions make_options(int cols, int rows, size_t sb) {
    TerminalOptions o;
    o.cols = cols;
    o.rows = rows;
    o.scrollback_rows = sb;
    return o;
}

// DEC Special Graphics for 0x5F..0x7E.
constexpr char16_t kDecGraphics[32] = {
    0x00A0, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1, 0x2424, 0x240B,
    0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C,
    0x2524, 0x2534, 0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,
};

} // namespace

Terminal::Terminal() : Terminal(TerminalOptions()) {}

Terminal::Terminal(int cols, int rows, size_t scrollback_rows)
    : Terminal(make_options(cols, rows, scrollback_rows)) {}

Terminal::Terminal(const TerminalOptions& options)
    : opts_(normalized(options)),
      parser_(this),
      cols_(opts_.cols),
      rows_(opts_.rows),
      primary_(opts_.cols, opts_.rows),
      alt_(opts_.cols, opts_.rows),
      active_(&primary_),
      scrollback_(opts_.scrollback_rows, opts_.cols) {
    parser_.set_max_string_bytes(opts_.max_string_bytes);
    palette_ = Palette::standard();
    reset();
}

Terminal::~Terminal() = default;

void Terminal::reset() {
    parser_.reset();
    modes_ = Modes{};
    modes_.grapheme_clustering = opts_.grapheme_clustering;
    for (Screen* s : {&primary_, &alt_}) {
        s->cur = Cursor{};
        s->saved = Saved{};
        s->kitty_flags.clear();
        for (int y = 0; y < rows_; ++y) s->grid.clear_row(y, Cell{});
    }
    active_ = &primary_;
    styles_.clear();
    update_pen();
    alt_.cur.pen_id = primary_.cur.pen_id;
    alt_.cur.bce_id = primary_.cur.bce_id;
    reset_margins();
    tabs_.assign(size_t(cols_), 0);
    for (int x = 8; x < cols_; x += 8) tabs_[size_t(x)] = 1;
    palette_ = Palette::standard();
    cursor_shape_ = CursorShape::Block;
    cursor_shape_blink_ = true;
    title_stack_.clear();
    last_ = LastPrint{};
    dcs_ = Dcs::None;
    invalidate_print();
    // Hyperlinks referenced only from history survive; the rest go at the next sweep.
}

void Terminal::feed(std::string_view bytes) {
    parser_.feed(bytes);
    maybe_collect_garbage();
}

CursorState Terminal::cursor() const noexcept {
    const Cursor& c = active_->cur;
    CursorState s;
    s.row = c.row;
    s.col = c.col;
    s.pending_wrap = c.pending_wrap;
    s.visible = modes_.cursor_visible;
    s.shape = cursor_shape_;
    s.blink = cursor_shape_blink_;
    return s;
}

uint32_t Terminal::kitty_keyboard_flags() const noexcept {
    return active_->kitty_flags.empty() ? 0u : active_->kitty_flags.back();
}

RowView Terminal::row(int y) const noexcept { return active_->grid.view(y, styles_.data()); }
RowView Terminal::primary_row(int y) const noexcept { return primary_.grid.view(y, styles_.data()); }

const Hyperlink* Terminal::hyperlink(uint32_t id) const noexcept {
    auto it = links_.find(id);
    return it == links_.end() ? nullptr : &it->second;
}

void Terminal::update_pen() {
    Cursor& c = cur();
    c.pen_id = styles_.intern(c.pen);
    Style b;
    b.bg = c.pen.bg;
    c.bce_id = styles_.intern(b);
}

void Terminal::reset_margins() {
    top_ = 0;
    bottom_ = rows_ - 1;
    left_ = 0;
    right_ = cols_ - 1;
}

bool Terminal::cursor_in_margins() const noexcept {
    const Cursor& c = active_->cur;
    return c.row >= top_ && c.row <= bottom_ && c.col >= left_ && c.col <= right_;
}

int Terminal::right_edge() const noexcept { return active_->cur.col <= right_ ? right_ : cols_ - 1; }
int Terminal::left_edge() const noexcept { return active_->cur.col >= left_ ? left_ : 0; }

// ---------------------------------------------------------------------------
// Printing

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
    Cell c = Cell::make(cp, cur().pen_id, w);
    c.set_protected(cur().protect);
    grid().row(y)[x] = c;
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
        uint32_t base = c.protect ? Cell::kProtectedBit : 0u;
        for (int j = 0; j < k; ++j) r[first + j] = Cell{uint32_t(uint8_t(s[i + size_t(j)])) | base, c.pen_id};
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
    last_.seg.reset();
    last_.seg.next(last_.cp);
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
    int w = unicode::width(cp, opts_.ambiguous_wide);
    if (modes_.grapheme_clustering && try_extend_cluster(cp)) return;
    if (w == 0) {
        attach_zero_width(cp);
        return;
    }
    print_cluster_start(cp, w);
}

bool Terminal::try_extend_cluster(char32_t cp) {
    const Cursor& c = cur();
    if (!last_.valid || last_.epoch != epoch_ || c.row != last_.cur_row || c.col != last_.cur_col ||
        c.pending_wrap != last_.cur_pending)
        return false;
    unicode::GraphemeSegmenter seg = last_.seg;
    if (seg.next(cp)) return false;
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

void Terminal::print_cluster_start(char32_t cp, int w) {
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
    int x = c.col;
    clear_wide_at(c.row, x);
    if (w == 2) {
        clear_wide_at(c.row, x + 1);
        write_cell(c.row, x, cp, Wide::Lead);
        write_cell(c.row, x + 1, 0, Wide::SpacerTail);
    } else {
        write_cell(c.row, x, cp, Wide::Narrow);
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
    last_.seg.reset();
    last_.seg.next(cp);
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
            int w = unicode::width(cp, opts_.ambiguous_wide);
            if (w > 0) print_cluster_start(cp, w);
        }
    }
}

// ---------------------------------------------------------------------------
// C0 controls and cursor motion

void Terminal::execute(uint8_t c0) {
    switch (c0) {
    case 0x05:
        if (!opts_.answerback.empty()) reply(opts_.answerback);
        return;
    case 0x07:
        if (host_) host_->bell();
        return;
    case 0x08: backspace(); break;
    case 0x09: tab_forward(1); break;
    case 0x0A:
    case 0x0B:
    case 0x0C: linefeed(); break;
    case 0x0D: carriage_return(); break;
    case 0x0E: cur().cs.gl = 1; return;
    case 0x0F: cur().cs.gl = 0; return;
    default: return;
    }
    invalidate_print();
}

void Terminal::backspace() {
    Cursor& c = cur();
    // From the pending-wrap state BS moves left like anywhere else (xterm);
    // only with reverse-wrap does it merely cancel the pending wrap.
    if (c.pending_wrap) {
        c.pending_wrap = false;
        if (modes_.reverse_wrap) return;
    }
    int le = left_edge();
    if (c.col > le) {
        --c.col;
    } else if (modes_.reverse_wrap && modes_.autowrap && c.row > 0 && grid().wrapped(c.row - 1)) {
        --c.row;
        c.col = right_edge();
    }
}

void Terminal::carriage_return() {
    Cursor& c = cur();
    c.pending_wrap = false;
    c.col = (modes_.origin || c.col >= left_) ? left_ : 0;
}

void Terminal::linefeed() {
    index();
    if (modes_.linefeed_newline) carriage_return();
}

void Terminal::index() {
    Cursor& c = cur();
    c.pending_wrap = false;
    if (c.row == bottom_) {
        if (c.col >= left_ && c.col <= right_) scroll_up(1);
    } else if (c.row < rows_ - 1) {
        ++c.row;
    }
}

void Terminal::reverse_index() {
    Cursor& c = cur();
    c.pending_wrap = false;
    if (c.row == top_) {
        if (c.col >= left_ && c.col <= right_) scroll_down(1);
    } else if (c.row > 0) {
        --c.row;
    }
}

void Terminal::tab_forward(int n) {
    Cursor& c = cur();
    int edge = right_edge();
    while (n-- > 0 && c.col < edge) {
        ++c.col;
        while (c.col < edge && !tabs_[size_t(c.col)]) ++c.col;
    }
}

void Terminal::tab_backward(int n) {
    Cursor& c = cur();
    c.pending_wrap = false;
    int edge = left_edge();
    while (n-- > 0 && c.col > edge) {
        --c.col;
        while (c.col > edge && !tabs_[size_t(c.col)]) --c.col;
    }
}

void Terminal::move_to(int row, int col) {
    Cursor& c = cur();
    c.pending_wrap = false;
    if (modes_.origin) {
        c.row = std::clamp(row + top_, top_, bottom_);
        c.col = std::clamp(col + left_, left_, right_);
    } else {
        c.row = std::clamp(row, 0, rows_ - 1);
        c.col = std::clamp(col, 0, cols_ - 1);
    }
}

void Terminal::move_cursor_rel(int drow, int dcol) {
    Cursor& c = cur();
    c.pending_wrap = false;
    if (drow < 0) c.row = std::max(c.row + drow, c.row >= top_ ? top_ : 0);
    if (drow > 0) c.row = std::min(c.row + drow, c.row <= bottom_ ? bottom_ : rows_ - 1);
    if (dcol < 0) c.col = std::max(c.col + dcol, c.col >= left_ ? left_ : 0);
    if (dcol > 0) c.col = std::min(c.col + dcol, c.col <= right_ ? right_ : cols_ - 1);
}

void Terminal::save_cursor() {
    Screen& s = *active_;
    s.saved.valid = true;
    s.saved.row = s.cur.row;
    s.saved.col = s.cur.col;
    s.saved.pending_wrap = s.cur.pending_wrap;
    s.saved.pen = s.cur.pen;
    s.saved.protect = s.cur.protect;
    s.saved.origin = modes_.origin;
    s.saved.cs = s.cur.cs;
}

void Terminal::restore_cursor() {
    Screen& s = *active_;
    if (!s.saved.valid) {
        s.cur.pen = Style{};
        s.cur.protect = false;
        s.cur.cs = Charsets{};
        modes_.origin = false;
        s.cur.row = 0;
        s.cur.col = 0;
        s.cur.pending_wrap = false;
    } else {
        s.cur.pen = s.saved.pen;
        s.cur.protect = s.saved.protect;
        s.cur.cs = s.saved.cs;
        modes_.origin = s.saved.origin;
        s.cur.row = std::clamp(s.saved.row, 0, rows_ - 1);
        s.cur.col = std::clamp(s.saved.col, 0, cols_ - 1);
        s.cur.pending_wrap = s.saved.pending_wrap && s.cur.col == cols_ - 1;
    }
    update_pen();
    invalidate_print();
}

void Terminal::switch_screen(bool alt, bool clear_alt, bool save_restore) {
    Screen* target = alt ? &alt_ : &primary_;
    if (alt && save_restore) save_cursor();
    if (target != active_) {
        Cursor carried = active_->cur;
        if (!alt && clear_alt) {
            for (int y = 0; y < rows_; ++y) alt_.grid.clear_row(y, Cell{});
        }
        active_ = target;
        active_->cur = carried;
        if (alt && clear_alt) {
            for (int y = 0; y < rows_; ++y) alt_.grid.clear_row(y, Cell::blank(carried.bce_id));
        }
        active_->grid.mark_all_dirty();
    }
    if (!alt && save_restore) restore_cursor();
    invalidate_print();
}

// ---------------------------------------------------------------------------
// Scrolling and editing

void Terminal::scroll_up(int n) {
    bool full_width = left_ == 0 && right_ == cols_ - 1;
    scroll_region_up(top_, bottom_, left_, right_, n, full_width && top_ == 0 && active_ == &primary_);
}

void Terminal::scroll_down(int n) { scroll_region_down(top_, bottom_, left_, right_, n); }

// Blank any half of a wide pair that an operation on [x0, x1) of a row
// orphaned, and any SpacerHead it moved off the last column.
void Terminal::sanitize_row(int y, int x0, int x1) {
    Grid& g = grid();
    Cell* r = g.row(y);
    int cols = g.cols();
    x0 = std::max(0, x0 - 1);
    x1 = std::min(cols, x1 + 1);
    for (int x = x0; x < x1; ++x) {
        Wide w = r[x].wide();
        if (w == Wide::Lead && (x + 1 >= cols || r[x + 1].wide() != Wide::SpacerTail)) {
            if (r[x].has_cluster()) g.clusters_mut(y).erase(x);
            r[x] = Cell::blank(r[x].style);
        } else if (w == Wide::SpacerTail && (x == 0 || r[x - 1].wide() != Wide::Lead)) {
            r[x] = Cell::blank(r[x].style);
        } else if (w == Wide::SpacerHead && x != cols - 1) {
            r[x] = Cell::blank(r[x].style);
        }
    }
}

void Terminal::scroll_region_up(int top, int bottom, int left, int right, int n, bool to_history) {
    if (n <= 0 || top > bottom) return;
    n = std::min(n, bottom - top + 1);
    Grid& g = grid();
    Cell blank = Cell::blank(cur().bce_id);
    invalidate_print();
    if (left == 0 && right == cols_ - 1) {
        if (to_history) {
            for (int i = 0; i < n; ++i) {
                int y = top + i;
                scrollback_.push_row(g.row(y), cols_, g.flags(y), g.clusters(y), styles_.data());
            }
        }
        g.rotate_up(top, bottom, n);
        for (int y = bottom - n + 1; y <= bottom; ++y) g.clear_row(y, blank);
        return;
    }
    for (int y = top; y <= bottom - n; ++y) {
        g.copy_span(y + n, left, right + 1, y, left);
        sanitize_row(y, left, right + 1);
    }
    for (int y = std::max(top, bottom - n + 1); y <= bottom; ++y) {
        g.fill(y, left, right + 1, blank);
        sanitize_row(y, left, right + 1);
    }
}

void Terminal::scroll_region_down(int top, int bottom, int left, int right, int n) {
    if (n <= 0 || top > bottom) return;
    n = std::min(n, bottom - top + 1);
    Grid& g = grid();
    Cell blank = Cell::blank(cur().bce_id);
    invalidate_print();
    if (left == 0 && right == cols_ - 1) {
        g.rotate_down(top, bottom, n);
        for (int y = top; y < top + n; ++y) g.clear_row(y, blank);
        return;
    }
    for (int y = bottom; y >= top + n; --y) {
        g.copy_span(y - n, left, right + 1, y, left);
        sanitize_row(y, left, right + 1);
    }
    for (int y = top; y < top + n; ++y) {
        g.fill(y, left, right + 1, blank);
        sanitize_row(y, left, right + 1);
    }
}

void Terminal::erase_cells(int y, int x0, int x1, bool selective) {
    x0 = std::max(0, x0);
    x1 = std::min(cols_, x1);
    if (x0 >= x1) return;
    Grid& g = grid();
    Cell* r = g.row(y);
    // Erasing either half of a wide cell erases all of it.
    if (r[x0].wide() == Wide::SpacerTail && x0 > 0) --x0;
    if (r[x1 - 1].wide() == Wide::Lead && x1 < cols_) ++x1;
    Cell blank = Cell::blank(cur().bce_id);
    if (!selective) {
        g.fill(y, x0, x1, blank);
        return;
    }
    for (int x = x0; x < x1; ++x) {
        if (r[x].is_protected()) continue;
        if (r[x].has_cluster()) g.clusters_mut(y).erase(x);
        r[x] = blank;
    }
    sanitize_row(y, x0, x1);
    g.mark_dirty(y);
}

void Terminal::erase_line(int mode, bool selective) {
    Cursor& c = cur();
    c.pending_wrap = false;
    switch (mode) {
    case 0:
        erase_cells(c.row, c.col, cols_, selective);
        grid().set_flag(c.row, Row_Wrapped, false);
        break;
    case 1: erase_cells(c.row, 0, c.col + 1, selective); break;
    case 2:
        erase_cells(c.row, 0, cols_, selective);
        grid().set_flag(c.row, Row_Wrapped, false);
        break;
    default: return;
    }
    invalidate_print();
}

void Terminal::erase_display(int mode, bool selective) {
    Cursor& c = cur();
    switch (mode) {
    case 0:
        erase_line(0, selective);
        for (int y = c.row + 1; y < rows_; ++y) {
            erase_cells(y, 0, cols_, selective);
            if (!selective) grid().set_flags(y, 0);
        }
        break;
    case 1:
        for (int y = 0; y < c.row; ++y) {
            erase_cells(y, 0, cols_, selective);
            if (!selective) grid().set_flags(y, 0);
        }
        erase_line(1, selective);
        break;
    case 2:
        for (int y = 0; y < rows_; ++y) {
            erase_cells(y, 0, cols_, selective);
            if (!selective) grid().set_flags(y, 0);
        }
        c.pending_wrap = false;
        break;
    case 3:
        scrollback_.clear();
        break;
    default: return;
    }
    invalidate_print();
}

void Terminal::insert_chars(int n) {
    Cursor& c = cur();
    c.pending_wrap = false;
    if (c.col < left_ || c.col > right_) return;
    Grid& g = grid();
    int right = right_;
    n = std::min(n, right - c.col + 1);
    if (n <= 0) return;
    if (c.col + n <= right) g.copy_span(c.row, c.col, right + 1 - n, c.row, c.col + n);
    g.fill(c.row, c.col, c.col + n, Cell::blank(c.bce_id));
    sanitize_row(c.row, c.col, right + 1);
    invalidate_print();
}

void Terminal::delete_chars(int n) {
    Cursor& c = cur();
    c.pending_wrap = false;
    if (c.col < left_ || c.col > right_) return;
    Grid& g = grid();
    int right = right_;
    n = std::min(n, right - c.col + 1);
    if (n <= 0) return;
    if (g.at(c.row, c.col).wide() == Wide::SpacerTail) clear_wide_at(c.row, c.col);
    if (c.col + n <= right) g.copy_span(c.row, c.col + n, right + 1, c.row, c.col);
    g.fill(c.row, right + 1 - n, right + 1, Cell::blank(c.bce_id));
    sanitize_row(c.row, c.col, right + 1);
    invalidate_print();
}

void Terminal::erase_chars(int n) {
    Cursor& c = cur();
    c.pending_wrap = false;
    erase_cells(c.row, c.col, c.col + std::max(1, n), false);
    invalidate_print();
}

void Terminal::insert_lines(int n) {
    Cursor& c = cur();
    if (!cursor_in_margins()) return;
    scroll_region_down(c.row, bottom_, left_, right_, n);
    c.col = left_;
    c.pending_wrap = false;
}

void Terminal::delete_lines(int n) {
    Cursor& c = cur();
    if (!cursor_in_margins()) return;
    scroll_region_up(c.row, bottom_, left_, right_, n, false);
    c.col = left_;
    c.pending_wrap = false;
}

void Terminal::fill_alignment() {
    reset_margins();
    Grid& g = grid();
    for (int y = 0; y < rows_; ++y) {
        g.clear_row(y, Cell::make('E', 0));
    }
    modes_.origin = false;
    move_to(0, 0);
    invalidate_print();
}

// ---------------------------------------------------------------------------
// Garbage collection of interned styles and hyperlinks

void Terminal::maybe_collect_garbage() {
    if (styles_.wants_sweep()) {
        std::vector<uint8_t> marked(styles_.capacity_ids(), 0);
        auto mark = [&](const Cell& c) {
            if (c.style < marked.size()) marked[c.style] = 1;
        };
        primary_.grid.for_each_cell(mark);
        alt_.grid.for_each_cell(mark);
        for (Screen* s : {&primary_, &alt_}) {
            if (s->cur.pen_id < marked.size()) marked[s->cur.pen_id] = 1;
            if (s->cur.bce_id < marked.size()) marked[s->cur.bce_id] = 1;
        }
        styles_.sweep(marked);
    }
    if (links_.size() > link_sweep_threshold_) collect_links();
}

void Terminal::collect_links() {
    std::vector<uint32_t> live;
    for (size_t id = 0; id < styles_.capacity_ids(); ++id) {
        if (uint32_t l = styles_.get(uint32_t(id)).link) live.push_back(l);
    }
    for (Screen* s : {&primary_, &alt_}) {
        live.push_back(s->cur.pen.link);
        live.push_back(s->saved.pen.link);
    }
    scrollback_.collect_links(live);
    std::sort(live.begin(), live.end());
    for (auto it = links_.begin(); it != links_.end();) {
        if (!std::binary_search(live.begin(), live.end(), it->first)) {
            if (!it->second.id.empty()) link_by_id_.erase(it->second.id + '\x1f' + it->second.uri);
            it = links_.erase(it);
        } else {
            ++it;
        }
    }
    link_sweep_threshold_ = std::max<size_t>(1024, links_.size() * 2);
}

} // namespace bropty
