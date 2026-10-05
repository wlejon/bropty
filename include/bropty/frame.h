#pragma once
// Frame snapshots: what a renderer on another thread reads.
//
// The emulator runs on one thread (the PTY / parser thread, or under the
// host's lock); a renderer reads Frames on another, never touching the
// Terminal. Mechanism (see TerminalView::publish):
//
//  * Copy-on-write rows. A Frame is a vector of shared_ptrs to immutable
//    FrameRows. Publishing re-snapshots only rows whose content changed since
//    the last publish (the grid stamps rows as it writes them, and the stamp
//    follows a row when the screen scrolls, so a scroll costs the new rows,
//    not the screen); every other row is the previous frame's pointer. History
//    rows in a scrolled-back viewport are immutable and cached by row number.
//    A FrameRow resolves its styles into a row-local table (and its OSC 8
//    links into URIs), so nothing in it refers to the terminal's mutable,
//    garbage-collected style table.
//  * Triple-buffered handoff. FrameChannel holds three frame slots; the
//    writer fills its slot and swaps it with the shared middle one in one
//    atomic exchange, the reader swaps the middle one with its own when a
//    fresh frame is there. Neither side ever waits for the other or holds a
//    lock; the reader keeps whatever frame it took for as long as it likes.
//
// Damage: each frame says which rows differ from the frame published before
// it. A renderer that skips frames compares against the frame it last drew
// instead (row_differs(), FrameRow::serial).
//
// Images: Frame::images lists every image visible in the viewport (kitty
// placements, and the runs of image cells sixel, iTerm2 and kitty Unicode
// placeholders leave in the text), converted from absolute buffer positions
// to viewport cells when the frame is built. Pixel data is shared and
// immutable (ImagePixels): upload each buffer once, keyed by its serial.

#include "bropty/cell.h"
#include "bropty/color.h"
#include "bropty/position.h"
#include "bropty/style.h"
#include "bropty/terminal.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bropty {

struct FrameRow {
    uint64_t serial{0};  // unique per snapshot: equal serials, equal content
    int cols{0};
    uint32_t flags{0};
    std::vector<Cell> cells;   // Cell::style indexes `styles`
    std::vector<Style> styles; // row-local; Style::link is resolved by link_uri()
    ClusterMap clusters;
    std::vector<std::pair<uint32_t, std::string>> links;  // hyperlink id -> URI, for the ids used here

    [[nodiscard]] RowView view() const noexcept {
        return RowView{cells.data(), cols, flags, styles.data(), clusters.empty() ? nullptr : &clusters};
    }
    [[nodiscard]] const std::string* link_uri(uint32_t id) const noexcept {
        for (const auto& [k, uri] : links)
            if (k == id) return &uri;
        return nullptr;
    }
};

enum class HighlightKind : uint8_t { Selection, Match, CurrentMatch, Hover };

// Columns [col0, col1) of viewport row y.
struct Highlight {
    int y{0};
    int col0{0};
    int col1{0};
    HighlightKind kind{HighlightKind::Selection};
    bool operator==(const Highlight&) const noexcept = default;
};

// Where an image is drawn relative to the text. Kitty's z-index rules:
// z < INT32_MIN / 2 under cell backgrounds, other negative z between the
// backgrounds and the text, z >= 0 over the text. Image cells (sixel,
// iTerm2, kitty placeholders) are the text: draw them in place of the glyphs
// of the cells they cover.
enum class ImagePlane : uint8_t { BelowBackground, BelowText, Text, AboveText };

struct FrameImage {
    ImagePixelsPtr pixels;  // the image's current frame
    uint32_t image_id{0};   // kitty image id (0: anonymous), or the cell image id
    uint32_t placement_id{0};
    ImageSource source{ImageSource::Kitty};
    ImagePlane plane{ImagePlane::AboveText};
    int32_t z{0};
    // Source rectangle, in pixels of `pixels`.
    float src_x{0}, src_y{0}, src_w{0}, src_h{0};
    // Destination in viewport cells: column x, row y (fractional: cell
    // offsets and aspect-preserving sizes are not whole cells). May extend
    // past the viewport; clip when drawing.
    float x{0}, y{0}, w{0}, h{0};
    bool operator==(const FrameImage&) const noexcept = default;
};

struct Frame {
    uint64_t seq{0};
    int cols{0};
    int rows{0};
    std::vector<std::shared_ptr<const FrameRow>> lines;  // viewport rows, top to bottom

    // Viewport: lines[y] is absolute row top_row + y (position.h).
    int64_t top_row{0};
    int64_t first_row{0};       // the oldest row the viewport can scroll to
    int64_t screen_top_row{0};  // the live screen; top_row == screen_top_row at the bottom
    [[nodiscard]] bool at_bottom() const noexcept { return top_row == screen_top_row; }

    CursorState cursor;      // screen coordinates
    int cursor_y{-1};        // viewport row of the cursor, -1 when scrolled out of view
    Modes modes;
    bool alt_screen{false};
    std::shared_ptr<const Palette> palette;  // shared until it changes

    std::vector<Highlight> highlights;  // sorted by row, then column

    // Images in drawing order: by plane, then z, then age. Laid out with
    // the terminal's image cell size (Terminal::image_cell_width / height);
    // a renderer with other cells scales positions and sizes alike.
    std::vector<FrameImage> images;
    int image_cell_width{0};
    int image_cell_height{0};
    // Search status for the chrome.
    size_t match_count{0};
    std::optional<size_t> current_match;
    bool search_active{false};
    bool search_complete{false};
    bool selection_active{false};

    std::vector<uint8_t> damage;  // per row: differs from the previously published frame

    // [first, last) indices of row y's highlights.
    [[nodiscard]] std::pair<size_t, size_t> highlights_of(int y) const noexcept;
    // Whether row y must be redrawn relative to `drawn` (content, highlights,
    // the cursor or an image on it).
    [[nodiscard]] bool row_differs(const Frame& drawn, int y) const noexcept;
};

// Single-writer / single-reader lock-free handoff of the newest frame.
class FrameChannel {
public:
    FrameChannel() = default;
    FrameChannel(const FrameChannel&) = delete;
    FrameChannel& operator=(const FrameChannel&) = delete;

    // Writer.
    void publish(std::shared_ptr<const Frame> frame) noexcept;
    // Whether the reader has taken the last published frame.
    [[nodiscard]] bool consumed() const noexcept {
        return (middle_.load(std::memory_order_acquire) & kFresh) == 0;
    }
    // Reader: the newest published frame (the same one again when nothing
    // newer is there; null before the first publish).
    std::shared_ptr<const Frame> acquire() noexcept;
    // Reader: whether acquire() would return a newer frame.
    [[nodiscard]] bool has_new() const noexcept { return !consumed(); }

private:
    static constexpr uint32_t kFresh = 4;
    std::shared_ptr<const Frame> slots_[3];
    std::atomic<uint32_t> middle_{1};
    uint32_t back_{0};   // writer's slot
    uint32_t front_{2};  // reader's slot
};

} // namespace bropty
