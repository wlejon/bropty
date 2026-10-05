// OSC, DCS and APC handling: titles, colors, hyperlinks, clipboard, shell
// integration, DECRQSS and XTGETTCAP.
#include "bropty/terminal.h"

#include "graphics_state.h"

#include <cstdio>
#include <initializer_list>
#include <string>

namespace bropty {

namespace {

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(std::string_view in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        uint32_t v = (uint32_t(uint8_t(in[i])) << 16) | (uint32_t(uint8_t(in[i + 1])) << 8) | uint8_t(in[i + 2]);
        out.push_back(kB64[v >> 18]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        out.push_back(kB64[v & 63]);
        i += 3;
    }
    if (i < in.size()) {
        uint32_t v = uint32_t(uint8_t(in[i])) << 16;
        if (i + 1 < in.size()) v |= uint32_t(uint8_t(in[i + 1])) << 8;
        out.push_back(kB64[v >> 18]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(i + 1 < in.size() ? kB64[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

bool base64_decode(std::string_view in, std::string& out) {
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char ch : in) {
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+') v = 62;
        else if (ch == '/') v = 63;
        else if (ch == '=') break;
        else if (ch == '\r' || ch == '\n' || ch == ' ') continue;
        else return false;
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(char((acc >> bits) & 0xFF));
        }
    }
    return true;
}

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parse 1..4 hex digits scaled to 0..255.
bool hex_channel(std::string_view s, uint8_t& out) {
    if (s.empty() || s.size() > 4) return false;
    uint32_t v = 0;
    for (char c : s) {
        int h = hexval(c);
        if (h < 0) return false;
        v = v * 16 + uint32_t(h);
    }
    uint32_t max = (1u << (4 * s.size())) - 1;
    out = uint8_t((v * 255 + max / 2) / max);
    return true;
}

bool parse_color(std::string_view spec, Rgb& out) {
    if (spec.substr(0, 4) == "rgb:") {
        std::string_view rest = spec.substr(4);
        size_t a = rest.find('/');
        if (a == std::string_view::npos) return false;
        size_t b = rest.find('/', a + 1);
        if (b == std::string_view::npos) return false;
        return hex_channel(rest.substr(0, a), out.r) && hex_channel(rest.substr(a + 1, b - a - 1), out.g) &&
               hex_channel(rest.substr(b + 1), out.b);
    }
    if (!spec.empty() && spec[0] == '#') {
        std::string_view h = spec.substr(1);
        if (h.empty() || h.size() % 3 != 0 || h.size() > 12) return false;
        size_t n = h.size() / 3;
        return hex_channel(h.substr(0, n), out.r) && hex_channel(h.substr(n, n), out.g) &&
               hex_channel(h.substr(2 * n, n), out.b);
    }
    return false;
}

std::string color_reply(Rgb c) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "rgb:%04x/%04x/%04x", c.r * 257, c.g * 257, c.b * 257);
    return buf;
}

std::string_view next_field(std::string_view& s) {
    size_t p = s.find(';');
    std::string_view f = s.substr(0, p);
    s = p == std::string_view::npos ? std::string_view() : s.substr(p + 1);
    return f;
}

int to_int(std::string_view s, int def) {
    if (s.empty() || s.size() > 9) return def;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return def;
        v = v * 10 + (c - '0');
    }
    return v;
}

std::string hex_encode(std::string_view s) {
    static const char* d = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        out.push_back(d[c >> 4]);
        out.push_back(d[c & 15]);
    }
    return out;
}

bool hex_decode(std::string_view s, std::string& out) {
    out.clear();
    if (s.size() % 2) return false;
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = hexval(s[i]), b = hexval(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back(char(a * 16 + b));
    }
    return true;
}

} // namespace

void Terminal::reply(std::string_view bytes) {
    if (host_) host_->write_to_pty(bytes);
}

void Terminal::osc_dispatch(std::string_view payload, bool bel) {
    size_t semi = payload.find(';');
    std::string_view num = payload.substr(0, semi);
    std::string_view rest = semi == std::string_view::npos ? std::string_view() : payload.substr(semi + 1);
    int cmd = to_int(num, -1);
    switch (cmd) {
    case 0:
    case 1:
    case 2:
        if (cmd != 2) {
            icon_name_.assign(rest);
            if (host_) host_->icon_name_changed(icon_name_);
        }
        if (cmd != 1) {
            title_.assign(rest);
            if (host_) host_->title_changed(title_);
        }
        break;
    case 4: osc_palette(rest, bel); break;
    case 7:
        cwd_.assign(rest);
        if (host_) host_->cwd_changed(cwd_);
        break;
    case 8: osc_hyperlink(rest); break;
    case 9:
        if (rest.substr(0, 2) == "4;") {
            std::string_view r = rest.substr(2);
            int st = to_int(next_field(r), 0);
            int val = to_int(next_field(r), 0);
            if (host_) host_->progress(st, val);
        } else if (host_) {
            host_->notification({}, rest);
        }
        break;
    case 10:
    case 11:
    case 12: {
        int which = cmd;
        while (!rest.empty() && which <= 12) {
            osc_color(which, next_field(rest), bel);
            ++which;
        }
        break;
    }
    case 52: osc_clipboard(rest, bel); break;
    case 104:
        if (rest.empty()) {
            Palette fresh = Palette::standard();
            palette_.colors = fresh.colors;
        } else {
            while (!rest.empty()) {
                int idx = to_int(next_field(rest), -1);
                if (idx >= 0 && idx < 256) palette_.colors[size_t(idx)] = Palette::standard_index(uint8_t(idx));
            }
        }
        if (host_) host_->palette_changed();
        break;
    case 110:
    case 111:
    case 112: {
        Palette fresh;
        if (cmd == 110) palette_.foreground = fresh.foreground;
        if (cmd == 111) palette_.background = fresh.background;
        if (cmd == 112) palette_.cursor = fresh.cursor;
        if (host_) host_->palette_changed();
        break;
    }
    case 133: osc_semantic(rest); break;
    case 1337:
        if (opts_.graphics.iterm2) osc_iterm(rest);
        break;
    case 777: {
        std::string_view r = rest;
        if (next_field(r) == "notify" && host_) {
            std::string_view t = next_field(r);
            host_->notification(t, r);
        }
        break;
    }
    default: break;
    }
}

void Terminal::osc_color(int which, std::string_view spec, bool bel) {
    Rgb* target = which == 10 ? &palette_.foreground : which == 11 ? &palette_.background : &palette_.cursor;
    if (spec == "?") {
        reply("\x1b]" + std::to_string(which) + ";" + color_reply(*target) + (bel ? "\x07" : "\x1b\\"));
        return;
    }
    Rgb c;
    if (parse_color(spec, c)) {
        *target = c;
        if (host_) host_->palette_changed();
    }
}

void Terminal::osc_palette(std::string_view rest, bool bel) {
    bool changed = false;
    while (!rest.empty()) {
        int idx = to_int(next_field(rest), -1);
        std::string_view spec = next_field(rest);
        if (idx < 0 || idx > 255) continue;
        if (spec == "?") {
            reply("\x1b]4;" + std::to_string(idx) + ";" + color_reply(palette_.colors[size_t(idx)]) +
                  (bel ? "\x07" : "\x1b\\"));
        } else {
            Rgb c;
            if (parse_color(spec, c)) {
                palette_.colors[size_t(idx)] = c;
                changed = true;
            }
        }
    }
    if (changed && host_) host_->palette_changed();
}

void Terminal::osc_hyperlink(std::string_view rest) {
    size_t semi = rest.find(';');
    if (semi == std::string_view::npos) return;
    std::string_view params = rest.substr(0, semi);
    std::string_view uri = rest.substr(semi + 1);
    Cursor& c = cur();
    if (uri.empty()) {
        c.pen.link = 0;
        update_pen();
        return;
    }
    std::string id;
    while (!params.empty()) {
        size_t colon = params.find(':');
        std::string_view kv = params.substr(0, colon);
        params = colon == std::string_view::npos ? std::string_view() : params.substr(colon + 1);
        if (kv.substr(0, 3) == "id=") id.assign(kv.substr(3));
    }
    uint32_t link = 0;
    std::string key;
    if (!id.empty()) {
        key = id + '\x1f' + std::string(uri);
        auto it = link_by_id_.find(key);
        if (it != link_by_id_.end() && links_.count(it->second)) link = it->second;
    }
    if (!link) {
        do {
            link = next_link_++;
        } while (link == 0 || links_.count(link));
        links_.emplace(link, Hyperlink{id, std::string(uri)});
        if (!key.empty()) link_by_id_[key] = link;
    }
    c.pen.link = link;
    update_pen();
}

void Terminal::osc_clipboard(std::string_view rest, bool bel) {
    size_t semi = rest.find(';');
    if (semi == std::string_view::npos) return;
    std::string_view sel = rest.substr(0, semi);
    std::string_view data = rest.substr(semi + 1);
    if (!host_) return;
    if (data == "?") {
        if (auto content = host_->clipboard_read(sel)) {
            reply("\x1b]52;" + std::string(sel) + ";" + base64_encode(*content) + (bel ? "\x07" : "\x1b\\"));
        }
        return;
    }
    std::string decoded;
    if (base64_decode(data, decoded)) host_->clipboard_write(sel, decoded);
}

void Terminal::osc_semantic(std::string_view rest) {
    if (rest.empty()) return;
    char kind = rest[0];
    std::string_view params = rest.size() > 1 && rest[1] == ';' ? rest.substr(2) : std::string_view();
    uint32_t flag = kind == 'A' ? Row_Prompt : kind == 'B' ? Row_Input : kind == 'C' ? Row_Output : 0u;
    if (flag) {
        grid().set_flag(cur().row, flag, true);
        grid().mark_dirty(cur().row);
    }
    // Text printed from here on carries the zone (Style::zone). 'D' (command
    // finished) and 'P' (prompt property) leave no zone / keep the current one.
    Zone z = kind == 'A' ? Zone::Prompt : kind == 'B' ? Zone::Input : kind == 'C' ? Zone::Output
           : kind == 'D' ? Zone::None : zone_;
    if (z != zone_) {
        zone_ = z;
        update_pen();
    }
    if (host_) host_->semantic_mark(kind, params);
}

// ---------------------------------------------------------------------------
// DCS

void Terminal::dcs_hook(const CsiSeq& seq) {
    dcs_data_.clear();
    if (seq.prefix == 0 && seq.ninter == 1 && seq.inter[0] == '$' && seq.final == 'q') dcs_ = Dcs::Decrqss;
    else if (seq.prefix == 0 && seq.ninter == 1 && seq.inter[0] == '+' && seq.final == 'q') dcs_ = Dcs::Xtgettcap;
    else if (seq.prefix == 0 && seq.ninter == 0 && seq.final == 'q' && opts_.graphics.sixel) {
        dcs_ = Dcs::Sixel;
        sixel_start(seq);
    } else dcs_ = Dcs::Ignore;
}

void Terminal::dcs_put(std::string_view data) {
    if (dcs_ == Dcs::Sixel) {
        if (gfx_->sixel) gfx_->sixel->feed(data);
        return;
    }
    if (dcs_ == Dcs::Ignore || dcs_ == Dcs::None) return;
    if (dcs_data_.size() + data.size() > 4096) {
        dcs_ = Dcs::Ignore;
        dcs_data_.clear();
        return;
    }
    dcs_data_.append(data);
}

void Terminal::dcs_unhook(bool aborted) {
    Dcs kind = dcs_;
    dcs_ = Dcs::None;
    // A sixel image cut short (CAN, SUB, ESC) still shows what arrived, as
    // xterm draws it while it parses.
    if (kind == Dcs::Sixel) {
        sixel_finish();
        return;
    }
    if (aborted) return;
    if (kind == Dcs::Decrqss) decrqss(dcs_data_);
    else if (kind == Dcs::Xtgettcap) xtgettcap(dcs_data_);
    dcs_data_.clear();
}

namespace {
// Appends ";n" for each value (piecewise: GCC 12 raises a false -Wrestrict on
// `"literal" + std::string&&`, PR 105329).
void sgr_params(std::string& out, std::initializer_list<int> values) {
    for (int v : values) {
        out.push_back(';');
        out += std::to_string(v);
    }
}

void sgr_color(std::string& out, int base, Color c) {
    if (c.is_indexed()) {
        int i = c.index();
        if (base != 58 && i < 8) sgr_params(out, {base - 8 + i});
        else if (base != 58 && i < 16) sgr_params(out, {base - 8 + 60 + i - 8});
        else sgr_params(out, {base, 5, i});
    } else if (c.is_rgb()) {
        Rgb v = c.rgb_value();
        sgr_params(out, {base, 2, v.r, v.g, v.b});
    }
}
} // namespace

void Terminal::decrqss(std::string_view req) {
    std::string val;
    const Cursor& c = cur();
    if (req == "m") {
        const Style& p = c.pen;
        val = "0";
        if (p.has(Attr_Bold)) val += ";1";
        if (p.has(Attr_Dim)) val += ";2";
        if (p.has(Attr_Italic)) val += ";3";
        if (p.underline == Underline::Single) val += ";4";
        else if (p.underline == Underline::Double) val += ";21";
        else if (p.underline != Underline::None) val += ";4:" + std::to_string(int(p.underline));
        if (p.has(Attr_Blink)) val += ";5";
        if (p.has(Attr_RapidBlink)) val += ";6";
        if (p.has(Attr_Inverse)) val += ";7";
        if (p.has(Attr_Invisible)) val += ";8";
        if (p.has(Attr_Strike)) val += ";9";
        if (p.has(Attr_Overline)) val += ";53";
        sgr_color(val, 38, p.fg);
        sgr_color(val, 48, p.bg);
        sgr_color(val, 58, p.underline_color);
        val += "m";
    } else if (req == "r") {
        val = std::to_string(top_ + 1) + ";" + std::to_string(bottom_ + 1) + "r";
    } else if (req == "s") {
        val = std::to_string(left_ + 1) + ";" + std::to_string(right_ + 1) + "s";
    } else if (req == " q") {
        int ps = cursor_shape_ == CursorShape::Block ? 1 : cursor_shape_ == CursorShape::Underline ? 3 : 5;
        if (!cursor_shape_blink_) ++ps;
        val = std::to_string(ps) + " q";
    } else if (req == "\"q") {
        val = c.protect ? "1\"q" : "0\"q";
    } else if (req == "\"p") {
        val = "64;1\"p";
    } else if (req == "t") {
        val = std::to_string(rows_) + "t";
    } else {
        reply("\x1bP0$r\x1b\\");
        return;
    }
    reply("\x1bP1$r" + val + "\x1b\\");
}

void Terminal::xtgettcap(std::string_view req) {
    while (!req.empty()) {
        std::string_view hexname = next_field(req);
        std::string name;
        if (!hex_decode(hexname, name)) {
            reply("\x1bP0+r\x1b\\");
            return;
        }
        std::string value;
        bool known = true;
        if (name == "TN" || name == "name") value = opts_.term_name;
        else if (name == "Co" || name == "colors") value = "256";
        else if (name == "RGB") value = "8";
        else known = false;
        if (known) reply("\x1bP1+r" + std::string(hexname) + "=" + hex_encode(value) + "\x1b\\");
        else reply("\x1bP0+r" + std::string(hexname) + "\x1b\\");
    }
}

void Terminal::string_dispatch(StringKind kind, std::string_view payload) {
    if (kind != StringKind::Apc) return;
    if (opts_.graphics.kitty && !payload.empty() && payload[0] == 'G') {
        kitty_command(payload.substr(1));
        return;
    }
    if (host_) host_->apc(payload);
}

} // namespace bropty
