#pragma once
// RowSource: a terminal buffer seen as rows with absolute numbers
// (position.h), plus the little state a renderer needs (cursor, modes,
// palette). Selection, Search, link detection and TerminalView read a buffer
// only through this interface, so they run over a live Terminal (which is a
// RowSource) or over any other model of a terminal's screen -- a client-side
// mirror of a remote session, a recording, a test fixture.
//
// Contract for an implementation:
//  * Rows first_row() .. end_row() - 1 are the buffer; screen rows start at
//    screen_top_row(). An absolute row keeps its number (and history rows
//    their content) until the source is resized; on a resize, call the
//    observers' before_resize() / after_resize() (notify_*() below), and
//    screen_switched() when the alternate screen is entered or left.
//  * row_at() may return an empty view (cells == nullptr) for a row the
//    source does not hold (yet): readers treat it as a blank, unwrapped row,
//    and ask for it with request_rows() where they need its text (a search
//    waits for it; a frame shows it blank meanwhile).
//  * change_count() changes whenever anything a reader sees may have.
//  * Style::link ids in a row resolve through hyperlink_uri(row, id). The id
//    space may differ from row to row (a mirror holding rows fetched at
//    different times); the row number says which one.
//
// Not thread-safe: a source and its readers live on one thread.

#include "bropty/cell.h"
#include "bropty/color.h"
#include "bropty/modes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bropty {

class Terminal;
class ImageLayer;

// The images a source shows on its active screen (graphics.h), for a
// TerminalView's frames: the layer (kitty images and placements, sixel /
// iTerm2 cell images), the cell size its positions were laid out with, and
// whether any cell may hold an image placeholder (U+10EEEE), which readers
// then look for in the rows.
struct SourceImages {
    const ImageLayer* layer{nullptr};
    int cell_width{0};
    int cell_height{0};
    bool may_have_cells{false};
};

// Things that keep positions in the buffer (TerminalView: selection, search,
// viewport) hear about the changes that renumber rows. For a Terminal these
// are called on its thread from inside resize() (also when an application
// resizes through DECCOLM, i.e. inside feed()).
class TerminalObserver {
public:
    virtual ~TerminalObserver() = default;
    // The source is about to resize; its state is still the old one, so
    // positions can be converted to text offsets (see TerminalView).
    virtual void before_resize() {}
    // The resize (and the primary screen's reflow) is complete.
    virtual void after_resize() {}
    // The alternate screen was entered or left.
    virtual void screen_switched() {}
};

class RowSource {
public:
    virtual ~RowSource() = default;

    [[nodiscard]] virtual int cols() const noexcept = 0;
    [[nodiscard]] virtual int rows() const noexcept = 0;  // screen rows
    // The oldest row held (on the alternate screen: screen row 0).
    [[nodiscard]] virtual int64_t first_row() const noexcept = 0;
    // Screen row 0.
    [[nodiscard]] virtual int64_t screen_top_row() const noexcept = 0;
    // One past the last row.
    [[nodiscard]] int64_t end_row() const noexcept { return screen_top_row() + rows(); }
    [[nodiscard]] virtual bool alt_screen_active() const noexcept = 0;

    // Row `abs`; an empty view when the source does not hold it. Views stay
    // valid until the next call into the source (history views may share a
    // decode cache: read one at a time).
    [[nodiscard]] virtual RowView row_at(int64_t abs) const = 0;
    // A content serial for screen row `abs`: two rows with equal non-zero
    // serials have equal content (a row keeps its serial while it scrolls).
    // 0 means unknown, and readers that cache rows re-read the row.
    [[nodiscard]] virtual uint64_t row_serial(int64_t abs) const noexcept {
        (void)abs;
        return 0;
    }
    // URI of hyperlink `id` (Style::link) as used in row `row`, or nullptr.
    [[nodiscard]] virtual const std::string* hyperlink_uri(int64_t row, uint32_t id) const noexcept = 0;

    [[nodiscard]] virtual uint64_t change_count() const noexcept = 0;
    [[nodiscard]] virtual CursorState cursor() const noexcept = 0;  // screen coordinates
    [[nodiscard]] virtual const Modes& modes() const noexcept = 0;
    [[nodiscard]] virtual const Palette& palette() const noexcept = 0;

    // A reader that cached rows by serial has read them all; later changes
    // must get serials it has not seen (Terminal::advance_generation()).
    virtual void advance_generation() noexcept {}
    // A hint: a reader wants rows [first, end) that row_at() did not have.
    virtual void request_rows(int64_t first, int64_t end) const {
        (void)first;
        (void)end;
    }
    // The Terminal behind this source, when it is one: readers use its
    // logical-line store directly, carry positions through its reflow, and
    // show its images.
    [[nodiscard]] virtual const Terminal* terminal() const noexcept { return nullptr; }
    // The images of a source that is not a Terminal (a Terminal's view reads
    // the Terminal's own). Default: none. The layer must stay valid, and
    // unchanged, until the next change_count() change.
    [[nodiscard]] virtual SourceImages source_images() const noexcept { return {}; }

    void add_observer(TerminalObserver* o);
    void remove_observer(TerminalObserver* o);

protected:
    RowSource() = default;
    RowSource(const RowSource&) = delete;
    RowSource& operator=(const RowSource&) = delete;
    void notify_before_resize();
    void notify_after_resize();
    void notify_screen_switched();

    std::vector<TerminalObserver*> observers_;
};

} // namespace bropty
