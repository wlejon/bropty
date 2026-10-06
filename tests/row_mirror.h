#pragma once
// A RowSource that mirrors a Terminal the way a remote client would: rows
// copied out with their own style tables, hyperlink ids renumbered per row
// (so rows do not share an id space), history that may be withheld (rows
// the client has not fetched), resizes and screen switches reported to
// observers. Tests run Selection / Search / links / TerminalView over it and
// compare with the same operations over the Terminal itself.

#include "bropty/graphics.h"
#include "bropty/terminal.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mirror {

struct MRow {
    std::vector<bropty::Cell> cells;
    std::vector<bropty::Style> styles;
    bropty::ClusterMap clusters;
    uint32_t flags{0};
    std::vector<std::pair<uint32_t, std::string>> links;  // local id -> URI
    uint64_t serial{0};
};

class Mirror final : public bropty::RowSource {
public:
    // Copy the terminal's state (and advance its generation: the mirror is a
    // reader of its stamps). Reports a resize (row_numbering changed) and a
    // screen switch to observers, as a client applying a frame would.
    void pull(bropty::Terminal& t, bool use_serials = true) {
        const bool first = !pulled_;
        const bool resized = !first && numbering_ != t.row_numbering();
        const bool switched = !first && alt_ != t.alt_screen_active();
        if (resized) notify_before_resize();
        pulled_ = true;
        numbering_ = t.row_numbering();
        alt_ = t.alt_screen_active();
        cols_ = t.cols();
        rows_ = t.rows();
        first_ = t.first_row();
        top_ = t.screen_top_row();
        cursor_ = t.cursor();
        modes_ = t.modes();
        palette_ = t.palette();
        rows_map_.clear();
        for (int64_t abs = first_; abs < top_ + rows_; ++abs) {
            const bropty::RowView v = t.row_at(abs);
            MRow m;
            m.flags = v.flags;
            m.cells.assign(v.cells, v.cells + v.cols);
            if (v.clusters) m.clusters = *v.clusters;
            std::vector<uint32_t> ids;  // local index -> terminal style id
            for (bropty::Cell& c : m.cells) {
                size_t k = 0;
                while (k < ids.size() && ids[k] != c.style) ++k;
                if (k == ids.size()) {
                    ids.push_back(c.style);
                    bropty::Style s = v.styles[c.style];
                    if (s.link) {
                        // A row-specific id space.
                        const uint32_t local = s.link * 7 + uint32_t(abs % 5) + 1000;
                        if (const bropty::Hyperlink* h = t.hyperlink(s.link)) m.links.emplace_back(local, h->uri);
                        s.link = local;
                    }
                    m.styles.push_back(s);
                }
                c.style = uint32_t(k);
            }
            if (m.styles.empty()) m.styles.push_back(bropty::Style{});
            if (use_serials && abs >= top_) m.serial = t.row_stamp(int(abs - top_));
            rows_map_.emplace(abs, std::move(m));
        }
        if (copy_images_) {
            // The active screen's images, copied the way a client builds its
            // own layer from what it was sent (pixels shared).
            layer_.clear();
            t.images().for_each_image([this](const bropty::Image& img) {
                layer_.put(std::make_unique<bropty::Image>(img));
            });
            for (const bropty::Placement& p : t.images().placements()) layer_.add_placement(p);
            images_.layer = &layer_;
            images_.cell_width = t.image_cell_width();
            images_.cell_height = t.image_cell_height();
            images_.may_have_cells = t.may_have_image_cells();
        }
        t.advance_generation();
        ++changes_;
        if (resized) notify_after_resize();
        if (switched) notify_screen_switched();
    }

    // History withheld: row_at() returns nothing for history rows below
    // `from` (INT64_MAX: none held; first_row(): all held).
    void hold_history_from(int64_t from) {
        held_from_ = from;
        ++changes_;
    }
    // Copy the terminal's images on each pull() and offer them (source_images).
    void copy_images(bool on) { copy_images_ = on; }
    [[nodiscard]] const std::vector<std::pair<int64_t, int64_t>>& requests() const noexcept { return requests_; }
    void clear_requests() { requests_.clear(); }

    // RowSource
    int cols() const noexcept override { return cols_; }
    int rows() const noexcept override { return rows_; }
    int64_t first_row() const noexcept override { return first_; }
    int64_t screen_top_row() const noexcept override { return top_; }
    bool alt_screen_active() const noexcept override { return alt_; }
    bropty::RowView row_at(int64_t abs) const override {
        if (abs < top_ && abs < held_from_) return bropty::RowView{};
        auto it = rows_map_.find(abs);
        if (it == rows_map_.end()) return bropty::RowView{};
        const MRow& m = it->second;
        return bropty::RowView{m.cells.data(), int(m.cells.size()), m.flags, m.styles.data(),
                               m.clusters.empty() ? nullptr : &m.clusters};
    }
    uint64_t row_serial(int64_t abs) const noexcept override {
        auto it = rows_map_.find(abs);
        return it == rows_map_.end() ? 0 : it->second.serial;
    }
    const std::string* hyperlink_uri(int64_t row, uint32_t id) const noexcept override {
        auto it = rows_map_.find(row);
        if (it == rows_map_.end()) return nullptr;
        for (const auto& [k, uri] : it->second.links)
            if (k == id) return &uri;
        return nullptr;
    }
    uint64_t change_count() const noexcept override { return changes_; }
    bropty::CursorState cursor() const noexcept override { return cursor_; }
    const bropty::Modes& modes() const noexcept override { return modes_; }
    const bropty::Palette& palette() const noexcept override { return palette_; }
    void request_rows(int64_t first, int64_t end) const override { requests_.emplace_back(first, end); }
    bropty::SourceImages source_images() const noexcept override {
        return copy_images_ ? images_ : bropty::SourceImages{};
    }

private:
    bool copy_images_{false};
    bropty::ImageLayer layer_;
    bropty::SourceImages images_;
    bool pulled_{false};
    uint64_t numbering_{0};
    bool alt_{false};
    int cols_{0};
    int rows_{0};
    int64_t first_{0};
    int64_t top_{0};
    bropty::CursorState cursor_{};
    bropty::Modes modes_{};
    bropty::Palette palette_{bropty::Palette::standard()};
    std::map<int64_t, MRow> rows_map_;
    int64_t held_from_{INT64_MIN};
    uint64_t changes_{0};
    mutable std::vector<std::pair<int64_t, int64_t>> requests_;
};

} // namespace mirror
