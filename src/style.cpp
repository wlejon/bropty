#include "bropty/style.h"

#include <algorithm>

namespace bropty {

StyleTable::StyleTable() { clear(); }

void StyleTable::clear() {
    styles_.assign(1, Style{});
    in_use_.assign(1, 1);
    free_.clear();
    index_.assign(1024, 0);
    indexed_ = 0;
    sweep_threshold_ = 4096;
}

void StyleTable::index_insert(uint32_t hash, uint32_t id) {
    const size_t mask = index_.size() - 1;
    size_t i = hash & mask;
    while (index_[i] != 0) i = (i + 1) & mask;
    index_[i] = (uint64_t(hash) << 32) | id;
    ++indexed_;
}

void StyleTable::rebuild_index(size_t slots) {
    index_.assign(slots, 0);
    indexed_ = 0;
    for (uint32_t id = 1; id < styles_.size(); ++id) {
        if (in_use_[id]) index_insert(uint32_t(StyleHash{}(styles_[id])), id);
    }
}

uint32_t StyleTable::intern(const Style& s) {
    if (s.is_default()) return 0;
    const uint32_t hash = uint32_t(StyleHash{}(s));
    const size_t mask = index_.size() - 1;
    for (size_t i = hash & mask;; i = (i + 1) & mask) {
        const uint64_t e = index_[i];
        if (e == 0) break;
        if (uint32_t(e >> 32) == hash && styles_[uint32_t(e)] == s) return uint32_t(e);
    }
    uint32_t id;
    if (!free_.empty()) {
        id = free_.back();
        free_.pop_back();
        styles_[id] = s;
        in_use_[id] = 1;
    } else {
        id = uint32_t(styles_.size());
        styles_.push_back(s);
        in_use_.push_back(1);
    }
    if ((indexed_ + 1) * 2 > index_.size()) rebuild_index(index_.size() * 2);  // includes the new id
    else index_insert(hash, id);
    return id;
}

void StyleTable::sweep(const std::vector<uint8_t>& marked) {
    for (uint32_t id = 1; id < styles_.size(); ++id) {
        if (!in_use_[id]) continue;
        if (id < marked.size() && marked[id]) continue;
        in_use_[id] = 0;
        free_.push_back(id);
    }
    // Trim trailing free ids so the table can shrink after a burst.
    while (styles_.size() > 1 && !in_use_.back()) {
        styles_.pop_back();
        in_use_.pop_back();
    }
    free_.erase(std::remove_if(free_.begin(), free_.end(),
                               [&](uint32_t id) { return id >= styles_.size(); }),
                free_.end());
    size_t slots = 1024;
    while (slots < live() * 4) slots *= 2;
    rebuild_index(slots);
    sweep_threshold_ = std::max<size_t>(4096, live() * 2);
}

} // namespace bropty
