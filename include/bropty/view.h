#pragma once
// TerminalView: the renderer-facing state that lives with a Terminal's text
// (the viewport into history, the selection, the search, the hovered link)
// and the frames that carry it to a renderer on another thread.
//
// Threading: a TerminalView belongs to its Terminal's thread, like the
// Terminal itself (call it from the thread that feeds the terminal, or under
// the same lock). Only Frames, published through a FrameChannel, cross to the
// renderer; the renderer never locks and never stalls the parser. A host
// whose UI thread is not the parser thread sends gestures (scroll, select,
// search) to the parser thread or takes the lock for them: they are rare
// and cheap, and the parser holds it only for a bounded update slice
// (Session::UpdateBudget).
//
// Typical loop on the parser thread:
//     session.update();                       // feed PTY output
//     view.search().step();                   // if a search is running
//     view.publish(channel, session.has_pending_output());
// With `only_if_consumed` set while output is still pending, a flood
// publishes at most one frame per frame the renderer actually takes; the
// last update of a burst publishes unconditionally.

#include "bropty/frame.h"
#include "bropty/links.h"
#include "bropty/position.h"
#include "bropty/search.h"
#include "bropty/selection.h"
#include "bropty/terminal.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace bropty {

class TerminalView final : private TerminalObserver {
public:
    explicit TerminalView(Terminal& t);
    // A view over any RowSource (row_source.h), e.g. a multiplexer client's
    // model of a remote screen: the same viewport, selection, search, links
    // and frames. Differences from a Terminal-backed view: frames carry the
    // images the source offers (RowSource::source_images; none by default),
    // rows the source does not hold show blank until it has them
    // (they are requested), and a resize clears the selection, restarts the
    // search and returns the viewport to the bottom (the source's reflow
    // happened where positions cannot be carried through it).
    explicit TerminalView(RowSource& source);
    ~TerminalView() override;
    TerminalView(const TerminalView&) = delete;
    TerminalView& operator=(const TerminalView&) = delete;

    [[nodiscard]] RowSource& source() noexcept { return t_; }
    // The Terminal behind the view, or nullptr for another source.
    [[nodiscard]] Terminal* terminal_or_null() noexcept { return term_; }
    // Only for a view over a Terminal.
    [[nodiscard]] Terminal& terminal() noexcept { return *term_; }

    // ---- viewport ----------------------------------------------------------
    // Absolute row shown at the top. At the bottom the view follows output;
    // scrolled back it stays on its text while output arrives (until that
    // text is evicted from history).
    [[nodiscard]] int64_t top_row() const noexcept;
    [[nodiscard]] bool at_bottom() const noexcept { return follow_ || t_.alt_screen_active(); }
    void scroll_by(int64_t rows);       // negative: back into history
    void scroll_to_row(int64_t row);    // `row` at the top, clamped
    void scroll_to_bottom() noexcept { follow_ = true; ++version_; }
    // Bring a range into view (no scroll when it already is).
    void reveal(RowRange r);
    // Scroll to the previous / next OSC 133 prompt above / below the top row.
    bool scroll_to_prompt(bool backward);
    // Viewport cell -> absolute position.
    [[nodiscard]] RowPos pos_at(int y, int col) const noexcept { return RowPos{top_row() + y, col}; }

    // ---- selection, search, links --------------------------------------------
    [[nodiscard]] Selection& selection() noexcept { return selection_; }
    [[nodiscard]] const Selection& selection() const noexcept { return selection_; }
    [[nodiscard]] Search& search() noexcept { return search_; }
    [[nodiscard]] const Search& search() const noexcept { return search_; }
    // Move to the next / previous match (from the viewport when there is no
    // current one) and bring it into view.
    std::optional<RowRange> search_next(bool backward);
    // The link under the pointer, kept current as output arrives.
    void set_hover(std::optional<RowPos> cell);
    [[nodiscard]] const std::optional<LinkHit>& hover() const noexcept { return hover_; }

    // ---- frames --------------------------------------------------------------
    // Bring selection / search / hover / viewport up to date with the
    // terminal (publish() does this first; call it after feeding when reading
    // view state without publishing).
    void sync();
    // Build and publish a frame. With only_if_consumed, nothing is built while
    // the renderer has not taken the previous frame. Nothing is published
    // when nothing changed since the last frame. Returns whether it published.
    bool publish(FrameChannel& channel, bool only_if_consumed = false);
    // Build a frame without publishing it (tests, single-threaded hosts).
    std::shared_ptr<const Frame> snapshot();

private:
    void before_resize() override;
    void after_resize() override;
    void screen_switched() override;
    void clamp_viewport() noexcept;
    std::shared_ptr<Frame> build();
    std::shared_ptr<const FrameRow> screen_row(int y);
    std::shared_ptr<const FrameRow> history_row(int64_t row);
    std::shared_ptr<FrameRow> snapshot_row(const RowView& v, int64_t row);
    void build_highlights(Frame& f) const;
    void build_images(Frame& f) const;  // frame_images.cpp
    void build_damage(Frame& f) const;

    RowSource& t_;
    Terminal* term_;  // t_ when it is a Terminal
    Selection selection_;
    Search search_;
    bool follow_{true};
    int64_t top_{0};  // when !follow_
    int64_t carried_top_line_{0};
    size_t carried_top_offset_{0};
    std::optional<RowPos> hover_pos_;
    std::optional<LinkHit> hover_;
    uint64_t version_{0};       // viewport / hover changes
    uint64_t seen_change_{~0ull};

    // Frame building. Screen rows are cached by the source's row serial
    // (a scrolled row keeps its serial, so a scroll costs only the new rows);
    // sorted by serial, holding the rows of the last frame.
    struct ScreenEntry {
        uint64_t serial{0};
        std::shared_ptr<const FrameRow> row;
    };
    std::vector<ScreenEntry> screen_cache_;
    std::vector<ScreenEntry> screen_next_;
    int64_t miss_lo_{0};  // history rows a build found missing: [lo, hi)
    int64_t miss_hi_{0};
    std::unordered_map<int64_t, std::shared_ptr<const FrameRow>> history_cache_;
    std::shared_ptr<const Palette> palette_;
    std::shared_ptr<const Frame> last_;
    uint64_t serial_{0};
    uint64_t seq_{0};
    struct Signature {
        uint64_t change, view, selection, search;
        bool operator==(const Signature&) const noexcept = default;
    } last_sig_{~0ull, 0, 0, 0};
};

} // namespace bropty
