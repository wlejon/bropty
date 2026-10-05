#pragma once
// Scrollback search over a terminal's active buffer.
//
// The buffer is read as logical lines (soft-wrapped rows joined) straight
// from the compact history store and the screen; each line's text goes to a
// host-supplied SearchMatcher (bropty knows nothing about regex engines; a
// literal / case-insensitive LiteralMatcher is built in). Matches are held as
// absolute RowRanges (position.h), so they stay on their text as output
// scrolls it into history; a resize carries them through the reflow.
//
// Incremental: start() matches the screen at once, then step() works back
// through history newest-first within a time budget (call it from the
// terminal's thread until it returns false). New output is matched as it
// arrives (sync(), which TerminalView calls): lines that scroll into history
// are matched once, the lines still on the screen whenever they change.
// cancel() may be called from any thread; the next step() or sync() on the
// terminal's thread drops the search.
//
// A match lies within one logical line (a match may span the rows of a
// wrapped line). Lines are matched as UTF-8 where empty cells read as
// spaces and trailing empty cells are dropped.
//
// The buffer is any RowSource (row_source.h). When the backward scan
// reaches history rows the source does not hold, step() asks for them
// (RowSource::request_rows) and stops there, still returning true
// (waiting() says so); call step() again once the source has them. Over a
// source that is not a Terminal, a resize restarts the search.

#include "bropty/position.h"
#include "bropty/row_source.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bropty {

class SearchMatcher {
public:
    virtual ~SearchMatcher() = default;
    // Append the [begin, end) byte ranges of the matches in `line` (one
    // logical line, UTF-8): in order, non-overlapping, non-empty.
    virtual void find(std::string_view line, std::vector<std::pair<size_t, size_t>>& out) = 0;
};

// Literal substring search; case-insensitive folds ASCII, Latin-1, Greek
// and Cyrillic simple case pairs (all of which keep their UTF-8 length, so
// byte offsets need no remapping).
class LiteralMatcher final : public SearchMatcher {
public:
    explicit LiteralMatcher(std::string_view needle, bool case_sensitive = false);
    void find(std::string_view line, std::vector<std::pair<size_t, size_t>>& out) override;

private:
    std::string needle_;
    bool case_sensitive_;
    std::string folded_;  // scratch
};

class Search {
public:
    explicit Search(const RowSource& source);

    void start(std::shared_ptr<SearchMatcher> matcher);
    // Drop the search and its matches.
    void clear();
    // Match more history; false once the whole buffer has been matched (or
    // the search was cancelled / cleared).
    bool step(std::chrono::microseconds budget = std::chrono::microseconds(2000));
    // Thread-safe request to stop.
    void cancel() noexcept { cancel_.store(true, std::memory_order_relaxed); }

    [[nodiscard]] bool active() const noexcept { return matcher_ != nullptr; }
    [[nodiscard]] bool complete() const noexcept { return active() && scan_next_ < scan_floor_ && gaps_.empty(); }
    // The scan is stopped at rows the source does not hold yet (requested).
    [[nodiscard]] bool waiting() const noexcept { return active() && waiting_; }
    [[nodiscard]] size_t size() const noexcept { return frozen_.size() + live_.size(); }
    // Matches in buffer order.
    [[nodiscard]] const RowRange& at(size_t i) const noexcept {
        return i < frozen_.size() ? frozen_[i] : live_[i - frozen_.size()];
    }
    // Index of the first match ending after row `row`.
    [[nodiscard]] size_t lower_bound_row(int64_t row) const noexcept;

    // Navigation. The current match moves to the next match after it (or
    // after `from` when there is none), towards older output when `backward`;
    // wraps around. Returns the new current match.
    std::optional<RowRange> next(bool backward, RowPos from);
    [[nodiscard]] std::optional<RowRange> current() const noexcept { return current_; }
    // Index of the current match in buffer order.
    [[nodiscard]] std::optional<size_t> current_index() const noexcept;
    // Bumped whenever matches or the current match change.
    [[nodiscard]] uint64_t version() const noexcept { return version_; }

    // ---- maintenance (TerminalView calls these)
    // Match what changed since the last sync / start.
    void sync();
    void before_resize();
    void after_resize();
    void screen_switched();

private:
    struct Pending {
        int64_t line;
        size_t start, end;  // cell offsets
    };
    // Match line `number` into `out`; false (nothing matched, the rows
    // requested) when a row of it is missing from the source. `prev` / `next`
    // get the lines before and after it.
    bool match_line(int64_t number, std::vector<RowRange>& out, int64_t* prev, int64_t* next);
    void rescan_live(int64_t from_line);
    bool take_cancel();
    void refresh_current();
    bool fill_gaps();  // false: waiting for rows
    void insert_frozen(const std::vector<RowRange>& found);

    const RowSource& t_;
    bool waiting_{false};
    // Lines [first, second) that left the screen while the source did not
    // hold their rows: matched (into frozen_) once it does.
    std::vector<std::pair<int64_t, int64_t>> gaps_;
    std::shared_ptr<SearchMatcher> matcher_;
    std::atomic<bool> cancel_{false};
    std::deque<RowRange> frozen_;  // matches in lines wholly in history, in order
    std::vector<RowRange> live_;   // matches in lines from live_line_ on
    int64_t live_line_{0};         // first line that was (partly) on the screen at the last sync
    int64_t scan_next_{0};         // next history line the backward scan matches
    int64_t scan_floor_{0};        // the scan is done when scan_next_ < scan_floor_
    uint64_t seen_change_{~0ull};
    std::optional<RowRange> current_;
    uint64_t version_{0};
    std::vector<Pending> carried_;  // matches as (line, offsets) across a resize
    std::optional<Pending> carried_current_;
    std::vector<std::pair<size_t, size_t>> scratch_;
};

} // namespace bropty
