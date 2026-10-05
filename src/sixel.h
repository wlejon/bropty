#pragma once
// Internal: a streaming sixel decoder (DEC STD 070 / VT340, with xterm's
// semantics where implementations differ).
//
//  - Data arrives in pieces (DCS passthrough), so no copy of the sixel
//    string is ever held; memory is the pixel buffer, bounded by the limits.
//  - Pixels hold color-register numbers until finish(): registers redefined
//    after use recolor earlier pixels, as on the VT340, in xterm and libsixel.
//  - 1024 registers, numbers wrapping modulo 1024 (xterm). Registers 0-15
//    start as the VT340 palette, 16-255 as libsixel's 6x6x6 cube and gray
//    ramp. The current register starts at 15 (libsixel). Colors are given in
//    RGB or HLS percent; out-of-range components are clamped (libsixel; xterm
//    abandons the image instead).
//  - Raster attributes ("Pan;Pad;Ph;Pv) declare a minimum size. The pixel
//    aspect ratio (Pan/Pad, P1) is ignored: pixels are square (xterm forces
//    1:1 too).
//  - Background (P2): 1 leaves unset pixels transparent; 0 or 2 paints the
//    area declared by the raster attributes, as it was when the first sixel
//    arrived, with register 0 (xterm). Unset pixels outside it stay
//    transparent.
//  - The image grows with the pixels set (and the declared size) up to
//    max_width x max_height; pixels beyond are dropped.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace bropty::detail {

constexpr int kSixelRegisters = 1024;

struct SixelPalette {
    std::array<uint32_t, kSixelRegisters> rgb{};  // 0xRRGGBB
    static SixelPalette standard();
};

// Percent (0..100) to 0..255, rounded (libsixel's PALVAL).
[[nodiscard]] constexpr uint8_t sixel_percent(int v) noexcept {
    v = v < 0 ? 0 : (v > 100 ? 100 : v);
    return uint8_t((v * 255 + 50) / 100);
}
// HLS (hue 0..360 with blue at 0, as DEC defines it; lightness and
// saturation 0..100) to 0xRRGGBB, the way xterm converts it.
[[nodiscard]] uint32_t sixel_hls(int h, int l, int s) noexcept;

class SixelDecoder {
public:
    // p1, p2, p3: the DCS parameters (-1 when absent).
    SixelDecoder(int p1, int p2, int p3, uint32_t max_width, uint32_t max_height, const SixelPalette& registers);

    void feed(std::string_view data);

    [[nodiscard]] uint32_t width() const noexcept { return width_; }
    [[nodiscard]] uint32_t height() const noexcept { return height_; }
    [[nodiscard]] bool transparent_background() const noexcept { return transparent_; }
    [[nodiscard]] const SixelPalette& registers() const noexcept { return reg_; }
    // Whether any pixel was set or a size declared.
    [[nodiscard]] bool empty() const noexcept { return width_ == 0 || height_ == 0; }

    // The image as 8-bit RGBA (width() * height() * 4 bytes).
    void to_rgba(std::vector<uint8_t>& out) const;

private:
    enum class State : uint8_t { Data, Repeat, Color, Raster };
    void finish_command(char next);
    void sixel(uint32_t bits, uint32_t count);
    bool grow(uint32_t need_w, uint32_t need_h);
    void start_pixels();

    void reset_params() noexcept {
        nparams_ = 0;
        cur_ = 0;
        have_digits_ = false;
    }

    State state_{State::Data};
    int params_[5]{};   // completed fields; -1 = empty
    int nparams_{0};
    int cur_{0};        // the field being read
    bool have_digits_{false};

    SixelPalette reg_;
    uint32_t color_{15};
    uint32_t col_{0};
    uint32_t row_{0};  // top pixel row of the current sixel band
    uint32_t max_w_;
    uint32_t max_h_;

    bool transparent_{false};
    bool started_{false};    // first sixel seen (background area fixed)
    uint32_t declared_w_{0};
    uint32_t declared_h_{0};
    uint32_t bg_w_{0};
    uint32_t bg_h_{0};
    uint32_t width_{0};   // max(declared, set pixels)
    uint32_t height_{0};

    std::vector<uint16_t> pix_;  // register numbers, kHole for unset
    uint32_t alloc_w_{0};
    uint32_t alloc_h_{0};
    static constexpr uint16_t kHole = 0xFFFF;
};

} // namespace bropty::detail
