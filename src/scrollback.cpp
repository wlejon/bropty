#include "bropty/scrollback.h"

#include "logical_line.h"

#include <algorithm>
#include <cstring>

namespace bropty {

using detail::LogicalLine;

Scrollback::Scrollback(size_t max_rows, int cols) : max_rows_(max_rows), cols_(std::max(2, cols)) {}
Scrollback::~Scrollback() = default;

void Scrollback::set_max_rows(size_t n) {
    max_rows_ = n;
    enforce_capacity();
}

bool Scrollback::last_continued() const noexcept {
    return !recs_.empty() && (recs_.back().flags & kContinued) != 0;
}

const uint8_t* Scrollback::blob(const Rec& rec) const {
    return blocks_[size_t(rec.block - block_base_)].data.get() + rec.offset;
}

void Scrollback::append_bytes(Rec& rec, const std::string& bytes, bool new_line) {
    uint32_t add = uint32_t(bytes.size());
    if (new_line) {
        if (blocks_.empty() || blocks_.back().cap - blocks_.back().used < add) {
            Block b;
            b.cap = uint32_t(std::max<size_t>(kBlockSize, add));
            b.data = std::unique_ptr<uint8_t[]>(new uint8_t[b.cap]);  // not zero-filled
            blocks_.push_back(std::move(b));
        }
        Block& blk = blocks_.back();
        rec.block = block_base_ + blocks_.size() - 1;
        rec.offset = blk.used;
        rec.bytes = 0;
        blk.live++;
    }
    Block* blk = &blocks_[size_t(rec.block - block_base_)];
    bool at_end = rec.offset + rec.bytes == blk->used;
    if (!at_end || blk->cap - blk->used < add) {
        // Move the line to a fresh block with room to grow.
        Block nb;
        nb.cap = uint32_t(std::max<size_t>(kBlockSize, 2 * (size_t(rec.bytes) + add)));
        nb.data = std::unique_ptr<uint8_t[]>(new uint8_t[nb.cap]);
        if (rec.bytes) std::memcpy(nb.data.get(), blk->data.get() + rec.offset, rec.bytes);
        nb.used = rec.bytes;
        nb.live = 1;
        blk->live--;
        if (at_end) blk->used -= rec.bytes;
        blocks_.push_back(std::move(nb));
        rec.block = block_base_ + blocks_.size() - 1;
        rec.offset = 0;
        blk = &blocks_.back();
    }
    if (add) std::memcpy(blk->data.get() + blk->used, bytes.data(), add);
    blk->used += add;
    rec.bytes += add;
}

uint32_t Scrollback::rows_for(const Rec& rec) const {
    if (!(rec.flags & kHasWide)) {
        return std::max<uint32_t>(1, uint32_t((uint64_t(rec.columns) + uint64_t(cols_) - 1) / uint64_t(cols_)));
    }
    LogicalLine line;
    std::vector<Style> palette;
    decode_rec(rec, line, palette);
    return uint32_t(detail::count_wrapped_rows(line.cells.data(), line.cells.size(), cols_));
}

void Scrollback::push_row(const Cell* cells, int ncols, uint32_t row_flags, const ClusterMap* clusters,
                          const Style* styles) {
    if (max_rows_ == 0) return;
    bool wrapped = (row_flags & Row_Wrapped) != 0;
    int n = ncols;
    if (!wrapped) {
        // Trailing blanks: empty, narrow, default style (protection is irrelevant here).
        constexpr uint32_t kShape = Cell::kCpMask | (3u << 21);
        auto ink = [&](int x) { return (cells[x].bits & kShape) | cells[x].style; };
        while (n >= 4 && (ink(n - 1) | ink(n - 2) | ink(n - 3) | ink(n - 4)) == 0) n -= 4;
        while (n > 0 && ink(n - 1) == 0) --n;
    }
    scratch_.clear();
    const detail::EncodeStats stats = detail::encode_cells(
        scratch_, cells, size_t(n), styles,
        [&](size_t i) { return clusters ? clusters->find(int(i)) : std::u32string_view(); });
    const uint32_t columns = uint32_t(stats.columns);
    const bool has_wide = stats.has_wide;

    const uint32_t next_wide = (wrapped && ncols > 0 && cells[ncols - 1].wide() == Wide::SpacerHead) ? kNextWide : 0u;
    bool cont = last_continued();
    if (cont && (recs_.back().flags & kNextWide) && !(ncols > 0 && cells[0].wide() == Wide::Lead)) {
        // The previous row wrapped early for a wide character that has since
        // been overwritten: joining would rewrap this row's start onto it. End
        // the line there so history shows the rows as they were displayed.
        Rec& prev = recs_.back();
        prev.flags &= ~kContinued;
        cache_.erase(prev.seq);
        cont = false;
    }
    if (cont) {
        Rec& rec = recs_.back();
        append_bytes(rec, scratch_, false);
        rec.columns += columns;
        rec.rows += 1;
        total_rows_ += 1;
        rec.flags = (rec.flags & ~(kContinued | kNextWide)) | (row_flags & Row_SemanticMask) | (has_wide ? kHasWide : 0u);
        // A runaway line is split here (its tail starts a new record).
        rec.flags |= next_wide;
        if (wrapped && rec.rows < std::max<size_t>(1, max_rows_ / 2)) rec.flags |= kContinued;
        if (!cache_.empty()) cache_.erase(rec.seq);
    } else {
        Rec rec{};
        rec.seq = next_seq_++;
        rec.first_row = recs_.empty() ? 0 : recs_.back().first_row + recs_.back().rows;
        append_bytes(rec, scratch_, true);
        rec.columns = columns;
        rec.rows = 1;
        rec.flags = (row_flags & Row_SemanticMask) | (has_wide ? kHasWide : 0u) | (wrapped ? kContinued : 0u) | next_wide;
        recs_.push_back(rec);
        total_rows_ += 1;
    }
    enforce_capacity();
}

void Scrollback::push_line(const LogicalLine& line, const Style* styles) {
    if (max_rows_ == 0) return;
    scratch_.clear();
    detail::encode_cells(scratch_, line.cells.data(), line.cells.size(), styles, [&](size_t i) {
        auto it = std::lower_bound(line.clusters.begin(), line.clusters.end(), uint32_t(i),
                                   [](const auto& e, uint32_t k) { return e.first < k; });
        return (it != line.clusters.end() && it->first == i) ? std::u32string_view(it->second)
                                                             : std::u32string_view();
    });
    Rec rec{};
    rec.seq = next_seq_++;
    rec.first_row = recs_.empty() ? 0 : recs_.back().first_row + recs_.back().rows;
    append_bytes(rec, scratch_, true);
    uint32_t columns = 0;
    bool has_wide = false;
    for (const Cell& c : line.cells) {
        if (c.wide() == Wide::Lead) { has_wide = true; columns += 1; }
        else if (c.wide() == Wide::Narrow || c.wide() == Wide::SpacerTail) columns += 1;
    }
    rec.columns = columns;
    rec.flags = (line.flags & Row_SemanticMask) | (has_wide ? kHasWide : 0u) | (line.continued ? kContinued : 0u) |
                (line.continued && line.next_wide ? kNextWide : 0u);
    rec.rows = rows_for(rec);
    recs_.push_back(rec);
    total_rows_ += rec.rows;
    enforce_capacity();
}

void Scrollback::decode_rec(const Rec& rec, LogicalLine& out, std::vector<Style>& palette) const {
    out.clear();
    palette.clear();
    palette.push_back(Style{});
    const uint8_t* p = blob(rec);
    detail::decode_cells(p, p + rec.bytes, out, [&](const Style& s) -> uint32_t {
        for (size_t i = 0; i < palette.size(); ++i)
            if (palette[i] == s) return uint32_t(i);
        palette.push_back(s);
        return uint32_t(palette.size() - 1);
    });
    out.flags = rec.flags & Row_SemanticMask;
    out.continued = (rec.flags & kContinued) != 0;
    out.next_wide = (rec.flags & kNextWide) != 0;
}

void Scrollback::pop_last(LogicalLine& out, StyleTable& table) {
    out.clear();
    if (recs_.empty()) return;
    Rec rec = recs_.back();
    const uint8_t* p = blob(rec);
    detail::decode_cells(p, p + rec.bytes, out, [&](const Style& s) { return table.intern(s); });
    out.flags = rec.flags & Row_SemanticMask;
    out.continued = (rec.flags & kContinued) != 0;
    out.next_wide = (rec.flags & kNextWide) != 0;

    Block& blk = blocks_[size_t(rec.block - block_base_)];
    blk.live--;
    if (rec.offset + rec.bytes == blk.used) blk.used -= rec.bytes;
    total_rows_ -= rec.rows;
    if (!cache_.empty()) cache_.erase(rec.seq);
    recs_.pop_back();
    while (!blocks_.empty() && blocks_.back().live == 0 && blocks_.size() > 1) blocks_.pop_back();
    if (recs_.empty()) clear();
}

void Scrollback::pop_front() {
    const Rec& rec = recs_.front();
    blocks_[size_t(rec.block - block_base_)].live--;
    total_rows_ -= rec.rows;
    if (!cache_.empty()) cache_.erase(rec.seq);
    recs_.pop_front();
    while (blocks_.size() > 1 && blocks_.front().live == 0) {
        blocks_.pop_front();
        ++block_base_;
    }
}

void Scrollback::enforce_capacity() {
    while (!recs_.empty() && total_rows_ > max_rows_) pop_front();
    if (recs_.empty() && !blocks_.empty()) clear();
}

void Scrollback::clear() {
    recs_.clear();
    blocks_.clear();
    block_base_ = 0;
    total_rows_ = 0;
    cache_.clear();
}

void Scrollback::set_cols(int cols) {
    cols = std::max(2, cols);
    if (cols == cols_) return;
    cols_ = cols;
    cache_.clear();
    uint64_t row = recs_.empty() ? 0 : recs_.front().first_row;
    total_rows_ = 0;
    for (size_t i = 0; i < recs_.size(); ++i) {
        Rec& rec = recs_[i];
        rec.first_row = row;
        rec.rows = rows_for(rec);
        row += rec.rows;
        total_rows_ += rec.rows;
    }
    enforce_capacity();
}

size_t Scrollback::find_line(uint64_t global_row) const {
    // Last line whose first_row <= global_row (upper_bound - 1).
    size_t lo = 0, hi = recs_.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (global_row < recs_[mid].first_row) hi = mid;
        else lo = mid + 1;
    }
    return lo - 1;
}

const Scrollback::Decoded& Scrollback::decoded(size_t li) const {
    const Rec& rec = recs_[li];
    auto it = cache_.find(rec.seq);
    if (it != cache_.end()) return it->second;
    if (cache_.size() > 512) cache_.clear();

    Decoded d;
    LogicalLine line;
    decode_rec(rec, line, d.palette);
    std::vector<detail::WrapSpan> spans;
    detail::wrap_cells(line.cells.data(), line.cells.size(), cols_, spans);
    size_t nrows = rec.rows;
    d.cells.assign(nrows * size_t(cols_), Cell{});
    d.flags.assign(nrows, 0);
    if (!line.clusters.empty()) d.clusters.resize(nrows);
    size_t ci = 0;
    for (size_t r = 0; r < nrows && r < spans.size(); ++r) {
        const auto& sp = spans[r];
        Cell* dst = &d.cells[r * size_t(cols_)];
        std::copy(line.cells.begin() + sp.begin, line.cells.begin() + sp.end, dst);
        if (sp.spacer_head) dst[sp.end - sp.begin] = Cell::make(0, 0, Wide::SpacerHead);
        while (ci < line.clusters.size() && line.clusters[ci].first < sp.end) {
            if (line.clusters[ci].first >= sp.begin)
                d.clusters[r].set(int(line.clusters[ci].first - sp.begin), line.clusters[ci].second);
            ++ci;
        }
        bool last = (r + 1 == nrows);
        // Cells == columns here (SpacerHeads are not stored).
        if (last && line.next_wide && sp.end - sp.begin + 1 == uint32_t(cols_))
            dst[sp.end - sp.begin] = Cell::make(0, 0, Wide::SpacerHead);
        d.flags[r] = (last && !line.continued) ? 0u : uint32_t(Row_Wrapped);
        if (r == 0) d.flags[r] |= line.flags;
    }
    return cache_.emplace(rec.seq, std::move(d)).first->second;
}

RowView Scrollback::row(size_t i) const {
    RowView v;
    if (i >= total_rows_) return v;
    uint64_t global = recs_.front().first_row + i;
    size_t li = find_line(global);
    const Decoded& d = decoded(li);
    size_t r = size_t(global - recs_[li].first_row);
    v.cells = &d.cells[r * size_t(cols_)];
    v.cols = cols_;
    v.flags = d.flags[r];
    v.styles = d.palette.data();
    v.clusters = d.clusters.empty() ? nullptr : &d.clusters[r];
    return v;
}

size_t Scrollback::memory_bytes() const noexcept {
    size_t n = recs_.size() * sizeof(Rec);
    for (const Block& b : blocks_) n += b.cap;
    return n;
}

void Scrollback::collect_links(std::vector<uint32_t>& out) const {
    for (size_t i = 0; i < recs_.size(); ++i) {
        const Rec& rec = recs_[i];
        const uint8_t* p = blob(rec);
        detail::for_each_link(p, p + rec.bytes, [&](uint32_t id) { out.push_back(id); });
    }
}

} // namespace bropty
