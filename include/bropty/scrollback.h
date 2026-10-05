#pragma once
// Scrollback: rows scrolled off the top of the primary screen, stored as
// compactly encoded *logical* lines (soft-wrapped rows joined). Because lines
// are stored unwrapped, a width change only recomputes how many rows each
// line occupies; nothing is re-encoded. Blobs live in large shared blocks
// (no per-line heap allocation); a plain ASCII line costs about its length in
// bytes plus a 40-byte record.
//
// Capacity is counted in physical rows at the current width. When it is
// exceeded the oldest lines are dropped. A single line that alone grows past
// half the capacity stops being continued (it is split there), so appending
// to it stays O(row) and evicting it never empties the whole history.

#include "bropty/cell.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bropty {

namespace detail {
struct LogicalLine;

// A growable byte buffer that, unlike std::string::resize, does not zero the
// space it grows into (the line encoder reserves worst-case room per run).
class ByteBuf {
public:
    [[nodiscard]] const char* data() const noexcept { return p_.get(); }
    [[nodiscard]] char* mdata() noexcept { return p_.get(); }
    [[nodiscard]] size_t size() const noexcept { return n_; }
    void clear() noexcept { n_ = 0; }
    // Room for `extra` more bytes; returns where they go. Commit with set_end().
    char* reserve(size_t extra) {
        if (n_ + extra > cap_) {
            size_t cap = cap_ ? cap_ : 256;
            while (cap < n_ + extra) cap *= 2;
            std::unique_ptr<char[]> q(new char[cap]);
            if (n_) std::memcpy(q.get(), p_.get(), n_);
            p_ = std::move(q);
            cap_ = cap;
        }
        return p_.get() + n_;
    }
    void set_end(const char* end) noexcept { n_ = size_t(end - p_.get()); }

private:
    std::unique_ptr<char[]> p_;
    size_t n_{0};
    size_t cap_{0};
};

// A growable circular buffer: deque operations without a heap allocation per
// element (MSVC's std::deque stores one 40-byte record per block).
template <class T>
class Ring {
public:
    [[nodiscard]] size_t size() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    T& operator[](size_t i) noexcept { return buf_[(head_ + i) & (buf_.size() - 1)]; }
    const T& operator[](size_t i) const noexcept { return buf_[(head_ + i) & (buf_.size() - 1)]; }
    T& front() noexcept { return (*this)[0]; }
    const T& front() const noexcept { return (*this)[0]; }
    T& back() noexcept { return (*this)[count_ - 1]; }
    const T& back() const noexcept { return (*this)[count_ - 1]; }
    void push_back(const T& v) {
        if (count_ == buf_.size()) grow();
        buf_[(head_ + count_) & (buf_.size() - 1)] = v;
        ++count_;
    }
    void pop_front() noexcept {
        head_ = (head_ + 1) & (buf_.size() - 1);
        --count_;
    }
    void pop_back() noexcept { --count_; }
    void clear() noexcept {
        head_ = 0;
        count_ = 0;
    }

private:
    void grow() {
        std::vector<T> next(buf_.empty() ? 64 : buf_.size() * 2);
        for (size_t i = 0; i < count_; ++i) next[i] = (*this)[i];
        buf_ = std::move(next);
        head_ = 0;
    }
    std::vector<T> buf_;
    size_t head_{0};
    size_t count_{0};
};
} // namespace detail

class Scrollback {
public:
    explicit Scrollback(size_t max_rows = 10000, int cols = 80);
    ~Scrollback();
    Scrollback(const Scrollback&) = delete;
    Scrollback& operator=(const Scrollback&) = delete;

    [[nodiscard]] size_t max_rows() const noexcept { return max_rows_; }
    void set_max_rows(size_t n);
    [[nodiscard]] int cols() const noexcept { return cols_; }
    // Change the wrap width; recomputes row counts (and trims to capacity).
    void set_cols(int cols);

    // Append one screen row. If the previous pushed row soft-wrapped, this row
    // continues that logical line. `styles` resolves the row's style ids.
    void push_row(const Cell* cells, int ncols, uint32_t row_flags, const ClusterMap* clusters,
                  const Style* styles);

    [[nodiscard]] size_t rows() const noexcept { return total_rows_; }
    [[nodiscard]] size_t lines() const noexcept { return recs_.size(); }
    [[nodiscard]] bool empty() const noexcept { return recs_.empty(); }

    // Row i of the history (0 = oldest), wrapped at the current width. The
    // view is valid until the scrollback is next modified.
    [[nodiscard]] RowView row(size_t i) const;

    // Whether the newest line continues onto the screen.
    [[nodiscard]] bool last_continued() const noexcept;

    // Rows / lines ever dropped from the front (capacity or clear()). They
    // number history absolutely: row i is the (dropped_rows() + i)th row
    // kept, line k the (dropped_lines() + k)th line (see position.h).
    [[nodiscard]] uint64_t dropped_rows() const noexcept { return dropped_rows_; }
    [[nodiscard]] uint64_t dropped_lines() const noexcept { return dropped_lines_; }
    // Logical line access: line k (0 = oldest) and the rows it occupies.
    [[nodiscard]] size_t line_of_row(size_t i) const;
    [[nodiscard]] size_t line_first_row(size_t k) const noexcept {
        return size_t(recs_[k].first_row - recs_.front().first_row);
    }
    [[nodiscard]] size_t line_rows(size_t k) const noexcept { return recs_[k].rows; }
    // Semantic Row_* flags of the line (any of its rows).
    [[nodiscard]] uint32_t line_flags(size_t k) const noexcept { return recs_[k].flags & Row_SemanticMask; }
    // Whether line k soft-wraps into the screen (only the newest can).
    [[nodiscard]] bool line_continued(size_t k) const noexcept { return (recs_[k].flags & kContinued) != 0; }
    // Decode line k; cell styles index `palette` (palette[0] is the default style).
    void decode_line(size_t k, detail::LogicalLine& out, std::vector<Style>& palette) const;

    // Reflow support: remove the newest line and decode it with styles
    // interned through `table`.
    void pop_last(detail::LogicalLine& out, StyleTable& table);
    // Append a whole logical line (cell styles index `styles`).
    void push_line(const detail::LogicalLine& line, const Style* styles);

    void clear();

    // Approximate heap bytes held (blocks + records).
    [[nodiscard]] size_t memory_bytes() const noexcept;

    // Visit every hyperlink id referenced from history.
    template <class F>
    void for_each_link(F&& f) const;
    void collect_links(std::vector<uint32_t>& out) const;

private:
    struct Rec {
        uint64_t seq;        // monotonic line id (cache key)
        uint64_t first_row;  // global row counter of the line's first row
        uint64_t block;      // block sequence number
        uint32_t offset;
        uint32_t bytes;
        uint32_t columns;    // total columns of content (wide = 2)
        uint32_t rows;       // rows at the current width
        uint32_t flags;      // Row_* semantic flags | kContinued | kHasWide
    };
    struct Block {
        std::unique_ptr<uint8_t[]> data;
        uint32_t cap{0};
        uint32_t used{0};
        uint32_t live{0};
    };
    struct Decoded {
        std::vector<Cell> cells;  // rows * cols
        std::vector<uint32_t> flags;
        std::vector<ClusterMap> clusters;  // empty unless the line has clusters
        std::vector<Style> palette;
    };

    static constexpr uint32_t kContinued = 1u << 30;
    static constexpr uint32_t kHasWide = 1u << 29;
    // The line's last row ended in a SpacerHead (its continuation started, or
    // once started, with a wide cell): a last row one column short of the
    // width is shown ending in a SpacerHead again.
    static constexpr uint32_t kNextWide = 1u << 28;
    static constexpr size_t kBlockSize = 64 * 1024;

    void append_bytes(Rec& rec, const detail::ByteBuf& bytes, bool new_line);
    uint32_t rows_for(const Rec& rec) const;
    void decode_rec(const Rec& rec, detail::LogicalLine& out, std::vector<Style>& palette) const;
    void pop_front();
    void enforce_capacity();
    const uint8_t* blob(const Rec& rec) const;
    size_t find_line(uint64_t global_row) const;
    const Decoded& decoded(size_t line_index) const;

    size_t max_rows_;
    int cols_;
    detail::Ring<Rec> recs_;
    std::deque<Block> blocks_;
    uint64_t block_base_{0};  // sequence number of blocks_.front()
    uint64_t next_seq_{0};
    size_t total_rows_{0};
    uint64_t dropped_rows_{0};
    uint64_t dropped_lines_{0};
    detail::ByteBuf scratch_;

    mutable std::unordered_map<uint64_t, Decoded> cache_;
};

template <class F>
void Scrollback::for_each_link(F&& f) const {
    std::vector<uint32_t> ids;
    collect_links(ids);
    for (uint32_t id : ids) f(id);
}

} // namespace bropty
