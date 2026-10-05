// iTerm2 inline images: OSC 1337 ; File=<args>:<base64> ST, and the
// multipart form (MultipartFile=<args>, FilePart=<base64>..., FileEnd).
// Arguments: name (base64), size (bytes), width / height (N cells, Npx,
// N%, auto), preserveAspectRatio (default 1), inline (must be 1 to display;
// otherwise it is a download, which a terminal library ignores), and
// WezTerm's doNotMoveCursor.
#include "bropty/terminal.h"

#include "base64.h"
#include "graphics_load.h"
#include "graphics_state.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace bropty {

namespace {

struct ItermArgs {
    std::string_view width{"auto"};
    std::string_view height{"auto"};
    bool preserve{true};
    bool show{false};
    bool stay{false};
    uint64_t size{0};
};

ItermArgs parse_args(std::string_view s) {
    ItermArgs a;
    while (!s.empty()) {
        const size_t semi = s.find(';');
        const std::string_view kv = s.substr(0, semi);
        s = semi == std::string_view::npos ? std::string_view{} : s.substr(semi + 1);
        const size_t eq = kv.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view k = kv.substr(0, eq), v = kv.substr(eq + 1);
        if (k == "width") a.width = v;
        else if (k == "height") a.height = v;
        else if (k == "preserveAspectRatio") a.preserve = v != "0";
        else if (k == "inline") a.show = v == "1";
        else if (k == "doNotMoveCursor") a.stay = v == "1";
        else if (k == "size") std::from_chars(v.data(), v.data() + v.size(), a.size);
    }
    return a;
}

// A width / height argument in pixels: N cells, Npx, N% of the screen, or
// auto (0: from the image). Malformed values count as auto.
double dimension(std::string_view v, int cell, int screen_cells) {
    if (v.empty() || v == "auto") return 0;
    // Digits with an optional fraction (no locale, no floating from_chars:
    // not every standard library has it).
    double n = 0;
    size_t i = 0;
    bool digits = false;
    for (; i < v.size() && v[i] >= '0' && v[i] <= '9' && n < 1e7; ++i, digits = true) n = n * 10 + (v[i] - '0');
    if (i < v.size() && v[i] == '.') {
        double scale = 0.1;
        for (++i; i < v.size() && v[i] >= '0' && v[i] <= '9'; ++i, scale /= 10, digits = true)
            n += (v[i] - '0') * scale;
    }
    if (!digits || !(n > 0) || n > 1e6) return 0;
    const std::string_view unit = v.substr(i);
    if (unit.empty()) return n * cell;
    if (unit == "px") return n;
    if (unit == "%") return n / 100.0 * double(cell) * screen_cells;
    return 0;
}

} // namespace

void Terminal::osc_iterm(std::string_view rest) {
    detail::Graphics& g = *gfx_;
    auto& up = g.iterm;
    std::string args;
    std::vector<uint8_t> data;
    if (rest.substr(0, 5) == "File=") {
        up = detail::Graphics::ItermUpload{};
        const size_t colon = rest.find(':');
        if (colon == std::string_view::npos) return;
        args.assign(rest.substr(5, colon - 5));
        if (!detail::base64_decode_append(rest.substr(colon + 1), data)) return;
    } else if (rest.substr(0, 14) == "MultipartFile=") {
        up = detail::Graphics::ItermUpload{};
        up.active = true;
        up.args.assign(rest.substr(14));
        if (parse_args(up.args).size > g.options().max_transmission_bytes) up.failed = true;
        return;
    } else if (rest.substr(0, 9) == "FilePart=") {
        if (!up.active || up.failed) return;
        if (!detail::base64_decode_append(rest.substr(9), up.data) ||
            up.data.size() > g.options().max_transmission_bytes) {
            up.failed = true;
            up.data = {};
        }
        return;
    } else if (rest == "FileEnd") {
        const bool ok = up.active && !up.failed;
        args = std::move(up.args);
        data = std::move(up.data);
        up = detail::Graphics::ItermUpload{};
        if (!ok) return;
    } else {
        return;
    }
    const ItermArgs a = parse_args(args);
    if (!a.show || data.empty()) return;

    DecodedImage d;
    std::string error;
    if (!detail::decode_with_host(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()),
                                  g.options(), host_, d, error))
        return;
    data = {};

    const int cw = image_cell_width(), ch = image_cell_height();
    const double iw = d.width, ih = d.height;
    double w = dimension(a.width, cw, cols_), h = dimension(a.height, ch, rows_);
    if (w == 0 && h == 0) {
        w = iw;
        h = ih;
        // Too wide for the screen: shrink to fit, keeping the aspect (iTerm2).
        const double screen_w = double(cols_) * cw;
        if (w > screen_w) {
            h = h * screen_w / w;
            w = screen_w;
        }
    } else if (w == 0) {
        w = a.preserve ? iw * h / ih : iw;
    } else if (h == 0) {
        h = a.preserve ? ih * w / iw : ih;
    } else if (a.preserve) {
        const double s = std::min(w / iw, h / ih);
        w = iw * s;
        h = ih * s;
    }
    w = std::clamp(w, 1.0, double(cw) * 10000);
    h = std::clamp(h, 1.0, double(ch) * 10000);

    auto img = std::make_unique<Image>();
    img->source = ImageSource::Iterm2;
    img->width = d.width;
    img->height = d.height;
    for (DecodedFrame& f : d.frames) {
        if (img->frames.size() >= g.options().max_frames) break;
        // Browsers' rule for GIFs: delays under 20 ms play at 100 ms.
        const uint32_t gap = f.delay_ms < 20 ? 100 : f.delay_ms;
        img->frames.push_back(ImageFrame{detail::Graphics::make_pixels(d.width, d.height, std::move(f.rgba)), gap});
    }
    if (img->frames.size() > 1) {
        img->animation = AnimationState::Running;
        img->frame_shown_ms = g.clock_ms();
    } else {
        img->frames[0].gap_ms = 0;
    }
    img->cell_width = float(w / cw);
    img->cell_height = float(h / ch);
    place_cell_image(std::move(img), a.stay ? CellCursor::Stay : CellCursor::Iterm, false);
}

} // namespace bropty
