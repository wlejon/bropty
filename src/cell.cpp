#include "bropty/cell.h"

#include <algorithm>

namespace bropty {

namespace {
auto lower(std::vector<std::pair<uint16_t, std::u32string>>& v, int col) {
    return std::lower_bound(v.begin(), v.end(), col,
                            [](const auto& e, int c) { return int(e.first) < c; });
}
auto lower(const std::vector<std::pair<uint16_t, std::u32string>>& v, int col) {
    return std::lower_bound(v.begin(), v.end(), col,
                            [](const auto& e, int c) { return int(e.first) < c; });
}
} // namespace

std::u32string_view ClusterMap::find(int col) const noexcept {
    auto it = lower(entries_, col);
    if (it != entries_.end() && it->first == col) return it->second;
    return {};
}

void ClusterMap::set(int col, std::u32string_view tail) {
    auto it = lower(entries_, col);
    if (it != entries_.end() && it->first == col) {
        it->second.assign(tail);
    } else {
        entries_.insert(it, {uint16_t(col), std::u32string(tail)});
    }
}

void ClusterMap::append(int col, char32_t cp) {
    auto it = lower(entries_, col);
    if (it != entries_.end() && it->first == col) {
        it->second.push_back(cp);
    } else {
        entries_.insert(it, {uint16_t(col), std::u32string(1, cp)});
    }
}

void ClusterMap::erase(int col) {
    auto it = lower(entries_, col);
    if (it != entries_.end() && it->first == col) entries_.erase(it);
}

void ClusterMap::erase_range(int first, int last) {
    if (first >= last) return;
    auto a = lower(entries_, first);
    auto b = lower(entries_, last);
    entries_.erase(a, b);
}

void ClusterMap::shift(int first, int last, int delta) {
    for (auto& e : entries_) {
        if (e.first >= first && e.first < last) e.first = uint16_t(e.first + delta);
    }
    std::sort(entries_.begin(), entries_.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
}

void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(char(cp));
    } else if (cp < 0x800) {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

std::u32string RowView::cluster(int col) const {
    std::u32string out;
    const Cell& c = cells[col];
    if (c.is_empty() || c.is_spacer()) return out;
    out.push_back(c.cp());
    if (c.has_cluster() && clusters) out.append(clusters->find(col));
    return out;
}

std::string RowView::text(bool trim) const {
    std::string out;
    for (int x = 0; x < cols; ++x) {
        const Cell& c = cells[x];
        if (c.is_spacer()) continue;
        if (c.is_empty()) {
            out.push_back(' ');
            continue;
        }
        append_utf8(out, c.cp());
        if (c.has_cluster() && clusters) {
            for (char32_t cp : clusters->find(x)) append_utf8(out, cp);
        }
    }
    if (trim) {
        while (!out.empty() && out.back() == ' ') out.pop_back();
    }
    return out;
}

} // namespace bropty
