#pragma once
// Shared fixtures for the inline-image tests: a host that decodes PNG / GIF
// with stb_image (test-only, public domain), PNG / zlib encoding with
// stb_image_write, fixture loading, and pixel helpers.

#include "bropty/frame.h"
#include "bropty/terminal.h"
#include "term_helpers.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ih {

struct Rgba {
    uint32_t w{0}, h{0};
    std::vector<uint8_t> px;  // w * h * 4
    [[nodiscard]] const uint8_t* at(uint32_t x, uint32_t y) const { return px.data() + (size_t(y) * w + x) * 4; }
};

// stb_image: PNG / GIF / ... to RGBA. GIFs keep every frame.
bool stb_decode(std::string_view data, bropty::DecodedImage& out);
bool load_png_file(const std::string& path, Rgba& out);
// stb_image_write: RGBA to PNG bytes; a zlib stream of `data`.
std::string encode_png(const Rgba& img);
std::string zlib_compress(std::string_view data, int quality = 8);
// stb's own inflater (the reference for bropty's): zlib, and raw DEFLATE.
bool stb_inflate(std::string_view in, std::string& out, bool raw);

std::string read_file(const std::string& path);
bool write_file(const std::string& path, std::string_view data);
// tests/data/images/<name>
std::string fixture(const std::string& name);

// A host whose decode_image uses stb_image; counts calls.
struct DecodingHost : th::Capture {
    int decodes = 0;
    bool refuse = false;
    bool decode_image(std::string_view data, const bropty::ImageLimits& limits, bropty::DecodedImage& out) override;
};

// A terminal with the decoding host attached.
struct IT {
    DecodingHost host;
    bropty::Terminal t;
    explicit IT(const bropty::TerminalOptions& o) : t(o) { t.set_host(&host); }
    IT(int cols, int rows, size_t sb = 1000) : t(th::opts(cols, rows, sb)) {
        t.set_host(&host);
        t.set_cell_pixel_size(10, 20);
    }
    IT& operator<<(std::string_view s) {
        t.feed(s);
        return *this;
    }
    std::string reply() {
        std::string r = host.out;
        host.out.clear();
        return r;
    }
};

// Deterministic test pattern: pixel (x, y) of image `seed`.
Rgba pattern(uint32_t w, uint32_t h, uint32_t seed, bool alpha = false);
// Kitty APC: ESC _ G <control> ; <payload> ESC \  (payload base64-encoded here).
std::string kitty(std::string_view control, std::string_view raw_payload = {});
std::string b64(std::string_view raw);
// Same pixels (exactly).
bool same_pixels(const bropty::ImagePixels& p, const Rgba& want);

} // namespace ih
