// The kitty graphics protocol: transmission (direct, chunked, zlib, PNG
// through the host, files / temp files / shared memory under policy),
// replies (the spec's examples and kitty's rules for i / I / p / q), placement
// geometry and cursor movement, relative placements, deletion, quotas, and
// a fuzzed command parser. Animation is in test_kitty_anim.cpp.
#include "image_helpers.h"

#include "bropty/view.h"
#include "kitty_command.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace bropty;
using ih::IT;
using ih::kitty;

namespace {

std::string raw(const ih::Rgba& img, bool rgb = false) {
    if (!rgb) return std::string(reinterpret_cast<const char*>(img.px.data()), img.px.size());
    std::string s;
    for (size_t i = 0; i < img.px.size(); i += 4) s.append(reinterpret_cast<const char*>(&img.px[i]), 3);
    return s;
}

std::string dims(const ih::Rgba& img) { return ",s=" + std::to_string(img.w) + ",v=" + std::to_string(img.h); }

const Placement* only_placement(const Terminal& t) {
    return t.images().placements().size() == 1 ? &t.images().placements()[0] : nullptr;
}

void spec_replies() {
    IT t(80, 24);
    // The spec's query example: a 1x1 RGB image, answered OK, nothing stored.
    t << "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\";
    CHECK_EQ(t.reply(), std::string("\x1b_Gi=31;OK\x1b\\"));
    CHECK_EQ(t.t.images().image_count(), size_t(0));
    // Without i or I there is nothing to address a reply to.
    t << "\x1b_Ga=q,s=1,v=1,f=24;AAAA\x1b\\";
    CHECK_EQ(t.reply(), std::string(""));
    // Errors carry the code and message; q=1 hides OK only, q=2 everything.
    t << "\x1b_Gi=7,a=t,f=24,s=10,v=10;AAAA\x1b\\";
    CHECK(t.reply().rfind("\x1b_Gi=7;ENODATA:", 0) == 0);
    t << "\x1b_Gi=7,q=1,a=t,f=24,s=1,v=1;AAAA\x1b\\";
    CHECK_EQ(t.reply(), std::string(""));
    t << "\x1b_Gi=8,q=1,a=t,f=24,s=10,v=10;AAAA\x1b\\";
    CHECK(t.reply().rfind("\x1b_Gi=8;ENODATA", 0) == 0);
    t << "\x1b_Gi=8,q=2,a=t,f=24,s=10,v=10;AAAA\x1b\\";
    CHECK_EQ(t.reply(), std::string(""));
    // Both i and I: EINVAL.
    t << "\x1b_Gi=9,I=3,a=t,f=24,s=1,v=1;AAAA\x1b\\";
    CHECK(t.reply().rfind("\x1b_Gi=9,I=3;EINVAL", 0) == 0);
    // I= gets a fresh id, reported with the number.
    t << "\x1b_GI=13,a=t,f=24,s=1,v=1;AAAA\x1b\\";
    const std::string r = t.reply();
    CHECK(r.rfind("\x1b_Gi=", 0) == 0);
    CHECK(r.find(",I=13;OK\x1b\\") != std::string::npos);
    // a=T with a placement id echoes it.
    t << "\x1b_Gi=10,p=4,a=T,f=24,s=1,v=1;AAAA\x1b\\";
    CHECK_EQ(t.reply(), std::string("\x1b_Gi=10,p=4;OK\x1b\\"));
    // Put of a missing image.
    t << "\x1b_Ga=p,i=999\x1b\\";
    CHECK(t.reply().rfind("\x1b_Gi=999;ENOENT", 0) == 0);
    // Malformed control data.
    t << "\x1b_Gi=5,s=abc;AAAA\x1b\\";
    CHECK(t.reply().rfind("\x1b_Gi=5;EINVAL", 0) == 0);
    // Delete never replies.
    t << "\x1b_Ga=d,d=i,i=10\x1b\\";
    CHECK_EQ(t.reply(), std::string(""));
    // Other APCs still reach the host; G is never passed on.
    CHECK_EQ(t.host.apcs.size(), size_t(0));
}

void formats() {
    IT t(80, 24);
    const ih::Rgba img = ih::pattern(13, 7, 1, true);
    // RGBA (default f=32).
    t << kitty("a=t,i=1" + dims(img), raw(img));
    const Image* a = t.t.images().find(1);
    CHECK(a && ih::same_pixels(*a->pixels(), img));
    // RGB: alpha becomes opaque.
    t << kitty("a=t,i=2,f=24" + dims(img), raw(img, true));
    const Image* b = t.t.images().find(2);
    ih::Rgba opaque = img;
    for (size_t i = 3; i < opaque.px.size(); i += 4) opaque.px[i] = 255;
    CHECK(b && ih::same_pixels(*b->pixels(), opaque));
    // zlib.
    t << kitty("a=t,i=3,o=z" + dims(img), ih::zlib_compress(raw(img)));
    const Image* c = t.t.images().find(3);
    CHECK(c && ih::same_pixels(*c->pixels(), img));
    CHECK_EQ(t.reply(), std::string("\x1b_Gi=1;OK\x1b\\\x1b_Gi=2;OK\x1b\\\x1b_Gi=3;OK\x1b\\"));
    t << kitty("a=t,i=4,o=z" + dims(img), "not zlib");
    CHECK(t.reply().rfind("\x1b_Gi=4;EINVAL", 0) == 0);
    // PNG through the host's decoder.
    t << kitty("a=t,i=5,f=100", ih::encode_png(img));
    const Image* d = t.t.images().find(5);
    CHECK(d && ih::same_pixels(*d->pixels(), img));
    CHECK_EQ(t.host.decodes, 1);
    // zlib-compressed PNG.
    t << kitty("a=t,i=6,f=100,o=z", ih::zlib_compress(ih::encode_png(img)));
    CHECK(t.t.images().find(6) != nullptr);
    t.reply();
    // No host decoder: EBADPNG.
    t.host.refuse = true;
    t << kitty("a=t,i=7,f=100", ih::encode_png(img));
    CHECK(t.reply().rfind("\x1b_Gi=7;EBADPNG", 0) == 0);
    CHECK(t.t.images().find(7) == nullptr);
    // Unknown format / compression / medium.
    t << kitty("a=t,i=8,f=99,s=1,v=1", "abcd");
    CHECK(t.reply().rfind("\x1b_Gi=8;EINVAL", 0) == 0);
    t << kitty("a=t,i=8,o=x,s=1,v=1", "abcd");
    CHECK(t.reply().rfind("\x1b_Gi=8;EINVAL", 0) == 0);
    t << kitty("a=t,i=8,t=q,s=1,v=1", "abcd");
    CHECK(t.reply().rfind("\x1b_Gi=8;EINVAL", 0) == 0);
    // Too large dimensions.
    t << kitty("a=t,i=8,s=20000,v=1", "abcd");
    CHECK(t.reply().rfind("\x1b_Gi=8;EFBIG", 0) == 0);
    // Replacing an image with the same id drops its placements.
    t << kitty("a=p,i=1");
    CHECK_EQ(t.t.images().placements().size(), size_t(1));
    t << kitty("a=t,i=1" + dims(img), raw(img));
    CHECK_EQ(t.t.images().placements().size(), size_t(0));
    t.reply();
}

void chunked() {
    IT t(80, 24);
    const ih::Rgba img = ih::pattern(40, 30, 2, true);
    const std::string enc = ih::b64(ih::zlib_compress(raw(img)));
    std::string stream;
    for (size_t off = 0; off < enc.size(); off += 128) {
        const bool last = off + 128 >= enc.size();
        stream += "\x1b_G";
        stream += off == 0 ? "a=T,i=21,f=32,o=z" + dims(img) + ",m=1" : std::string(last ? "m=0" : "m=1");
        stream += ";" + enc.substr(off, 128) + "\x1b\\";
    }
    // Feed in odd pieces.
    for (size_t i = 0; i < stream.size(); i += 37) t << std::string_view(stream).substr(i, 37);
    const Image* a = t.t.images().find(21);
    CHECK(a && ih::same_pixels(*a->pixels(), img));
    CHECK_EQ(t.t.images().placements().size(), size_t(1));
    CHECK_EQ(t.reply(), std::string("\x1b_Gi=21;OK\x1b\\"));
    // A chunk may carry q (and repeat a=).
    t << "\x1b_Ga=t,i=22,f=24,s=1,v=1,m=1;AAAA\x1b\\\x1b_Gm=0,q=1;AAAA\x1b\\";
    CHECK_EQ(t.reply(), std::string(""));
    CHECK(t.t.images().find(22) != nullptr);
    // A command with other keys aborts the transfer in progress (a delete must).
    t << "\x1b_Ga=t,i=23,f=24,s=1,v=1,m=1;AA\x1b\\\x1b_Ga=d,d=A\x1b\\\x1b_Gm=0;AA\x1b\\";
    CHECK(t.t.images().find(23) == nullptr);
    // A transmission over the size cap fails with EFBIG.
    TerminalOptions o = th::opts(80, 24);
    o.graphics.max_transmission_bytes = 100;
    IT s(o);
    s << "\x1b_Ga=t,i=24,s=10,v=10,m=1;" + ih::b64(std::string(90, 'x')) + "\x1b\\";
    s << "\x1b_Gm=0;" + ih::b64(std::string(90, 'x')) + "\x1b\\";
    CHECK(s.reply().rfind("\x1b_Gi=24;EFBIG", 0) == 0);
    CHECK(s.t.images().find(24) == nullptr);
}

void placement_geometry() {
    IT t(80, 24);
    const ih::Rgba img = ih::pattern(25, 45, 3);  // 3 x 3 cells at 10 x 20
    t << "\x1b[3;5H" << kitty("a=T,i=1,q=2" + dims(img), raw(img));
    const Placement* p = only_placement(t.t);
    CHECK(p != nullptr);
    if (!p) return;
    CHECK_EQ(p->row, int64_t(2));
    CHECK_EQ(p->col, 4);
    CHECK_EQ(ImageLayer::extent(*p, 10, 20), std::make_pair(3, 3));
    // The cursor moves past it: right by its columns, down to its last row.
    CHECK_EQ(t.t.cursor().row, 4);
    CHECK_EQ(t.t.cursor().col, 7);
    const Placement first = *p;  // p dangles once more placements are added
    // C=1: the cursor stays.
    t << "\x1b[1;1H" << kitty("a=p,i=1,p=2,C=1");
    CHECK_EQ(t.t.cursor().row, 0);
    CHECK_EQ(t.t.cursor().col, 0);
    // Sizing in cells, source rectangles, offsets.
    t << kitty("a=p,i=1,p=2,c=10,r=4,x=5,y=6,w=10,h=20,X=3,Y=4,z=-5,C=1");
    CHECK_EQ(t.t.images().placements().size(), size_t(2));  // p=2 replaced in place
    const Placement* q = nullptr;
    for (const Placement& pl : t.t.images().placements())
        if (pl.placement_id == 2) q = &pl;
    CHECK(q != nullptr);
    if (q) {
        CHECK_EQ(q->src_x, 5u);
        CHECK_EQ(q->src_y, 6u);
        CHECK_EQ(q->src_w, 10u);
        CHECK_EQ(q->src_h, 20u);
        CHECK_EQ(q->x_offset, 3);
        CHECK_EQ(q->y_offset, 4);
        CHECK_EQ(q->z, -5);
        CHECK_EQ(ImageLayer::extent(*q, 10, 20), std::make_pair(10, 4));
    }
    // Only columns: rows follow the aspect ratio (kitty's update_dest_rect).
    Placement r = first;
    r.cols = 5;
    r.rows = 0;
    CHECK_EQ(ImageLayer::extent(r, 10, 20), std::make_pair(5, 5));  // ceil(50 * 45/25 / 20)
    // The frame shows them with kitty's geometry.
    TerminalView view(t.t);
    auto f = view.snapshot();
    CHECK_EQ(f->images.size(), size_t(2));
    if (f->images.size() == 2) {
        const FrameImage& below = f->images[0];  // z=-5 first
        CHECK_EQ(below.plane, ImagePlane::BelowText);
        CHECK_EQ(below.x, 0.3f);
        CHECK_EQ(below.y, 0.2f);
        CHECK_EQ(below.w, 9.7f);
        CHECK_EQ(below.h, 3.8f);
        CHECK_EQ(below.src_w, 10.0f);
        const FrameImage& above = f->images[1];
        CHECK_EQ(above.plane, ImagePlane::AboveText);
        CHECK_EQ(above.x, 4.0f);
        CHECK_EQ(above.y, 2.0f);
        CHECK_EQ(above.w, 2.5f);
        CHECK_EQ(above.h, 2.25f);
        CHECK(above.pixels == t.t.images().find(1)->pixels());
    }
    // z below INT32_MIN / 2: under the cell backgrounds.
    t << kitty("a=p,i=1,p=3,z=-1073741825,C=1");
    f = view.snapshot();
    CHECK(!f->images.empty() && f->images[0].plane == ImagePlane::BelowBackground);
}

void cursor_scrolls() {
    IT t(20, 5);
    const ih::Rgba img = ih::pattern(10, 60, 4);  // 1 x 3 cells
    t << "\x1b[5;1H" << kitty("a=T,i=1,q=2" + dims(img), raw(img));
    // From the last row, a 3-row image scrolls the screen by 2.
    CHECK_EQ(t.t.history_rows(), size_t(2));
    CHECK_EQ(t.t.cursor().row, 4);
    CHECK_EQ(t.t.cursor().col, 1);
    const Placement* p = only_placement(t.t);
    CHECK(p && p->row == t.t.screen_top_row() + 2);
    // Ending at the right edge wraps to the next line (kitty).
    t << "\x1b[1;20H" << kitty("a=p,i=1,q=2");
    CHECK_EQ(t.t.cursor().col, 0);
    CHECK_EQ(t.t.cursor().row, 3);
}

void relative() {
    IT t(80, 24);
    const ih::Rgba img = ih::pattern(10, 20, 5);
    t << kitty("a=t,i=1,q=2" + dims(img), raw(img)) << kitty("a=t,i=2,q=2" + dims(img), raw(img));
    t << "\x1b[5;10H" << kitty("a=p,i=1,p=1,C=1");
    t << kitty("a=p,i=2,p=1,P=1,Q=1,H=3,V=-2");
    t.reply();
    TerminalView view(t.t);
    auto f = view.snapshot();
    CHECK_EQ(f->images.size(), size_t(2));
    if (f->images.size() == 2) {
        CHECK_EQ(f->images[1].x, 12.0f);
        CHECK_EQ(f->images[1].y, 2.0f);
    }
    // A child of a child, then a cycle attempt.
    t << kitty("a=p,i=1,p=2,P=2,Q=1,H=1");
    t << kitty("a=p,i=2,p=1,P=1,Q=2");
    CHECK(t.reply().find("ECYCLE") != std::string::npos);
    t << kitty("a=p,i=1,p=3,P=5");
    CHECK(t.reply().find("ENOPARENT") != std::string::npos);
    // Deleting the root removes the chain.
    t << kitty("a=d,d=i,i=1,p=1");
    CHECK_EQ(t.t.images().placements().size(), size_t(0));
}

void deletes() {
    IT t(80, 24);
    const ih::Rgba img = ih::pattern(20, 40, 6);  // 2 x 2 cells
    auto setup = [&]() {
        t << "\x1b_Ga=d,d=A\x1b\\";
        for (uint32_t i = 1; i <= 4; ++i) t << kitty("a=t,q=2,i=" + std::to_string(i) + dims(img), raw(img));
        // i=1 at (0,0) z=1; i=2 at (5,10) z=2; i=3 at (10,20) z=1; i=4 virtual.
        t << "\x1b[1;1H" << kitty("a=p,i=1,p=1,z=1,C=1") << kitty("a=p,i=1,p=2,z=3,C=1");
        t << "\x1b[6;11H" << kitty("a=p,i=2,z=2,C=1");
        t << "\x1b[11;21H" << kitty("a=p,i=3,z=1,C=1");
        t << kitty("a=p,i=4,U=1,q=2");
        t.reply();
    };
    auto count = [&]() { return t.t.images().placements().size(); };
    setup();
    CHECK_EQ(count(), size_t(5));
    t << kitty("a=d");  // d=a: all visible, images kept, virtual kept
    CHECK_EQ(count(), size_t(1));
    CHECK_EQ(t.t.images().kitty_image_count(), size_t(4));
    setup();
    t << kitty("a=d,d=A");
    CHECK_EQ(t.t.images().kitty_image_count(), size_t(1));  // only the virtual placement's image stays
    setup();
    t << kitty("a=d,d=i,i=1,p=2");
    CHECK_EQ(count(), size_t(4));
    t << kitty("a=d,d=I,i=1");
    CHECK_EQ(count(), size_t(3));
    CHECK(t.t.images().find(1) == nullptr);
    setup();
    t << "\x1b[6;11H" << kitty("a=d,d=c");
    CHECK_EQ(count(), size_t(4));
    setup();
    t << kitty("a=d,d=p,x=22,y=12");  // 1-based cell inside i=3's box
    CHECK_EQ(count(), size_t(4));
    setup();
    t << kitty("a=d,d=q,x=1,y=1,z=3");
    CHECK_EQ(count(), size_t(4));
    setup();
    t << kitty("a=d,d=x,x=12");
    CHECK_EQ(count(), size_t(4));
    setup();
    t << kitty("a=d,d=y,y=2");
    CHECK_EQ(count(), size_t(3));
    setup();
    t << kitty("a=d,d=z,z=1");
    CHECK_EQ(count(), size_t(3));
    setup();
    t << kitty("a=d,d=R,x=2,y=3");
    CHECK_EQ(count(), size_t(3));
    CHECK(t.t.images().find(2) == nullptr && t.t.images().find(3) == nullptr);
    CHECK(t.t.images().find(1) != nullptr);
    // Upper case frees images left without placements, lower case keeps them.
    setup();
    t << kitty("a=d,d=z,z=2");
    CHECK(t.t.images().find(2) != nullptr);
    t << kitty("a=d,d=I,i=2");
    CHECK(t.t.images().find(2) == nullptr);
    CHECK_EQ(t.reply(), std::string(""));
}

// Image numbers (I=): each transmission gets a fresh id; commands with I=
// address the newest image with that number.
void numbers() {
    IT t(80, 24);
    t << kitty("a=t,I=41,s=1,v=1,f=24", "abc") << kitty("a=t,I=41,s=1,v=1,f=24", "def");
    t.reply();
    CHECK_EQ(t.t.images().kitty_image_count(), size_t(2));
    t << kitty("a=p,I=41");
    const std::string r = t.reply();
    const Placement* p = only_placement(t.t);
    CHECK(p != nullptr);
    if (!p) return;
    const Image* newest = t.t.images().find(p->image_id);
    CHECK(newest && newest->pixels()->rgba[0] == 'd');
    CHECK_EQ(r, "\x1b_Gi=" + std::to_string(p->image_id) + ",I=41;OK\x1b\\");
    t << kitty("a=d,d=N,I=41");
    CHECK_EQ(t.t.images().kitty_image_count(), size_t(1));
    CHECK_EQ(t.t.images().placements().size(), size_t(0));
}

void quotas() {
    TerminalOptions o = th::opts(80, 24);
    const ih::Rgba img = ih::pattern(32, 32, 7);  // 4 KiB
    o.graphics.storage_limit = 4096 * 3;
    IT t(o);
    t.t.set_cell_pixel_size(10, 20);
    t << kitty("a=T,i=1,q=2" + dims(img), raw(img));  // placed
    t << kitty("a=t,i=2,q=2" + dims(img), raw(img));
    t << kitty("a=t,i=3,q=2" + dims(img), raw(img));
    CHECK_EQ(t.t.image_bytes(), size_t(4096 * 3));
    // Over quota: unreferenced images go first, oldest first.
    t << kitty("a=t,i=4,q=2" + dims(img), raw(img));
    CHECK(t.t.images().find(1) != nullptr);
    CHECK(t.t.images().find(2) == nullptr);
    CHECK(t.t.images().find(3) != nullptr);
    // Using an image refreshes it.
    t << kitty("a=p,i=3,q=2");
    t << kitty("a=t,i=5,q=2" + dims(img), raw(img));
    CHECK(t.t.images().find(4) == nullptr);
    CHECK(t.t.images().find(3) != nullptr);
    CHECK(t.t.image_bytes() <= o.graphics.storage_limit);
    // One image larger than the whole quota: ENOSPC.
    const ih::Rgba big = ih::pattern(64, 64, 8);
    t << kitty("a=t,i=9" + dims(big), raw(big));
    CHECK(t.reply().rfind("\x1b_Gi=9;ENOSPC", 0) == 0);
    // Lowering the quota at run time evicts now, in the same order. Held:
    // 1 (placed first), 3 (placed later) and 5 (no placement). A quota of
    // two images drops 5, the one nothing shows.
    CHECK_EQ(t.t.image_bytes(), size_t(4096 * 3));
    const uint64_t changes = t.t.change_count();
    t.t.set_image_storage_limit(4096 * 2);
    CHECK_EQ(t.t.graphics_options().storage_limit, size_t(4096 * 2));
    CHECK_EQ(t.t.image_bytes(), size_t(4096 * 2));
    CHECK(t.t.images().find(5) == nullptr);
    CHECK(t.t.images().find(1) != nullptr && t.t.images().find(3) != nullptr);
    CHECK(t.t.change_count() != changes);
    // Down to one: of the placed ones, the least recently used goes.
    t.t.set_image_storage_limit(4096);
    CHECK(t.t.images().find(1) == nullptr && t.t.images().find(3) != nullptr);
    // Zero: everything, and new images are refused.
    t.t.set_image_storage_limit(0);
    CHECK_EQ(t.t.image_bytes(), size_t(0));
    CHECK_EQ(t.t.images().placements().size(), size_t(0));
    t << kitty("a=t,i=10" + dims(img), raw(img));
    CHECK(t.reply().rfind("\x1b_Gi=10;ENOSPC", 0) == 0);
    // Raising it lets them in again.
    t.t.set_image_storage_limit(4096 * 4);
    t << kitty("a=t,i=11,q=2" + dims(img), raw(img));
    CHECK(t.t.images().find(11) != nullptr);
    // Image and placement counts are capped too.
    TerminalOptions o2 = th::opts(80, 24);
    o2.graphics.max_images = 3;
    o2.graphics.max_placements = 4;
    IT u(o2);
    for (int i = 1; i <= 5; ++i) u << kitty("a=t,q=2,i=" + std::to_string(i) + ",s=1,v=1,f=24", "abc");
    CHECK_EQ(u.t.images().kitty_image_count(), size_t(3));
    for (int i = 1; i <= 6; ++i) u << kitty("a=p,i=5,C=1,q=2,p=" + std::to_string(i));
    CHECK_EQ(u.t.images().placements().size(), size_t(4));
}

void media() {
    namespace fs = std::filesystem;
    const ih::Rgba img = ih::pattern(4, 3, 9);
    const std::string data = raw(img);
    const fs::path dir = fs::temp_directory_path();
    const std::string file = (dir / "bropty-kitty-file-test.rgba").string();
    CHECK(ih::write_file(file, data));
    {
        IT t(80, 24);  // policy off by default
        t << kitty("a=t,i=1,t=f" + dims(img), file);
        CHECK(t.reply().rfind("\x1b_Gi=1;EPERM", 0) == 0);
        t << kitty("a=t,i=1,t=t" + dims(img), file);
        CHECK(t.reply().rfind("\x1b_Gi=1;EPERM", 0) == 0);
        t << kitty("a=t,i=1,t=s" + dims(img), "/bropty-shm");
        CHECK(t.reply().rfind("\x1b_Gi=1;EPERM", 0) == 0);
    }
    TerminalOptions o = th::opts(80, 24);
    o.graphics.allow_files = o.graphics.allow_temp_files = o.graphics.allow_shared_memory = true;
    IT t(o);
    t << kitty("a=t,i=1,t=f" + dims(img), file);
    CHECK_EQ(t.reply(), std::string("\x1b_Gi=1;OK\x1b\\"));
    CHECK(t.t.images().find(1) && ih::same_pixels(*t.t.images().find(1)->pixels(), img));
    // Offset and size within the file.
    const std::string padded = "0123456789" + data + "tail";
    CHECK(ih::write_file(file, padded));
    t << kitty("a=t,i=2,t=f,O=10,S=" + std::to_string(data.size()) + dims(img), file);
    CHECK(t.t.images().find(2) && ih::same_pixels(*t.t.images().find(2)->pixels(), img));
    t << kitty("a=t,i=3,t=f" + dims(img), file + ".missing");
    CHECK(t.reply().find("EBADF") != std::string::npos);
    t << kitty("a=t,i=3,t=f" + dims(img), dir.string());
    CHECK(t.reply().find("EBADF") != std::string::npos);
    fs::remove(file);
    // A temp file must be in a temp directory and named for the protocol;
    // it is deleted after reading.
    const std::string tmp = (dir / "tty-graphics-protocol-bropty.rgba").string();
    CHECK(ih::write_file(tmp, data));
    t << kitty("a=t,i=4,t=t" + dims(img), tmp);
    CHECK(t.t.images().find(4) != nullptr);
    CHECK(!fs::exists(tmp));
    const std::string not_tmp = (dir / "bropty-not-protocol.rgba").string();
    CHECK(ih::write_file(not_tmp, data));
    t << kitty("a=t,i=5,t=t" + dims(img), not_tmp);
    CHECK(t.reply().find("EPERM") != std::string::npos);
    CHECK(fs::exists(not_tmp));
    fs::remove(not_tmp);
    // Shared memory.
#if defined(_WIN32)
    const char* name = "bropty_kitty_shm_test";
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(data.size()), name);
    CHECK(h != nullptr);
    if (h) {
        void* v = MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, data.size());
        std::memcpy(v, data.data(), data.size());
        UnmapViewOfFile(v);
        t << kitty("a=t,i=6,t=s" + dims(img), name);
        CHECK(t.t.images().find(6) && ih::same_pixels(*t.t.images().find(6)->pixels(), img));
        CloseHandle(h);
    }
#else
    const char* name = "/bropty_kitty_shm_test";
    shm_unlink(name);
    const int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(ftruncate(fd, off_t(data.size())) == 0);
        void* v = mmap(nullptr, data.size(), PROT_WRITE, MAP_SHARED, fd, 0);
        std::memcpy(v, data.data(), data.size());
        munmap(v, data.size());
        close(fd);
        t << kitty("a=t,i=6,t=s" + dims(img), name);
        CHECK(t.t.images().find(6) && ih::same_pixels(*t.t.images().find(6)->pixels(), img));
        // kitty unlinks the object after reading it.
        const int again = shm_open(name, O_RDONLY, 0);
        CHECK(again < 0);
        if (again >= 0) close(again);
        shm_unlink(name);
    }
#endif
    t << kitty("a=t,i=7,t=s" + dims(img), "no_such_shm_bropty");
    CHECK(t.reply().find("EBADF") != std::string::npos);
}

// Random and mutated commands: the terminal must stay consistent and
// never exceed its caps.
void fuzz() {
    std::mt19937 rng(99);
    TerminalOptions o = th::opts(40, 12);
    o.graphics.storage_limit = 1 << 20;
    IT t(o);
    const char keys[] = "atofqsvSOiIpmxywhXYcrCUPQzHVd";
    const char* actions[] = {"t", "T", "q", "p", "d", "f", "a", "c", "x"};
    for (int iter = 0; iter < 20000; ++iter) {
        std::string ctl;
        const int n = int(rng() % 8);
        for (int k = 0; k < n; ++k) {
            if (!ctl.empty()) ctl += ',';
            const char key = keys[rng() % (sizeof keys - 1)];
            ctl += key;
            ctl += '=';
            switch (rng() % 5) {
            case 0: ctl += std::to_string(rng() % 8); break;
            case 1: ctl += std::to_string(rng() % 70); break;
            case 2: ctl += std::to_string(int32_t(rng())); break;
            case 3: ctl += key == 'a' ? actions[rng() % 9] : std::string(1, char('a' + rng() % 26)); break;
            default: ctl += char(rng() % 96 + 32);
            }
        }
        std::string payload;
        for (int k = int(rng() % 40); k > 0; --k) payload += "ABCDEFGHabcdefgh0123+/="[rng() % 23];
        std::string s = "\x1b_G" + ctl + (rng() % 2 ? ";" + payload : "") + "\x1b\\";
        if (rng() % 10 == 0) s += "\x1b[" + std::to_string(rng() % 12) + ";" + std::to_string(rng() % 40) + "H";
        if (rng() % 20 == 0) s += "\r\n\r\n\r\n";
        t << s;
        t.reply();
    }
    CHECK(t.t.image_bytes() <= o.graphics.storage_limit);
    CHECK(t.t.images().placements().size() <= o.graphics.max_placements);
    TerminalView view(t.t);
    auto f = view.snapshot();
    CHECK(f != nullptr);
    // The parser alone, on arbitrary bytes.
    for (int iter = 0; iter < 20000; ++iter) {
        std::string s;
        for (int k = int(rng() % 30); k > 0; --k) s += char(rng() % 128);
        detail::KittyCommand c;
        std::string err;
        (void)detail::parse_kitty_command(s, c, err);
    }
}

} // namespace

int main() {
    init_test();
    spec_replies();
    formats();
    chunked();
    placement_geometry();
    cursor_scrolls();
    relative();
    deletes();
    numbers();
    quotas();
    media();
    fuzz();
    return check::finish("test_kitty_graphics");
}
