// Terminal core: construction, printing (the hot path), C0 controls, cursor
// movement and the screen-editing primitives the CSI dispatcher builds on.
#include "bropty/terminal.h"

#include "graphics_state.h"

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
      scrollback_(opts_.scrollback_rows, opts_.cols),
      gfx_(std::make_unique<detail::Graphics>(opts_.graphics)) {
    parser_.set_max_string_bytes(opts_.max_string_bytes);
    palette_ = Palette::standard();
    primary_.grid.rebase(reserve_stamps());
    alt_.grid.rebase(reserve_stamps());
    reset();
}

Terminal::~Terminal() = default;

void Terminal::reset() {
    ++change_count_;
    parser_.reset();
    const bool was_132 = modes_.deccolm;
    modes_ = Modes{};
    xtsaved_.clear();
    modes_.grapheme_clustering = opts_.grapheme_clustering;
    for (Screen* s : {&primary_, &alt_}) {
        s->cur = Cursor{};
        s->saved = Saved{};
        s->kitty_flags.clear();
        for (int y = 0; y < rows_; ++y) s->grid.clear_row(y, Cell{});
    }
    active_ = &primary_;
    styles_.clear();
    zone_ = Zone::None;
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
    gfx_->reset();
    invalidate_print();
    // Hyperlinks referenced only from history survive; the rest go at the next sweep.
    if (was_132 && cols_ != 80) {
        // xterm's RIS returns a 132-column screen to 80 columns.
        resize(80, rows_);
        if (host_) host_->resized_by_application(cols_, rows_);
    }
}

void Terminal::feed(std::string_view bytes) {
    ++change_count_;
    parser_.feed(bytes);
    graphics_after_feed();
    maybe_collect_garbage();
    ++change_count_;  // observers (resize, screen switch) may have looked mid-feed
}

RowView Terminal::row_at(int64_t abs) const {
    const int64_t top = screen_top_row();
    if (abs >= top) return abs < top + rows_ ? row(int(abs - top)) : RowView{};
    if (alt_screen_active() || abs < first_row()) return RowView{};
    return scrollback_.row(size_t(abs - first_row()));
}

void RowSource::add_observer(TerminalObserver* o) {
    if (o && std::find(observers_.begin(), observers_.end(), o) == observers_.end()) observers_.push_back(o);
}

void RowSource::remove_observer(TerminalObserver* o) {
    observers_.erase(std::remove(observers_.begin(), observers_.end(), o), observers_.end());
}

void RowSource::notify_before_resize() {
    for (TerminalObserver* o : observers_) o->before_resize();
}

void RowSource::notify_after_resize() {
    for (TerminalObserver* o : observers_) o->after_resize();
}

void RowSource::notify_screen_switched() {
    for (TerminalObserver* o : observers_) o->screen_switched();
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
    c.pen.zone = zone_;  // the OSC 133 zone is terminal state, not SGR state
    c.pen_id = styles_.intern(c.pen);
    // Most SGRs leave the background alone: skip re-interning the erase style.
    // (bce_id stays alive across style sweeps: it is a GC root.)
    if (c.bce_valid && c.bce_bg == c.pen.bg) return;
    Style b;
    b.bg = c.pen.bg;
    c.bce_id = styles_.intern(b);
    c.bce_bg = c.pen.bg;
    c.bce_valid = true;
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
    const bool target_changed = target != active_;
    if (target_changed) {
        Cursor carried = active_->cur;
        if (!alt && clear_alt) {
            for (int y = 0; y < rows_; ++y) alt_.grid.clear_row(y, Cell{});
        }
        active_ = target;
        active_->cur = carried;
        if (alt && clear_alt) {
            for (int y = 0; y < rows_; ++y) alt_.grid.clear_row(y, Cell::blank(carried.bce_id));
        }
        // The alternate screen's images go with its text.
        if (clear_alt) gfx_->clear_layer(true);
        active_->grid.mark_all_dirty();
    }
    if (!alt && save_restore) restore_cursor();
    invalidate_print();
    if (target_changed) {
        for (TerminalObserver* o : observers_) o->screen_switched();
    }
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
    if (gfx_->anchored(active_ == &alt_)) graphics_scrolled(top, bottom, left, right, n, to_history);
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
    if (gfx_->anchored(active_ == &alt_)) graphics_scrolled(top, bottom, left, right, -n, false);
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
        // "The clear screen escape code should also clear all images" (kitty).
        if (!selective && gfx_->anchored(active_ == &alt_)) {
            gfx_->clear_rows(active_ == &alt_, screen_top_row(), end_row(), image_cell_width(),
                             image_cell_height());
        }
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
