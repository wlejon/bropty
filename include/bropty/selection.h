#pragma once
// Selection over a terminal's active buffer (screen and history).
//
// A selection is held as absolute positions (position.h), so it stays on
// its text while that text scrolls into history and while new output
// arrives; a resize carries it to the same characters through the reflow.
// When the text under it changes (an application overwrites it, or the
// region scrolls inside the screen), it is cleared; when its start scrolls
// out of the front of history it is trimmed to what is left. TerminalView
// drives that maintenance (verify(), the resize hooks); a Selection used on
// its own must be told the same things.
//
// Modes:
//  * Character: between two cell boundaries (the half of the cell a pointer
//    is on picks the boundary, as in most terminals).
//  * Word: whole words at both ends. Word characters are letters, digits,
//    everything outside ASCII except spaces, plus a configurable set (kitty's
//    default "@-./_~?&=%+#"); a click on spaces takes the run of spaces, on
//    other punctuation the run of that character. Words continue across soft
//    wraps, not hard line breaks.
//  * Line: whole logical lines (soft-wrapped rows together).
//  * Block: a rectangle of cells.
//  * Zone: an OSC 133 zone (a prompt, a command's input, or its output),
//    from the zone each cell was printed in (Style::zone).
//
// Extraction (text()) follows xterm / kitty / Alacritty: soft-wrapped rows
// join without a newline, each hard line break becomes one, trailing blanks
// of each line are dropped, a wide character split by an edge is taken
// whole, multi-code-point clusters come out whole, and a block selection
// takes the same columns of each row. html() gives the same text as styled
// HTML (inline CSS against a Palette; OSC 8 links become <a href>). Image
// cells (U+10EEEE: kitty Unicode placeholders, the cells sixel and iTerm2
// images are drawn in) are part of a picture, not of the text: they are
// left out of both, like the blanks they would otherwise trail.
//
// The buffer is any RowSource (row_source.h): a live Terminal, or a model of
// one such as a multiplexer client's mirror of a remote screen.

#include "bropty/color.h"
#include "bropty/position.h"
#include "bropty/row_source.h"
#include "bropty/style.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bropty {

enum class SelectionMode : uint8_t { Character, Word, Line, Block, Zone };

struct TextOptions {
    std::string_view newline{"\n"};
    bool trim_trailing{true};  // drop trailing blanks at line ends
};

class Selection {
public:
    // Over a Terminal, or any RowSource (row_source.h). Over a source that
    // is not a Terminal, a resize clears the selection: the source's rows
    // were renumbered by a reflow it cannot carry positions through.
    explicit Selection(const RowSource& source);

    // ---- gestures. `cell` is a cell position; `right_half` says which half
    // of it the pointer is over (Character mode).
    void start(RowPos cell, SelectionMode mode, bool right_half = false);
    void extend(RowPos cell, bool right_half = false);
    // Select the OSC 133 zone at `cell` (a blank cell takes the zone around
    // it). False, and nothing selected, when there is none.
    bool select_zone(RowPos cell);
    // Select the output of the command around `cell`: the output zone it is
    // in, or the one that follows the prompt / input it is in. With no cell
    // given, the most recent command output. False when there is none.
    bool select_output(RowPos cell);
    bool select_last_output();
    void select_all();
    // Select an explicit stream range (e.g. a search match).
    void select_range(RowRange r);
    void clear() noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] SelectionMode mode() const noexcept { return mode_; }
    // Stream modes: [start, end) between boundaries. Block: rows
    // start.row..end.row (inclusive) and columns [start.col, end.col).
    [[nodiscard]] RowRange range() const noexcept { return range_; }
    [[nodiscard]] bool is_block() const noexcept { return mode_ == SelectionMode::Block; }
    // Columns [c0, c1) of absolute row `row` that are selected; false if none.
    bool row_span(int64_t row, int& c0, int& c1) const noexcept;
    // Bumped by every change (for renderers caching highlights).
    [[nodiscard]] uint64_t version() const noexcept { return version_; }

    [[nodiscard]] std::string text(const TextOptions& o = {}) const;
    [[nodiscard]] std::string html(const Palette& palette) const;

    // Characters that count as part of a word besides letters and digits.
    void set_word_chars(std::u32string_view chars) { word_chars_.assign(chars); }
    [[nodiscard]] const std::u32string& word_chars() const noexcept { return word_chars_; }

    // ---- maintenance (TerminalView calls these)
    // After output: clear if the text under the selection changed, trim what
    // was evicted from history.
    void verify();
    void before_resize();
    void after_resize();

private:
    void set_range(RowRange r);
    void recompute();
    void rebaseline();
    bool word_bounds(RowPos cell, RowPos& lo, RowPos& hi) const;
    bool line_bounds(RowPos cell, RowPos& lo, RowPos& hi) const;
    [[nodiscard]] RowPos boundary(RowPos cell, bool right_half) const noexcept;
    bool zone_range(RowPos cell, Zone want, RowRange& out) const;

    const RowSource& t_;
    bool active_{false};
    SelectionMode mode_{SelectionMode::Character};
    RowPos anchor_lo_;  // the unit the gesture started on (word, line, cell)
    RowPos anchor_hi_;
    RowRange range_;
    uint64_t version_{0};
    std::u32string word_chars_{U"@-./_~?&=%+#"};

    // Rows of the selection that were on the screen when it was made, with
    // their text hashes; once such a row is found unchanged in history it is
    // immutable and dropped from the list.
    struct RowCheck {
        int64_t row;
        uint64_t hash;
    };
    std::vector<RowCheck> checks_;
    // Positions carried through a resize as (line, offset).
    // `ink` is where the line's content ended before the resize: a reflow
    // that crops rows below the cursor can shorten a line, and a selection
    // reaching past what is left of it has lost its text.
    struct Carried {
        int64_t line;
        size_t offset;
        size_t ink;
    };
    Carried carried_[4]{};
};

} // namespace bropty
