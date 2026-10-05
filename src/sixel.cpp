// Streaming sixel decoder (see sixel.h for the semantics chosen).
#include "sixel.h"

#include <algorithm>
#include <cstring>

namespace bropty::detail {

namespace {

constexpr uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) noexcept { return uint32_t(r) << 16 | uint32_t(g) << 8 | b; }
constexpr uint32_t pct(int r, int g, int b) noexcept { return rgb(sixel_percent(r), sixel_percent(g), sixel_percent(b)); }

constexpr int kParamCap = 1 << 30;

inline uint32_t sat_add(uint32_t a, uint32_t b) noexcept { return a > 0xFFFFFFFFu - b ? 0xFFFFFFFFu : a + b; }

} // namespace

SixelPalette SixelPalette::standard() {
    SixelPalette p;
    // VT340 default colors (registers 0..15), as xterm and libsixel define them.
    static constexpr uint32_t vt340[16] = {
        pct(0, 0, 0),    pct(20, 20, 80), pct(80, 13, 13), pct(20, 80, 20), pct(80, 20, 80), pct(20, 80, 80),
        pct(80, 80, 20), pct(53, 53, 53), pct(26, 26, 26), pct(33, 33, 60), pct(60, 26, 26), pct(33, 60, 33),
        pct(60, 33, 60), pct(33, 60, 60), pct(60, 60, 33), pct(80, 80, 80),
    };
    size_t n = 0;
    for (; n < 16; ++n) p.rgb[n] = vt340[n];
    for (int r = 0; r < 6; ++r)
        for (int g = 0; g < 6; ++g)
            for (int b = 0; b < 6; ++b) p.rgb[n++] = detail::rgb(uint8_t(r * 42), uint8_t(g * 42), uint8_t(b * 42));
    for (int i = 0; i < 24; ++i) p.rgb[n++] = detail::rgb(uint8_t(i * 11), uint8_t(i * 11), uint8_t(i * 11));
    for (; n < 256; ++n) p.rgb[n] = detail::rgb(255, 255, 255);
    return p;  // 256..1023 start black
}

uint32_t sixel_hls(int h, int l, int s) noexcept {
    h = std::clamp(h, 0, 360);
    l = std::clamp(l, 0, 100);
    s = std::clamp(s, 0, 100);
    if (s == 0) return pct(l, l, l);
    const double lv = l / 100.0, sv = s / 100.0;
    h -= 120;  // DEC puts blue at 0 degrees
    while (h < 0) h += 360;
    while (h >= 360) h -= 360;
    int hs = (h % 120) - 60;
    if (hs < 0) hs = -hs;
    double c2 = 2.0 * lv - 1.0;
    if (c2 < 0) c2 = -c2;
    const double c = (1.0 - c2) * sv;
    const double x = ((60 - hs) / 60.0) * c;
    const double m = lv - 0.5 * c;
    double r1 = 0, g1 = 0, b1 = 0;
    switch (h / 60) {
    case 0: r1 = c; g1 = x; break;
    case 1: r1 = x; g1 = c; break;
    case 2: g1 = c; b1 = x; break;
    case 3: g1 = x; b1 = c; break;
    case 4: r1 = x; b1 = c; break;
    default: r1 = c; b1 = x; break;
    }
    auto to_pct = [](double v) { return std::clamp(int(v * 100.0 + 0.5), 0, 100); };
    return pct(to_pct(r1 + m), to_pct(g1 + m), to_pct(b1 + m));
}

SixelDecoder::SixelDecoder(int p1, int p2, int p3, uint32_t max_width, uint32_t max_height,
                           const SixelPalette& registers)
    : reg_(registers), max_w_(std::max<uint32_t>(1, max_width)), max_h_(std::max<uint32_t>(1, max_height)) {
    (void)p1;  // pixel aspect ratio: ignored (square pixels)
    (void)p3;  // horizontal grid size: no effect on a raster display
    transparent_ = p2 == 1;
}

bool SixelDecoder::grow(uint32_t need_w, uint32_t need_h) {
    need_w = std::min(need_w, max_w_);
    need_h = std::min(need_h, max_h_);
    if (need_w <= alloc_w_ && need_h <= alloc_h_) return true;
    uint32_t w = std::max({alloc_w_, need_w, 64u});
    uint32_t h = std::max({alloc_h_, need_h, 6u});
    if (need_w > alloc_w_) w = std::min(max_w_, std::max(need_w, alloc_w_ * 2));
    if (need_h > alloc_h_) h = std::min(max_h_, std::max(need_h, alloc_h_ * 2));
    std::vector<uint16_t> next(size_t(w) * h, kHole);
    for (uint32_t y = 0; y < alloc_h_; ++y)
        std::memcpy(next.data() + size_t(y) * w, pix_.data() + size_t(y) * alloc_w_, size_t(alloc_w_) * 2);
    pix_ = std::move(next);
    alloc_w_ = w;
    alloc_h_ = h;
    return true;
}

void SixelDecoder::start_pixels() {
    if (started_) return;
    started_ = true;
    bg_w_ = declared_w_;
    bg_h_ = declared_h_;
}

void SixelDecoder::sixel(uint32_t bits, uint32_t count) {
    start_pixels();
    if (bits == 0 || col_ >= max_w_ || row_ >= max_h_) {
        col_ = sat_add(col_, count);
        return;
    }
    const uint32_t n = std::min(count, max_w_ - col_);
    int top = 0;
    while (!(bits >> top & 1u)) ++top;
    int bottom = 5;
    while (!(bits >> bottom & 1u)) --bottom;
    const uint32_t last_row = std::min(row_ + uint32_t(bottom), max_h_ - 1);
    grow(col_ + n, last_row + 1);
    for (int k = top; k <= bottom; ++k) {
        if (!(bits >> k & 1u)) continue;
        const uint32_t y = row_ + uint32_t(k);
        if (y >= max_h_) break;
        uint16_t* p = pix_.data() + size_t(y) * alloc_w_ + col_;
        std::fill(p, p + n, uint16_t(color_));
    }
    width_ = std::max(width_, col_ + n);
    if (row_ + uint32_t(top) < max_h_) height_ = std::max(height_, last_row + 1);
    col_ = sat_add(col_, count);
}

void SixelDecoder::finish_command(char next) {
    // Close the field in progress (an empty field reads as absent: -1).
    if (nparams_ < 5) params_[nparams_++] = have_digits_ ? cur_ : -1;
    auto param = [&](int i, int def) { return i < nparams_ && params_[i] >= 0 ? params_[i] : def; };
    switch (state_) {
    case State::Repeat: {
        state_ = State::Data;
        const int count = param(0, 1);
        reset_params();
        if (next >= 0x3F && next <= 0x7E) {
            sixel(uint32_t(next - 0x3F), uint32_t(count <= 0 ? 1 : count));
            return;
        }
        break;  // a repeat not followed by a sixel is dropped
    }
    case State::Color: {
        state_ = State::Data;
        const uint32_t r = uint32_t(param(0, 0)) % kSixelRegisters;
        if (nparams_ >= 5) {
            const int pu = param(1, 0), px = param(2, 0), py = param(3, 0), pz = param(4, 0);
            if (pu == 1) reg_.rgb[r] = sixel_hls(px, py, pz);
            else if (pu == 2) reg_.rgb[r] = pct(px, py, pz);
        }
        color_ = r;
        break;
    }
    case State::Raster: {
        state_ = State::Data;
        const int ph = param(2, 0), pv = param(3, 0);
        if (ph > 0) declared_w_ = std::max(declared_w_, std::min<uint32_t>(uint32_t(ph), max_w_));
        if (pv > 0) declared_h_ = std::max(declared_h_, std::min<uint32_t>(uint32_t(pv), max_h_));
        width_ = std::max(width_, declared_w_);
        height_ = std::max(height_, declared_h_);
        break;
    }
    case State::Data: break;
    }
    reset_params();
    // `next` still has to be processed as data.
    const uint8_t b = uint8_t(next);
    if (b >= 0x3F && b <= 0x7E) {
        sixel(b - 0x3Fu, 1);
    } else if (b == '$') {
        col_ = 0;
    } else if (b == '-') {
        col_ = 0;
        row_ = sat_add(row_, 6);
    } else if (b == '!') {
        state_ = State::Repeat;
    } else if (b == '#') {
        state_ = State::Color;
    } else if (b == '"') {
        state_ = State::Raster;
    }
}

void SixelDecoder::feed(std::string_view data) {
    const auto* p = reinterpret_cast<const uint8_t*>(data.data());
    const auto* end = p + data.size();
    while (p < end) {
        const uint8_t b = *p++;
        if (state_ == State::Data) {
            if (b >= 0x3F && b <= 0x7E) {
                // Runs of plain sixels: the bulk of every image.
                sixel(b - 0x3Fu, 1);
                continue;
            }
            switch (b) {
            case '$': col_ = 0; break;
            case '-':
                col_ = 0;
                row_ = sat_add(row_, 6);
                break;
            case '!': state_ = State::Repeat; break;
            case '#': state_ = State::Color; break;
            case '"': state_ = State::Raster; break;
            default: break;  // whitespace, controls, stray digits: ignored
            }
            if (state_ != State::Data) reset_params();
            continue;
        }
        if (b >= '0' && b <= '9') {
            cur_ = cur_ >= kParamCap / 10 ? kParamCap : cur_ * 10 + (b - '0');
            have_digits_ = true;
            continue;
        }
        if (b == ';' && state_ != State::Repeat) {
            if (nparams_ < 5) params_[nparams_++] = have_digits_ ? cur_ : -1;
            cur_ = 0;
            have_digits_ = false;
            continue;
        }
        if (b <= 0x20 || b == 0x7F) continue;  // whitespace and controls inside a command
        finish_command(char(b));
    }
}

void SixelDecoder::to_rgba(std::vector<uint8_t>& out) const {
    const size_t w = width_, h = height_;
    out.assign(w * h * 4, 0);
    const uint32_t bg = reg_.rgb[0];
    for (size_t y = 0; y < h; ++y) {
        uint8_t* d = out.data() + y * w * 4;
        const uint16_t* s = y < alloc_h_ ? pix_.data() + y * alloc_w_ : nullptr;
        for (size_t x = 0; x < w; ++x, d += 4) {
            const uint16_t idx = (s && x < alloc_w_) ? s[x] : kHole;
            uint32_t c;
            if (idx == kHole) {
                if (transparent_ || x >= bg_w_ || y >= bg_h_) continue;
                c = bg;
            } else {
                c = reg_.rgb[idx];
            }
            d[0] = uint8_t(c >> 16);
            d[1] = uint8_t(c >> 8);
            d[2] = uint8_t(c);
            d[3] = 255;
        }
    }
}

} // namespace bropty::detail
