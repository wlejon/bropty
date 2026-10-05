// The kitty graphics protocol: chunk assembly, transmission, placement,
// deletion and replies. Animation (a=f, a=a, a=c) is in graphics_kitty_anim.cpp.
#include "bropty/terminal.h"

#include "base64.h"
#include "graphics_load.h"
#include "graphics_state.h"

#include <algorithm>
#include <cctype>
#include <functional>

namespace bropty {

using detail::KittyCommand;

namespace {

// Keys a continuation chunk may carry (besides its payload).
constexpr uint64_t key_bit(char k) { return uint64_t(1) << (k - 'A'); }
constexpr uint64_t kChunkKeys = key_bit('m') | key_bit('q') | key_bit('a');

bool takes_data(char action) { return action == 't' || action == 'T' || action == 'q' || action == 'f'; }

} // namespace

void Terminal::kitty_respond(const KittyCommand& c, uint32_t id, std::string_view result) {
    const bool ok = result == "OK";
    if (c.quiet >= 2 || (ok && c.quiet >= 1)) return;
    if (!id && !c.number) return;  // nothing to address the reply to
    std::string r = "\x1b_G";
    bool first = true;
    auto field = [&](char k, uint32_t v) {
        if (!first) r.push_back(',');
        first = false;
        r.push_back(k);
        r.push_back('=');
        r += std::to_string(v);
    };
    if (id) field('i', id);
    if (c.number) field('I', c.number);
    if (c.placement) field('p', c.placement);
    if (c.rows && (c.action == 'f' || c.action == 'a')) field('r', c.rows);
    r.push_back(';');
    // Replies carry only printable ASCII.
    for (char ch : result) r.push_back((ch >= 0x20 && ch < 0x7F) ? ch : ' ');
    r += "\x1b\\";
    reply(r);
}

void Terminal::kitty_command(std::string_view payload) {
    detail::Graphics& g = *gfx_;
    KittyCommand c;
    std::string error;
    if (!detail::parse_kitty_command(payload, c, error)) {
        g.upload = detail::Graphics::KittyUpload{};
        kitty_respond(c, c.id, "EINVAL:" + error);
        return;
    }
    auto& up = g.upload;
    if (up.active) {
        const bool continuation = (c.present & ~kChunkKeys) == 0 && (!c.has('a') || c.action == up.cmd.action);
        if (continuation) {
            if (!up.failed) {
                if (!detail::base64_decode_append(c.payload, up.data)) {
                    up.failed = true;
                    up.error = "EINVAL:Invalid base64 data";
                    up.data.clear();
                } else if (up.data.size() > g.options().max_transmission_bytes) {
                    up.failed = true;
                    up.error = "EFBIG:Transmission is too large";
                    up.data.clear();
                    up.data.shrink_to_fit();
                }
            }
            if (c.more) return;
            KittyCommand first = up.cmd;
            if (c.has('q')) first.quiet = c.quiet;
            std::vector<uint8_t> data = std::move(up.data);
            const bool failed = up.failed;
            const std::string err = up.error;
            const bool alt = up.alt;
            up = detail::Graphics::KittyUpload{};
            if (failed) {
                kitty_respond(first, first.id, err);
                return;
            }
            if (alt != alt_screen_active()) return;  // the screen it was meant for is gone
            kitty_execute(first, data);
            return;
        }
        // Anything else aborts the upload in progress (a delete must, per the spec).
        up = detail::Graphics::KittyUpload{};
    }
    std::vector<uint8_t> data;
    if (takes_data(c.action)) {
        if (!detail::base64_decode_append(c.payload, data)) {
            kitty_respond(c, c.id, "EINVAL:Invalid base64 data");
            return;
        }
        if (c.more) {
            up.active = true;
            up.alt = alt_screen_active();
            up.cmd = c;
            up.cmd.payload = {};
            up.data = std::move(data);
            if (up.data.size() > g.options().max_transmission_bytes) {
                up.failed = true;
                up.error = "EFBIG:Transmission is too large";
                up.data.clear();
            }
            return;
        }
    }
    kitty_execute(c, data);
}

void Terminal::kitty_execute(KittyCommand& c, std::vector<uint8_t>& data) {
    detail::Graphics& g = *gfx_;
    const bool alt = alt_screen_active();
    if (c.id && c.number) {
        kitty_respond(c, c.id, "EINVAL:Must not specify both an image id and an image number");
        return;
    }
    switch (c.action) {
    case 't':
    case 'T':
    case 'q': {
        uint64_t key = 0;
        std::string r = kitty_transmit(c, data, key);
        uint32_t id = c.id;
        if (r.empty() && c.action != 'q') {
            if (const Image* img = g.by_key(alt, key)) id = img->id;
            if (c.action == 'T') r = kitty_put(c, key);
        }
        kitty_respond(c, id, r.empty() ? "OK" : r);
        return;
    }
    case 'p': {
        const Image* img = c.id ? g.by_id(alt, c.id) : g.by_number(alt, c.number);
        if (!img) {
            kitty_respond(c, c.id, "ENOENT:Put command refers to a non-existent image");
            return;
        }
        const uint32_t id = img->id;
        std::string r = kitty_put(c, img->key);
        kitty_respond(c, id, r.empty() ? "OK" : r);
        return;
    }
    case 'd': kitty_delete(c); return;
    case 'f': {
        std::string r = kitty_frame(c, data);
        kitty_respond(c, c.id, r.empty() ? "OK" : r);
        return;
    }
    case 'a': {
        std::string r = kitty_animate(c);
        kitty_respond(c, c.id, r.empty() ? "OK" : r);
        return;
    }
    case 'c': {
        std::string r = kitty_compose(c);
        kitty_respond(c, c.id, r.empty() ? "OK" : r);
        return;
    }
    default: kitty_respond(c, c.id, "EINVAL:Unknown action"); return;
    }
}

std::string Terminal::kitty_transmit(const KittyCommand& c, std::vector<uint8_t>& data, uint64_t& key) {
    detail::Graphics& g = *gfx_;
    std::string error;
    if (!detail::read_medium(c, data, g.options(), error)) return error;
    DecodedImage d;
    if (!detail::decode_kitty_pixels(c, data, g.options(), host_, d, error)) return error;
    if (c.action == 'q') return {};  // a query loads but never stores
    const bool alt = alt_screen_active();
    auto img = std::make_unique<Image>();
    img->id = c.id;
    img->number = c.number;
    if (c.number && !c.id) img->id = g.free_kitty_id(alt);
    img->source = ImageSource::Kitty;
    img->width = d.width;
    img->height = d.height;
    img->frames.push_back(ImageFrame{detail::Graphics::make_pixels(d.width, d.height, std::move(d.frames[0].rgba)), 0});
    Image* stored = g.store_kitty(alt, std::move(img), error);
    if (!stored) return error;
    key = stored->key;
    return {};
}

std::string Terminal::kitty_put(const KittyCommand& c, uint64_t key) {
    detail::Graphics& g = *gfx_;
    const bool alt = alt_screen_active();
    Image* img = g.by_key(alt, key);
    if (!img) return "ENOENT:Put command refers to a non-existent image";
    if (c.unicode && c.parent_id) return "EINVAL:A virtual placement cannot be relative";
    const int cw = image_cell_width(), ch = image_cell_height();
    Placement p;
    p.image_key = key;
    p.image_id = img->id;
    p.placement_id = img->id ? c.placement : 0;
    p.src_x = std::min(c.x, img->width);
    p.src_y = std::min(c.y, img->height);
    p.src_w = std::min(c.w ? c.w : img->width, img->width - p.src_x);
    p.src_h = std::min(c.h ? c.h : img->height, img->height - p.src_y);
    if (p.src_w == 0 || p.src_h == 0) return "EINVAL:Empty source rectangle";
    p.x_offset = int(std::min<uint32_t>(c.cell_x, uint32_t(cw - 1)));
    p.y_offset = int(std::min<uint32_t>(c.cell_y, uint32_t(ch - 1)));
    p.cols = int(std::min<uint32_t>(c.cols, 10000));
    p.rows = int(std::min<uint32_t>(c.rows, 10000));
    p.z = c.z;
    p.is_virtual = c.unicode != 0;
    if (!p.is_virtual) {
        const RowPos here = cursor_pos();
        p.row = here.row;
        p.col = here.col;
    }
    if (c.parent_id) {
        Image* parent = g.by_id(alt, c.parent_id);
        if (!parent) return "ENOPARENT:Parent image does not exist";
        const Placement* pp = nullptr;
        for (const Placement& q : g.layer(alt).placements()) {
            if (q.image_key != parent->key) continue;
            if (c.parent_placement ? q.placement_id == c.parent_placement : (!pp || q.serial < pp->serial)) pp = &q;
        }
        if (!pp) return "ENOPARENT:Parent placement does not exist";
        p.parent_image_key = parent->key;
        p.parent_placement_id = pp->placement_id;
        p.parent_serial = pp->serial;
        p.parent_dx = c.parent_dx;
        p.parent_dy = c.parent_dy;
        if (p.placement_id) {
            if (const Placement* old = g.placement(alt, key, p.placement_id)) p.serial = old->serial;
        }
        if (p.serial && p.serial == p.parent_serial) return "EINVAL:A placement cannot be its own parent";
        if (const char* bad = g.check_parent(alt, p)) return std::string(bad) + ":Invalid parent placement";
    }
    g.place(alt, p);
    g.used(*img);
    if (p.is_virtual || p.parent_serial || c.cursor == 1) return {};
    // Move the cursor past the image: right by its columns, down to its last
    // row; then into bounds the way kitty does it.
    const auto [ec, er] = ImageLayer::extent(p, cw, ch);
    Cursor& k = cur();
    int x = k.col + ec;
    int y = k.row + er - 1;
    if (x >= cols_) {
        x = 0;
        ++y;
    }
    if (y > bottom_) {
        scroll_up(y - bottom_);
        y = bottom_;
    }
    k.row = std::clamp(y, 0, rows_ - 1);
    k.col = std::clamp(x, 0, cols_ - 1);
    k.pending_wrap = false;
    invalidate_print();
    return {};
}

void Terminal::kitty_delete(const KittyCommand& c) {
    detail::Graphics& g = *gfx_;
    const bool alt = alt_screen_active();
    const bool free = std::isupper(uint8_t(c.del)) != 0;
    const char what = char(std::tolower(uint8_t(c.del)));
    const int cw = image_cell_width(), ch = image_cell_height();
    const int64_t top = screen_top_row();
    // Where a non-virtual placement is: its cell box.
    auto box = [&](const Placement& p, int64_t& r0, int64_t& r1, int& c0, int& c1) {
        if (p.is_virtual) return false;
        auto pos = g.position(alt, p);
        if (!pos) return false;
        const auto [ec, er] = ImageLayer::extent(p, cw, ch);
        r0 = pos->first;
        r1 = r0 + er;
        c0 = pos->second;
        c1 = c0 + ec;
        return true;
    };
    auto at_cell = [&](int64_t row, int col, const Placement& p) {
        int64_t r0, r1;
        int c0, c1;
        return box(p, r0, r1, c0, c1) && row >= r0 && row < r1 && col >= c0 && col < c1;
    };
    std::function<bool(const Placement&)> pred;
    std::vector<uint64_t> image_keys;  // images addressed directly (i, n, r)
    switch (what) {
    case 'a':
        pred = [&](const Placement& p) {
            int64_t r0, r1;
            int c0, c1;
            return box(p, r0, r1, c0, c1) && r1 > top;
        };
        break;
    case 'i':
    case 'n': {
        const Image* img = what == 'i' ? g.by_id(alt, c.id) : g.by_number(alt, c.number);
        if (!img) return;
        const uint64_t key = img->key;
        image_keys.push_back(key);
        const uint32_t pid = c.placement;
        pred = [key, pid](const Placement& p) { return p.image_key == key && (!pid || p.placement_id == pid); };
        break;
    }
    case 'r': {
        g.layer(alt).for_each_image([&](const Image& img) {
            if (img.source == ImageSource::Kitty && img.id && img.id >= c.x && img.id <= c.y)
                image_keys.push_back(img.key);
        });
        pred = [&](const Placement& p) {
            return std::find(image_keys.begin(), image_keys.end(), p.image_key) != image_keys.end();
        };
        break;
    }
    case 'c': {
        const RowPos cp = cursor_pos();
        pred = [&, cp](const Placement& p) { return at_cell(cp.row, cp.col, p); };
        break;
    }
    case 'p':
    case 'q': {
        const int64_t row = top + int64_t(c.y) - 1;
        const int col = int(c.x) - 1;
        const bool zq = what == 'q';
        pred = [&, row, col, zq](const Placement& p) { return (!zq || p.z == c.z) && at_cell(row, col, p); };
        break;
    }
    case 'x': {
        const int col = int(c.x) - 1;
        pred = [&, col](const Placement& p) {
            int64_t r0, r1;
            int c0, c1;
            return box(p, r0, r1, c0, c1) && col >= c0 && col < c1;
        };
        break;
    }
    case 'y': {
        const int64_t row = top + int64_t(c.y) - 1;
        pred = [&, row](const Placement& p) {
            int64_t r0, r1;
            int c0, c1;
            return box(p, r0, r1, c0, c1) && row >= r0 && row < r1;
        };
        break;
    }
    case 'z':
        pred = [&](const Placement& p) { return !p.is_virtual && p.z == c.z; };
        break;
    case 'f': {
        Image* img = c.id ? g.by_id(alt, c.id) : g.by_number(alt, c.number);
        if (!img) return;
        if (img->frames.size() <= 1) {
            if (free) g.remove_image(alt, img->key);
            return;
        }
        const size_t old = img->bytes();
        const uint32_t n = std::clamp<uint32_t>(c.rows ? c.rows : 1, 1, uint32_t(img->frames.size()));
        img->frames.erase(img->frames.begin() + long(n - 1));
        if (img->current_frame > n - 1) --img->current_frame;
        img->current_frame = std::min(img->current_frame, uint32_t(img->frames.size() - 1));
        std::string ignored;
        g.resized(alt, *img, old, ignored);
        return;
    }
    default: return;
    }
    g.remove_placements(alt, pred, free);
    if (free) {
        // Directly addressed images go even when they had no placements.
        for (uint64_t key : image_keys) {
            if (const Image* img = g.by_key(alt, key); img && g.refs(alt, *img) == 0) g.remove_image(alt, key);
        }
    }
}

} // namespace bropty
