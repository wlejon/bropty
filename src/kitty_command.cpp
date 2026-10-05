#include "kitty_command.h"

namespace bropty::detail {

namespace {

bool parse_uint(std::string_view v, uint32_t& out) {
    if (v.empty() || v.size() > 10) return false;
    uint64_t n = 0;
    for (char c : v) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + uint64_t(c - '0');
    }
    if (n > 0xFFFFFFFFull) return false;
    out = uint32_t(n);
    return true;
}

bool parse_int(std::string_view v, int32_t& out) {
    bool neg = false;
    if (!v.empty() && (v[0] == '-' || v[0] == '+')) {
        neg = v[0] == '-';
        v.remove_prefix(1);
    }
    if (v.empty() || v.size() > 10) return false;
    int64_t n = 0;
    for (char c : v) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + (c - '0');
    }
    if (neg) n = -n;
    if (n < INT32_MIN || n > INT32_MAX) return false;
    out = int32_t(n);
    return true;
}

} // namespace

bool parse_kitty_command(std::string_view data, KittyCommand& out, std::string& error) {
    out = KittyCommand{};
    const size_t semi = data.find(';');
    std::string_view control = data.substr(0, semi);
    out.payload = semi == std::string_view::npos ? std::string_view() : data.substr(semi + 1);
    while (!control.empty()) {
        const size_t comma = control.find(',');
        std::string_view kv = control.substr(0, comma);
        control = comma == std::string_view::npos ? std::string_view() : control.substr(comma + 1);
        if (kv.empty()) continue;
        if (kv.size() < 2 || kv[1] != '=') {
            error = "Malformed key in graphics command";
            return false;
        }
        const char key = kv[0];
        const std::string_view v = kv.substr(2);
        auto ch = [&](char& dst) {
            if (v.size() != 1) return false;
            dst = v[0];
            return true;
        };
        bool ok = true;
        switch (key) {
        case 'a': ok = ch(out.action); break;
        case 't': ok = ch(out.medium); break;
        case 'o': ok = ch(out.compression); break;
        case 'd': ok = ch(out.del); break;
        case 'q': ok = parse_uint(v, out.quiet); break;
        case 'f': ok = parse_uint(v, out.format); break;
        case 's': ok = parse_uint(v, out.width); break;
        case 'v': ok = parse_uint(v, out.height); break;
        case 'S': ok = parse_uint(v, out.size); break;
        case 'O': ok = parse_uint(v, out.offset); break;
        case 'i': ok = parse_uint(v, out.id); break;
        case 'I': ok = parse_uint(v, out.number); break;
        case 'p': ok = parse_uint(v, out.placement); break;
        case 'm': ok = parse_uint(v, out.more); break;
        case 'x': ok = parse_uint(v, out.x); break;
        case 'y': ok = parse_uint(v, out.y); break;
        case 'w': ok = parse_uint(v, out.w); break;
        case 'h': ok = parse_uint(v, out.h); break;
        case 'X': ok = parse_uint(v, out.cell_x); break;
        case 'Y': ok = parse_uint(v, out.cell_y); break;
        case 'c': ok = parse_uint(v, out.cols); break;
        case 'r': ok = parse_uint(v, out.rows); break;
        case 'C': ok = parse_uint(v, out.cursor); break;
        case 'U': ok = parse_uint(v, out.unicode); break;
        case 'P': ok = parse_uint(v, out.parent_id); break;
        case 'Q': ok = parse_uint(v, out.parent_placement); break;
        case 'z': ok = parse_int(v, out.z); break;
        case 'H': ok = parse_int(v, out.parent_dx); break;
        case 'V': ok = parse_int(v, out.parent_dy); break;
        default: continue;  // unknown keys are ignored
        }
        if (!ok) {
            error = std::string("Malformed value for key: ") + key;
            return false;
        }
        if (key >= 'A' && key <= 'z') out.present |= uint64_t(1) << (key - 'A');
    }
    return true;
}

} // namespace bropty::detail
