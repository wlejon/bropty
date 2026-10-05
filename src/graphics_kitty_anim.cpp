// The kitty graphics protocol's animation half: frame transmission (a=f),
// animation control (a=a) and frame composition (a=c). Frames are stored
// fully composed (image-sized RGBA), so a renderer never needs to know how a
// frame was built; editing one makes a new immutable buffer.
#include "bropty/terminal.h"

#include "graphics_load.h"
#include "graphics_state.h"

#include <algorithm>

namespace bropty {

using detail::KittyCommand;

namespace {

constexpr uint32_t kDefaultGap = 40;

// kitty's alpha_blend: non-premultiplied "over".
inline void blend_px(uint8_t* dst, const uint8_t* src) {
    if (!src[3]) return;
    const float da = float(dst[3]) / 255.f, sa = float(src[3]) / 255.f;
    const float a = sa + da * (1.f - sa);
    for (int i = 0; i < 3; ++i) dst[i] = uint8_t((float(src[i]) * sa + float(dst[i]) * da * (1.f - sa)) / a);
    dst[3] = uint8_t(255.f * a);
}

// Draw `src` (sw x sh, RGBA) rectangle (sx, sy, w, h) onto `dst` (dw x dh) at
// (dx, dy), clipped to both.
void compose(std::vector<uint8_t>& dst, uint32_t dw, uint32_t dh, uint32_t dx, uint32_t dy, const uint8_t* src,
             uint32_t sw, uint32_t sh, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, bool blend) {
    if (dx >= dw || dy >= dh || sx >= sw || sy >= sh) return;
    w = std::min({w, dw - dx, sw - sx});
    h = std::min({h, dh - dy, sh - sy});
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* d = dst.data() + (size_t(dy + y) * dw + dx) * 4;
        const uint8_t* s = src + (size_t(sy + y) * sw + sx) * 4;
        if (!blend) {
            std::copy(s, s + size_t(w) * 4, d);
            continue;
        }
        for (uint32_t x = 0; x < w; ++x) blend_px(d + x * 4, s + x * 4);
    }
}

Image* animation_target(detail::Graphics& g, bool alt, const KittyCommand& c, std::string& error) {
    if (!c.id && !c.number) {
        error = "EINVAL:Animation command without an image id or number";
        return nullptr;
    }
    Image* img = c.id ? g.by_id(alt, c.id) : g.by_number(alt, c.number);
    if (!img) error = "ENOENT:Animation command refers to a non-existent image";
    return img;
}

} // namespace

std::string Terminal::kitty_frame(KittyCommand& cmd, std::vector<uint8_t>& data) {
    detail::Graphics& g = *gfx_;
    const bool alt = alt_screen_active();
    std::string error;
    Image* img = animation_target(g, alt, cmd, error);
    if (!img) return error;
    KittyCommand c = cmd;
    if (!c.width) c.width = img->width;
    if (!c.height) c.height = img->height;
    const size_t count = img->frames.size();
    uint32_t number = c.rows;
    if (!number || number > count + 1) number = uint32_t(count + 1);
    const bool is_new = number == count + 1;
    cmd.rows = number;  // kitty reports the frame number written
    if (!detail::read_medium(c, data, g.options(), error)) return error;
    DecodedImage d;
    if (!detail::decode_kitty_pixels(c, data, g.options(), host_, d, error)) return error;
    if (d.width > img->width || d.height > img->height) return "EINVAL:Frame is larger than the image";
    if (is_new && count >= g.options().max_frames) return "ENOSPC:Too many frames";
    const bool blend = c.cell_x != 1 && c.format != 24;
    const uint32_t gap = c.z > 0 ? uint32_t(c.z) : c.z < 0 ? 0u : kDefaultGap;

    std::vector<uint8_t> canvas;
    if (is_new) {
        if (c.cols) {
            if (c.cols > count) return "EINVAL:No such base frame";
            canvas = img->frames[c.cols - 1].pixels->rgba;
        } else {
            canvas.resize(size_t(img->width) * img->height * 4);
            const uint8_t bg[4] = {uint8_t(c.cell_y >> 24), uint8_t(c.cell_y >> 16), uint8_t(c.cell_y >> 8),
                                   uint8_t(c.cell_y)};
            for (size_t i = 0; i < canvas.size(); i += 4) std::copy(bg, bg + 4, canvas.data() + i);
        }
    } else {
        canvas = img->frames[number - 1].pixels->rgba;
    }
    compose(canvas, img->width, img->height, c.x, c.y, d.frames[0].rgba.data(), d.width, d.height, 0, 0, d.width,
            d.height, blend);
    const size_t old = img->bytes();
    ImagePixelsPtr px = detail::Graphics::make_pixels(img->width, img->height, std::move(canvas));
    if (is_new) {
        img->frames.push_back(ImageFrame{std::move(px), gap});
    } else {
        img->frames[number - 1].pixels = std::move(px);
        if (c.z != 0) img->frames[number - 1].gap_ms = gap;
    }
    const uint64_t key = img->key;
    if (!g.resized(alt, *img, old, error)) {
        if (Image* again = g.by_key(alt, key); again && is_new) {
            const size_t before = again->bytes();
            again->frames.pop_back();
            std::string ignored;
            g.resized(alt, *again, before, ignored);
        }
        return error;
    }
    return {};
}

std::string Terminal::kitty_animate(const KittyCommand& c) {
    detail::Graphics& g = *gfx_;
    std::string error;
    Image* img = animation_target(g, alt_screen_active(), c, error);
    if (!img) return error;
    const size_t n = img->frames.size();
    if (c.rows && c.rows <= n && c.z) img->frames[c.rows - 1].gap_ms = c.z > 0 ? uint32_t(c.z) : 0;
    if (c.cols && c.cols <= n && c.cols - 1 != img->current_frame) {
        img->current_frame = c.cols - 1;
        img->frame_shown_ms = g.clock_ms();
    }
    if (c.width >= 1 && c.width <= 3) {
        const AnimationState old = img->animation;
        img->animation = c.width == 1 ? AnimationState::Stopped
                         : c.width == 2 ? AnimationState::Loading
                                        : AnimationState::Running;
        if (old == AnimationState::Stopped && img->animation != AnimationState::Stopped)
            img->frame_shown_ms = g.clock_ms();
        img->current_loop = 0;
    }
    if (c.height) img->max_loops = c.height - 1;
    g.touch_version();
    return {};
}

std::string Terminal::kitty_compose(const KittyCommand& c) {
    detail::Graphics& g = *gfx_;
    const bool alt = alt_screen_active();
    std::string error;
    Image* img = animation_target(g, alt, c, error);
    if (!img) return error;
    const size_t n = img->frames.size();
    // kitty: r = source frame, c = destination frame (1-based).
    if (!c.rows || c.rows > n) return "ENOENT:No such source frame";
    if (!c.cols || c.cols > n) return "ENOENT:No such destination frame";
    const uint64_t w = c.w ? c.w : img->width, h = c.h ? c.h : img->height;
    const uint64_t dx = c.x, dy = c.y, sx = c.cell_x, sy = c.cell_y;
    if (dx + w > img->width || dy + h > img->height) return "EINVAL:The destination rectangle is out of bounds";
    if (sx + w > img->width || sy + h > img->height) return "EINVAL:The source rectangle is out of bounds";
    if (c.rows == c.cols) {
        const bool xo = std::max(sx, dx) < std::min(sx, dx) + w;
        const bool yo = std::max(sy, dy) < std::min(sy, dy) + h;
        if (xo && yo) return "EINVAL:The source and destination rectangles overlap";
    }
    const ImagePixelsPtr src = img->frames[c.rows - 1].pixels;
    std::vector<uint8_t> canvas = img->frames[c.cols - 1].pixels->rgba;
    compose(canvas, img->width, img->height, uint32_t(dx), uint32_t(dy), src->rgba.data(), img->width, img->height,
            uint32_t(sx), uint32_t(sy), uint32_t(w), uint32_t(h), c.cursor == 0);
    img->frames[c.cols - 1].pixels = detail::Graphics::make_pixels(img->width, img->height, std::move(canvas));
    g.touch_version();
    return {};
}

} // namespace bropty
