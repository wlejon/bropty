#include "image_helpers.h"

#include "base64.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#include "third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "third_party/stb/stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#ifndef BROPTY_TEST_DATA
#define BROPTY_TEST_DATA "tests/data"
#endif

namespace ih {

namespace {

// Baseline uncompressed TIFF (what chafa sends over iTerm2's protocol, which
// stb_image does not read): 8-bit RGB / RGBA, chunky, any strip layout, either
// byte order; unassociated or premultiplied alpha.
bool tiff_decode(std::string_view d, bropty::DecodedImage& out) {
    const auto* b = reinterpret_cast<const uint8_t*>(d.data());
    const size_t n = d.size();
    if (n < 8) return false;
    const bool le = b[0] == 'I' && b[1] == 'I';
    if (!le && !(b[0] == 'M' && b[1] == 'M')) return false;
    auto u16 = [&](size_t o) -> uint32_t { return o + 2 > n ? 0 : le ? b[o] | b[o + 1] << 8 : b[o] << 8 | b[o + 1]; };
    auto u32 = [&](size_t o) -> uint32_t {
        return o + 4 > n ? 0 : le ? u16(o) | u16(o + 2) << 16 : u16(o) << 16 | u16(o + 2);
    };
    if (u16(2) != 42) return false;
    const size_t ifd = u32(4);
    const uint32_t entries = u16(ifd);
    uint32_t w = 0, h = 0, comp = 1, spp = 1, rps = 0xffffffff, extra = 0, bps = 8;
    std::vector<uint32_t> offs, counts;
    for (uint32_t e = 0; e < entries; ++e) {
        const size_t at = ifd + 2 + size_t(e) * 12;
        const uint32_t tag = u16(at), type = u16(at + 2), count = u32(at + 4);
        const uint32_t size = type == 3 ? 2 : 4;
        const size_t vals = count * size <= 4 ? at + 8 : u32(at + 8);
        auto val = [&](uint32_t i) { return size == 2 ? u16(vals + i * 2) : u32(vals + i * 4); };
        switch (tag) {
        case 256: w = val(0); break;
        case 257: h = val(0); break;
        case 258: bps = val(0); break;
        case 259: comp = val(0); break;
        case 277: spp = val(0); break;
        case 278: rps = val(0); break;
        case 338: extra = val(0); break;
        case 273: for (uint32_t i = 0; i < count && i < 65536; ++i) offs.push_back(val(i)); break;
        case 279: for (uint32_t i = 0; i < count && i < 65536; ++i) counts.push_back(val(i)); break;
        default: break;
        }
    }
    if (!w || !h || w > 16384 || h > 16384 || comp != 1 || bps != 8 || (spp != 3 && spp != 4)) return false;
    if (offs.size() != counts.size() || offs.empty()) return false;
    std::string raw;
    for (size_t i = 0; i < offs.size(); ++i) {
        if (offs[i] > n || counts[i] > n - offs[i]) return false;
        raw.append(d.substr(offs[i], counts[i]));
    }
    (void)rps;
    if (raw.size() < size_t(w) * h * spp) return false;
    out.width = w;
    out.height = h;
    out.frames.resize(1);
    std::vector<uint8_t>& px = out.frames[0].rgba;
    px.resize(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        const auto* s = reinterpret_cast<const uint8_t*>(raw.data()) + i * spp;
        const uint8_t a = spp == 4 ? s[3] : 255;
        for (int c = 0; c < 3; ++c)
            px[i * 4 + c] = extra == 1 && a ? uint8_t(std::min(255, (s[c] * 255 + a / 2) / a)) : s[c];
        px[i * 4 + 3] = a;
    }
    return true;
}

} // namespace

bool stb_decode(std::string_view data, bropty::DecodedImage& out) {
    out = bropty::DecodedImage{};
    if (data.size() >= 4 && (data.substr(0, 4) == std::string_view("II*\0", 4) ||
                             data.substr(0, 4) == std::string_view("MM\0*", 4)))
        return tiff_decode(data, out);
    const auto* p = reinterpret_cast<const stbi_uc*>(data.data());
    const int len = int(data.size());
    if (data.size() >= 6 && std::memcmp(data.data(), "GIF", 3) == 0) {
        int* delays = nullptr;
        int w = 0, h = 0, frames = 0, comp = 0;
        stbi_uc* px = stbi_load_gif_from_memory(p, len, &delays, &w, &h, &frames, &comp, 4);
        if (!px) return false;
        out.width = uint32_t(w);
        out.height = uint32_t(h);
        const size_t fb = size_t(w) * size_t(h) * 4;
        for (int i = 0; i < frames; ++i) {
            bropty::DecodedFrame f;
            f.rgba.assign(px + fb * size_t(i), px + fb * size_t(i + 1));
            f.delay_ms = delays ? uint32_t(delays[i]) : 0;
            out.frames.push_back(std::move(f));
        }
        stbi_image_free(px);
        stbi_image_free(delays);
        return true;
    }
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(p, len, &w, &h, &comp, 4);
    if (!px) return false;
    out.width = uint32_t(w);
    out.height = uint32_t(h);
    out.frames.resize(1);
    out.frames[0].rgba.assign(px, px + size_t(w) * size_t(h) * 4);
    stbi_image_free(px);
    return true;
}

bool load_png_file(const std::string& path, Rgba& out) {
    bropty::DecodedImage d;
    if (!stb_decode(read_file(path), d) || d.frames.empty()) return false;
    out.w = d.width;
    out.h = d.height;
    out.px = std::move(d.frames[0].rgba);
    return true;
}

namespace {
void append_cb(void* ctx, void* data, int size) {
    static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), size_t(size));
}
} // namespace

std::string encode_png(const Rgba& img) {
    std::string out;
    stbi_write_png_to_func(append_cb, &out, int(img.w), int(img.h), 4, img.px.data(), int(img.w * 4));
    return out;
}

std::string zlib_compress(std::string_view data, int quality) {
    int n = 0;
    unsigned char* z = stbi_zlib_compress(reinterpret_cast<unsigned char*>(const_cast<char*>(data.data())),
                                          int(data.size()), &n, quality);
    std::string out(reinterpret_cast<const char*>(z), size_t(n));
    STBIW_FREE(z);
    return out;
}

bool stb_inflate(std::string_view in, std::string& out, bool raw) {
    int n = 0;
    char* p = raw ? stbi_zlib_decode_noheader_malloc(in.data(), int(in.size()), &n)
                  : stbi_zlib_decode_malloc(in.data(), int(in.size()), &n);
    if (!p) return false;
    out.assign(p, size_t(n));
    stbi_image_free(p);
    return true;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_file(const std::string& path, std::string_view data) {
    std::ofstream f(path, std::ios::binary);
    f.write(data.data(), std::streamsize(data.size()));
    return bool(f);
}

std::string fixture(const std::string& name) { return std::string(BROPTY_TEST_DATA) + "/images/" + name; }

bool DecodingHost::decode_image(std::string_view data, const bropty::ImageLimits& limits, bropty::DecodedImage& out) {
    ++decodes;
    if (refuse || !stb_decode(data, out)) return false;
    size_t total = 0;
    for (const auto& f : out.frames) total += f.rgba.size();
    return out.width <= limits.max_width && out.height <= limits.max_height && total <= limits.max_bytes;
}

Rgba pattern(uint32_t w, uint32_t h, uint32_t seed, bool alpha) {
    Rgba r;
    r.w = w;
    r.h = h;
    r.px.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = r.px.data() + (size_t(y) * w + x) * 4;
            p[0] = uint8_t(x * 7 + seed * 31);
            p[1] = uint8_t(y * 11 + seed * 17);
            p[2] = uint8_t((x ^ y) * 5 + seed);
            p[3] = alpha ? uint8_t((x + y) * 9) : 255;
        }
    }
    return r;
}

std::string b64(std::string_view raw) { return bropty::detail::base64_encode(raw); }

std::string kitty(std::string_view control, std::string_view raw_payload) {
    std::string s = "\x1b_G";
    s += control;
    if (!raw_payload.empty()) {
        s += ';';
        s += b64(raw_payload);
    }
    s += "\x1b\\";
    return s;
}

bool same_pixels(const bropty::ImagePixels& p, const Rgba& want) {
    return p.width == want.w && p.height == want.h && p.rgba == want.px;
}

} // namespace ih
