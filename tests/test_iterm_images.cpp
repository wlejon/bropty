// iTerm2 inline images (OSC 1337 File= / MultipartFile=): argument parsing,
// sizing (cells, px, %, auto, preserveAspectRatio), cursor placement,
// animated GIFs, limits, and a fuzzed parser.
#include "image_helpers.h"

#include "bropty/view.h"

#include <random>

using namespace bropty;
using ih::IT;

namespace {

std::string osc(std::string_view args, std::string_view file) {
    return "\x1b]1337;File=" + std::string(args) + ":" + ih::b64(file) + "\x07";
}

const Image* only_image(const Terminal& t) {
    const Image* found = nullptr;
    t.images().for_each_image([&](const Image& i) { found = &i; });
    return t.images().cell_image_count() == 1 ? found : nullptr;
}

void sizing() {
    const std::string png = ih::encode_png(ih::pattern(40, 30, 1));  // 4 x 1.5 cells natural
    struct Case {
        const char* args;
        float cols, rows;
    } cases[] = {
        {"inline=1", 4.0f, 1.5f},
        {"inline=1;width=8", 8.0f, 3.0f},                      // cells; height follows the aspect
        {"inline=1;height=3", 8.0f, 3.0f},
        {"inline=1;width=100px", 10.0f, 3.75f},
        {"inline=1;width=50%", 20.0f, 7.5f},                   // of 40 columns
        {"inline=1;width=2;height=3", 2.0f, 0.75f},            // fitted inside, aspect kept
        {"inline=1;width=8;height=1;preserveAspectRatio=0", 8.0f, 1.0f},
        {"inline=1;width=auto;height=auto", 4.0f, 1.5f},
        {"inline=1;width=junk", 4.0f, 1.5f},
        {"name=Zm9vLnBuZw==;size=999;inline=1;width=2.5", 2.5f, 0.9375f},
    };
    for (const Case& c : cases) {
        IT t(40, 20);
        t << osc(c.args, png);
        const Image* img = only_image(t.t);
        CHECK(img != nullptr);
        if (!img) continue;
        CHECK_EQ(img->source, ImageSource::Iterm2);
        if (img->cell_width != c.cols || img->cell_height != c.rows) {
            std::printf("  %s: %g x %g, want %g x %g\n", c.args, double(img->cell_width), double(img->cell_height),
                        double(c.cols), double(c.rows));
            CHECK(false);
        }
        CHECK_EQ(img->width, 40u);  // pixels stay native; the renderer scales
    }
    // Wider than the screen at its natural size: shrunk to fit (iTerm2).
    IT t(10, 10);
    t << osc("inline=1", ih::encode_png(ih::pattern(200, 50, 2)));
    const Image* img = only_image(t.t);
    CHECK(img && img->cell_width == 10.0f && img->cell_height == 1.25f);
}

void placement_and_cursor() {
    IT t(40, 6);
    const std::string png = ih::encode_png(ih::pattern(40, 30, 3));  // 4 x 2 cells box
    t << "\x1b[2;3H" << osc("inline=1", png);
    // Cells (1..2, 2..5); the cursor ends on the image's last row, after it.
    CHECK_EQ(t.t.row(1).cells[2].cp(), kImagePlaceholder);
    CHECK_EQ(t.t.row(2).cells[5].cp(), kImagePlaceholder);
    CHECK_EQ(t.t.row(1).cells[6].cp(), char32_t(0));
    CHECK_EQ(t.t.cursor().row, 2);
    CHECK_EQ(t.t.cursor().col, 6);
    // doNotMoveCursor (WezTerm): the cursor stays.
    t << "\x1b[1;20H" << osc("inline=1;doNotMoveCursor=1", png);
    CHECK_EQ(t.t.cursor().row, 0);
    CHECK_EQ(t.t.cursor().col, 19);
    // inline=0 is a download: nothing shown.
    const size_t n = t.t.images().cell_image_count();
    t << osc("inline=0", png) << osc("", png);
    CHECK_EQ(t.t.images().cell_image_count(), n);
    // At the bottom the screen scrolls; the frame shows the image scaled.
    t << "\x1b[6;1H" << osc("inline=1;width=8", png);  // 8 x 3 cells
    CHECK_EQ(t.t.cursor().row, 5);
    CHECK_EQ(t.t.history_rows(), size_t(2));
    TerminalView view(t.t);
    auto f = view.snapshot();
    float covered = 0;
    for (const FrameImage& im : f->images)
        if (im.source == ImageSource::Iterm2 && im.pixels->width == 40 && im.w == 8.0f) covered += im.h;
    CHECK_EQ(covered, 3.0f);
    // Undecodable data (no host decoder, or garbage) shows nothing.
    IT u(40, 6);
    u.host.refuse = true;
    u << osc("inline=1", png);
    CHECK_EQ(u.t.images().cell_image_count(), size_t(0));
    IT v(40, 6);
    v << osc("inline=1", "not an image") << "\x1b]1337;File=inline=1:!!!!\x07";
    CHECK_EQ(v.t.images().cell_image_count(), size_t(0));
    // iTerm2 off: OSC 1337 ignored.
    TerminalOptions o = th::opts(40, 6);
    o.graphics.iterm2 = false;
    IT w(o);
    w << osc("inline=1", png);
    CHECK_EQ(w.host.decodes, 0);
}

void multipart() {
    IT t(40, 6);
    const ih::Rgba src = ih::pattern(30, 20, 4);
    const std::string enc = ih::b64(ih::encode_png(src));
    t << "\x1b]1337;MultipartFile=inline=1;width=3\x07";
    for (size_t i = 0; i < enc.size(); i += 100) t << "\x1b]1337;FilePart=" + enc.substr(i, 100) + "\x07";
    CHECK_EQ(t.t.images().cell_image_count(), size_t(0));
    t << "\x1b]1337;FileEnd\x07";
    const Image* img = only_image(t.t);
    CHECK(img && ih::same_pixels(*img->pixels(), src));
    CHECK(img && img->cell_width == 3.0f);
    // FilePart / FileEnd without a MultipartFile: ignored.
    t << "\x1b]1337;FilePart=AAAA\x07\x1b]1337;FileEnd\x07";
    CHECK_EQ(t.t.images().cell_image_count(), size_t(1));
    // Over the transmission cap: dropped.
    TerminalOptions o = th::opts(40, 6);
    o.graphics.max_transmission_bytes = 50;
    CHECK(enc.size() > 4 * 50);
    IT u(o);
    u << "\x1b]1337;MultipartFile=inline=1\x07";
    for (size_t i = 0; i < enc.size(); i += 100) u << "\x1b]1337;FilePart=" + enc.substr(i, 100) + "\x07";
    u << "\x1b]1337;FileEnd\x07";
    CHECK_EQ(u.t.images().cell_image_count(), size_t(0));
    u << "\x1b]1337;MultipartFile=inline=1;size=999999\x07\x1b]1337;FilePart=AAAA\x07\x1b]1337;FileEnd\x07";
    CHECK_EQ(u.t.images().cell_image_count(), size_t(0));
}

void animated_gif() {
    IT t(40, 6);
    t << osc("inline=1", ih::read_file(ih::fixture("anim.gif")));
    const Image* img = only_image(t.t);
    CHECK(img != nullptr);
    if (!img) return;
    CHECK_EQ(img->frames.size(), size_t(3));
    CHECK_EQ(img->animation, AnimationState::Running);
    CHECK_EQ(img->frames[0].gap_ms, 70u);  // GIF delay 7 (1/100 s)
    CHECK_EQ(int(img->frames[0].pixels->rgba[0]), 255);  // red
    CHECK_EQ(int(img->frames[1].pixels->rgba[1]), 255);  // lime
    CHECK_EQ(t.t.advance_animations(0), uint64_t(70));
    t.t.advance_animations(70);
    CHECK_EQ(img->current_frame, 1u);
    TerminalView view(t.t);
    auto f = view.snapshot();
    CHECK(!f->images.empty() && f->images[0].pixels == img->frames[1].pixels);
    t.t.advance_animations(140);
    t.t.advance_animations(210);
    CHECK_EQ(img->current_frame, 0u);  // loops
}

void fuzz() {
    std::mt19937 rng(3);
    IT t(30, 8);
    const std::string png = ih::encode_png(ih::pattern(12, 9, 5));
    const std::string enc = ih::b64(png);
    const char* keys[] = {"inline", "width", "height", "preserveAspectRatio", "size", "name", "doNotMoveCursor", "x"};
    const char* vals[] = {"1", "0", "auto", "50%", "12px", "3", "-4", "99999999999", "", "1.5", "%", "px", "1e9"};
    for (int iter = 0; iter < 3000; ++iter) {
        std::string args;
        for (int k = int(rng() % 6); k > 0; --k) {
            args += keys[rng() % 8];
            if (rng() % 8) args += std::string("=") + vals[rng() % 13];
            args += ';';
        }
        std::string payload = enc;
        if (rng() % 3 == 0)
            for (int m = int(rng() % 5); m >= 0; --m) payload[rng() % payload.size()] = char(32 + rng() % 95);
        switch (rng() % 4) {
        case 0: t << "\x1b]1337;File=" + args + ":" + payload + "\x07"; break;
        case 1: t << "\x1b]1337;MultipartFile=" + args + "\x07"; break;
        case 2: t << "\x1b]1337;FilePart=" + payload.substr(0, rng() % payload.size()) + "\x1b\\"; break;
        default: t << "\x1b]1337;FileEnd\x07";
        }
        if (rng() % 10 == 0) t << "\r\n";
    }
    CHECK(t.t.image_bytes() <= t.t.graphics_options().storage_limit);
    TerminalView view(t.t);
    CHECK(view.snapshot() != nullptr);
}

} // namespace

int main() {
    init_test();
    sizing();
    placement_and_cursor();
    multipart();
    animated_gif();
    fuzz();
    return check::finish("test_iterm_images");
}
