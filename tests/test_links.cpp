// Link detection: URLs and paths in text, OSC 8 hyperlinks, links that wrap
// across rows, and the hover range a TerminalView keeps current.
#include "bropty/view.h"
#include "term_helpers.h"

using namespace bropty;

namespace {

std::vector<std::string> targets(std::string_view s, LinkKind* kinds = nullptr) {
    std::vector<TextLink> out;
    detect_links(s, out);
    std::vector<std::string> v;
    for (size_t i = 0; i < out.size(); ++i) {
        v.push_back(out[i].target);
        if (kinds) kinds[i] = out[i].kind;
        if (std::string(s.substr(out[i].begin, out[i].end - out[i].begin)) != out[i].target &&
            out[i].target.rfind("http://www.", 0) != 0)
            CHECK(false);
    }
    return v;
}

using SV = std::vector<std::string>;

void detection() {
    CHECK(targets("see https://example.com/a_(b). ok") == SV{"https://example.com/a_(b)"});
    CHECK(targets("(http://x.y/z)") == SV{"http://x.y/z"});
    CHECK(targets("\"https://q.io/?a=1&b=2\",") == SV{"https://q.io/?a=1&b=2"});
    CHECK(targets("go to www.foo.com, now") == SV{"http://www.foo.com"});
    CHECK(targets("www.nodot") == SV{});
    CHECK(targets("mail mailto:a@b.c!") == SV{"mailto:a@b.c"});
    CHECK(targets("git+ssh://h/r.git and ftp://f") == SV({"git+ssh://h/r.git", "ftp://f"}));
    CHECK(targets("xhttp://a") == SV{"xhttp://a"});  // any RFC 3986 scheme
    CHECK(targets("1http://a") == SV{});             // a scheme starts at a word start
    CHECK(targets("https://\xe4\xbe\x8b\xe3\x81\x88.jp/\xe3\x83\x91\xe3\x82\xb9 x") ==
          SV{"https://\xe4\xbe\x8b\xe3\x81\x88.jp/\xe3\x83\x91\xe3\x82\xb9"});
    LinkKind k[4];
    CHECK(targets("src/main.cpp:12:5: error", k) == SV{"src/main.cpp:12:5"});
    CHECK(k[0] == LinkKind::Path);
    CHECK(targets("at /usr/bin/env and ~/x.txt, ./run ../up") == SV({"/usr/bin/env", "~/x.txt", "./run", "../up"}));
    CHECK(targets("C:\\dir\\f.txt and D:/x") == SV({"C:\\dir\\f.txt", "D:/x"}));
    CHECK(targets("and/or 1/2 // // a") == SV{});
    CHECK(targets("dir/sub/") == SV{"dir/sub/"});
    CHECK(targets("https://a.b/c/d.txt") == SV{"https://a.b/c/d.txt"});  // the URL, not a path inside it
}

void wrapped_url_and_hover() {
    th::T t(20, 4);
    TerminalView view(t.t);
    t << "go https://example.com/very/long/path ok";
    // Row 0: "go https://example.c", row 1: "om/very/long/path ok"
    const int64_t top = t.t.screen_top_row();
    auto hit = link_at(t.t, RowPos{top + 1, 4});
    CHECK(hit.has_value());
    if (hit) {
        CHECK(hit->kind == LinkKind::Url);
        CHECK_EQ(hit->target, std::string("https://example.com/very/long/path"));
        CHECK(hit->range.start == (RowPos{top, 3}));
        CHECK(hit->range.end == (RowPos{top + 1, 17}));
    }
    CHECK(!link_at(t.t, RowPos{top + 1, 18}).has_value());
    // The view keeps a hover range and shows it in frames on both rows.
    view.set_hover(RowPos{top, 5});
    CHECK(view.hover().has_value());
    auto f = view.snapshot();
    int hover_rows = 0;
    for (const Highlight& h : f->highlights) hover_rows += h.kind == HighlightKind::Hover;
    CHECK_EQ(hover_rows, 2);
    // Text under the pointer changes: the hover follows.
    t << "\x1b[1;1H\x1b[2K\x1b[2;1H\x1b[2K";
    view.sync();
    CHECK(!view.hover().has_value());
}

void osc8() {
    th::T t(40, 3);
    t << "pre \x1b]8;id=a;https://h.example/x\x1b\\click here\x1b]8;;\x1b\\ /tmp/f.txt";
    const int64_t top = t.t.screen_top_row();
    auto hit = link_at(t.t, RowPos{top, 6});
    CHECK(hit.has_value());
    if (hit) {
        CHECK(hit->kind == LinkKind::Hyperlink);
        CHECK_EQ(hit->target, std::string("https://h.example/x"));
        CHECK(hit->range.start == (RowPos{top, 4}));
        CHECK(hit->range.end == (RowPos{top, 14}));
        CHECK(hit->link_id != 0);
    }
    std::vector<LinkHit> hits;
    links_in_rows(t.t, top, top + 3, hits);
    CHECK_EQ(hits.size(), size_t(2));
    if (hits.size() == 2) {
        CHECK(hits[0].kind == LinkKind::Hyperlink);
        CHECK(hits[1].kind == LinkKind::Path);
        CHECK_EQ(hits[1].target, std::string("/tmp/f.txt"));
    }
    // An OSC 8 link wins over a URL in its text.
    th::T u(40, 2);
    u << "\x1b]8;;https://real\x1b\\https://shown\x1b]8;;\x1b\\";
    auto h2 = link_at(u.t, RowPos{u.t.screen_top_row(), 3});
    CHECK(h2 && h2->target == "https://real");
    std::vector<LinkHit> hits2;
    links_in_rows(u.t, u.t.screen_top_row(), u.t.screen_top_row() + 1, hits2);
    CHECK_EQ(hits2.size(), size_t(1));
    // History rows keep their OSC 8 links (and frames resolve the URIs).
    TerminalView view(u.t);
    u << "\r\n\r\n\r\n";
    view.scroll_to_row(u.t.first_row());
    auto f = view.snapshot();
    const FrameRow& row = *f->lines[0];
    const uint32_t id = row.styles[row.cells[0].style].link;
    CHECK(id != 0);
    CHECK(row.link_uri(id) && *row.link_uri(id) == "https://real");
}

} // namespace

int main() {
    init_test();
    detection();
    wrapped_url_and_hover();
    osc8();
    return check::finish("test_links");
}
