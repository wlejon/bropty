#pragma once
// Inline images: the kitty graphics protocol, sixel and iTerm2 inline images.
//
// Model. Images live with the text they were drawn into:
//
//  * Kitty placements (a=T / a=p) are overlays anchored to an absolute cell
//    (position.h): they scroll with the text, leave with it when history
//    evicts it, and are carried through reflow with their top-left cell's
//    character (a placement moves as a whole; it is never torn). Text
//    written over them does not remove them (kitty); z-index decides which
//    is drawn on top. ED 2 removes those that reach the screen, ED 3 those
//    in history, RIS all; the alternate screen has its own set, cleared with
//    its text.
//  * Sixel and iTerm2 images, and kitty Unicode placeholders (U+10EEEE), are
//    cells: each covered cell holds a placeholder that says which part of
//    which image it shows. They therefore behave exactly like text: text
//    printed over an image replaces that part of it, erasing erases it, they
//    scroll, go into history, are evicted with it, and reflow like any other
//    characters (as WezTerm does; a narrowing that wraps an image row splits
//    it, widening again rejoins it). Kitty's own policy, for comparison: its
//    placements keep their screen row through a width change (only a height
//    change shifts them) and its placeholder images reflow as text.
//
// Pixel data is decoded once into immutable RGBA buffers (ImagePixels)
// shared by every frame that shows them, so a renderer uploads each buffer
// once (keyed by ImagePixels::serial) and reuses it.
//
// Decoding. Sixel, RGB and RGBA are decoded here, as is kitty's zlib `o=z`
// compression (a small in-tree inflater: see src/inflate.h). PNG and every
// other compressed format go through TerminalHost::decode_image; without a
// host decoder those transmissions fail with an error reply.
//
// Safety. Everything a program sends is bounded: decoded pixels per
// terminal (storage_limit, least-recently-used images evicted first, those
// without placements before those with), image dimensions, compressed and
// encoded transmission sizes, placements, and frames. Reading files, temp
// files and shared memory is off by default (a program on the other side of
// an ssh connection must not be able to read the local filesystem); hosts
// that only run local programs can turn them on.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bropty {

namespace detail {
class Graphics;
}

// The kitty Unicode placeholder. Kitty's placeholders follow it with
// diacritics naming the image row / column (and the image id's high byte);
// the cells of sixel and iTerm2 images follow it with two private-use code
// points instead, U+F0000 + image row and U+100000 + image column, and carry
// their image (ImageLayer::cell_image) as a 24-bit RGB foreground.
constexpr char32_t kImagePlaceholder = 0x10EEEE;
constexpr char32_t kCellImageRowBase = 0xF0000;
constexpr char32_t kCellImageColBase = 0x100000;

// Decoded pixels: 8-bit RGBA, non-premultiplied, rows top to bottom, no
// padding. Immutable once published; `serial` is unique per buffer.
struct ImagePixels {
    uint64_t serial{0};
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> rgba;
};
using ImagePixelsPtr = std::shared_ptr<const ImagePixels>;

// What TerminalHost::decode_image produces: one frame for a still image,
// several (with their delays) for an animation.
struct DecodedFrame {
    std::vector<uint8_t> rgba;  // width * height * 4
    uint32_t delay_ms{0};
};
struct DecodedImage {
    uint32_t width{0};
    uint32_t height{0};
    std::vector<DecodedFrame> frames;
};
// Bounds a decoder must respect (refuse, rather than produce, anything larger).
struct ImageLimits {
    uint32_t max_width{0};
    uint32_t max_height{0};
    size_t max_bytes{0};  // all frames' RGBA together
};

struct GraphicsOptions {
    bool kitty{true};
    bool sixel{true};
    bool iterm2{true};
    // Decoded RGBA (all frames of all images, both screens) per terminal;
    // kitty's default quota. Exceeding it evicts least-recently-used images.
    size_t storage_limit{320u << 20};
    uint32_t max_width{10000};
    uint32_t max_height{10000};
    // Sixel images are limited separately (their size is not declared up front).
    uint32_t max_sixel_width{4096};
    uint32_t max_sixel_height{4096};
    // Encoded / compressed data accepted for one transmission (kitty chunks
    // accumulated, files, shared memory, iTerm2 multipart).
    size_t max_transmission_bytes{256u << 20};
    size_t max_images{4096};       // per screen
    size_t max_placements{16384};  // per screen
    size_t max_frames{1024};       // per image
    // Transmission media beyond direct (in-band) data. Off by default.
    bool allow_files{false};          // kitty t=f
    bool allow_temp_files{false};     // kitty t=t (read, then deleted)
    bool allow_shared_memory{false};  // kitty t=s (POSIX shm_open / Windows named mapping)
    // Cell size assumed while the host has not set one (Terminal::set_cell_pixel_size).
    int fallback_cell_width{10};
    int fallback_cell_height{20};
};

enum class ImageSource : uint8_t { Kitty, Sixel, Iterm2 };
enum class AnimationState : uint8_t { Stopped, Loading, Running };

struct ImageFrame {
    ImagePixelsPtr pixels;  // the fully composed frame, image-sized
    // Milliseconds this frame shows for while animating; 0 = gapless (never
    // shown by a running animation). Kitty's root frame starts gapless and
    // its later frames at 40 ms.
    uint32_t gap_ms{0};
};

struct Image {
    uint32_t id{0};      // kitty image id (0: anonymous); cell images number their own id space
    uint32_t number{0};  // kitty image number (I=)
    ImageSource source{ImageSource::Kitty};
    uint32_t width{0};
    uint32_t height{0};
    std::vector<ImageFrame> frames;  // [0] is the root frame
    uint32_t current_frame{0};
    AnimationState animation{AnimationState::Stopped};
    uint32_t max_loops{0};  // 0: loop forever (kitty v=1); kitty v=n: n - 1 loops

    // Cell images (sixel, iTerm2): the image's extent in cells and the cell
    // box its placeholders cover (ceil of the extent, clipped to the screen).
    float cell_width{0};
    float cell_height{0};
    int box_cols{0};
    int box_rows{0};

    [[nodiscard]] const ImagePixelsPtr& pixels() const noexcept { return frames[current_frame].pixels; }
    [[nodiscard]] size_t bytes() const noexcept;

    // Bookkeeping.
    uint64_t key{0};        // identity within its layer
    uint64_t serial{0};     // creation order
    uint64_t last_used{0};  // LRU clock
    uint64_t frame_shown_ms{0};  // when the current frame was shown (animation clock)
    uint32_t current_loop{0};
};

// A kitty placement.
struct Placement {
    uint64_t image_key{0};
    uint32_t image_id{0};
    uint32_t placement_id{0};
    int64_t row{0};  // absolute row of the top-left cell (position.h)
    int col{0};
    int x_offset{0};  // pixels into the top-left cell
    int y_offset{0};
    uint32_t src_x{0};  // source rectangle (pixels, inside the image)
    uint32_t src_y{0};
    uint32_t src_w{0};
    uint32_t src_h{0};
    int cols{0};  // c / r as given (0: from the image size)
    int rows{0};
    int32_t z{0};
    bool is_virtual{false};  // U=1: a prototype for Unicode placeholders
    // Relative placement (P, Q, H, V): positioned from its parent's top-left cell.
    uint64_t parent_image_key{0};
    uint32_t parent_placement_id{0};
    uint64_t parent_serial{0};
    int parent_dx{0};
    int parent_dy{0};
    uint64_t serial{0};  // creation order (also orders equal z)
};

// The images and placements of one screen (primary or alternate). Read-only
// outside the terminal.
class ImageLayer {
public:
    // A kitty image by its id (not anonymous ones).
    [[nodiscard]] const Image* find(uint32_t id) const noexcept;
    [[nodiscard]] const Image* by_key(uint64_t key) const noexcept;
    // A sixel / iTerm2 image by the id its cells carry.
    [[nodiscard]] const Image* cell_image(uint32_t id) const noexcept;
    [[nodiscard]] const std::vector<Placement>& placements() const noexcept { return placements_; }
    [[nodiscard]] size_t image_count() const noexcept { return images_.size() + cell_images_.size(); }
    [[nodiscard]] size_t kitty_image_count() const noexcept { return images_.size(); }
    [[nodiscard]] size_t cell_image_count() const noexcept { return cell_images_.size(); }
    template <class F>
    void for_each_image(F&& f) const {
        for (const auto& kv : images_) f(*kv.second);
        for (const auto& kv : cell_images_) f(*kv.second);
    }
    // The extent (columns, rows) a placement covers at the given cell size.
    [[nodiscard]] static std::pair<int, int> extent(const Placement& p, int cell_w, int cell_h) noexcept;
    // The top-left cell (absolute row, column) of a placement, following
    // relative placements up to their root; false when a parent is gone or
    // virtual (such a placement is not shown).
    bool position(const Placement& p, int64_t& row, int& col) const noexcept;

private:
    friend class detail::Graphics;
    struct Anchor {  // where a cell image's cells were written, for liveness
        uint32_t id{0};
        int64_t row{0};
        int rows{0};
    };
    std::unordered_map<uint64_t, std::unique_ptr<Image>> images_;
    std::unordered_map<uint32_t, std::unique_ptr<Image>> cell_images_;
    std::vector<Placement> placements_;
    std::vector<Anchor> anchors_;
};

} // namespace bropty
