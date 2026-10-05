// Selection extraction: plain text and styled HTML.
#include "bropty/selection.h"

#include "bropty/terminal.h"
#include "buffer_lines.h"

#include <algorithm>
#include <cstdio>

namespace bropty {

using detail::BufferLines;
using detail::Line;

namespace {

// Receives the selected text in order, in runs of one style.
// Receives the selected text in order, in runs of one style (`uri`: the
// style's OSC 8 link target, or nullptr).
struct Sink {
    virtual ~Sink() = default;
    virtual void run(std::string_view utf8, const Style& s, const std::string* uri) = 0;
    virtual void newline() = 0;
};

struct PlainSink final : Sink {
    std::string out;
    std::string_view nl;
    explicit PlainSink(std::string_view n) : nl(n) {}
    void run(std::string_view utf8, const Style&, const std::string*) override { out.append(utf8); }
    void newline() override { out.append(nl); }
};

// Cells extraction drops from the end of a line when trimming: blanks, and
// image cells (a picture has no text).
bool trailing_blank(const Cell& c) noexcept {
    return c.is_empty() || (c.cp() == U' ' && !c.has_cluster()) || detail::is_image_cell(c);
}

// One past the last cell of a line with text in it.
size_t text_end(const Line& l) noexcept {
    size_t i = l.size();
    while (i > 0) {
        const Cell& c = l.cell(i - 1);
        if (c.wide() == Wide::SpacerTail) {
            if (i >= 2 && detail::is_image_cell(l.cell(i - 2))) {
                i -= 2;
                continue;
            }
            break;
        }
        if (!trailing_blank(c)) break;
        --i;
    }
    return i;
}

// Emit cells [a, b) of a cell array, grouped into runs of equal style.
// Image cells are skipped.
template <class CellAt, class StyleAt, class TailAt, class UriAt>
void emit_cells(Sink& sink, size_t a, size_t b, CellAt&& cell_at, StyleAt&& style_at, TailAt&& tail_at,
                UriAt&& uri_at) {
    std::string run;
    const Style* cur = nullptr;
    size_t cur_i = 0;
    for (size_t i = a; i < b; ++i) {
        const Cell& c = cell_at(i);
        if (c.is_spacer() || detail::is_image_cell(c)) continue;
        const Style& s = style_at(i);
        if (cur && !(s == *cur)) {
            sink.run(run, *cur, uri_at(cur_i));
            run.clear();
        }
        if (!cur || !(s == *cur)) cur_i = i;
        cur = &s;
        if (c.is_empty()) {
            run.push_back(' ');
            continue;
        }
        append_utf8(run, c.cp());
        if (c.has_cluster())
            for (char32_t t : tail_at(i)) append_utf8(run, t);
    }
    if (cur && !run.empty()) sink.run(run, *cur, uri_at(cur_i));
}

void emit_stream(const RowSource& t, RowRange r, bool trim, Sink& sink) {
    BufferLines bl(t);
    Line l;
    if (r.start.row < t.first_row()) r.start = RowPos{t.first_row(), 0};
    if (!(r.start < r.end) || !bl.line_at_row(r.start.row, l)) return;
    bool first = true;
    for (;;) {
        if (!first) sink.newline();
        size_t a = first ? l.offset_of(r.start) : 0;
        const bool last = r.end.row < l.end_row();
        size_t b = last ? l.offset_of(r.end) : l.size();
        l.widen(a, b);
        if (trim) b = std::min(b, std::max(a, text_end(l)));
        emit_cells(
            sink, a, b, [&](size_t i) -> const Cell& { return l.cell(i); },
            [&](size_t i) -> const Style& { return l.style(i); }, [&](size_t i) { return l.tail(i); },
            [&](size_t i) { return l.link_uri(l.style(i).link); });
        if (last) return;
        const int64_t next = bl.next_line(l);
        if (!bl.line(next, l)) return;
        first = false;
    }
}

void emit_block(const RowSource& t, RowRange r, bool trim, Sink& sink) {
    const int64_t r0 = std::max(r.start.row, t.first_row());
    const int64_t r1 = std::min(r.end.row, t.end_row() - 1);
    for (int64_t row = r0; row <= r1; ++row) {
        if (row != r0) sink.newline();
        const RowView v = t.row_at(row);
        if (!v.cells) continue;
        int c0 = std::clamp(r.start.col, 0, v.cols);
        int c1 = std::clamp(r.end.col, 0, v.cols);
        if (c0 > 0 && c0 < v.cols && v.cells[c0].wide() == Wide::SpacerTail) --c0;
        if (c1 > 0 && c1 < v.cols && v.cells[c1].wide() == Wide::SpacerTail) ++c1;
        if (trim) {
            while (c1 > c0) {
                const Cell& c = v.cells[c1 - 1];
                if (c.wide() == Wide::SpacerTail && c1 - 1 > c0 && detail::is_image_cell(v.cells[c1 - 2])) {
                    c1 -= 2;
                    continue;
                }
                if (c.wide() == Wide::SpacerTail || c.wide() == Wide::Lead) {
                    if (!detail::is_image_cell(c)) break;
                }
                if (!trailing_blank(c)) break;
                --c1;
            }
        }
        emit_cells(
            sink, size_t(c0), size_t(c1), [&](size_t i) -> const Cell& { return v.cells[i]; },
            [&](size_t i) -> const Style& { return v.style(int(i)); },
            [&](size_t i) { return v.clusters ? v.clusters->find(int(i)) : std::u32string_view(); },
            [&](size_t i) { return t.hyperlink_uri(row, v.style(int(i)).link); });
    }
}

// ---- HTML

void hex(std::string& out, Rgb c) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    out += buf;
}

Rgb mix(Rgb a, Rgb b) {  // halfway, for SGR 2 (dim)
    return Rgb{uint8_t((a.r + b.r) / 2), uint8_t((a.g + b.g) / 2), uint8_t((a.b + b.b) / 2)};
}

void escape_html(std::string& out, std::string_view s) {
    for (char ch : s) {
        switch (ch) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out.push_back(ch);
        }
    }
}

struct HtmlSink final : Sink {
    const Palette& pal;
    std::string out;
    explicit HtmlSink(const Palette& p) : pal(p) {}

    void run(std::string_view utf8, const Style& s, const std::string* link) override {
        std::string css;
        Rgb fg = pal.resolve_fg(s.fg);
        Rgb bg = pal.resolve_bg(s.bg);
        bool has_bg = !s.bg.is_default();
        if (s.has(Attr_Inverse)) {
            std::swap(fg, bg);
            has_bg = true;
        }
        if (s.has(Attr_Dim)) fg = mix(fg, bg);
        if (!s.fg.is_default() || s.has(Attr_Inverse) || s.has(Attr_Dim)) {
            css += "color:";
            hex(css, fg);
            css += ';';
        }
        if (has_bg) {
            css += "background-color:";
            hex(css, bg);
            css += ';';
        }
        if (s.has(Attr_Bold)) css += "font-weight:bold;";
        if (s.has(Attr_Italic)) css += "font-style:italic;";
        if (s.has(Attr_Invisible)) css += "visibility:hidden;";
        std::string deco;
        if (s.underline != Underline::None) deco += " underline";
        if (s.has(Attr_Strike)) deco += " line-through";
        if (s.has(Attr_Overline)) deco += " overline";
        if (!deco.empty()) {
            css += "text-decoration-line:" + deco.substr(1) + ';';
            switch (s.underline) {
            case Underline::Double: css += "text-decoration-style:double;"; break;
            case Underline::Curly: css += "text-decoration-style:wavy;"; break;
            case Underline::Dotted: css += "text-decoration-style:dotted;"; break;
            case Underline::Dashed: css += "text-decoration-style:dashed;"; break;
            default: break;
            }
            if (!s.underline_color.is_default()) {
                css += "text-decoration-color:";
                hex(css, pal.resolve_fg(s.underline_color));
                css += ';';
            }
        }
        if (link) {
            out += "<a href=\"";
            escape_html(out, *link);
            out += "\">";
        }
        if (!css.empty()) out += "<span style=\"" + css + "\">";
        escape_html(out, utf8);
        if (!css.empty()) out += "</span>";
        if (link) out += "</a>";
    }
    void newline() override { out.push_back('\n'); }
};

} // namespace

std::string Selection::text(const TextOptions& o) const {
    PlainSink sink(o.newline);
    if (!active_) return sink.out;
    if (mode_ == SelectionMode::Block) emit_block(t_, range_, o.trim_trailing, sink);
    else emit_stream(t_, range_, o.trim_trailing, sink);
    return std::move(sink.out);
}

std::string Selection::html(const Palette& palette) const {
    HtmlSink sink(palette);
    if (!active_) return {};
    if (mode_ == SelectionMode::Block) emit_block(t_, range_, true, sink);
    else emit_stream(t_, range_, true, sink);
    std::string out = "<pre style=\"color:";
    hex(out, palette.foreground);
    out += ";background-color:";
    hex(out, palette.background);
    out += "\">";
    out += sink.out;
    out += "</pre>";
    return out;
}

} // namespace bropty
