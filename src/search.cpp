#include "bropty/search.h"

#include "buffer_lines.h"

#include <algorithm>

namespace bropty {

using detail::BufferLines;
using detail::Line;
using detail::LineText;

// ---------------------------------------------------------------------------
// LiteralMatcher

namespace {

char32_t fold_cp(char32_t cp) noexcept {
    if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return cp + 0x20;
    if (cp >= 0x100 && cp <= 0x17F) {
        const bool even_upper = (cp <= 0x12F) || (cp >= 0x132 && cp <= 0x137) || (cp >= 0x14A && cp <= 0x177);
        const bool odd_upper = (cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E);
        if (even_upper && (cp % 2) == 0) return cp + 1;
        if (odd_upper && (cp % 2) == 1) return cp + 1;
        return cp;
    }
    if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) return cp + 0x20;
    if (cp >= 0x410 && cp <= 0x42F) return cp + 0x20;
    if (cp >= 0x400 && cp <= 0x40F) return cp + 0x50;
    return cp;
}

// Fold in place; every mapping keeps the UTF-8 length.
void fold_utf8(std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const uint8_t b = uint8_t(s[i]);
        if (b < 0x80) {
            if (b >= 'A' && b <= 'Z') s[i] = char(b + 32);
            ++i;
            continue;
        }
        if ((b & 0xE0) == 0xC0 && i + 1 < s.size()) {
            const char32_t cp = (char32_t(b & 0x1F) << 6) | (uint8_t(s[i + 1]) & 0x3F);
            const char32_t f = fold_cp(cp);
            if (f != cp) {
                s[i] = char(0xC0 | (f >> 6));
                s[i + 1] = char(0x80 | (f & 0x3F));
            }
            i += 2;
            continue;
        }
        i += (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 1;
    }
}

// Match one line into `out`.
void match_text(SearchMatcher& m, const Line& l, std::vector<std::pair<size_t, size_t>>& scratch,
                std::vector<RowRange>& out) {
    LineText lt;
    lt.build(l);
    scratch.clear();
    m.find(lt.text, scratch);
    for (const auto& [b0, b1] : scratch) {
        if (b1 <= b0 || b1 > lt.text.size()) continue;
        size_t c0, c1;
        lt.cells_of(l, b0, b1, c0, c1);
        out.push_back(RowRange{l.pos_of(c0), l.end_pos(c1)});
    }
}

} // namespace

LiteralMatcher::LiteralMatcher(std::string_view needle, bool case_sensitive)
    : needle_(needle), case_sensitive_(case_sensitive) {
    if (!case_sensitive_) fold_utf8(needle_);
}

void LiteralMatcher::find(std::string_view line, std::vector<std::pair<size_t, size_t>>& out) {
    if (needle_.empty() || line.size() < needle_.size()) return;
    std::string_view hay = line;
    if (!case_sensitive_) {
        folded_.assign(line);
        fold_utf8(folded_);
        hay = folded_;
    }
    size_t pos = 0;
    while ((pos = hay.find(needle_, pos)) != std::string_view::npos) {
        out.emplace_back(pos, pos + needle_.size());
        pos += needle_.size();
    }
}

// ---------------------------------------------------------------------------
// Search

Search::Search(const RowSource& source) : t_(source) {}

void Search::clear() {
    matcher_.reset();
    frozen_.clear();
    live_.clear();
    gaps_.clear();
    waiting_ = false;
    current_.reset();
    carried_.clear();
    carried_current_.reset();
    ++version_;
}

bool Search::take_cancel() {
    if (!cancel_.exchange(false, std::memory_order_relaxed)) return false;
    clear();
    return true;
}

void Search::start(std::shared_ptr<SearchMatcher> matcher) {
    clear();
    cancel_.store(false, std::memory_order_relaxed);
    if (!matcher) return;
    matcher_ = std::move(matcher);
    BufferLines bl(t_);
    rescan_live(bl.screen_first_line());
    scan_next_ = bl.prev_line(live_line_);
    scan_floor_ = bl.first_line();
    seen_change_ = t_.change_count();
}

bool Search::match_line(int64_t number, std::vector<RowRange>& out, int64_t* prev, int64_t* next) {
    BufferLines bl(t_);
    Line l;
    if (!bl.line(number, l)) {
        if (prev) *prev = bl.prev_line(number);
        if (next) *next = bl.next_line(number);
        return true;
    }
    if (prev) *prev = bl.prev_line(l);
    if (next) *next = bl.next_line(l);
    if (l.missing) {
        t_.request_rows(l.first_row, l.end_row());
        return false;
    }
    match_text(*matcher_, l, scratch_, out);
    return true;
}

void Search::rescan_live(int64_t from_line) {
    live_.clear();
    live_line_ = from_line;
    BufferLines bl(t_);
    Line l;
    int64_t row = bl.line_first_row(from_line);
    while (row < t_.end_row() && bl.line_at_row(row, l)) {
        // A live line starting in history the source does not hold yet:
        // what it has is matched now, the rest once it arrives (a change).
        if (l.missing) t_.request_rows(l.first_row, l.end_row());
        match_text(*matcher_, l, scratch_, live_);
        row = l.end_row();
    }
    ++version_;
}

void Search::insert_frozen(const std::vector<RowRange>& found) {
    for (const RowRange& r : found) {
        auto it = std::lower_bound(frozen_.begin(), frozen_.end(), r,
                                   [](const RowRange& a, const RowRange& b) { return a.start < b.start; });
        frozen_.insert(it, r);
    }
    if (!found.empty()) ++version_;
}

bool Search::fill_gaps() {
    BufferLines bl(t_);
    const int64_t floor = bl.first_line();
    std::vector<RowRange> found;
    while (!gaps_.empty()) {
        auto& [from, to] = gaps_.back();
        from = std::max(from, floor);
        while (from < to) {
            found.clear();
            int64_t next = from + 1;
            if (!match_line(from, found, nullptr, &next)) return false;
            insert_frozen(found);
            from = std::max(next, from + 1);
        }
        gaps_.pop_back();
    }
    return true;
}

bool Search::step(std::chrono::microseconds budget) {
    if (take_cancel() || !matcher_) return false;
    waiting_ = false;
    if (!fill_gaps()) {
        waiting_ = true;
        return true;
    }
    BufferLines bl(t_);
    scan_floor_ = bl.first_line();
    if (scan_next_ < scan_floor_) return false;
    const auto deadline = std::chrono::steady_clock::now() + budget;
    std::vector<RowRange> found;
    int n = 0;
    while (scan_next_ >= scan_floor_) {
        found.clear();
        int64_t prev = scan_next_ - 1;
        if (!match_line(scan_next_, found, &prev, nullptr)) {
            waiting_ = true;
            return true;
        }
        for (auto it = found.rbegin(); it != found.rend(); ++it) frozen_.push_front(*it);
        if (!found.empty()) ++version_;
        scan_next_ = std::min(prev, scan_next_ - 1);
        if ((++n & 15) == 0) {
            if (cancel_.load(std::memory_order_relaxed)) return !take_cancel();
            if (std::chrono::steady_clock::now() >= deadline) break;
        }
    }
    return scan_next_ >= scan_floor_;
}

void Search::sync() {
    if (!matcher_ || take_cancel()) return;
    if (t_.change_count() == seen_change_) return;
    seen_change_ = t_.change_count();
    BufferLines bl(t_);
    const int64_t first_row = t_.first_row();
    while (!frozen_.empty() && frozen_.front().start.row < first_row) frozen_.pop_front();
    scan_floor_ = bl.first_line();
    for (auto& g : gaps_) g.first = std::max(g.first, scan_floor_);
    gaps_.erase(std::remove_if(gaps_.begin(), gaps_.end(), [](const auto& g) { return g.first >= g.second; }),
                gaps_.end());
    const int64_t live = bl.screen_first_line();
    if (live < live_line_) {
        // Lines came back to the screen (not by output; defensive).
        const int64_t row = bl.line_first_row(live);
        while (!frozen_.empty() && frozen_.back().start.row >= row) frozen_.pop_back();
        scan_next_ = std::min(scan_next_, bl.prev_line(live));
    }
    // Lines that left the screen since the last sync: matched once, for good
    // (or, when the source does not hold them yet, as soon as it does).
    std::vector<RowRange> fresh;
    for (int64_t n = std::max(live_line_, scan_floor_); n < live;) {
        int64_t next = n + 1;
        if (!match_line(n, fresh, nullptr, &next)) {
            gaps_.emplace_back(n, live);
            break;
        }
        n = std::max(next, n + 1);
    }
    for (const RowRange& r : fresh) frozen_.push_back(r);
    rescan_live(live);
    refresh_current();
}

void Search::refresh_current() {
    if (current_ && !current_index()) current_.reset();
}

size_t Search::lower_bound_row(int64_t row) const noexcept {
    size_t lo = 0, hi = size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (at(mid).end.row < row) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

std::optional<size_t> Search::current_index() const noexcept {
    if (!current_) return std::nullopt;
    size_t lo = 0, hi = size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (at(mid).start < current_->start) lo = mid + 1;
        else hi = mid;
    }
    if (lo < size() && at(lo) == *current_) return lo;
    return std::nullopt;
}

std::optional<RowRange> Search::next(bool backward, RowPos from) {
    const size_t n = size();
    if (n == 0) {
        current_.reset();
        return std::nullopt;
    }
    size_t idx;
    if (auto ci = current_index()) {
        idx = backward ? (*ci == 0 ? n - 1 : *ci - 1) : (*ci + 1 == n ? 0 : *ci + 1);
    } else {
        size_t lo = 0, hi = n;  // first match starting at or after `from`
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (at(mid).start < from) lo = mid + 1;
            else hi = mid;
        }
        idx = backward ? (lo == 0 ? n - 1 : lo - 1) : (lo == n ? 0 : lo);
    }
    current_ = at(idx);
    ++version_;
    return current_;
}

// ---------------------------------------------------------------------------
// Resize: carry matches as (line, cell offsets). A source that is not a
// Terminal cannot carry them: the search starts over.

void Search::before_resize() {
    carried_.clear();
    carried_current_.reset();
    sync();  // the screen's matches must describe what is about to be reflowed
    if (!matcher_) return;
    BufferLines bl(t_);
    if (!bl.can_carry()) return;
    Line l;
    bool have = false;
    auto carry = [&](const RowRange& r) -> std::optional<Pending> {
        if (!have || r.start.row < l.first_row || r.start.row >= l.end_row()) {
            have = bl.line_at_row(r.start.row, l);
            if (!have) return std::nullopt;
        }
        return Pending{l.number, l.offset_of(r.start), l.offset_of(r.end)};
    };
    for (size_t i = 0; i < size(); ++i)
        if (auto p = carry(at(i))) carried_.push_back(*p);
    if (current_) {
        have = false;
        carried_current_ = carry(*current_);
    }
}

void Search::after_resize() {
    if (!matcher_) return;
    BufferLines bl(t_);
    if (!bl.can_carry()) {
        start(matcher_);
        return;
    }
    Line l;
    bool have = false;
    auto place = [&](const Pending& p) -> std::optional<RowRange> {
        if (!have || l.number != p.line) {
            have = bl.line(p.line, l);
            if (!have) return std::nullopt;
        }
        return RowRange{l.pos_of(std::min(p.start, l.size())), l.end_pos(std::min(p.end, l.size()))};
    };
    frozen_.clear();
    live_.clear();
    const int64_t live = bl.screen_first_line();
    for (const Pending& p : carried_) {
        if (p.line >= live) break;  // the screen's lines are matched afresh below
        if (auto r = place(p)) frozen_.push_back(*r);
    }
    current_.reset();
    if (carried_current_) {
        have = false;
        current_ = place(*carried_current_);
    }
    carried_.clear();
    carried_current_.reset();
    scan_floor_ = bl.first_line();
    scan_next_ = std::min(scan_next_, live - 1);
    rescan_live(live);
    seen_change_ = t_.change_count();
    refresh_current();
}

void Search::screen_switched() {
    if (matcher_) start(matcher_);
}

} // namespace bropty
