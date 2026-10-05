// Keeping images anchored to their text: region scrolls, renumbered rows,
// history eviction, erasing and screen clears (graphics.h states the rules).
#include "graphics_state.h"

#include <algorithm>
#include <unordered_set>

namespace bropty::detail {

namespace {

// Clip k rows off the top (`top` true) or bottom of a placement that kept
// `er` rows. False when nothing would be left.
bool clip_rows(Placement& p, int k, int er, int ec, int cell_h, bool top) {
    uint32_t px;
    if (p.cols == 0 && p.rows == 0) {
        // Natural size: whole cells of source pixels (the first one less the offset).
        const int64_t v = int64_t(k) * cell_h - (top ? p.y_offset : 0);
        px = uint32_t(std::max<int64_t>(0, v));
    } else {
        // Scaled: freeze the box so the rest keeps its scale, cut proportionally.
        p.cols = ec;
        p.rows = er - k;
        px = uint32_t(uint64_t(p.src_h) * uint64_t(k) / uint64_t(std::max(1, er)));
    }
    if (px >= p.src_h || er - k <= 0) return false;
    p.src_h -= px;
    if (top) {
        p.src_y += px;
        p.row += k;
        p.y_offset = 0;
    }
    return true;
}

} // namespace

void Graphics::scroll_region(bool alt, int64_t top, int64_t bottom, int left, int right, int n, int cell_w,
                             int cell_h) {
    ImageLayer& l = layer(alt);
    if (n == 0) return;
    std::unordered_set<uint64_t> drop;
    for (Placement& p : l.placements_) {
        if (p.is_virtual || p.parent_serial) continue;
        auto [ec, er] = ImageLayer::extent(p, cell_w, cell_h);
        const bool inside = p.row >= top && p.row + er - 1 <= bottom && p.col >= left && p.col + ec - 1 <= right;
        if (!inside) continue;
        p.row -= n;
        ++version_;
        if (p.row + er <= top || p.row > bottom) {
            drop.insert(p.serial);
        } else if (p.row < top) {
            if (!clip_rows(p, int(top - p.row), er, ec, cell_h, true)) drop.insert(p.serial);
        } else if (p.row + er - 1 > bottom) {
            if (!clip_rows(p, int(p.row + er - 1 - bottom), er, ec, cell_h, false)) drop.insert(p.serial);
        }
    }
    if (!drop.empty()) remove_placements(alt, [&](const Placement& p) { return drop.count(p.serial) != 0; }, false);
    bool anchors_dropped = false;
    for (auto it = l.anchors_.begin(); it != l.anchors_.end();) {
        ImageLayer::Anchor& a = *it;
        if (a.row >= top && a.row + a.rows - 1 <= bottom) {
            a.row -= n;
            if (a.row < top) {
                a.rows -= int(top - a.row);
                a.row = top;
            }
            if (a.row + a.rows - 1 > bottom) a.rows = int(bottom - a.row + 1);
            if (a.rows <= 0) {
                it = l.anchors_.erase(it);
                anchors_dropped = true;
                continue;
            }
        }
        ++it;
    }
    if (anchors_dropped) drop_orphans(alt);
}

void Graphics::shift_rows(bool alt, int64_t from, int64_t delta) {
    if (delta == 0) return;
    ImageLayer& l = layer(alt);
    for (Placement& p : l.placements_)
        if (!p.is_virtual && !p.parent_serial && p.row >= from) p.row += delta;
    for (ImageLayer::Anchor& a : l.anchors_)
        if (a.row >= from) a.row += delta;
    ++version_;
}

void Graphics::evict_before(bool alt, int64_t first_row, int cell_w, int cell_h) {
    ImageLayer& l = layer(alt);
    bool any = false;
    for (const Placement& p : l.placements_) {
        if (p.is_virtual || p.parent_serial) continue;
        if (p.row + ImageLayer::extent(p, cell_w, cell_h).second <= first_row) {
            any = true;
            break;
        }
    }
    if (any) {
        remove_placements(
            alt,
            [&](const Placement& p) {
                return !p.is_virtual && !p.parent_serial &&
                       p.row + ImageLayer::extent(p, cell_w, cell_h).second <= first_row;
            },
            false);
    }
    bool dropped = false;
    for (auto it = l.anchors_.begin(); it != l.anchors_.end();) {
        if (it->row + it->rows <= first_row) {
            it = l.anchors_.erase(it);
            dropped = true;
            continue;
        }
        if (it->row < first_row) {
            it->rows -= int(first_row - it->row);
            it->row = first_row;
        }
        ++it;
    }
    if (dropped) drop_orphans(alt);
}

void Graphics::clear_rows(bool alt, int64_t top, int64_t end, int cell_w, int cell_h) {
    ImageLayer& l = layer(alt);
    remove_placements(
        alt,
        [&](const Placement& p) {
            if (p.is_virtual) return false;
            auto pos = position(alt, p);
            if (!pos) return false;
            const int er = ImageLayer::extent(p, cell_w, cell_h).second;
            return pos->first < end && pos->first + er > top;
        },
        false);
    bool dropped = false;
    for (auto it = l.anchors_.begin(); it != l.anchors_.end();) {
        if (it->row >= top && it->row + it->rows <= end) {
            it = l.anchors_.erase(it);
            dropped = true;
            continue;
        }
        if (it->row < top && it->row + it->rows > top) it->rows = int(top - it->row);
        ++it;
    }
    if (dropped) drop_orphans(alt);
    ++version_;
}

void Graphics::add_anchor(bool alt, uint32_t id, int64_t row, int rows) {
    layer(alt).anchors_.push_back(ImageLayer::Anchor{id, row, rows});
}

void Graphics::retain_cell_images(bool alt, const std::vector<uint32_t>& on_screen, int64_t top, int64_t end) {
    ImageLayer& l = layer(alt);
    bool dropped = false;
    for (auto it = l.anchors_.begin(); it != l.anchors_.end();) {
        if (it->row >= top && it->row + it->rows <= end &&
            !std::binary_search(on_screen.begin(), on_screen.end(), it->id)) {
            it = l.anchors_.erase(it);
            dropped = true;
            continue;
        }
        ++it;
    }
    if (dropped) drop_orphans(alt);
}

void Graphics::prune_anchors(bool alt) {
    ImageLayer& l = layer(alt);
    const size_t before = l.anchors_.size();
    l.anchors_.erase(std::remove_if(l.anchors_.begin(), l.anchors_.end(),
                                    [](const ImageLayer::Anchor& a) { return a.rows <= 0; }),
                     l.anchors_.end());
    if (l.anchors_.size() != before) drop_orphans(alt);
}

void Graphics::drop_orphans(bool alt) {
    ImageLayer& l = layer(alt);
    std::unordered_set<uint32_t> anchored;
    for (const ImageLayer::Anchor& a : l.anchors_) anchored.insert(a.id);
    std::vector<uint32_t> gone;
    for (const auto& kv : l.cell_images_)
        if (!anchored.count(kv.first)) gone.push_back(kv.first);
    for (uint32_t id : gone) evict_image(alt ? 1 : 0, id, true);
}

void Graphics::clear_layer(bool alt) {
    ImageLayer& l = layer(alt);
    for (const auto& kv : l.images_) bytes_ -= std::min(bytes_, kv.second->bytes());
    for (const auto& kv : l.cell_images_) bytes_ -= std::min(bytes_, kv.second->bytes());
    l.images_.clear();
    l.cell_images_.clear();
    l.placements_.clear();
    l.anchors_.clear();
    if (upload.active && upload.alt == alt) upload = KittyUpload{};
    ++version_;
}

void Graphics::reset() {
    clear_layer(false);
    clear_layer(true);
    upload = KittyUpload{};
    iterm = ItermUpload{};
    sixel.reset();
    sixel_shared = SixelPalette::standard();
    bytes_ = 0;
}

} // namespace bropty::detail
