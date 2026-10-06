// TerminalView: the images a frame shows. Kitty placements become
// rectangles by kitty's own geometry (grman_update_layers); image cells
// (sixel, iTerm2, kitty Unicode placeholders) become one rectangle per run of
// adjacent cells showing adjacent parts of one image.
#include "bropty/view.h"

#include "placeholder.h"

#include <algorithm>
#include <cmath>

namespace bropty {

namespace {

ImagePlane plane_of(int32_t z) {
    if (z < INT32_MIN / 2) return ImagePlane::BelowBackground;
    return z < 0 ? ImagePlane::BelowText : ImagePlane::AboveText;
}

// kitty's color_to_id: an indexed color is its index, an RGB color its 24 bits.
uint32_t color_id(const Color& c) {
    if (c.is_indexed()) return c.index();
    if (c.is_rgb()) return c.rgb_value().to_u32();
    return 0;
}

struct Ctx {
    const ImageLayer& layer;
    Frame& f;
    float cw, ch;
};

// A kitty placement drawn as an overlay.
void add_placement(Ctx& c, const Placement& p, const Image& img) {
    int64_t row;
    int col;
    if (!c.layer.position(p, row, col)) return;
    const double y0 = double(row - c.f.top_row);
    const double top = y0 + p.y_offset / c.ch;
    const double left = col + p.x_offset / c.cw;
    const double sw = std::max<uint32_t>(1, p.src_w), sh = std::max<uint32_t>(1, p.src_h);
    double right, bottom;
    if (p.rows) {
        bottom = y0 + p.rows;
        if (p.cols) {
            right = col + p.cols;
        } else {
            const double height_px = (bottom - top) * c.ch;
            right = left + height_px * sw / sh / c.cw;
        }
    } else {
        right = p.cols ? double(col + p.cols) : left + sw / c.cw;
        const double width_px = (right - left) * c.cw;
        bottom = top + width_px * sh / sw / c.ch;
    }
    if (bottom <= 0 || top >= c.f.rows || right <= left || bottom <= top) return;
    FrameImage fi;
    fi.pixels = img.pixels();
    fi.image_id = img.id;
    fi.placement_id = p.placement_id;
    fi.source = ImageSource::Kitty;
    fi.plane = plane_of(p.z);
    fi.z = p.z;
    fi.src_x = float(p.src_x);
    fi.src_y = float(p.src_y);
    fi.src_w = float(p.src_w);
    fi.src_h = float(p.src_h);
    fi.x = float(left);
    fi.y = float(top);
    fi.w = float(right - left);
    fi.h = float(bottom - top);
    c.f.images.push_back(std::move(fi));
}

// Image pixels [sx, sx + sw) x [sy, sy + sh) at scale (xs, ys) drawn with its
// top-left at viewport pixel position (dx, dy): clipped to the image, pushed
// in cell units.
void push_cell_run(Ctx& c, const Image& img, uint32_t placement_id, int32_t z, double sx, double sy, double sw,
                   double sh, double xs, double ys, double dx, double dy) {
    if (sx < 0) {
        dx += -sx * xs;
        sw += sx;
        sx = 0;
    }
    if (sy < 0) {
        dy += -sy * ys;
        sh += sy;
        sy = 0;
    }
    sw = std::min(sw, double(img.width) - sx);
    sh = std::min(sh, double(img.height) - sy);
    if (sw <= 0 || sh <= 0) return;
    FrameImage fi;
    fi.pixels = img.pixels();
    fi.image_id = img.id;
    fi.placement_id = placement_id;
    fi.source = img.source;
    fi.plane = ImagePlane::Text;
    fi.z = z;
    fi.src_x = float(sx);
    fi.src_y = float(sy);
    fi.src_w = float(sw);
    fi.src_h = float(sh);
    fi.x = float(dx / c.cw);
    fi.y = float(dy / c.ch);
    fi.w = float(sw * xs / c.cw);
    fi.h = float(sh * ys / c.ch);
    c.f.images.push_back(std::move(fi));
}

// Cells [x0, x0 + n) of viewport row y show image row i, columns j0.. of a
// sixel / iTerm2 image.
void sixel_run(Ctx& c, const Image& img, int y, int x0, int n, uint32_t i, uint32_t j0) {
    if (img.cell_width <= 0 || img.cell_height <= 0) return;
    const double xs = img.cell_width * c.cw / img.width;  // screen pixels per image pixel
    const double ys = img.cell_height * c.ch / img.height;
    push_cell_run(c, img, 0, 0, j0 * c.cw / xs, i * c.ch / ys, n * c.cw / xs, c.ch / ys, xs, ys, x0 * c.cw,
                  y * c.ch);
}

// kitty's grman_put_cell_image: the run shows box cells (img_row, img_col..)
// of the virtual placement's box, the image fitted into it and centred.
void kitty_run(Ctx& c, uint32_t image_id, uint32_t placement_id, int y, int x0, int n, uint32_t img_row,
               uint32_t img_col) {
    const Image* img = c.layer.find(image_id);
    if (!img || !img->width || !img->height) return;
    const Placement* v = nullptr;
    for (const Placement& p : c.layer.placements()) {
        if (!p.is_virtual || p.image_key != img->key) continue;
        if (placement_id ? p.placement_id == placement_id : (!v || p.serial > v->serial)) v = &p;
    }
    if (!v) return;
    const double cw = c.cw, ch = c.ch, W = img->width, H = img->height;
    const double cols = v->cols ? v->cols : std::ceil(W / cw);
    const double rows = v->rows ? v->rows : std::ceil(H / ch);
    double xo, yo, xs, ys;
    if (W * rows * ch > H * cols * cw) {
        xo = 0;
        xs = ys = cols * cw / W;
        yo = (rows * ch - H * ys) / 2;
    } else {
        yo = 0;
        ys = xs = rows * ch / H;
        xo = (cols * cw - W * xs) / 2;
    }
    const double x_dst = img_col * cw, y_dst = img_row * ch;
    push_cell_run(c, *img, v->placement_id, v->z, (x_dst - xo) / xs, (y_dst - yo) / ys, n * cw / xs, ch / ys, xs,
                  ys, x0 * cw, y * ch);
}

void scan_row(Ctx& c, const FrameRow& r, int y) {
    // Sixel / iTerm2 runs.
    const Image* cur = nullptr;
    int run_x = 0, run_n = 0;
    uint32_t run_i = 0, run_j = 0;
    auto flush_cell = [&]() {
        if (cur && run_n) sixel_run(c, *cur, y, run_x, run_n, run_i, run_j);
        cur = nullptr;
        run_n = 0;
    };
    // kitty runs (screen_render_line_graphics); row / column / msb are 1-based, 0 unknown.
    uint32_t k_len = 0, p_lo = 0, p_pid = 0, p_hi = 0, p_row = 0, p_col = 0;
    auto flush_kitty = [&](int end_x) {
        if (k_len) kitty_run(c, p_lo | ((p_hi - 1) << 24), p_pid, y, end_x - int(k_len), int(k_len), p_row - 1,
                             p_col - k_len);
        k_len = 0;
    };
    for (int x = 0; x < r.cols; ++x) {
        const Cell& cell = r.cells[size_t(x)];
        const bool ph = cell.cp() == kImagePlaceholder;
        std::u32string_view tail = ph && cell.has_cluster() ? r.clusters.find(x) : std::u32string_view{};
        const Style& st = r.styles[cell.style];
        // Our own image cells.
        if (ph && tail.size() == 2 && tail[0] >= kCellImageRowBase && tail[0] < kCellImageColBase &&
            tail[1] >= kCellImageColBase) {
            flush_kitty(x);
            p_lo = p_pid = p_hi = p_row = p_col = 0;
            const uint32_t id = st.fg.is_rgb() ? st.fg.rgb_value().to_u32() : 0;
            const uint32_t i = uint32_t(tail[0] - kCellImageRowBase), j = uint32_t(tail[1] - kCellImageColBase);
            const Image* img = c.layer.cell_image(id);
            if (!(cur && img == cur && i == run_i && j == run_j + uint32_t(run_n) && x == run_x + run_n)) {
                flush_cell();
                cur = img;
                run_x = x;
                run_i = i;
                run_j = j;
            }
            if (cur) ++run_n;
            continue;
        }
        flush_cell();
        uint32_t lo = 0, pid = 0, hi = 0, row = 0, col = 0;
        if (ph) {
            lo = color_id(st.fg);
            pid = color_id(st.underline_color);
            if (tail.size() > 0) row = detail::kitty_diacritic_number(tail[0]);
            if (tail.size() > 1) col = detail::kitty_diacritic_number(tail[1]);
            if (tail.size() > 2) hi = detail::kitty_diacritic_number(tail[2]);
        }
        if (k_len > 0 && ph && lo == p_lo && pid == p_pid && (!row || row == p_row) && (!col || col == p_col + 1) &&
            (!hi || hi == p_hi)) {
            ++k_len;
            row = std::max(p_row, 1u);
            col = p_col + 1;
            hi = std::max(p_hi, 1u);
        } else {
            flush_kitty(x);
            if (ph) {
                k_len = 1;
                if (!col) col = 1;
                if (!row) row = 1;
                if (!hi) hi = 1;
            }
        }
        p_lo = lo;
        p_hi = hi;
        p_pid = pid;
        p_row = row;
        p_col = col;
    }
    flush_cell();
    flush_kitty(r.cols);
}

} // namespace

void TerminalView::build_images(Frame& f) const {
    SourceImages si;
    if (term_) {
        si.layer = &term_->images();
        si.cell_width = term_->image_cell_width();
        si.cell_height = term_->image_cell_height();
        si.may_have_cells = term_->may_have_image_cells();
    } else {
        si = t_.source_images();
        if (!si.layer) return;
    }
    f.image_cell_width = si.cell_width;
    f.image_cell_height = si.cell_height;
    if (si.cell_width <= 0 || si.cell_height <= 0) return;
    const ImageLayer& layer = *si.layer;
    const bool cells = si.may_have_cells;
    if (layer.placements().empty() && !cells) return;
    Ctx c{layer, f, float(f.image_cell_width), float(f.image_cell_height)};
    // Overlays in creation order (the sort below keeps it among equal z).
    std::vector<const Placement*> ps;
    ps.reserve(layer.placements().size());
    for (const Placement& p : layer.placements())
        if (!p.is_virtual) ps.push_back(&p);
    std::sort(ps.begin(), ps.end(), [](const Placement* a, const Placement* b) { return a->serial < b->serial; });
    for (const Placement* p : ps)
        if (const Image* img = layer.by_key(p->image_key); img && !img->frames.empty()) add_placement(c, *p, *img);
    if (cells) {
        for (int y = 0; y < f.rows; ++y)
            if (const FrameRow* r = f.lines[size_t(y)].get(); r && r->cols) scan_row(c, *r, y);
    }
    std::stable_sort(f.images.begin(), f.images.end(), [](const FrameImage& a, const FrameImage& b) {
        return a.plane != b.plane ? a.plane < b.plane : a.z < b.z;
    });
}

} // namespace bropty
