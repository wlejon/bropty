// Real programs as oracles: what `kitten icat` (kitty), `img2sixel`
// (libsixel), `chafa` (sixel / kitty / iTerm2) and an imgcat-style writer
// send, fed through bropty and checked against the source images (decoded by
// stb_image) for pixels and placement.
//
// Three modes:
//   (default)  replay the checked-in captures under tests/data/images/programs/
//              (every platform), then -- POSIX only -- run whichever of the
//              programs this machine has, live through a pty and a Session
//              (so their queries are answered), with the same checks;
//   --record   run them live and rewrite the captures (needs every program).
// Live runs are skipped on Windows: ConPTY re-renders the child's output and
// does not pass graphics through.
#include "image_helpers.h"

#include "bropty/pty.h"
#include "bropty/session.h"
#include "bropty/view.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>

using namespace bropty;
namespace fs = std::filesystem;

namespace {

constexpr int kCols = 80, kRows = 24, kCellW = 10, kCellH = 20;

struct Program {
    const char* name;
    std::vector<std::string> argv;
};

const std::vector<Program>& programs() {
    static const std::vector<Program> p = {
        {"icat", {"kitten", "icat", "--transfer-mode=stream", "--use-window-size", "80,24,800,480", "--stdin=no",
                  "src_a.png"}},
        {"icat_placeholder", {"kitten", "icat", "--transfer-mode=stream", "--use-window-size", "80,24,800,480",
                              "--stdin=no", "--unicode-placeholder", "src_a.png"}},
        {"icat_place", {"kitten", "icat", "--transfer-mode=stream", "--use-window-size", "80,24,800,480",
                        "--stdin=no", "--place", "20x6@4x3", "src_b.png"}},
        {"img2sixel", {"img2sixel", "src_a.png"}},
        {"chafa_sixel", {"chafa", "-f", "sixel", "--size", "8x4", "--animate", "off", "src_a.png"}},
        {"chafa_kitty", {"chafa", "-f", "kitty", "--size", "8x4", "--animate", "off", "src_a.png"}},
        {"chafa_iterm", {"chafa", "-f", "iterm", "--size", "8x4", "--animate", "off", "src_a.png"}},
        // The iTerm2 imgcat script's output: name and size, then the base64 file.
        {"imgcat", {"/bin/sh", "-c",
                    "printf '\\033]1337;File=name=%s;size=%d;inline=1;width=6:' \"$(printf src_b.png | base64)\" "
                    "\"$(wc -c < src_b.png | tr -d ' ')\"; base64 < src_b.png | tr -d '\\n'; printf '\\a\\n'"}},
    };
    return p;
}

std::string capture_path(const char* name) {
    return std::string(BROPTY_TEST_DATA) + "/images/programs/" + name + ".vt";
}

ih::Rgba source(const char* file) {
    ih::Rgba r;
    CHECK(ih::load_png_file(ih::fixture(file), r));
    return r;
}

ih::Rgba of(const ImagePixels& p) { return ih::Rgba{p.width, p.height, p.rgba}; }

// Largest difference between two images' mean colours over a grid of blocks
// (the images may differ in size: blocks cover the same fraction of each).
int block_diff(const ih::Rgba& a, const ih::Rgba& b, int grid = 6) {
    int worst = 0;
    for (int by = 0; by < grid; ++by)
        for (int bx = 0; bx < grid; ++bx) {
            double mean[2][3] = {};
            const ih::Rgba* imgs[2] = {&a, &b};
            for (int k = 0; k < 2; ++k) {
                const ih::Rgba& m = *imgs[k];
                const uint32_t x0 = m.w * bx / grid, x1 = m.w * (bx + 1) / grid;
                const uint32_t y0 = m.h * by / grid, y1 = m.h * (by + 1) / grid;
                size_t n = 0;
                for (uint32_t y = y0; y < y1; ++y)
                    for (uint32_t x = x0; x < x1; ++x, ++n)
                        for (int c = 0; c < 3; ++c) mean[k][c] += m.at(x, y)[c];
                for (int c = 0; c < 3 && n; ++c) mean[k][c] /= double(n);
            }
            for (int c = 0; c < 3; ++c) worst = std::max(worst, int(std::abs(mean[0][c] - mean[1][c]) + 0.5));
        }
    return worst;
}

// The bounding box of the non-transparent pixels.
ih::Rgba opaque_part(const ih::Rgba& m) {
    uint32_t x0 = m.w, y0 = m.h, x1 = 0, y1 = 0;
    for (uint32_t y = 0; y < m.h; ++y)
        for (uint32_t x = 0; x < m.w; ++x)
            if (m.at(x, y)[3]) {
                x0 = std::min(x0, x), y0 = std::min(y0, y);
                x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
            }
    ih::Rgba r;
    if (x1 <= x0 || y1 <= y0) return r;
    r.w = x1 - x0;
    r.h = y1 - y0;
    for (uint32_t y = y0; y < y1; ++y) r.px.insert(r.px.end(), m.at(x0, y), m.at(x1 - 1, y) + 4);
    return r;
}

std::vector<const Image*> images_of(Terminal& t, ImageSource s) {
    std::vector<const Image*> v;
    t.images().for_each_image([&](const Image& i) {
        if (i.source == s) v.push_back(&i);
    });
    return v;
}

// The frame's images, viewport-relative.
std::shared_ptr<const Frame> frame_of(Terminal& t) {
    TerminalView view(t);
    return view.snapshot();
}

// Each program's checks; `shown` collects the pixels for the cross-checks.
using Shown = std::map<std::string, ih::Rgba>;

void check_icat(Terminal& t, Shown& shown) {
    const ih::Rgba src = source("src_a.png");
    const auto imgs = images_of(t, ImageSource::Kitty);
    CHECK_EQ(imgs.size(), size_t(1));
    if (imgs.size() != 1) return;
    CHECK(ih::same_pixels(*imgs[0]->pixels(), src));  // PNG passed through
    CHECK_EQ(t.images().placements().size(), size_t(1));
    if (t.images().placements().empty()) return;
    const Placement& p = t.images().placements()[0];
    // 61 x 37 px at 10 x 20 cells, centred in 80 columns: icat moves the
    // cursor 36 columns and shifts the image 4 px into the cell (X=4); it is
    // drawn at its native size. The extent it claims (scrolling, eviction,
    // cursor movement) is kitty's update_dest_rect: 7 columns, then rows from
    // the aspect ratio of those columns, (70 + 4) * 37 / 61 / 20 -> 3.
    CHECK_EQ(p.col, 36);
    CHECK_EQ(p.row, int64_t(0));
    CHECK_EQ(p.x_offset, 4);
    CHECK_EQ(ImageLayer::extent(p, kCellW, kCellH), std::make_pair(7, 3));
    auto f = frame_of(t);
    CHECK_EQ(f->images.size(), size_t(1));
    if (f->images.size() == 1) {
        const FrameImage& im = f->images[0];
        CHECK(std::abs(im.x - 36.4f) < 1e-4f && im.y == 0.0f);
        CHECK(std::abs(im.w - 6.1f) < 1e-4f && std::abs(im.h - 1.85f) < 1e-4f);
        CHECK(im.src_w == 61.0f && im.src_h == 37.0f);
        CHECK(im.pixels == imgs[0]->pixels());
    }
    // The cursor ends below the image.
    CHECK(t.cursor().row >= 2);
    shown["icat"] = src;
}

void check_icat_placeholder(Terminal& t, Shown&) {
    const ih::Rgba src = source("src_a.png");
    const auto imgs = images_of(t, ImageSource::Kitty);
    CHECK_EQ(imgs.size(), size_t(1));
    if (imgs.size() != 1) return;
    CHECK(ih::same_pixels(*imgs[0]->pixels(), src));
    CHECK_EQ(t.images().placements().size(), size_t(1));
    if (t.images().placements().empty()) return;
    CHECK(t.images().placements()[0].is_virtual);
    // The placeholder cells: 7 x 2 of U+10EEEE, centred.
    int cells = 0, first_col = kCols;
    for (int y = 0; y < kRows; ++y)
        for (int x = 0; x < kCols; ++x)
            if (t.row(y).cells[size_t(x)].cp() == kImagePlaceholder) {
                ++cells;
                first_col = std::min(first_col, x);
            }
    CHECK_EQ(cells, 14);
    CHECK_EQ(first_col, 36);
    // The frame draws the whole image over those cells.
    auto f = frame_of(t);
    float area = 0, src_area = 0;
    for (const FrameImage& im : f->images) {
        CHECK(im.pixels == imgs[0]->pixels());
        CHECK(im.x >= 36.0f && im.x + im.w <= 43.0f && im.y >= 0.0f && im.y + im.h <= 2.0f);
        area += im.w * im.h;
        src_area += im.src_w * im.src_h;
    }
    CHECK(!f->images.empty());
    CHECK(std::abs(src_area - 61.0f * 37.0f) < 1.0f);
    CHECK(area > 0.0f && area <= 14.0f);
}

void check_icat_place(Terminal& t, Shown&) {
    const ih::Rgba src = source("src_b.png");  // with alpha
    const auto imgs = images_of(t, ImageSource::Kitty);
    CHECK_EQ(imgs.size(), size_t(1));
    if (imgs.size() != 1) return;
    CHECK(ih::same_pixels(*imgs[0]->pixels(), src));
    CHECK_EQ(t.images().placements().size(), size_t(1));
    if (t.images().placements().empty()) return;
    const Placement& p = t.images().placements()[0];
    // --place 20x6@4x3: inside the 20 x 6 box at column 4, row 3.
    CHECK_EQ(p.row, int64_t(3));
    CHECK(p.col >= 4 && p.col < 24);
    const auto ext = ImageLayer::extent(p, kCellW, kCellH);
    CHECK(ext.first >= 1 && p.col + ext.first <= 24 && ext.second >= 1 && ext.second <= 6);
    auto f = frame_of(t);
    CHECK_EQ(f->images.size(), size_t(1));
    if (f->images.size() == 1) CHECK(f->images[0].y == 3.0f && f->images[0].x == float(p.col));
}

// A sixel / iTerm2 cell image of `cols` x `rows` cells at the left margin
// (at the top, unless the program wrote text first: Homebrew's img2sixel
// 1.8.7 prints debug lines on stderr).
const Image* cell_image(Terminal& t, ImageSource s, int cols, int rows) {
    const auto imgs = images_of(t, s);
    CHECK_EQ(imgs.size(), size_t(1));
    if (imgs.size() != 1) return nullptr;
    CHECK_EQ(imgs[0]->box_cols, cols);
    CHECK_EQ(imgs[0]->box_rows, rows);
    int top = 0;
    while (top < kRows - 1 && t.row(top).cells[0].cp() != kImagePlaceholder) ++top;
    CHECK(top + rows <= kRows);
    if (top + rows > kRows) return nullptr;
    int cells = 0;
    for (int y = 0; y < kRows; ++y)
        for (int x = 0; x < kCols; ++x)
            if (t.row(y).cells[size_t(x)].cp() == kImagePlaceholder) {
                ++cells;
                CHECK(y >= top && y < top + rows && x < cols);
            }
    CHECK_EQ(cells, cols * rows);
    auto f = frame_of(t);
    float area = 0;
    for (const FrameImage& im : f->images)
        if (im.pixels == imgs[0]->pixels()) area += im.w * im.h;
    CHECK(area > 0.0f && area <= float(cols * rows));
    return imgs[0];
}

void check_img2sixel(Terminal& t, Shown& shown) {
    const ih::Rgba src = source("src_a.png");
    const Image* img = cell_image(t, ImageSource::Sixel, 7, 2);
    if (!img) return;
    CHECK_EQ(img->width, 61u);
    CHECK_EQ(img->height, 37u);
    const int d = block_diff(of(*img->pixels()), src);
    if (d > 12) std::printf("  img2sixel block diff %d\n", d);
    CHECK(d <= 12);  // palette quantisation (and dithering)
    shown["img2sixel"] = of(*img->pixels());
}

void check_chafa_sixel(Terminal& t, Shown& shown) {
    const Image* img = cell_image(t, ImageSource::Sixel, 8, 3);
    if (!img) return;
    CHECK_EQ(img->width, 80u);
    CHECK_EQ(img->height, 60u);
    shown["chafa_sixel"] = of(*img->pixels());
}

void check_chafa_kitty(Terminal& t, Shown& shown) {
    const auto imgs = images_of(t, ImageSource::Kitty);
    CHECK_EQ(imgs.size(), size_t(1));
    if (imgs.size() != 1) return;
    CHECK_EQ(imgs[0]->width, 80u);  // chafa scales, then sends f=32 in chunks
    CHECK_EQ(imgs[0]->height, 60u);
    CHECK_EQ(t.images().placements().size(), size_t(1));
    if (!t.images().placements().empty()) {
        const Placement& p = t.images().placements()[0];
        CHECK(p.row == 0 && p.col == 0 && p.cols == 8 && p.rows == 3);
    }
    shown["chafa_kitty"] = of(*imgs[0]->pixels());
}

void check_chafa_iterm(Terminal& t, Shown& shown) {
    const Image* img = cell_image(t, ImageSource::Iterm2, 8, 3);
    if (!img) return;
    CHECK_EQ(img->width, 80u);  // a TIFF, decoded by the host
    CHECK_EQ(img->cell_width, 8.0f);
    CHECK_EQ(img->cell_height, 3.0f);  // preserveAspectRatio=0
    shown["chafa_iterm"] = of(*img->pixels());
}

void check_imgcat(Terminal& t, Shown&) {
    // width=6 cells = 60 px of a 40 x 30 image: 45 px = 2.25 rows.
    const Image* img = cell_image(t, ImageSource::Iterm2, 6, 3);
    if (!img) return;
    CHECK(ih::same_pixels(*img->pixels(), source("src_b.png")));
    CHECK_EQ(img->cell_width, 6.0f);
    CHECK_EQ(img->cell_height, 2.25f);
    CHECK_EQ(t.cursor().row, 3);  // the trailing newline after the image's last row
}

// The same picture three ways from chafa, and img2sixel's against the source.
void cross_checks(const Shown& shown, const char* mode) {
    const auto k = shown.find("chafa_kitty"), i = shown.find("chafa_iterm"), s = shown.find("chafa_sixel");
    if (k != shown.end() && i != shown.end()) {
        const int d = block_diff(k->second, i->second);
        if (d > 1) std::printf("  [%s] chafa kitty vs iterm block diff %d\n", mode, d);
        CHECK(d <= 1);
    }
    if (k != shown.end() && s != shown.end()) {
        const int d = block_diff(k->second, s->second);
        if (d > 12) std::printf("  [%s] chafa kitty vs sixel block diff %d\n", mode, d);
        CHECK(d <= 12);
    }
    if (k != shown.end()) {
        // chafa pads the picture to whole cells with transparent pixels.
        const int d = block_diff(opaque_part(k->second), source("src_a.png"));
        if (d > 16) std::printf("  [%s] chafa kitty vs source block diff %d\n", mode, d);
        CHECK(d <= 16);
    }
}

using Check = void (*)(Terminal&, Shown&);
Check check_for(const std::string& name) {
    static const std::map<std::string, Check> m = {
        {"icat", check_icat},           {"icat_placeholder", check_icat_placeholder},
        {"icat_place", check_icat_place}, {"img2sixel", check_img2sixel},
        {"chafa_sixel", check_chafa_sixel}, {"chafa_kitty", check_chafa_kitty},
        {"chafa_iterm", check_chafa_iterm}, {"imgcat", check_imgcat},
    };
    return m.at(name);
}

void run_check(const char* mode, const char* name, Terminal& t, Shown& shown) {
    const int before = check::g_failures;
    check_for(name)(t, shown);
    if (check::g_failures != before) std::printf("  ^ [%s] %s\n", mode, name);
}

void replay() {
    Shown shown;
    for (const Program& p : programs()) {
        const std::string bytes = ih::read_file(capture_path(p.name));
        CHECK(!bytes.empty());
        if (bytes.empty()) {
            std::printf("  missing capture %s\n", capture_path(p.name).c_str());
            continue;
        }
        ih::IT t(kCols, kRows);
        t << bytes;
        run_check("replay", p.name, t.t, shown);
    }
    cross_checks(shown, "replay");
}

#if !defined(_WIN32)
bool on_path(const std::string& exe) {
    if (exe.find('/') != std::string::npos) return fs::exists(exe);
    const char* path = std::getenv("PATH");
    std::string_view rest = path ? path : "";
    while (!rest.empty()) {
        const size_t c = rest.find(':');
        const std::string dir(rest.substr(0, c));
        std::error_code ec;
        if (!dir.empty() && fs::exists(fs::path(dir) / exe, ec)) return true;
        rest = c == std::string_view::npos ? std::string_view() : rest.substr(c + 1);
    }
    return false;
}

// Runs a program in an 80 x 24 pty (800 x 480 px) whose output drives a
// Session (its replies go back to the program). Returns what it wrote.
std::string run_live(const Program& p, ih::DecodingHost& host, Session& s) {
    s.set_delegate(&host);
    s.set_cell_pixel_size(kCellW, kCellH);
    std::shared_ptr<IPtyProcess> pty(create_pty());
    PtyConfig c;
    c.command = p.argv[0];
    c.args.assign(p.argv.begin() + 1, p.argv.end());
    c.cwd = std::string(BROPTY_TEST_DATA) + "/images";
    c.size = PtySize{kCols, kRows, kCols * kCellW, kRows * kCellH};
    if (!pty->spawn(c)) {
        std::printf("  %s: spawn failed: %s\n", p.name, pty->last_error().c_str());
        return {};
    }
    s.attach_pty(pty);
    std::string out;
    std::vector<char> buf(1 << 16);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!pty->eof() && std::chrono::steady_clock::now() < deadline) {
        const size_t n = pty->read_timeout(buf.data(), buf.size(), std::chrono::milliseconds(100));
        if (!n) continue;
        out.append(buf.data(), n);
        s.feed(std::string_view(buf.data(), n));
    }
    if (!pty->eof()) std::printf("  %s: timed out\n", p.name);
    pty->terminate();
    return out;
}

void live(bool record) {
    Shown shown;
    int ran = 0;
    if (record) fs::create_directories(std::string(BROPTY_TEST_DATA) + "/images/programs");
    for (const Program& p : programs()) {
        if (!on_path(p.argv[0])) {
            std::printf("  live: %s not installed, skipped\n", p.argv[0].c_str());
            CHECK(!record);
            continue;
        }
        ih::DecodingHost host;
        Session s(th::opts(kCols, kRows));
        const std::string out = run_live(p, host, s);
        CHECK(!out.empty());
        ++ran;
        run_check("live", p.name, s.terminal(), shown);
        if (record) CHECK(ih::write_file(capture_path(p.name), out));
    }
    cross_checks(shown, "live");
    std::printf("  live: %d of %zu programs ran\n", ran, programs().size());
}
#endif

} // namespace

int main(int argc, char** argv) {
    init_test();
    const bool record = argc > 1 && std::strcmp(argv[1], "--record") == 0;
#if defined(_WIN32)
    if (record) {
        std::printf("--record needs a POSIX pty\n");
        return 1;
    }
#else
    if (record) {
        live(true);
        return check::finish("test_image_programs --record");
    }
#endif
    replay();
#if !defined(_WIN32)
    live(false);
#endif
    return check::finish("test_image_programs");
}
