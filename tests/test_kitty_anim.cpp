// The kitty graphics protocol's animation (frames a=f, control a=a,
// composition a=c, frame deletion) and Unicode placeholders (U+10EEEE with
// row / column diacritics, as tmux / remote programs use them).
#include "image_helpers.h"

#include "bropty/view.h"
#include "placeholder.h"

#include <array>

using namespace bropty;
using ih::IT;
using ih::kitty;

namespace {

std::string solid(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    std::string s;
    for (uint32_t i = 0; i < w * h; ++i) {
        s.push_back(char(r));
        s.push_back(char(g));
        s.push_back(char(b));
        s.push_back(char(a));
    }
    return s;
}

std::array<int, 4> px(const Image& img, uint32_t frame, uint32_t x, uint32_t y) {
    const uint8_t* p = img.frames[frame].pixels->rgba.data() + (size_t(y) * img.width + x) * 4;
    return {p[0], p[1], p[2], p[3]};
}

// Within 1 per channel: blending works in floats, as kitty's does.
bool is(const std::array<int, 4>& got, int r, int g, int b, int a) {
    const int want[4] = {r, g, b, a};
    for (int i = 0; i < 4; ++i)
        if (got[size_t(i)] < want[i] - 1 || got[size_t(i)] > want[i] + 1) return false;
    return true;
}

void frames() {
    IT t(80, 24);
    t << kitty("a=t,i=1,s=4,v=4", solid(4, 4, 255, 0, 0));
    t.reply();
    // A new frame: transparent canvas (no c, no Y), data composed at (1, 1).
    t << kitty("a=f,i=1,s=2,v=2,x=1,y=1", solid(2, 2, 0, 0, 255));
    CHECK_EQ(t.reply(), std::string("\x1b_Gi=1,r=2;OK\x1b\\"));
    const Image* img = t.t.images().find(1);
    CHECK(img && img->frames.size() == 2);
    if (!img || img->frames.size() != 2) return;
    CHECK(is(px(*img, 1, 0, 0), 0, 0, 0, 0));
    CHECK(is(px(*img, 1, 1, 1), 0, 0, 255, 255));
    CHECK(is(px(*img, 1, 3, 3), 0, 0, 0, 0));
    CHECK_EQ(img->frames[0].gap_ms, 0u);  // the root frame starts gapless
    CHECK_EQ(img->frames[1].gap_ms, 40u);
    // Base frame c=1, data at the origin; explicit gap.
    t << kitty("a=f,i=1,c=1,s=1,v=1,z=70,q=1", solid(1, 1, 0, 255, 0));
    CHECK(img->frames.size() == 3 && is(px(*img, 2, 0, 0), 0, 255, 0, 255) && is(px(*img, 2, 1, 0), 255, 0, 0, 255));
    CHECK_EQ(img->frames[2].gap_ms, 70u);
    // Background color Y (0xRRGGBBAA), gapless frame (z < 0).
    t << kitty("a=f,i=1,Y=16711935,s=1,v=1,z=-1,q=1", solid(1, 1, 9, 9, 9));  // 0x00ff00ff
    CHECK(img->frames.size() == 4 && is(px(*img, 3, 3, 3), 0, 255, 0, 255));
    CHECK_EQ(img->frames[3].gap_ms, 0u);
    // Alpha blending (the default) and replacement (X=1), editing frame 2.
    t << kitty("a=f,i=1,r=2,s=1,v=1,x=1,y=1,q=1", solid(1, 1, 255, 255, 255, 128));
    const auto blended = px(*img, 1, 1, 1);
    CHECK(is(blended, 128, 128, 255, 255));
    t << kitty("a=f,i=1,r=2,s=1,v=1,x=2,y=2,X=1,q=1", solid(1, 1, 255, 255, 255, 128));
    CHECK(is(px(*img, 1, 2, 2), 255, 255, 255, 128));
    // RGB data is opaque: no blending.
    t << kitty("a=f,i=1,r=2,f=24,s=1,v=1,q=1", std::string("\x10\x20\x30", 3));
    CHECK(is(px(*img, 1, 0, 0), 16, 32, 48, 255));
    // Errors.
    t << kitty("a=f,i=1,s=5,v=1", solid(5, 1, 1, 1, 1));
    CHECK(t.reply().find("EINVAL") != std::string::npos);
    t << kitty("a=f,i=1,c=9,s=1,v=1", solid(1, 1, 1, 1, 1));
    CHECK(t.reply().find("EINVAL") != std::string::npos);
    t << kitty("a=f,i=77,s=1,v=1", solid(1, 1, 1, 1, 1));
    CHECK(t.reply().rfind("\x1b_Gi=77;ENOENT", 0) == 0);
    // Pixels are new buffers, never edited in place (renderers hold them).
    const ImagePixelsPtr before = img->frames[1].pixels;
    t << kitty("a=f,i=1,r=2,s=1,v=1,q=1", solid(1, 1, 1, 2, 3));
    CHECK(before != img->frames[1].pixels);
    CHECK(is(std::array<int, 4>{before->rgba[0], before->rgba[1], before->rgba[2], before->rgba[3]}, 16, 32, 48,
             255));
}

void control() {
    IT t(80, 24);
    t << kitty("a=T,i=1,s=1,v=1,q=2", solid(1, 1, 1, 0, 0));
    t << kitty("a=f,i=1,s=1,v=1,q=2", solid(1, 1, 2, 0, 0));
    t << kitty("a=f,i=1,s=1,v=1,z=-1,q=2", solid(1, 1, 3, 0, 0));  // gapless
    t << kitty("a=f,i=1,s=1,v=1,z=60,q=2", solid(1, 1, 4, 0, 0));
    const Image* img = t.t.images().find(1);
    CHECK(img && img->frames.size() == 4);
    if (!img) return;
    // The root frame has no gap until one is set.
    t << kitty("a=a,i=1,r=1,z=100,q=2");
    CHECK_EQ(img->frames[0].gap_ms, 100u);
    CHECK(t.t.advance_animations(0) == UINT64_MAX);  // stopped
    t << kitty("a=a,i=1,s=3,v=2,q=2");  // run; v=2: one loop
    CHECK_EQ(img->animation, AnimationState::Running);
    CHECK_EQ(img->max_loops, 1u);
    TerminalView view(t.t);
    auto f0 = view.snapshot();
    const uint64_t changes = t.t.change_count();
    CHECK_EQ(t.t.advance_animations(50), uint64_t(50));
    CHECK_EQ(img->current_frame, 0u);
    CHECK_EQ(t.t.change_count(), changes);
    CHECK_EQ(t.t.advance_animations(100), uint64_t(40));  // frame 2 (40 ms)
    CHECK_EQ(img->current_frame, 1u);
    CHECK(t.t.change_count() != changes);
    auto f1 = view.snapshot();
    CHECK(!f1->images.empty() && !f0->images.empty() && f1->images[0].pixels != f0->images[0].pixels);
    CHECK(f1->damage[0] == 1);
    CHECK_EQ(t.t.advance_animations(140), uint64_t(60));  // frame 3 is gapless: straight to 4
    CHECK_EQ(img->current_frame, 3u);
    // The end of the only loop: the last frame stays.
    t.t.advance_animations(200);
    CHECK_EQ(img->current_frame, 3u);
    CHECK(t.t.advance_animations(1000) == UINT64_MAX);
    // Client-driven: c= picks the current frame.
    t << kitty("a=a,i=1,c=2,q=2");
    CHECK_EQ(img->current_frame, 1u);
    // Loading mode waits at the last frame for more.
    t << kitty("a=a,i=1,s=1,q=2") << kitty("a=a,i=1,c=4,s=2,v=1,q=2");
    t.t.advance_animations(5000);
    t.t.advance_animations(5100);
    CHECK_EQ(img->current_frame, 3u);
    t << kitty("a=f,i=1,s=1,v=1,z=30,q=2", solid(1, 1, 5, 0, 0));
    t.t.advance_animations(5200);
    CHECK_EQ(img->current_frame, 4u);
    // Stop.
    t << kitty("a=a,i=1,s=1,q=2");
    CHECK(t.t.advance_animations(9000) == UINT64_MAX);
    // Deleting frames: d=f removes frame r (clamped), F also frees a
    // single-frame image.
    t << kitty("a=d,d=f,i=1,r=2");
    CHECK_EQ(img->frames.size(), size_t(4));
    CHECK_EQ(img->frames[1].pixels->rgba[0], uint8_t(3));
    t << kitty("a=d,d=f,i=1,r=99");
    CHECK_EQ(img->frames.size(), size_t(3));
    t << kitty("a=d,d=f,i=1") << kitty("a=d,d=f,i=1");  // r=0: the root frame
    CHECK_EQ(img->frames.size(), size_t(1));
    t << kitty("a=d,d=f,i=1");
    CHECK(t.t.images().find(1) != nullptr);
    t << kitty("a=d,d=F,i=1");
    CHECK(t.t.images().find(1) == nullptr);
    // Frame count cap.
    TerminalOptions o = th::opts(80, 24);
    o.graphics.max_frames = 3;
    IT u(o);
    u << kitty("a=t,i=1,s=1,v=1,q=2", solid(1, 1, 1, 0, 0));
    for (int i = 0; i < 4; ++i) u << kitty("a=f,i=1,s=1,v=1", solid(1, 1, 1, 0, 0));
    CHECK_EQ(u.t.images().find(1)->frames.size(), size_t(3));
    CHECK(u.reply().find("ENOSPC") != std::string::npos);
}

void compose() {
    IT t(80, 24);
    std::string root = solid(4, 2, 255, 0, 0);
    t << kitty("a=t,i=1,s=4,v=2,q=2", root);
    t << kitty("a=f,i=1,s=4,v=2,q=2", solid(4, 2, 0, 0, 255, 128));
    const Image* img = t.t.images().find(1);
    CHECK(img && img->frames.size() == 2);
    if (!img) return;
    // Copy a 2x1 rectangle of frame 2 at (0, 0) onto frame 1 at (2, 1), blending.
    t << kitty("a=c,i=1,r=2,c=1,w=2,h=1,x=2,y=1");
    CHECK_EQ(t.reply(), std::string("\x1b_Gi=1;OK\x1b\\"));
    CHECK(is(px(*img, 0, 2, 1), 127, 0, 128, 255));
    CHECK(is(px(*img, 0, 1, 1), 255, 0, 0, 255));
    // C=1: replace.
    t << kitty("a=c,i=1,r=2,c=1,w=1,h=1,C=1,q=1");
    CHECK(is(px(*img, 0, 0, 0), 0, 0, 255, 128));
    // Errors: missing frames, out of bounds, overlap within one frame.
    t << kitty("a=c,i=1,r=3,c=1");
    CHECK(t.reply().find("ENOENT") != std::string::npos);
    t << kitty("a=c,i=1,r=1,c=2,w=3,x=2");
    CHECK(t.reply().find("EINVAL") != std::string::npos);
    t << kitty("a=c,i=1,r=1,c=2,w=3,X=2");
    CHECK(t.reply().find("EINVAL") != std::string::npos);
    t << kitty("a=c,i=1,r=1,c=1,w=2,h=1,x=1");
    CHECK(t.reply().find("EINVAL") != std::string::npos);
    t << kitty("a=c,i=1,r=1,c=1,w=2,h=1,x=2");
    CHECK_EQ(t.reply(), std::string("\x1b_Gi=1;OK\x1b\\"));
}

std::string utf8(char32_t c) {
    std::string s;
    append_utf8(s, c);
    return s;
}
std::string ph(int row = -1, int col = -1, int hi = -1) {
    std::string s = utf8(kImagePlaceholder);
    if (row >= 0) s += utf8(detail::kitty_diacritic(uint32_t(row + 1)));
    if (col >= 0) s += utf8(detail::kitty_diacritic(uint32_t(col + 1)));
    if (hi >= 0) s += utf8(detail::kitty_diacritic(uint32_t(hi + 1)));
    return s;
}

void placeholders() {
    IT t(40, 10);
    // 20x40 image, a virtual placement with a 4x2 cell box: fitted to the
    // height (scale 1), centred with 10 px on each side.
    t << kitty("a=t,i=42,s=20,v=40,q=2", solid(20, 40, 9, 9, 9));
    t << kitty("a=p,U=1,i=42,c=4,r=2,q=2");
    CHECK_EQ(t.t.cursor().col, 0);  // virtual placements never move the cursor
    t << "\x1b[38;5;42m" << ph(0, 0) << ph() << ph() << ph() << "\r\n";
    t << ph(1, 0) << ph() << ph(1) << ph(1, 3) << "\x1b[m";
    CHECK(t.t.may_have_image_cells());
    CHECK_EQ(t.t.row(0).cells[1].cp(), kImagePlaceholder);
    CHECK_EQ(t.t.row(0).cells[4].cp(), char32_t(0));  // one column each
    TerminalView view(t.t);
    auto f = view.snapshot();
    CHECK_EQ(f->images.size(), size_t(2));
    if (f->images.size() == 2) {
        for (int r = 0; r < 2; ++r) {
            const FrameImage& im = f->images[size_t(r)];
            CHECK_EQ(im.plane, ImagePlane::Text);
            CHECK_EQ(im.image_id, 42u);
            CHECK_EQ(im.x, 1.0f);
            CHECK_EQ(im.w, 2.0f);
            CHECK_EQ(im.y, float(r));
            CHECK_EQ(im.h, 1.0f);
            CHECK_EQ(im.src_x, 0.0f);
            CHECK_EQ(im.src_w, 20.0f);
            CHECK_EQ(im.src_y, float(r * 20));
            CHECK_EQ(im.src_h, 20.0f);
        }
    }
    // A run breaks where the column does not follow; a new run starts.
    t << "\x1b[5;1H\x1b[38;5;42m" << ph(0, 2) << ph(0, 1) << "\x1b[m";
    f = view.snapshot();
    CHECK_EQ(f->images.size(), size_t(4));
    // Images and placements chosen by color: 24-bit id with the high byte in
    // the third diacritic, placement id in the underline color.
    t << kitty("a=t,i=16909060,s=10,v=20,q=2", solid(10, 20, 1, 2, 3));  // 0x01020304
    t << kitty("a=p,U=1,i=16909060,p=7,q=2") << kitty("a=p,U=1,i=16909060,p=8,c=2,r=1,q=2");
    t << "\x1b[7;1H\x1b[38;2;2;3;4m\x1b[58;5;7m" << ph(0, 0, 1) << "\x1b[58;5;8m" << ph(0, 0, 1) << ph() << "\x1b[m";
    f = view.snapshot();
    int found7 = 0, found8 = 0;
    for (const FrameImage& im : f->images) {
        if (im.image_id != 16909060u) continue;
        if (im.placement_id == 7) {
            ++found7;
            CHECK_EQ(im.w, 1.0f);
        }
        if (im.placement_id == 8) {
            ++found8;
            CHECK_EQ(im.w, 1.0f);  // 2x1 box: 10x20 image fitted to height = 1 cell, centred
            CHECK_EQ(im.x, 1.5f);
        }
    }
    CHECK_EQ(found7, 1);
    CHECK_EQ(found8, 1);
    // No such image / no virtual placement: nothing drawn.
    t << "\x1b[9;1H\x1b[38;5;99m" << ph(0, 0) << "\x1b[m";
    const size_t before = f->images.size();
    f = view.snapshot();
    CHECK_EQ(f->images.size(), before);
    // Placeholders are text: erasing removes the image cells.
    t << "\x1b[2J";
    f = view.snapshot();
    CHECK_EQ(f->images.size(), size_t(0));
}

} // namespace

int main() {
    init_test();
    frames();
    control();
    compose();
    placeholders();
    return check::finish("test_kitty_anim");
}
