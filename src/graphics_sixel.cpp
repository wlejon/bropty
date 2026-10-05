// Sixel images (DCS P1;P2;P3 q ... ST) and XTSMGRAPHICS, as xterm does them.
#include "bropty/terminal.h"

#include "graphics_state.h"

#include <algorithm>
#include <string>

namespace bropty {

namespace {

// xterm's graphics limit is what the screen can show, capped by the options.
uint32_t sixel_limit(uint32_t sixel_max, uint32_t image_max) { return std::max(1u, std::min(sixel_max, image_max)); }

} // namespace

void Terminal::sixel_start(const CsiSeq& seq) {
    detail::Graphics& g = *gfx_;
    const GraphicsOptions& o = g.options();
    const detail::SixelPalette palette =
        modes_.sixel_private_colors ? detail::SixelPalette::standard() : g.sixel_shared;
    g.sixel = std::make_unique<detail::SixelDecoder>(seq.raw(0, -1), seq.raw(1, -1), seq.raw(2, -1),
                                                     sixel_limit(o.max_sixel_width, o.max_width),
                                                     sixel_limit(o.max_sixel_height, o.max_height), palette);
}

void Terminal::sixel_finish() {
    detail::Graphics& g = *gfx_;
    std::unique_ptr<detail::SixelDecoder> d = std::move(g.sixel);
    if (!d) return;
    // With shared registers (?1070 reset) the image's definitions persist.
    if (!modes_.sixel_private_colors) g.sixel_shared = d->registers();
    if (d->empty()) return;
    auto img = std::make_unique<Image>();
    img->source = ImageSource::Sixel;
    img->width = d->width();
    img->height = d->height();
    std::vector<uint8_t> rgba;
    d->to_rgba(rgba);
    d.reset();
    img->frames.push_back(ImageFrame{detail::Graphics::make_pixels(img->width, img->height, std::move(rgba)), 0});
    // Native size: one image pixel per screen pixel.
    img->cell_width = float(img->width) / float(image_cell_width());
    img->cell_height = float(img->height) / float(image_cell_height());
    place_cell_image(std::move(img), modes_.sixel_cursor_right ? CellCursor::SixelRight : CellCursor::Sixel,
                     modes_.sixel_display_mode);
}

// XTSMGRAPHICS: CSI ? Pi ; Pa ; Pv S. Pi 1 = color registers, 2 = sixel
// geometry; Pa 1 read, 2 reset, 3 set, 4 read maximum. Reply CSI ? Pi ; Ps ; Pv S
// with Ps 0 success, 1 unknown item, 2 unknown action, 3 failure. The number
// of registers and the geometry limit are fixed here, so setting them reports
// the value in force (xterm also clamps a set to the maximum).
void Terminal::xtsmgraphics(const CsiSeq& s) {
    if (!opts_.graphics.sixel) return;
    const int item = s.raw(0, 0);
    const int action = s.raw(1, 0);
    const std::string head = "\x1b[?" + std::to_string(item) + ";";
    if (item != 1 && item != 2) {
        reply(head + "1;0S");
        return;
    }
    if (action < 1 || action > 4) {
        reply(head + "2;0S");
        return;
    }
    if (item == 1) {
        reply(head + "0;" + std::to_string(detail::kSixelRegisters) + "S");
        return;
    }
    const GraphicsOptions& o = opts_.graphics;
    const uint32_t max_w = sixel_limit(o.max_sixel_width, o.max_width);
    const uint32_t max_h = sixel_limit(o.max_sixel_height, o.max_height);
    uint32_t w = max_w, h = max_h;
    if (action != 4) {
        w = std::min<uint32_t>(max_w, uint32_t(cols_) * uint32_t(image_cell_width()));
        h = std::min<uint32_t>(max_h, uint32_t(rows_) * uint32_t(image_cell_height()));
    }
    reply(head + "0;" + std::to_string(w) + ";" + std::to_string(h) + "S");
}

} // namespace bropty
