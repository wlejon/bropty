#include "bropty/links.h"

#include "bropty/terminal.h"
#include "buffer_lines.h"

#include <algorithm>

namespace bropty {

using detail::BufferLines;
using detail::Line;
using detail::LineText;

namespace {

bool is_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
bool is_alnum(char c) noexcept { return is_alpha(c) || is_digit(c); }

// RFC 3986 characters plus anything non-ASCII (IRIs).
bool is_url_char(char c) noexcept {
    const auto u = uint8_t(c);
    if (u >= 0x80) return true;
    if (is_alnum(c)) return true;
    switch (c) {
    case '-': case '.': case '_': case '~': case ':': case '/': case '?': case '#': case '[': case ']':
    case '@': case '!': case '$': case '&': case '\'': case '(': case ')': case '*': case '+': case ',':
    case ';': case '=': case '%':
        return true;
    default: return false;
    }
}

bool is_path_delim(char c) noexcept {
    const auto u = uint8_t(c);
    if (u <= 0x20 || u == 0x7F) return true;
    switch (c) {
    case '"': case '\'': case '`': case '(': case ')': case '[': case ']': case '{': case '}': case '<':
    case '>': case '|': case ',': case ';': case '=':
        return true;
    default: return false;
    }
}

// Drop trailing punctuation that belongs to the sentence, not the link.
size_t trim_end(std::string_view s, size_t b, size_t e) {
    for (;;) {
        if (e <= b) return e;
        const char c = s[e - 1];
        if (c == '.' || c == ',' || c == ':' || c == ';' || c == '!' || c == '?' || c == '\'' || c == '"' ||
            c == '>') {
            --e;
            continue;
        }
        auto unbalanced = [&](char open, char close) {
            long depth = 0;
            for (size_t i = b; i < e; ++i) depth += s[i] == open ? 1 : s[i] == close ? -1 : 0;
            return depth < 0;
        };
        if ((c == ')' && unbalanced('(', ')')) || (c == ']' && unbalanced('[', ']')) ||
            (c == '}' && unbalanced('{', '}'))) {
            --e;
            continue;
        }
        return e;
    }
}

bool starts_with_ci(std::string_view s, size_t i, std::string_view p) {
    if (s.size() - i < p.size()) return false;
    for (size_t k = 0; k < p.size(); ++k) {
        char a = s[i + k];
        if (a >= 'A' && a <= 'Z') a = char(a + 32);
        if (a != p[k]) return false;
    }
    return true;
}

// A URL starting at i, or 0.
size_t match_url(std::string_view s, size_t i, std::string& target) {
    if (i > 0 && (is_alnum(s[i - 1]) || s[i - 1] == '+' || s[i - 1] == '-' || s[i - 1] == '.')) return 0;
    size_t body = 0;
    if (is_alpha(s[i])) {
        size_t j = i + 1;
        while (j < s.size() && (is_alnum(s[j]) || s[j] == '+' || s[j] == '-' || s[j] == '.')) ++j;
        if (j - i >= 2 && s.substr(j, 3) == "://") {
            body = j + 3;
        } else if (j < s.size() && s[j] == ':' &&
                   (starts_with_ci(s, i, "mailto:") || starts_with_ci(s, i, "news:") || starts_with_ci(s, i, "tel:"))) {
            body = j + 1;
        }
    }
    bool www = false;
    if (!body && starts_with_ci(s, i, "www.")) {
        body = i + 4;
        www = true;
    }
    if (!body) return 0;
    size_t e = body;
    while (e < s.size() && is_url_char(s[e])) ++e;
    e = trim_end(s, body, e);
    if (e <= body) return 0;
    if (www && s.substr(body, e - body).find('.') == std::string_view::npos) return 0;
    target = www ? "http://" + std::string(s.substr(i, e - i)) : std::string(s.substr(i, e - i));
    return e - i;
}

bool looks_like_path(std::string_view tok) {
    if (tok.size() < 2) return false;
    if (tok[0] == '/') return tok[1] != '/';
    if (tok.substr(0, 2) == "~/" || tok.substr(0, 2) == "./" || tok.substr(0, 3) == "../") return tok.size() > 2;
    if (tok.size() >= 3 && is_alpha(tok[0]) && tok[1] == ':' && (tok[2] == '\\' || tok[2] == '/')) return true;
    // Relative: needs a slash and a last component with an extension (or a trailing slash).
    if (!(is_alnum(tok[0]) || tok[0] == '_' || tok[0] == '.')) return false;
    const size_t slash = tok.find('/');
    if (slash == std::string_view::npos || tok.find("//") != std::string_view::npos) return false;
    if (tok.back() == '/') return true;
    const size_t last = tok.rfind('/');
    const size_t dot = tok.rfind('.');
    return dot != std::string_view::npos && dot > last + 1 && dot + 1 < tok.size();
}

// Strip trailing punctuation, then split off a ":line[:col]" suffix.
size_t path_end(std::string_view s, size_t b, size_t e, size_t& core_end) {
    while (e > b && (s[e - 1] == '.' || s[e - 1] == ':' || s[e - 1] == '!' || s[e - 1] == '?')) --e;
    core_end = e;
    // Up to two trailing ":<digits>" groups.
    size_t k = e;
    for (int g = 0; g < 2; ++g) {
        size_t d = k;
        while (d > b && is_digit(s[d - 1])) --d;
        if (d < k && d > b && s[d - 1] == ':') {
            k = d - 1;
            core_end = k;
        } else {
            break;
        }
    }
    return e;
}

void add_osc8(const Terminal& t, const Line& l, size_t a, size_t b, uint32_t id, std::vector<LinkHit>& out) {
    LinkHit h;
    h.range = RowRange{l.pos_of(a), l.end_pos(b)};
    h.kind = LinkKind::Hyperlink;
    h.link_id = id;
    if (const Hyperlink* link = t.hyperlink(id)) h.target = link->uri;
    out.push_back(std::move(h));
}

// OSC 8 runs of a line, as cell ranges.
template <class F>
void for_each_osc8(const Line& l, F&& f) {
    size_t i = 0;
    while (i < l.size()) {
        const uint32_t id = l.style(i).link;
        size_t j = i + 1;
        while (j < l.size() && l.style(j).link == id) ++j;
        if (id) f(i, j, id);
        i = j;
    }
}

void line_links(const Terminal& t, const Line& l, std::vector<LinkHit>& out) {
    std::vector<std::pair<size_t, size_t>> osc;
    for_each_osc8(l, [&](size_t a, size_t b, uint32_t id) {
        add_osc8(t, l, a, b, id, out);
        osc.emplace_back(a, b);
    });
    LineText lt;
    lt.build(l);
    std::vector<TextLink> found;
    detect_links(lt.text, found);
    for (const TextLink& tl : found) {
        size_t c0, c1;
        lt.cells_of(l, tl.begin, tl.end, c0, c1);
        bool covered = std::any_of(osc.begin(), osc.end(), [&](const auto& r) { return r.first < c1 && c0 < r.second; });
        if (covered) continue;
        LinkHit h;
        h.range = RowRange{l.pos_of(c0), l.end_pos(c1)};
        h.kind = tl.kind;
        h.target = tl.target;
        out.push_back(std::move(h));
    }
}

} // namespace

void detect_links(std::string_view s, std::vector<TextLink>& out) {
    std::vector<std::pair<size_t, size_t>> urls;
    for (size_t i = 0; i < s.size();) {
        std::string target;
        if (size_t n = match_url(s, i, target)) {
            out.push_back(TextLink{i, i + n, LinkKind::Url, std::move(target)});
            urls.emplace_back(i, i + n);
            i += n;
        } else {
            ++i;
        }
    }
    // Paths: delimiter-separated tokens that are not (part of) a URL.
    for (size_t i = 0; i < s.size();) {
        if (is_path_delim(s[i])) {
            ++i;
            continue;
        }
        size_t e = i;
        while (e < s.size() && !is_path_delim(s[e])) ++e;
        const bool in_url = std::any_of(urls.begin(), urls.end(), [&](const auto& u) { return u.first < e && i < u.second; });
        if (!in_url) {
            size_t core = e;
            size_t end = path_end(s, i, e, core);
            if (end > i && looks_like_path(s.substr(i, core - i))) out.push_back(TextLink{i, end, LinkKind::Path, std::string(s.substr(i, end - i))});
        }
        i = e;
    }
    std::sort(out.begin(), out.end(), [](const TextLink& a, const TextLink& b) { return a.begin < b.begin; });
}

std::optional<LinkHit> link_at(const Terminal& t, RowPos cell) {
    BufferLines bl(t);
    Line l;
    if (!bl.line_at_row(cell.row, l)) return std::nullopt;
    size_t o = l.offset_of(cell);
    if (o >= l.size()) return std::nullopt;
    if (o > 0 && l.cell(o).wide() == Wide::SpacerTail) --o;
    std::vector<LinkHit> hits;
    if (const uint32_t id = l.style(o).link) {
        size_t a = o, b = o + 1;
        while (a > 0 && l.style(a - 1).link == id) --a;
        while (b < l.size() && l.style(b).link == id) ++b;
        add_osc8(t, l, a, b, id, hits);
        return hits.front();
    }
    line_links(t, l, hits);
    const RowPos at = l.pos_of(o);
    for (LinkHit& h : hits)
        if (h.kind != LinkKind::Hyperlink && h.range.contains(at.row, at.col)) return std::move(h);
    return std::nullopt;
}

void links_in_rows(const Terminal& t, int64_t row0, int64_t row1, std::vector<LinkHit>& out) {
    BufferLines bl(t);
    Line l;
    int64_t row = std::max(row0, t.first_row());
    while (row < row1 && bl.line_at_row(row, l)) {
        line_links(t, l, out);
        row = l.end_row();
    }
}

} // namespace bropty
