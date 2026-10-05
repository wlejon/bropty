// Selection gestures, word / line / zone expansion and the maintenance that
// keeps a selection on its text (verify, resize). Extraction is in
// selection_text.cpp.
#include "bropty/selection.h"

#include "bropty/terminal.h"
#include "buffer_lines.h"

#include <algorithm>

namespace bropty {

using detail::BufferLines;
using detail::Line;

namespace {

bool is_space_cp(char32_t cp) noexcept {
    return cp == 0 || cp == U' ' || cp == U'\t' || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
           cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

// Quotation marks and CJK sentence punctuation end words even outside ASCII.
bool is_unicode_punct(char32_t cp) noexcept {
    return (cp >= 0x2018 && cp <= 0x201F) || cp == 0xAB || cp == 0xBB || cp == 0x3001 || cp == 0x3002 ||
           (cp >= 0x300C && cp <= 0x300F) || (cp >= 0xFF08 && cp <= 0xFF09) || cp == 0xFF0C;
}

// Walk cells forward / backward from a position across logical lines. The
// callback gets (line, cell index) and returns false to stop.
template <class F>
void walk_forward(const BufferLines& bl, Line line, size_t from, F&& f) {
    for (;;) {
        for (size_t i = from; i < line.size(); ++i)
            if (!f(line, i)) return;
        const int64_t next = bl.next_line(line);
        if (!bl.line(next, line)) return;
        from = 0;
    }
}

template <class F>
void walk_backward(const BufferLines& bl, Line line, size_t from, F&& f) {
    for (;;) {
        for (size_t i = from; i > 0; --i)
            if (!f(line, i - 1)) return;
        const int64_t prev = bl.prev_line(line);
        if (prev < bl.first_line() || !bl.line(prev, line)) return;
        from = line.size();
    }
}

// The zone a cell reads as for selection: spacer tails take their lead's.
Zone zone_at(const Line& l, size_t i) noexcept {
    if (i > 0 && l.cell(i).wide() == Wide::SpacerTail) --i;
    return l.zone(i);
}

size_t cell_end(const Line& l, size_t i) noexcept {
    return i + ((l.cell(i).wide() == Wide::Lead && i + 1 < l.size()) ? 2 : 1);
}

} // namespace

Selection::Selection(const RowSource& source) : t_(source) {}

RowPos Selection::boundary(RowPos cell, bool right_half) const noexcept {
    return RowPos{cell.row, std::clamp(cell.col + (right_half ? 1 : 0), 0, t_.cols())};
}

void Selection::clear() noexcept {
    if (active_) ++version_;
    active_ = false;
    range_ = RowRange{};
    checks_.clear();
}

void Selection::set_range(RowRange r) {
    if (mode_ != SelectionMode::Block && r.end < r.start) std::swap(r.start, r.end);
    if (mode_ != SelectionMode::Block) {
        // Never split a wide character.
        RowView v = t_.row_at(r.start.row);
        if (v.cells && r.start.col > 0 && r.start.col < v.cols && v.cells[r.start.col].wide() == Wide::SpacerTail)
            --r.start.col;
        v = t_.row_at(r.end.row);
        if (v.cells && r.end.col > 0 && r.end.col < v.cols && v.cells[r.end.col].wide() == Wide::SpacerTail)
            ++r.end.col;
    }
    range_ = r;
    active_ = true;
    ++version_;
    rebaseline();
}

void Selection::start(RowPos cell, SelectionMode mode, bool right_half) {
    clear();
    mode_ = mode;
    switch (mode) {
    case SelectionMode::Character: anchor_lo_ = anchor_hi_ = boundary(cell, right_half); break;
    case SelectionMode::Word:
        if (!word_bounds(cell, anchor_lo_, anchor_hi_)) anchor_lo_ = anchor_hi_ = boundary(cell, right_half);
        break;
    case SelectionMode::Line:
        if (!line_bounds(cell, anchor_lo_, anchor_hi_)) return;
        break;
    case SelectionMode::Block:
        anchor_lo_ = anchor_hi_ = RowPos{cell.row, std::clamp(cell.col, 0, t_.cols() - 1)};
        break;
    case SelectionMode::Zone:
        select_zone(cell);
        return;
    }
    recompute();
}

void Selection::recompute() {
    if (mode_ == SelectionMode::Block) {
        RowRange r;
        r.start = RowPos{std::min(anchor_lo_.row, anchor_hi_.row), std::min(anchor_lo_.col, anchor_hi_.col)};
        r.end = RowPos{std::max(anchor_lo_.row, anchor_hi_.row), std::max(anchor_lo_.col, anchor_hi_.col) + 1};
        set_range(r);
        return;
    }
    set_range(RowRange{anchor_lo_, anchor_hi_});
}

void Selection::extend(RowPos cell, bool right_half) {
    if (!active_) return;
    RowPos lo, hi;
    switch (mode_) {
    case SelectionMode::Word:
        if (!word_bounds(cell, lo, hi)) lo = hi = boundary(cell, right_half);
        break;
    case SelectionMode::Line:
        if (!line_bounds(cell, lo, hi)) return;
        break;
    case SelectionMode::Block: {
        RowPos c{cell.row, std::clamp(cell.col, 0, t_.cols() - 1)};
        RowRange r;
        r.start = RowPos{std::min(anchor_lo_.row, c.row), std::min(anchor_lo_.col, c.col)};
        r.end = RowPos{std::max(anchor_lo_.row, c.row), std::max(anchor_lo_.col, c.col) + 1};
        set_range(r);
        return;
    }
    default: lo = hi = boundary(cell, right_half); break;
    }
    set_range(RowRange{std::min(anchor_lo_, lo), std::max(anchor_hi_, hi)});
}

void Selection::select_range(RowRange r) {
    clear();
    mode_ = SelectionMode::Character;
    anchor_lo_ = r.start;
    anchor_hi_ = r.end;
    set_range(r);
}

void Selection::select_all() {
    clear();
    mode_ = SelectionMode::Line;
    // To the end of the last row with anything on it (not the empty rows
    // below the output).
    int64_t last = t_.end_row() - 1;
    for (; last > t_.first_row(); --last) {
        const RowView v = t_.row_at(last);
        bool ink = v.wrapped();
        for (int x = 0; x < v.cols && !ink; ++x) ink = !v.cells[x].is_empty() || v.cells[x].is_spacer();
        if (ink) break;
    }
    anchor_lo_ = RowPos{t_.first_row(), 0};
    anchor_hi_ = RowPos{last, t_.cols()};
    set_range(RowRange{anchor_lo_, anchor_hi_});
}

bool Selection::word_bounds(RowPos cell, RowPos& lo, RowPos& hi) const {
    BufferLines bl(t_);
    Line l;
    if (!bl.line_at_row(cell.row, l)) return false;
    size_t o = l.offset_of(RowPos{cell.row, std::max(0, cell.col)});
    if (o >= l.size()) return false;
    if (o > 0 && l.cell(o).wide() == Wide::SpacerTail) --o;
    auto cls = [&](size_t i) -> uint64_t {
        const Cell& c = l.cell(i);
        const char32_t cp = c.cp();
        if (is_space_cp(cp)) return 0;
        bool word;
        if (cp < 0x80) {
            word = (cp >= U'0' && cp <= U'9') || (cp >= U'a' && cp <= U'z') || (cp >= U'A' && cp <= U'Z');
        } else {
            word = !is_unicode_punct(cp);
        }
        if (word || word_chars_.find(cp) != std::u32string::npos) return 1;
        return 2 + uint64_t(cp);
    };
    const uint64_t k = cls(o);
    size_t a = o;
    while (a > 0) {
        size_t p = a - 1;
        if (p > 0 && l.cell(p).wide() == Wide::SpacerTail) --p;
        if (cls(p) != k) break;
        a = p;
    }
    size_t b = cell_end(l, o);
    while (b < l.size() && cls(b) == k) b = cell_end(l, b);
    lo = l.pos_of(a);
    hi = l.end_pos(b);
    return true;
}

bool Selection::line_bounds(RowPos cell, RowPos& lo, RowPos& hi) const {
    BufferLines bl(t_);
    Line l;
    if (!bl.line_at_row(cell.row, l)) return false;
    lo = RowPos{l.first_row, 0};
    hi = RowPos{l.end_row() - 1, t_.cols()};
    return true;
}

// The zone range around `cell` (want == None: whichever zone it is in).
bool Selection::zone_range(RowPos cell, Zone want, RowRange& out) const {
    BufferLines bl(t_);
    Line l;
    if (!bl.line_at_row(cell.row, l)) return false;
    const size_t o = l.offset_of(cell);
    Zone z = o < l.size() ? zone_at(l, o) : Zone::None;
    if (z == Zone::None) {
        // A blank inside a zone (an empty output line): the zone on both sides.
        Zone before = Zone::None, after = Zone::None;
        walk_backward(bl, l, o, [&](const Line& ln, size_t i) {
            before = zone_at(ln, i);
            return before == Zone::None;
        });
        walk_forward(bl, l, o, [&](const Line& ln, size_t i) {
            after = zone_at(ln, i);
            return after == Zone::None;
        });
        if (before != after) return false;
        z = before;
    }
    if (z == Zone::None || (want != Zone::None && z != want)) return false;
    bool found = false;
    walk_backward(bl, l, std::min(o + 1, l.size()), [&](const Line& ln, size_t i) {
        Zone c = zone_at(ln, i);
        if (c == z) {
            out.start = ln.pos_of(i);
            found = true;
        }
        return c == z || c == Zone::None;
    });
    walk_forward(bl, l, o, [&](const Line& ln, size_t i) {
        Zone c = zone_at(ln, i);
        if (c == z) {
            out.end = ln.end_pos(cell_end(ln, i));
            found = true;
        }
        return c == z || c == Zone::None;
    });
    if (!found || !(out.start < out.end)) return false;
    return true;
}

bool Selection::select_zone(RowPos cell) {
    RowRange r;
    if (!zone_range(cell, Zone::None, r)) {
        clear();
        return false;
    }
    clear();
    mode_ = SelectionMode::Zone;
    anchor_lo_ = r.start;
    anchor_hi_ = r.end;
    set_range(r);
    return true;
}

bool Selection::select_output(RowPos cell) {
    BufferLines bl(t_);
    Line l;
    if (!bl.line_at_row(cell.row, l)) return false;
    const size_t o = std::min(l.offset_of(cell), l.size());
    // From the cell forward: the first output cell before the next prompt
    // that follows an input (the next command).
    bool seen_input = false;
    bool found = false;
    RowPos at;
    walk_forward(bl, l, o, [&](const Line& ln, size_t i) {
        Zone z = zone_at(ln, i);
        if (z == Zone::Output) {
            at = ln.pos_of(i);
            found = true;
            return false;
        }
        if (z == Zone::Input) seen_input = true;
        if (z == Zone::Prompt && seen_input) return false;
        return true;
    });
    if (!found) {
        clear();
        return false;
    }
    RowRange r;
    if (!zone_range(at, Zone::Output, r)) {
        clear();
        return false;
    }
    clear();
    mode_ = SelectionMode::Zone;
    anchor_lo_ = r.start;
    anchor_hi_ = r.end;
    set_range(r);
    return true;
}

bool Selection::select_last_output() {
    BufferLines bl(t_);
    Line l;
    if (!bl.line(bl.end_line() - 1, l)) return false;
    bool found = false;
    RowPos at;
    walk_backward(bl, l, l.size(), [&](const Line& ln, size_t i) {
        if (zone_at(ln, i) == Zone::Output) {
            at = ln.pos_of(i);
            found = true;
            return false;
        }
        return true;
    });
    if (!found) return false;
    return select_output(at);
}

bool Selection::row_span(int64_t row, int& c0, int& c1) const noexcept {
    if (!active_ || row < range_.start.row || row > range_.end.row) return false;
    if (mode_ == SelectionMode::Block) {
        c0 = range_.start.col;
        c1 = range_.end.col;
    } else {
        c0 = row == range_.start.row ? range_.start.col : 0;
        c1 = row == range_.end.row ? range_.end.col : t_.cols();
    }
    return c1 > c0;
}

// ---------------------------------------------------------------------------
// Maintenance

void Selection::rebaseline() {
    checks_.clear();
    if (!active_) return;
    const int64_t top = t_.screen_top_row();
    for (int64_t r = std::max(range_.start.row, top); r <= range_.end.row && r < t_.end_row(); ++r)
        checks_.push_back(RowCheck{r, detail::row_hash(t_.row_at(r))});
}

void Selection::verify() {
    if (!active_) return;
    const int64_t first = t_.first_row();
    const bool ends_before = range_.end.row < first || (range_.end.row == first && range_.end.col == 0 &&
                                                          mode_ != SelectionMode::Block);
    if (ends_before) {
        clear();
        return;
    }
    if (range_.start.row < first) {  // the front was evicted: keep the rest
        range_.start = RowPos{first, mode_ == SelectionMode::Block ? range_.start.col : 0};
        anchor_lo_ = std::max(anchor_lo_, RowPos{first, 0});
        anchor_hi_ = std::max(anchor_hi_, RowPos{first, 0});
        ++version_;
    }
    const int64_t top = t_.screen_top_row();
    size_t keep = 0;
    for (size_t i = 0; i < checks_.size(); ++i) {
        const RowCheck c = checks_[i];
        if (c.row < first) continue;
        if (c.row >= t_.end_row() || detail::row_hash(t_.row_at(c.row)) != c.hash) {
            clear();
            return;
        }
        if (c.row >= top) checks_[keep++] = c;  // still on the screen: keep watching
    }
    checks_.resize(keep);
}

void Selection::before_resize() {
    verify();  // what is carried must still be the selected text
    if (!active_) return;
    BufferLines bl(t_);
    if (!bl.can_carry()) return;  // after_resize() clears it
    const RowPos pos[4] = {anchor_lo_, anchor_hi_, range_.start, range_.end};
    for (int i = 0; i < 4; ++i) {
        detail::LinePos lp = bl.to_line_pos(pos[i]);
        detail::Line l;
        carried_[i] = Carried{lp.line, lp.offset, bl.line(lp.line, l) ? l.content_end() : 0};
    }
}

void Selection::after_resize() {
    if (!active_) return;
    BufferLines bl(t_);
    if (!bl.can_carry()) {
        clear();
        return;
    }
    RowPos pos[4];
    for (int i = 0; i < 4; ++i) pos[i] = bl.from_line_pos(detail::LinePos{carried_[i].line, carried_[i].offset});
    // The reflow cropped the selected text (rows below the cursor that no
    // longer fit the new height): the line is gone, or it is shorter than the
    // content the selection reached into.
    for (int i = 2; i < 4; ++i) {
        if (pos[i].row >= t_.end_row()) {
            clear();
            return;
        }
        detail::Line l;
        const size_t ink = bl.line(carried_[i].line, l) ? l.content_end() : 0;
        if (ink < carried_[i].ink && carried_[i].offset > ink) {
            clear();
            return;
        }
    }
    anchor_lo_ = pos[0];
    anchor_hi_ = pos[1];
    range_ = RowRange{pos[2], pos[3]};
    if (mode_ == SelectionMode::Block) {
        range_.end.col = std::clamp(range_.end.col, 1, t_.cols());
        range_.start.col = std::min(range_.start.col, range_.end.col - 1);
    }
    ++version_;
    checks_.clear();  // rows were renumbered: re-hash below
    verify();         // trims what fell off the front of history
    if (active_) rebaseline();
}

} // namespace bropty
