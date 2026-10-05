#pragma once
// Internal: the terminal's image state. Two ImageLayers (primary and
// alternate screen), the storage quota shared by both, in-progress
// transmissions (kitty chunks, iTerm2 multipart, a sixel being decoded) and
// the placement bookkeeping the terminal's scrolling, erasing and resizing
// drive. The protocol handlers are Terminal members (graphics_*.cpp); this
// class owns the data and the operations that need no Terminal internals.

#include "bropty/graphics.h"
#include "kitty_command.h"
#include "sixel.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bropty::detail {

class Graphics {
public:
    explicit Graphics(const GraphicsOptions& o);

    [[nodiscard]] const GraphicsOptions& options() const noexcept { return opts_; }
    [[nodiscard]] ImageLayer& layer(bool alt) noexcept { return layers_[alt ? 1 : 0]; }
    [[nodiscard]] const ImageLayer& layer(bool alt) const noexcept { return layers_[alt ? 1 : 0]; }
    // Cheap test for the scroll / erase hooks: anything anchored on this screen?
    [[nodiscard]] bool anchored(bool alt) const noexcept {
        const ImageLayer& l = layer(alt);
        return !l.placements_.empty() || !l.anchors_.empty();
    }
    [[nodiscard]] size_t total_bytes() const noexcept { return bytes_; }
    // Bumped by every change a frame shows (images, placements, animation).
    [[nodiscard]] uint64_t version() const noexcept { return version_; }
    void touch_version() noexcept { ++version_; }

    static ImagePixelsPtr make_pixels(uint32_t w, uint32_t h, std::vector<uint8_t>&& rgba);

    // ---- images -------------------------------------------------------------
    [[nodiscard]] Image* by_key(bool alt, uint64_t key) noexcept;
    [[nodiscard]] Image* by_id(bool alt, uint32_t id) noexcept;
    [[nodiscard]] Image* by_number(bool alt, uint32_t number) noexcept;  // newest
    [[nodiscard]] Image* cell_image(bool alt, uint32_t id) noexcept;
    // Store a kitty image (replacing one with the same id, and its
    // placements). Fails with ENOSPC when it alone exceeds the quota.
    Image* store_kitty(bool alt, std::unique_ptr<Image> img, std::string& error);
    // Store a sixel / iTerm2 image under a fresh 24-bit id (its cells carry it).
    Image* store_cell_image(bool alt, std::unique_ptr<Image> img, std::string& error);
    // A kitty id nobody uses (I= transmissions).
    uint32_t free_kitty_id(bool alt);
    void remove_image(bool alt, uint64_t key);
    void remove_cell_image(bool alt, uint32_t id);
    void used(Image& img) noexcept { img.last_used = ++clock_; }
    // Account for a change in an image's frames (bytes may grow: quota).
    bool resized(bool alt, Image& img, size_t old_bytes, std::string& error);
    [[nodiscard]] size_t refs(bool alt, const Image& img) const noexcept;

    // ---- placements -----------------------------------------------------------
    [[nodiscard]] Placement* placement(bool alt, uint64_t key, uint32_t id) noexcept;
    // Add (replacing one with the same image and non-zero id).
    Placement& place(bool alt, Placement p);
    // Remove placements matching `pred`, and their relative children. With
    // `free_images`, images left without placements are freed too.
    void remove_placements(bool alt, const std::function<bool(const Placement&)>& pred, bool free_images);
    void remove_all_placements_of(bool alt, uint64_t key);
    // Whether making `child` relative to (parent_key, parent_id) would form a
    // cycle or exceed the depth limit; returns an error code or nullptr.
    const char* check_parent(bool alt, const Placement& child) const;
    // The top-left cell of a placement, following relative parents; nullopt
    // when a parent is gone or virtual.
    [[nodiscard]] std::optional<std::pair<int64_t, int>> position(bool alt, const Placement& p) const;

    // ---- anchoring (driven by the terminal) -----------------------------------
    // A scroll of rows [top, bottom] (absolute) and columns [left, right] by n
    // rows (n > 0: up). Placements entirely inside move and are clipped to
    // the region (kitty); cell-image anchors move with their rows.
    void scroll_region(bool alt, int64_t top, int64_t bottom, int left, int right, int n, int cell_w, int cell_h);
    // Rows at or below `from` were renumbered by `delta` without their content
    // moving (a scroll into history above a bottom margin; a resize).
    void shift_rows(bool alt, int64_t from, int64_t delta);
    // History rows before `first_row` are gone.
    void evict_before(bool alt, int64_t first_row, int cell_w, int cell_h);
    // ED 2: placements reaching rows [top, end).
    void clear_rows(bool alt, int64_t top, int64_t end, int cell_w, int cell_h);
    // Everything on one screen (alternate-screen clear) or both (RIS).
    void clear_layer(bool alt);
    void reset();
    void add_anchor(bool alt, uint32_t id, int64_t row, int rows);
    // Cell images whose cells are gone: `on_screen` lists the image ids the
    // screen's cells still show; an anchor wholly inside the screen rows
    // [top, end) whose image is not among them is dropped, and an image left
    // without anchors is freed.
    void retain_cell_images(bool alt, const std::vector<uint32_t>& on_screen, int64_t top, int64_t end);
    // Every anchored position on a screen (placements without a parent, and
    // cell-image anchors), for carrying them through a resize:
    // f(row&, col&, rows*), rows null for placements.
    template <class F>
    void for_each_anchor(bool alt, F&& f) {
        ImageLayer& l = layer(alt);
        for (Placement& p : l.placements_)
            if (!p.is_virtual && !p.parent_serial) f(p.row, p.col, static_cast<int*>(nullptr));
        for (ImageLayer::Anchor& a : l.anchors_) {
            int col = 0;
            f(a.row, col, &a.rows);
        }
        ++version_;
    }
    // Anchors whose rows were all lost (rows <= 0) and the images they kept.
    void prune_anchors(bool alt);

    // ---- animation -------------------------------------------------------------
    // Advance running animations to `now_ms`. Returns the milliseconds until
    // the next frame change (UINT64_MAX when none is running); `changed`
    // reports whether any current frame moved.
    uint64_t advance(uint64_t now_ms, bool& changed);
    [[nodiscard]] uint64_t clock_ms() const noexcept { return now_ms_; }

    // ---- in-progress transmissions (owned here, driven by the handlers) --------
    struct KittyUpload {
        bool active{false};
        bool alt{false};
        KittyCommand cmd;           // the first chunk's control data (payload empty)
        std::vector<uint8_t> data;  // decoded so far
        bool failed{false};
        std::string error;
    } upload;
    struct ItermUpload {
        bool active{false};
        std::string args;           // the MultipartFile= arguments
        std::vector<uint8_t> data;
        bool failed{false};
    } iterm;
    int64_t evicted_before{0};  // the primary first_row() eviction last ran for
    std::unique_ptr<SixelDecoder> sixel;
    SixelPalette sixel_shared;  // registers kept between images when ?1070 is reset

private:
    void enforce_quota(size_t incoming, const Image* keep);
    void evict_image(int layer_index, uint64_t key, bool cell);
    void drop_orphans(bool alt);

    GraphicsOptions opts_;
    ImageLayer layers_[2];
    size_t bytes_{0};
    uint64_t clock_{0};
    uint64_t serial_{0};
    uint64_t anon_{0};
    uint32_t next_cell_id_{1};
    uint32_t next_kitty_id_{1};
    uint64_t version_{1};
    uint64_t now_ms_{0};
};

} // namespace bropty::detail
