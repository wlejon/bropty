// Image storage: layers, the storage quota, placements and animation clocks.
// Anchoring (scrolling, erasing, eviction) is in graphics_anchor.cpp.
#include "graphics_state.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <unordered_set>

namespace bropty {

// ---------------------------------------------------------------------------
// Public read-only types

size_t Image::bytes() const noexcept {
    size_t n = 0;
    for (const ImageFrame& f : frames)
        if (f.pixels) n += f.pixels->rgba.size();
    return n;
}

const Image* ImageLayer::find(uint32_t id) const noexcept {
    if (!id) return nullptr;
    auto it = images_.find(id);
    return it == images_.end() ? nullptr : it->second.get();
}

const Image* ImageLayer::by_key(uint64_t key) const noexcept {
    auto it = images_.find(key);
    return it == images_.end() ? nullptr : it->second.get();
}

const Image* ImageLayer::cell_image(uint32_t id) const noexcept {
    auto it = cell_images_.find(id);
    return it == cell_images_.end() ? nullptr : it->second.get();
}

std::pair<int, int> ImageLayer::extent(const Placement& p, int cell_w, int cell_h) noexcept {
    // kitty's update_dest_rect, including its order of evaluation.
    const double cw = std::max(1, cell_w), ch = std::max(1, cell_h);
    const double sw = std::max<uint32_t>(1, p.src_w), sh = std::max<uint32_t>(1, p.src_h);
    double nc = p.cols, nr = p.rows;
    if (nc == 0) {
        if (nr == 0) nc = std::ceil((sw + p.x_offset) / cw);
        else nc = std::ceil((ch * nr + p.y_offset) * sw / sh / cw);
    }
    if (nr == 0) {
        if (nc == 0) nr = std::ceil((sh + p.y_offset) / ch);
        else nr = std::ceil((cw * nc + p.x_offset) * sh / sw / ch);
    }
    auto clampi = [](double v) { return int(std::clamp(v, 1.0, 1e6)); };
    return {clampi(nc), clampi(nr)};
}

namespace {
constexpr int kMaxDepth = 8;
}

bool ImageLayer::position(const Placement& p, int64_t& row, int& col) const noexcept {
    // Offsets accumulate up the chain to the root placement's cell.
    int64_t dr = 0;
    int dc = 0;
    const Placement* cur = &p;
    for (int depth = 0; cur->parent_serial; ++depth) {
        if (depth >= kMaxDepth) return false;
        const Placement* parent = nullptr;
        for (const Placement& q : placements_)
            if (q.serial == cur->parent_serial) parent = &q;
        if (!parent || parent->is_virtual) return false;
        dr += cur->parent_dy;
        dc += cur->parent_dx;
        cur = parent;
    }
    row = cur->row + dr;
    col = cur->col + dc;
    return true;
}

namespace detail {

namespace {
std::atomic<uint64_t> g_pixel_serial{1};
constexpr uint64_t kAnonymousBase = uint64_t(1) << 32;
} // namespace

Graphics::Graphics(const GraphicsOptions& o) : opts_(o) { sixel_shared = SixelPalette::standard(); }

ImagePixelsPtr Graphics::make_pixels(uint32_t w, uint32_t h, std::vector<uint8_t>&& rgba) {
    auto p = std::make_shared<ImagePixels>();
    p->serial = g_pixel_serial.fetch_add(1, std::memory_order_relaxed);
    p->width = w;
    p->height = h;
    p->rgba = std::move(rgba);
    return p;
}

Image* Graphics::by_key(bool alt, uint64_t key) noexcept {
    auto& m = layer(alt).images_;
    auto it = m.find(key);
    return it == m.end() ? nullptr : it->second.get();
}

Image* Graphics::by_id(bool alt, uint32_t id) noexcept { return id ? by_key(alt, id) : nullptr; }

Image* Graphics::by_number(bool alt, uint32_t number) noexcept {
    if (!number) return nullptr;
    Image* best = nullptr;
    for (auto& kv : layer(alt).images_)
        if (kv.second->number == number && (!best || kv.second->serial > best->serial)) best = kv.second.get();
    return best;
}

Image* Graphics::cell_image(bool alt, uint32_t id) noexcept {
    auto& m = layer(alt).cell_images_;
    auto it = m.find(id);
    return it == m.end() ? nullptr : it->second.get();
}

size_t Graphics::refs(bool alt, const Image& img) const noexcept {
    const ImageLayer& l = layer(alt);
    size_t n = 0;
    if (img.source == ImageSource::Kitty) {
        for (const Placement& p : l.placements_) n += p.image_key == img.key;
    } else {
        for (const ImageLayer::Anchor& a : l.anchors_) n += a.id == img.id;
    }
    return n;
}

void Graphics::evict_image(int li, uint64_t key, bool cell) {
    ImageLayer& l = layers_[li];
    if (cell) {
        auto it = l.cell_images_.find(uint32_t(key));
        if (it == l.cell_images_.end()) return;
        bytes_ -= std::min(bytes_, it->second->bytes());
        l.cell_images_.erase(it);
        l.anchors_.erase(std::remove_if(l.anchors_.begin(), l.anchors_.end(),
                                        [&](const ImageLayer::Anchor& a) { return a.id == uint32_t(key); }),
                         l.anchors_.end());
    } else {
        auto it = l.images_.find(key);
        if (it == l.images_.end()) return;
        bytes_ -= std::min(bytes_, it->second->bytes());
        l.images_.erase(it);
        remove_placements(li == 1, [&](const Placement& p) { return p.image_key == key; }, false);
    }
    ++version_;
}

void Graphics::enforce_quota(size_t incoming, const Image* keep) {
    if (bytes_ + incoming <= opts_.storage_limit) return;
    struct Cand {
        int layer;
        uint64_t key;
        bool cell;
        bool referenced;
        uint64_t used;
    };
    std::vector<Cand> c;
    for (int li = 0; li < 2; ++li) {
        for (auto& kv : layers_[li].images_)
            if (kv.second.get() != keep)
                c.push_back({li, kv.first, false, refs(li == 1, *kv.second) > 0, kv.second->last_used});
        for (auto& kv : layers_[li].cell_images_)
            if (kv.second.get() != keep)
                c.push_back({li, kv.first, true, refs(li == 1, *kv.second) > 0, kv.second->last_used});
    }
    // Unreferenced images first, then the least recently used (kitty).
    std::sort(c.begin(), c.end(), [](const Cand& a, const Cand& b) {
        return a.referenced != b.referenced ? !a.referenced : a.used < b.used;
    });
    for (const Cand& k : c) {
        if (bytes_ + incoming <= opts_.storage_limit) break;
        evict_image(k.layer, k.key, k.cell);
    }
}

Image* Graphics::store_kitty(bool alt, std::unique_ptr<Image> img, std::string& error) {
    const size_t size = img->bytes();
    if (size > opts_.storage_limit) {
        error = "ENOSPC:Image is larger than the storage quota";
        return nullptr;
    }
    ImageLayer& l = layer(alt);
    if (img->id) {
        if (l.images_.count(img->id)) evict_image(alt ? 1 : 0, img->id, false);
        img->key = img->id;
    } else {
        img->key = kAnonymousBase + ++anon_;
    }
    while (l.images_.size() >= opts_.max_images && !l.images_.empty()) {
        auto oldest = std::min_element(l.images_.begin(), l.images_.end(), [](const auto& a, const auto& b) {
            return a.second->last_used < b.second->last_used;
        });
        evict_image(alt ? 1 : 0, oldest->first, false);
    }
    enforce_quota(size, nullptr);
    img->serial = ++serial_;
    img->last_used = ++clock_;
    bytes_ += size;
    Image* raw = img.get();
    l.images_[img->key] = std::move(img);
    ++version_;
    return raw;
}

Image* Graphics::store_cell_image(bool alt, std::unique_ptr<Image> img, std::string& error) {
    const size_t size = img->bytes();
    if (size > opts_.storage_limit) {
        error = "image larger than the storage quota";
        return nullptr;
    }
    ImageLayer& l = layer(alt);
    while (l.cell_images_.size() >= opts_.max_images && !l.cell_images_.empty()) {
        auto oldest = std::min_element(l.cell_images_.begin(), l.cell_images_.end(), [](const auto& a, const auto& b) {
            return a.second->last_used < b.second->last_used;
        });
        evict_image(alt ? 1 : 0, oldest->first, true);
    }
    enforce_quota(size, nullptr);
    // Ids fit in a 24-bit foreground color; skip ones still in use.
    for (int tries = 0; tries < (1 << 24); ++tries) {
        const uint32_t id = next_cell_id_;
        next_cell_id_ = next_cell_id_ >= 0xFFFFFFu ? 1u : next_cell_id_ + 1;
        if (!layers_[0].cell_images_.count(id) && !layers_[1].cell_images_.count(id)) {
            img->id = id;
            break;
        }
    }
    img->key = img->id;
    img->serial = ++serial_;
    img->last_used = ++clock_;
    bytes_ += size;
    Image* raw = img.get();
    l.cell_images_[img->id] = std::move(img);
    ++version_;
    return raw;
}

uint32_t Graphics::free_kitty_id(bool alt) {
    const ImageLayer& l = layer(alt);
    for (;;) {
        const uint32_t id = next_kitty_id_;
        next_kitty_id_ = next_kitty_id_ == 0xFFFFFFFFu ? 1u : next_kitty_id_ + 1;
        if (!l.images_.count(id)) return id;
    }
}

void Graphics::remove_image(bool alt, uint64_t key) { evict_image(alt ? 1 : 0, key, false); }
void Graphics::remove_cell_image(bool alt, uint32_t id) { evict_image(alt ? 1 : 0, id, true); }

bool Graphics::resized(bool alt, Image& img, size_t old_bytes, std::string& error) {
    (void)alt;
    const size_t now = img.bytes();
    bytes_ -= std::min(bytes_, old_bytes);
    if (now > opts_.storage_limit) {
        bytes_ += now;
        error = "ENOSPC:Image is larger than the storage quota";
        return false;
    }
    enforce_quota(now, &img);
    bytes_ += now;
    ++version_;
    return true;
}

// ---------------------------------------------------------------------------
// Placements

Placement* Graphics::placement(bool alt, uint64_t key, uint32_t id) noexcept {
    for (Placement& p : layer(alt).placements_)
        if (p.image_key == key && p.placement_id == id) return &p;
    return nullptr;
}

Placement& Graphics::place(bool alt, Placement p) {
    ImageLayer& l = layer(alt);
    ++version_;
    if (p.placement_id && p.image_id) {
        if (Placement* old = placement(alt, p.image_key, p.placement_id)) {
            p.serial = old->serial;  // relative children stay attached
            *old = p;
            return *old;
        }
    }
    if (l.placements_.size() >= opts_.max_placements) {
        auto oldest = std::min_element(l.placements_.begin(), l.placements_.end(),
                                       [](const Placement& a, const Placement& b) { return a.serial < b.serial; });
        const uint64_t s = oldest->serial;
        remove_placements(alt, [s](const Placement& q) { return q.serial == s; }, false);
    }
    p.serial = ++serial_;
    l.placements_.push_back(p);
    return l.placements_.back();
}

void Graphics::remove_placements(bool alt, const std::function<bool(const Placement&)>& pred, bool free_images) {
    ImageLayer& l = layer(alt);
    std::unordered_set<uint64_t> removed;     // placement serials
    std::unordered_set<uint64_t> touched;     // image keys that lost a placement
    std::vector<Placement> keep;
    keep.reserve(l.placements_.size());
    for (Placement& p : l.placements_) {
        if (pred(p)) {
            removed.insert(p.serial);
            touched.insert(p.image_key);
        } else {
            keep.push_back(p);
        }
    }
    if (removed.empty()) return;
    // Relative placements go with their parents (transitively).
    for (bool again = true; again;) {
        again = false;
        for (size_t i = 0; i < keep.size();) {
            if (keep[i].parent_serial && removed.count(keep[i].parent_serial)) {
                removed.insert(keep[i].serial);
                touched.insert(keep[i].image_key);
                keep.erase(keep.begin() + long(i));
                again = true;
            } else {
                ++i;
            }
        }
    }
    l.placements_ = std::move(keep);
    ++version_;
    auto unreferenced = [&](uint64_t key) {
        for (const Placement& p : l.placements_)
            if (p.image_key == key) return false;
        return true;
    };
    if (free_images) {
        for (uint64_t key : touched)
            if (unreferenced(key)) evict_image(alt ? 1 : 0, key, false);
    }
}

void Graphics::remove_all_placements_of(bool alt, uint64_t key) {
    remove_placements(alt, [key](const Placement& p) { return p.image_key == key; }, false);
}

const char* Graphics::check_parent(bool alt, const Placement& child) const {
    const ImageLayer& l = layer(alt);
    uint64_t s = child.parent_serial;
    for (int depth = 1; s; ++depth) {
        if (child.serial && s == child.serial) return "ECYCLE";
        if (depth > kMaxDepth) return "ETOODEEP";
        const Placement* parent = nullptr;
        for (const Placement& p : l.placements_)
            if (p.serial == s) parent = &p;
        if (!parent) return "ENOPARENT";
        s = parent->parent_serial;
    }
    return nullptr;
}

std::optional<std::pair<int64_t, int>> Graphics::position(bool alt, const Placement& p) const {
    int64_t row;
    int col;
    if (!layer(alt).position(p, row, col)) return std::nullopt;
    return std::make_pair(row, col);
}

// ---------------------------------------------------------------------------
// Animation

uint64_t Graphics::advance(uint64_t now_ms, bool& changed) {
    changed = false;
    now_ms_ = std::max(now_ms_, now_ms);
    uint64_t next = UINT64_MAX;
    for (ImageLayer& l : layers_) {
        // kitty's scan_active_animations: a frame shows for its gap from the
        // moment it was shown; gapless frames are stepped over; a finished
        // loop count leaves the last frame showing.
        auto step = [&](Image& img) {
            if (img.animation == AnimationState::Stopped || img.frames.size() < 2) return;
            if (img.max_loops && img.current_loop >= img.max_loops) return;
            uint64_t total = 0;
            for (const ImageFrame& f : img.frames) total += f.gap_ms;
            if (!total) return;
            const uint32_t n = uint32_t(img.frames.size());
            uint64_t due = img.frame_shown_ms + img.frames[img.current_frame].gap_ms;
            if (now_ms_ >= due) {
                uint32_t cur = img.current_frame;
                bool moved = false;
                do {
                    const uint32_t f = (cur + 1) % n;
                    if (f == 0) {
                        if (img.animation == AnimationState::Loading) break;
                        if (++img.current_loop >= img.max_loops && img.max_loops) break;
                    }
                    cur = f;
                    moved = true;
                } while (!img.frames[cur].gap_ms);
                if (moved) {
                    img.current_frame = cur;
                    img.frame_shown_ms = now_ms_;
                    changed = true;
                    due = now_ms_ + img.frames[cur].gap_ms;
                }
            }
            if (due > now_ms_) next = std::min(next, due - now_ms_);
        };
        for (auto& kv : l.images_) step(*kv.second);
        for (auto& kv : l.cell_images_) step(*kv.second);
    }
    if (changed) ++version_;
    return next;
}

} // namespace detail
} // namespace bropty
