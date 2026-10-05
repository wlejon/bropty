// Sixel: the decoder against libsixel's (random streams and img2sixel's
// real output), its documented semantics (palette, HLS, raster attributes,
// background, limits), a fuzz run, and the terminal side as xterm defines it
// (placement as cells, cursor rules, scrolling, DECSDM, ?8452, ?1070,
// XTSMGRAPHICS, DA1) plus the frames a renderer gets.
#include "image_helpers.h"

#include "bropty/view.h"
#include "sixel.h"

#include <cstdlib>
#include <random>

// libsixel's decoder (tests/third_party/libsixel), declared here because its
// header shares a name with bropty's.
extern "C" int sixel_decode_raw(unsigned char* p, int len, unsigned char** pixels, int* pwidth, int* pheight,
                                unsigned char** palette, int* ncolors, struct sixel_allocator* allocator);

using namespace bropty;
using detail::SixelDecoder;
using detail::SixelPalette;

namespace {

struct Decoded {
    uint32_t w{0}, h{0};
    std::vector<uint8_t> rgba;
};

Decoded ours(std::string_view body, int p2 = 1, uint32_t max = 4096) {
    SixelDecoder d(0, p2, 0, max, max, SixelPalette::standard());
    d.feed(body);
    Decoded r;
    r.w = d.width();
    r.h = d.height();
    d.to_rgba(r.rgba);
    return r;
}

// libsixel: indices + palette; index 255 (its "no background" fill) = unset.
bool reference(std::string_view body, Decoded& out, std::vector<int>& index) {
    std::string s = "\x1bPq" + std::string(body) + "\x1b\\";
    unsigned char *pixels = nullptr, *palette = nullptr;
    int w = 0, h = 0, ncolors = 0;
    const int st = sixel_decode_raw(reinterpret_cast<unsigned char*>(s.data()), int(s.size()), &pixels, &w, &h,
                                    &palette, &ncolors, nullptr);
    if (st & 0x1000) return false;  // SIXEL_FAILED
    out.w = uint32_t(w);
    out.h = uint32_t(h);
    out.rgba.assign(size_t(w) * size_t(h) * 4, 0);
    index.assign(size_t(w) * size_t(h), 255);
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
        const int k = pixels[i];
        index[i] = k;
        if (k >= ncolors) continue;
        out.rgba[i * 4 + 0] = palette[k * 3 + 0];
        out.rgba[i * 4 + 1] = palette[k * 3 + 1];
        out.rgba[i * 4 + 2] = palette[k * 3 + 2];
        out.rgba[i * 4 + 3] = 255;
    }
    std::free(pixels);
    std::free(palette);
    return true;
}

void palette_and_colors() {
    const SixelPalette p = SixelPalette::standard();
    CHECK_EQ(p.rgb[0], 0x000000u);
    CHECK_EQ(p.rgb[1], 0x3333CCu);   // 20% 20% 80%
    CHECK_EQ(p.rgb[2], 0xCC2121u);   // 80% 13% 13%
    CHECK_EQ(p.rgb[7], 0x878787u);   // 53%
    CHECK_EQ(p.rgb[15], 0xCCCCCCu);  // 80%
    CHECK_EQ(p.rgb[16], 0x000000u);  // libsixel's cube
    CHECK_EQ(p.rgb[16 + 215], 0xD2D2D2u);
    CHECK_EQ(p.rgb[232], 0x000000u);  // gray ramp
    CHECK_EQ(p.rgb[300], 0x000000u);
    // DEC HLS: blue at 0 degrees, red at 120, green at 240.
    CHECK_EQ(detail::sixel_hls(0, 50, 100), 0x0000FFu);
    CHECK_EQ(detail::sixel_hls(120, 50, 100), 0xFF0000u);
    CHECK_EQ(detail::sixel_hls(240, 50, 100), 0x00FF00u);
    CHECK_EQ(detail::sixel_hls(0, 100, 0), 0xFFFFFFu);
    CHECK_EQ(detail::sixel_hls(77, 30, 0), detail::sixel_hls(0, 30, 0));  // no saturation: gray
    // Register definitions, selection and the default register (15).
    Decoded d = ours("#1;2;100;0;0#1~-#2;1;240;50;100!3~-~");
    CHECK_EQ(d.w, 3u);
    CHECK_EQ(d.h, 18u);
    auto at = [&](uint32_t x, uint32_t y) {
        const uint8_t* q = d.rgba.data() + (size_t(y) * d.w + x) * 4;
        return uint32_t(q[0]) << 24 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 8 | q[3];
    };
    CHECK_EQ(at(0, 0), 0xFF0000FFu);
    CHECK_EQ(at(1, 0), 0u);  // never set: transparent (P2=1)
    CHECK_EQ(at(2, 6), 0x00FF00FFu);
    CHECK_EQ(at(0, 12), 0x00FF00FFu);  // the current register carries over bands
    // Registers redefined after use recolor earlier pixels (VT340 / xterm).
    d = ours("#5;2;0;0;100~#5;2;100;100;0$");  // a command ends at the next character (as in libsixel, xterm)
    CHECK_EQ(at(0, 0), 0xFFFF00FFu);
    // Out-of-range components are clamped; register numbers wrap at 1024.
    d = ours("#1027;2;200;0;0~");
    CHECK_EQ(at(0, 0), 0xFF0000FFu);
}

void raster_and_background() {
    // Raster attributes declare a minimum size; P2=0 paints the declared area
    // with register 0 when the first sixel arrives.
    SixelDecoder d(0, 0, 0, 100, 100, SixelPalette::standard());
    d.feed("\"1;1;5;8#0;2;0;0;100#1;2;100;0;0@");  // one pixel at (0,0)
    CHECK_EQ(d.width(), 5u);
    CHECK_EQ(d.height(), 8u);
    CHECK(!d.transparent_background());
    std::vector<uint8_t> px;
    d.to_rgba(px);
    CHECK_EQ(int(px[0]), 255);                    // the red pixel
    CHECK_EQ(int(px[(1 * 5 + 4) * 4 + 2]), 255);  // background: register 0, blue
    CHECK_EQ(int(px[(7 * 5 + 4) * 4 + 3]), 255);
    // P2=1: the background stays transparent.
    SixelDecoder t(0, 1, 0, 100, 100, SixelPalette::standard());
    t.feed("\"1;1;5;8@");
    t.to_rgba(px);
    CHECK_EQ(int(px[(7 * 5 + 4) * 4 + 3]), 0);
    // Pixel aspect ratio is ignored (square pixels).
    SixelDecoder a(0, 1, 0, 100, 100, SixelPalette::standard());
    a.feed("\"2;1~~");
    CHECK_EQ(a.width(), 2u);
    CHECK_EQ(a.height(), 6u);
}

void limits() {
    // Repeats and bands beyond the maximum are clipped, never allocated.
    SixelDecoder d(0, 1, 0, 50, 30, SixelPalette::standard());
    d.feed("!2000000000~");
    for (int i = 0; i < 100; ++i) d.feed("-~");
    d.feed("\"1;1;99999;99999");
    CHECK(d.width() <= 50);
    CHECK(d.height() <= 30);
    std::vector<uint8_t> px;
    d.to_rgba(px);
    CHECK_EQ(px.size(), size_t(d.width()) * d.height() * 4);
    // Huge parameters saturate.
    SixelDecoder e(0, 1, 0, 50, 30, SixelPalette::standard());
    e.feed("#99999999999999999999;2;99999999999;0;0~!99999999999999999?");
    CHECK(e.width() <= 50);
}

// Random streams within the subset where libsixel and xterm agree (see
// sixel.h): registers 0..254, no ';' inside a repeat, repeat counts < 65536.
std::string random_stream(std::mt19937& rng) {
    std::string s;
    if (rng() % 3 == 0)
        s += "\"1;1;" + std::to_string(rng() % 40) + ";" + std::to_string(rng() % 40);
    const int n = 20 + int(rng() % 300);
    for (int i = 0; i < n; ++i) {
        switch (rng() % 12) {
        case 0:
            s += "#" + std::to_string(rng() % 255) + ";2;" + std::to_string(rng() % 101) + ";" +
                 std::to_string(rng() % 101) + ";" + std::to_string(rng() % 101);
            break;
        case 1: s += "#" + std::to_string(rng() % 255); break;
        case 2:
            s += "#" + std::to_string(rng() % 255) + ";1;" + std::to_string(rng() % 361) + ";" +
                 std::to_string(rng() % 51) + ";" + std::to_string(rng() % 101);
            break;
        case 3: s += "!" + std::to_string(rng() % 20) + char('?' + rng() % 64); break;
        case 4: s += '$'; break;
        case 5: s += '-'; break;
        default: s += char('?' + rng() % 64);
        }
    }
    return s;
}

bool close_rgb(const uint8_t* a, const uint8_t* b, int tol) {
    for (int i = 0; i < 3; ++i)
        if (std::abs(int(a[i]) - int(b[i])) > tol) return false;
    return true;
}

void differential() {
    std::mt19937 rng(1234);
    int compared = 0;
    for (int iter = 0; iter < 1500; ++iter) {
        const std::string s = random_stream(rng);
        Decoded mine = ours(s);
        Decoded ref;
        std::vector<int> index;
        if (!reference(s, ref, index)) continue;
        ++compared;
        if (mine.w != ref.w || mine.h != ref.h) {
            CHECK_EQ(mine.w, ref.w);
            CHECK_EQ(mine.h, ref.h);
            std::printf("  stream: %s\n", check::escape(s).c_str());
            return;
        }
        for (size_t i = 0; i < size_t(mine.w) * mine.h; ++i) {
            const bool set_mine = mine.rgba[i * 4 + 3] != 0, set_ref = index[i] != 255;
            // HLS conversions round differently (libsixel truncates): 3 / 255.
            if (set_mine != set_ref || (set_mine && !close_rgb(&mine.rgba[i * 4], &ref.rgba[i * 4], 3))) {
                CHECK_EQ(i, size_t(~0));
                std::printf("  stream: %s\n", check::escape(s).c_str());
                return;
            }
        }
    }
    CHECK(compared > 1400);
}

// img2sixel's real output (libsixel's encoder) decodes to exactly what
// libsixel's decoder produces, and close to the source PNG.
// `max_error`: mean per-channel error allowed against the source over its
// opaque pixels (the encoder quantizes and dithers); < 0 skips that check
// (translucent sources, which img2sixel flattens).
void img2sixel_fixture(const char* six, const char* png, double max_error) {
    std::string s = ih::read_file(ih::fixture(six));
    CHECK(s.size() > 10);
    const size_t q = s.find('q');
    const size_t st = s.rfind("\x1b\\");
    CHECK(q != std::string::npos && st != std::string::npos);
    if (q == std::string::npos || st == std::string::npos) return;
    const std::string body = s.substr(q + 1, st - q - 1);
    Decoded mine = ours(body, 0);
    Decoded ref;
    std::vector<int> index;
    CHECK(reference(body, ref, index));
    CHECK_EQ(mine.w, ref.w);
    CHECK_EQ(mine.h, ref.h);
    if (mine.w != ref.w || mine.h != ref.h) return;
    size_t diff = 0;
    for (size_t i = 0; i < size_t(mine.w) * mine.h; ++i)
        if (index[i] != 255 && !close_rgb(&mine.rgba[i * 4], &ref.rgba[i * 4], 0)) ++diff;
    CHECK_EQ(diff, size_t(0));
    ih::Rgba src;
    CHECK(ih::load_png_file(ih::fixture(png), src));
    CHECK_EQ(mine.w, src.w);
    CHECK_EQ(mine.h, src.h);
    if (mine.w != src.w || mine.h != src.h) return;
    double err = 0;
    size_t n = 0;
    for (size_t i = 0; i < size_t(src.w) * src.h; ++i) {
        if (src.px[i * 4 + 3] < 255) continue;
        for (int c = 0; c < 3; ++c) err += std::abs(int(mine.rgba[i * 4 + size_t(c)]) - int(src.px[i * 4 + size_t(c)]));
        n += 3;
    }
    if (max_error < 0) return;
    std::printf("  %s: mean error against the source %.2f / 255\n", six, n ? err / double(n) : -1.0);
    CHECK(n > 0 && err / double(n) < max_error);
}

void fuzz() {
    std::mt19937 rng(77);
    const std::string seed = ih::read_file(ih::fixture("a16.six"));
    for (int iter = 0; iter < 3000; ++iter) {
        std::string s;
        if (iter % 2) {
            s = seed;
            for (int m = int(rng() % 20); m >= 0 && !s.empty(); --m) s[rng() % s.size()] = char(rng() % 128);
        } else {
            static constexpr char kAlphabet[] = "0123456789;#!\"$-?~@ABCxyz\x1b\x18";
            for (int k = int(rng() % 400); k > 0; --k) s += kAlphabet[rng() % (sizeof kAlphabet - 1)];
        }
        SixelDecoder d(int(rng() % 3) - 1, int(rng() % 3) - 1, -1, 1 + rng() % 300, 1 + rng() % 300,
                       SixelPalette::standard());
        for (size_t i = 0; i < s.size(); i += 1 + rng() % 50) d.feed(std::string_view(s).substr(i, 1 + rng() % 50));
        std::vector<uint8_t> px;
        d.to_rgba(px);
        if (px.size() != size_t(d.width()) * d.height() * 4) CHECK_EQ(iter, -1);
    }
    // Through the terminal: DCS strings cut by CAN / ESC / C1 at any point.
    ih::IT t(30, 8);
    for (int iter = 0; iter < 2000; ++iter) {
        std::string s = "\x1bP" + std::to_string(rng() % 10) + ";" + std::to_string(rng() % 3) + "q";
        for (int k = int(rng() % 200); k > 0; --k) s += "0123456789;#!\"$-?~@\r\n"[rng() % 21];
        s += rng() % 4 ? "\x1b\\" : "\x18";
        if (rng() % 5 == 0) s += "text\r\n";
        t << s;
    }
    CHECK(t.t.image_bytes() <= t.t.graphics_options().storage_limit);
    TerminalView view(t.t);
    CHECK(view.snapshot() != nullptr);
}

// A 20 x 45 px image at 10 x 20 px cells: 2 columns, 3 rows.
std::string sixel_20x45() {
    std::string s = "\x1bPq\"1;1;20;45#1;2;100;0;0";
    for (int band = 0; band < 7; ++band) s += "!20~-";
    return s + "!20F\x1b\\";  // the last band's top 3 rows: 45 px
}

void terminal_placement() {
    ih::IT t(20, 6);
    // Sixel is advertised in DA1.
    t << "\x1b[c";
    CHECK_EQ(t.reply(), std::string("\x1b[?62;4;22;52c"));
    t << "\x1b[2;3H" << sixel_20x45();
    const ImageLayer& L = t.t.images();
    CHECK_EQ(L.cell_image_count(), size_t(1));
    // Cells (1..3, 2..3) hold the image; the cursor ends on its last row at
    // its left column (xterm).
    for (int y = 1; y <= 3; ++y)
        for (int x = 2; x <= 3; ++x) CHECK_EQ(t.t.row(y).cells[x].cp(), kImagePlaceholder);
    CHECK_EQ(t.t.row(1).cells[4].cp(), char32_t(0));
    CHECK_EQ(t.t.cursor().row, 3);
    CHECK_EQ(t.t.cursor().col, 2);
    // Frame: one run per row, slices of the image.
    TerminalView view(t.t);
    auto f = view.snapshot();
    CHECK_EQ(f->images.size(), size_t(3));
    if (f->images.size() == 3) {
        for (int r = 0; r < 3; ++r) {
            const FrameImage& im = f->images[size_t(r)];
            CHECK_EQ(im.plane, ImagePlane::Text);
            CHECK_EQ(im.source, ImageSource::Sixel);
            CHECK_EQ(im.x, 2.0f);
            CHECK_EQ(im.y, float(1 + r));
            CHECK_EQ(im.w, 2.0f);
            CHECK_EQ(im.src_y, float(r * 20));
            CHECK_EQ(im.src_h, r == 2 ? 5.0f : 20.0f);  // the last row is partial
            CHECK_EQ(im.h, r == 2 ? 0.25f : 1.0f);
            CHECK_EQ(im.pixels->width, 20u);
            CHECK_EQ(im.pixels->height, 45u);
        }
    }
    // Text written over part of the image replaces it there.
    t << "\x1b[3;4HX";
    f = view.snapshot();
    CHECK_EQ(f->images.size(), size_t(3));
    if (f->images.size() == 3) CHECK_EQ(f->images[1].w, 1.0f);
    CHECK(f->damage[2] == 1);
    // ?8452: the cursor ends to the right of the image.
    t << "\x1b[?8452h\x1b[1;1H" << sixel_20x45();
    CHECK_EQ(t.t.cursor().row, 2);
    CHECK_EQ(t.t.cursor().col, 2);
    t << "\x1b[1;19H" << sixel_20x45();  // past the right margin: next line, left margin
    CHECK_EQ(t.t.cursor().col, 0);
    CHECK_EQ(t.t.cursor().row, 3);
    t << "\x1b[?8452l";
    // Erasing the display removes image cells; their images are freed when
    // the next image is drawn.
    t << "\x1b[2J" << "\x1b[1;1H" << sixel_20x45();
    CHECK_EQ(L.cell_image_count(), size_t(1));
}

void terminal_scrolling() {
    ih::IT t(20, 4);
    t << "\x1b[4;1H" << sixel_20x45();  // 3 rows from the last row: scrolls 2
    CHECK_EQ(t.t.history_rows(), size_t(2));
    CHECK_EQ(t.t.cursor().row, 3);
    CHECK_EQ(t.t.row(1).cells[0].cp(), kImagePlaceholder);
    CHECK_EQ(t.t.row(3).cells[0].cp(), kImagePlaceholder);
    // A taller image than the screen: its top goes into history with it.
    t << "\x1b[1;1H\x1bPq#1;2;0;100;0";
    for (int band = 0; band < 20; ++band) t << "!10~-";
    t << "\x1b\\";  // 120 px = 6 rows
    TerminalView view(t.t);
    view.scroll_to_row(t.t.first_row());
    auto f = view.snapshot();
    CHECK(!f->images.empty());
    // DECSDM (?80): drawn at the top-left, no scrolling, the cursor stays.
    ih::IT d(20, 4);
    d << "\x1b[?80h\x1b[3;5H" << sixel_20x45();
    CHECK_EQ(d.t.history_rows(), size_t(0));
    CHECK_EQ(d.t.cursor().row, 2);
    CHECK_EQ(d.t.cursor().col, 4);
    CHECK_EQ(d.t.row(0).cells[0].cp(), kImagePlaceholder);
    d << "\x1b[?80$p";
    CHECK_EQ(d.reply(), std::string("\x1b[?80;1$y"));
    // Inside a scroll region, rows scroll within it.
    ih::IT r(20, 6);
    r << "\x1b[2;4r\x1b[4;1H" << sixel_20x45();
    CHECK_EQ(r.t.history_rows(), size_t(0));
    CHECK_EQ(r.t.cursor().row, 3);
}

void terminal_palette() {
    // ?1070 reset: registers persist from one image to the next.
    ih::IT t(20, 6);
    t << "\x1b[?1070l\x1bPq#3;2;0;0;100~\x1b\\\x1b[1;5H\x1bPq#3~\x1b\\";
    const ImageLayer& L = t.t.images();
    std::vector<const Image*> imgs;
    L.for_each_image([&](const Image& i) { imgs.push_back(&i); });
    CHECK_EQ(imgs.size(), size_t(2));
    for (const Image* i : imgs) CHECK_EQ(int(i->pixels()->rgba[2]), 255);
    // ?1070 set (the default): each image starts from the default palette.
    t << "\x1b[?1070h\x1b[1;9H\x1bPq#3~\x1b\\";
    bool green = false;
    L.for_each_image([&](const Image& i) {
        const auto& p = i.pixels()->rgba;
        if (p[0] == 0x33 && p[1] == 0xCC && p[2] == 0x33) green = true;  // VT340 register 3
    });
    CHECK(green);
    t << "\x1b[?1070$p";
    CHECK_EQ(t.reply(), std::string("\x1b[?1070;1$y"));
}

void xtsmgraphics() {
    ih::IT t(80, 24);
    t << "\x1b[?1;1S";
    CHECK_EQ(t.reply(), std::string("\x1b[?1;0;1024S"));
    t << "\x1b[?1;4S";
    CHECK_EQ(t.reply(), std::string("\x1b[?1;0;1024S"));
    t << "\x1b[?2;1S";  // the screen, at 10 x 20 px cells
    CHECK_EQ(t.reply(), std::string("\x1b[?2;0;800;480S"));
    t << "\x1b[?2;4S";
    CHECK_EQ(t.reply(), std::string("\x1b[?2;0;4096;4096S"));
    t << "\x1b[?3;1S";
    CHECK_EQ(t.reply(), std::string("\x1b[?3;1;0S"));
    t << "\x1b[?1;9S";
    CHECK_EQ(t.reply(), std::string("\x1b[?1;2;0S"));
    // Sixel off: no DA1 attribute, no XTSMGRAPHICS, DCS q ignored.
    TerminalOptions o = th::opts(80, 24);
    o.graphics.sixel = false;
    ih::IT n(o);
    n << "\x1b[c\x1b[?1;1S" << sixel_20x45();
    CHECK_EQ(n.reply(), std::string("\x1b[?62;22;52c"));
    CHECK_EQ(n.t.images().cell_image_count(), size_t(0));
}

} // namespace

int main() {
    init_test();
    palette_and_colors();
    raster_and_background();
    limits();
    differential();
    img2sixel_fixture("a.six", "src_a.png", 10.0);
    img2sixel_fixture("b.six", "src_b.png", -1);
    img2sixel_fixture("a16.six", "src_a.png", 24.0);  // 16 colors
    fuzz();
    terminal_placement();
    terminal_scrolling();
    terminal_palette();
    xtsmgraphics();
    return check::finish("test_sixel");
}
