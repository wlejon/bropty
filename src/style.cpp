#include "bropty/style.h"

#include <algorithm>

namespace bropty {

StyleTable::StyleTable() { clear(); }

void StyleTable::clear() {
    styles_.assign(1, Style{});
    in_use_.assign(1, 1);
    free_.clear();
    index_.clear();
    sweep_threshold_ = 4096;
}

uint32_t StyleTable::intern(const Style& s) {
    if (s.is_default()) return 0;
    auto it = index_.find(s);
    if (it != index_.end()) return it->second;
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
    index_.emplace(s, id);
    return id;
}

void StyleTable::sweep(const std::vector<uint8_t>& marked) {
    for (uint32_t id = 1; id < styles_.size(); ++id) {
        if (!in_use_[id]) continue;
        if (id < marked.size() && marked[id]) continue;
        index_.erase(styles_[id]);
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
    sweep_threshold_ = std::max<size_t>(4096, live() * 2);
}

} // namespace bropty
