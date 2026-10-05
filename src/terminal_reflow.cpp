// Resize. The primary screen and its history are reflowed as one buffer of
// logical lines; the alternate screen (whose owner redraws on SIGWINCH) is
// cropped.
//
// Placement rule after rewrapping: the row that was at the top of the screen
// stays at the top (growing the height first pulls history back in, as xterm
// does), unless the content through the cursor no longer fits, in which case
// rows scroll into history so the cursor's line stays visible. The cursor and
// the DECSC-saved cursor follow the character they were on.
#include "bropty/terminal.h"

#include "logical_line.h"

#include <algorithm>

namespace bropty {

using detail::LogicalLine;
using detail::WrapSpan;

namespace {

struct Tracked {
    size_t line{0};
    size_t offset{0};
    bool valid{false};
};

struct Placed {
    long long row{0};
    int col{0};
    bool pending{false};
};

bool row_has_content(const Grid& g, int y) {
    const Cell* r = g.row(y);
    for (int x = 0; x < g.cols(); ++x) {
        if (r[x].cp() != 0 || r[x].style != 0 || r[x].wide() != Wide::Narrow) return true;
    }
    return (g.flags(y) & Row_SemanticMask) != 0;
}

} // namespace

void Terminal::resize(int cols, int rows) {
    cols = std::max(2, cols);
    rows = std::max(1, rows);
    if (cols == cols_ && rows == rows_) return;
    ++change_count_;
    for (TerminalObserver* o : observers_) o->before_resize();
    invalidate_print();

    // Alternate screen: crop / pad.
    alt_.grid.resize_crop(cols, rows);
    alt_.cur.row = std::min(alt_.cur.row, rows - 1);
    alt_.cur.col = std::min(alt_.cur.col, cols - 1);
    alt_.cur.pending_wrap = false;
    alt_.saved.row = std::min(alt_.saved.row, rows - 1);
    alt_.saved.col = std::min(alt_.saved.col, cols - 1);

    reflow_primary(cols, rows);

    int old_cols = cols_;
    cols_ = cols;
    rows_ = rows;
    tabs_.resize(size_t(cols), 0);
    for (int x = old_cols; x < cols; ++x) tabs_[size_t(x)] = (x % 8 == 0) ? 1 : 0;
    reset_margins();
    primary_.grid.mark_all_dirty();
    alt_.grid.mark_all_dirty();
    primary_.grid.set_generation(gen_);  // the reflowed grid is new storage
    alt_.grid.set_generation(gen_);
    maybe_collect_garbage();
    for (TerminalObserver* o : observers_) o->after_resize();
}

void Terminal::reflow_primary(int new_cols, int new_rows) {
    Grid& old = primary_.grid;
    const int old_rows = old.rows();
    Cursor& cur = primary_.cur;
    Saved& saved = primary_.saved;

    scrollback_.set_cols(new_cols);

    // 1. Screen rows -> logical lines (joined with a continued history tail).
    std::vector<LogicalLine> lines;
    if (scrollback_.last_continued()) {
        lines.emplace_back();
        scrollback_.pop_last(lines.back(), styles_);
        lines.back().continued = false;
    }
    int last_row = cur.row;
    for (int y = old_rows - 1; y > last_row; --y) {
        if (row_has_content(old, y)) {
            last_row = y;
            break;
        }
    }

    Tracked anchor, tcur, tsaved;
    bool cur_pending = cur.pending_wrap;
    bool open = !lines.empty();  // previous row soft-wrapped into this one
    for (int y = 0; y <= last_row; ++y) {
        if (!open) lines.emplace_back();
        LogicalLine& line = lines.back();
        size_t start = line.cells.size();
        const Cell* r = old.row(y);
        auto offset_of = [&](int col) {
            size_t off = start;
            for (int x = 0; x < col && x < old.cols(); ++x)
                if (r[x].wide() != Wide::SpacerHead) ++off;
            return off;
        };
        if (y == 0) anchor = {lines.size() - 1, start, true};
        if (y == cur.row) tcur = {lines.size() - 1, offset_of(cur.col) + (cur_pending ? 1 : 0), true};
        if (saved.valid && y == saved.row) tsaved = {lines.size() - 1, offset_of(saved.col) + (saved.pending_wrap ? 1 : 0), true};
        line.append_row(r, old.cols(), old.clusters(y));
        line.flags |= old.flags(y) & Row_SemanticMask;
        open = old.wrapped(y);
    }
    for (LogicalLine& l : lines) l.trim_trailing_blanks();

    // 2. Rewrap at the new width; locate tracked positions.
    std::vector<std::vector<WrapSpan>> spans;
    std::vector<long long> first_row;
    auto rewrap = [&]() {
        spans.resize(lines.size());
        first_row.resize(lines.size());
        long long row = 0;
        for (size_t i = 0; i < lines.size(); ++i) {
            detail::wrap_cells(lines[i].cells.data(), lines[i].cells.size(), new_cols, spans[i]);
            first_row[i] = row;
            row += (long long)spans[i].size();
        }
        return row;
    };
    auto place = [&](const Tracked& t, bool pending_hint) {
        Placed p;
        const auto& sp = spans[t.line];
        size_t n = lines[t.line].cells.size();
        if (t.offset < n) {
            for (size_t k = 0; k < sp.size(); ++k) {
                if (t.offset < sp[k].end) {
                    p.row = first_row[t.line] + (long long)k;
                    p.col = int(t.offset - sp[k].begin);
                    return p;
                }
            }
        }
        // At or beyond the end of the line's content.
        const WrapSpan& lastsp = sp.back();
        p.row = first_row[t.line] + (long long)sp.size() - 1;
        size_t col = t.offset - lastsp.begin;
        if (col >= size_t(new_cols)) {
            p.col = new_cols - 1;
            p.pending = (pending_hint || t.offset == n) && modes_.autowrap && col == size_t(new_cols);
        } else {
            p.col = int(col);
        }
        return p;
    };

    long long total = rewrap();
    long long top = 0;
    Placed pc;
    for (;;) {
        pc = place(tcur, cur_pending);
        long long anchor_row = anchor.valid ? place(anchor, false).row : 0;
        long long pulled = anchor_row - std::max(0, new_rows - old_rows);
        top = std::max(total - new_rows, pulled);
        top = std::min(top, pc.row);
        if (top >= 0 || scrollback_.empty()) break;
        // Pull one more line back from history and re-run the placement.
        LogicalLine back;
        scrollback_.pop_last(back, styles_);
        back.continued = false;
        lines.insert(lines.begin(), std::move(back));
        anchor.line += 1;
        tcur.line += 1;
        if (tsaved.valid) tsaved.line += 1;
        total = rewrap();
    }
    if (top < 0) top = 0;

    // 3. Rows above `top` go (back) to history; the rest fill the new screen.
    Grid grid(new_cols, new_rows);
    for (size_t i = 0; i < lines.size(); ++i) {
        const LogicalLine& line = lines[i];
        const auto& sp = spans[i];
        long long r0 = first_row[i];
        long long r1 = r0 + (long long)sp.size();
        if (r1 <= top) {
            scrollback_.push_line(line, styles_.data());
            continue;
        }
        if (r0 < top) {
            LogicalLine part;
            size_t cut = sp[size_t(top - r0)].begin;
            part.cells.assign(line.cells.begin(), line.cells.begin() + long(cut));
            for (const auto& c : line.clusters)
                if (c.first < cut) part.clusters.push_back(c);
            part.flags = line.flags;
            part.continued = true;
            part.next_wide = line.cells[cut].wide() == Wide::Lead;
            scrollback_.push_line(part, styles_.data());
        }
        for (long long r = std::max(r0, top); r < r1 && r - top < new_rows; ++r) {
            int y = int(r - top);
            const WrapSpan& s = sp[size_t(r - r0)];
            Cell* dst = grid.row(y);
            std::copy(line.cells.begin() + s.begin, line.cells.begin() + s.end, dst);
            if (s.spacer_head) dst[s.end - s.begin] = Cell::make(0, 0, Wide::SpacerHead);
            for (const auto& c : line.clusters) {
                if (c.first >= s.begin && c.first < s.end) grid.clusters_mut(y).set(int(c.first - s.begin), c.second);
            }
            uint32_t flags = 0;
            // A line cut off at the bottom (its rest no longer fits) ends
            // here: a soft wrap into the row below would join it to whatever
            // is written there next.
            if (r + 1 < r1 && r + 1 - top < new_rows) flags |= Row_Wrapped;
            if (r == r0) flags |= line.flags;
            grid.set_flags(y, flags);
        }
    }

    // 4. Cursor positions.
    cur.row = int(std::clamp<long long>(pc.row - top, 0, new_rows - 1));
    cur.col = std::clamp(pc.col, 0, new_cols - 1);
    cur.pending_wrap = pc.pending;
    if (saved.valid) {
        if (tsaved.valid) {
            Placed ps = place(tsaved, saved.pending_wrap);
            saved.row = int(std::clamp<long long>(ps.row - top, 0, new_rows - 1));
            saved.col = std::clamp(ps.col, 0, new_cols - 1);
            saved.pending_wrap = ps.pending;
        } else {
            saved.pending_wrap = false;
            saved.row = std::min(saved.row, new_rows - 1);
            saved.col = std::min(saved.col, new_cols - 1);
        }
    }
    primary_.grid = std::move(grid);
}

} // namespace bropty
