// OSC 22: the mouse pointer shape, as kitty specifies it
// (https://sw.kovidgoyal.net/kitty/pointer-shapes/) and implements it
// (kitty/window.py set_pointer_shape, kitty/screen.c change_pointer_shape):
//
//   OSC 22 ; [=]name[,name...]   set: each name in turn replaces the top of
//                                the stack (pushed when it is empty); an
//                                empty name resets the top to the default
//   OSC 22 ; >name[,name...]     push every name, the last on top
//   OSC 22 ; <                   pop one (the names are ignored)
//   OSC 22 ; ?q[,q...]           query: 1 / 0 per name for "is it a CSS
//                                pointer name", and for the special keys
//                                __current__ (the current shape, or 0 for
//                                the default), __default__ and __grabbed__
//
// Names are the CSS cursor names; the X11 / legacy names kitty accepts are
// aliases of them (left_ptr is default, fleur is move, ...), and the current
// shape always reads back as the CSS name. Unknown names are ignored. Each
// screen keeps its own stack (16 deep, the oldest entry dropped beyond), so
// a full-screen program's shapes go when it leaves the alternate screen;
// RIS empties both.
#include "bropty/terminal.h"

#include <array>

namespace bropty {

namespace {

// Index 0 is "no shape" (the default).
constexpr std::array<std::string_view, 31> kCssNames = {
    "",           "default",   "text",      "pointer",   "help",        "wait",        "progress",  "crosshair",
    "cell",       "vertical-text", "move",  "e-resize",  "ne-resize",   "nw-resize",   "n-resize",  "se-resize",
    "sw-resize",  "s-resize",  "w-resize",  "ew-resize", "ns-resize",   "nesw-resize", "nwse-resize", "zoom-in",
    "zoom-out",   "alias",     "copy",      "not-allowed", "no-drop",   "grab",        "grabbing",
};

uint8_t css_index(std::string_view name) {
    for (size_t i = 1; i < kCssNames.size(); ++i)
        if (kCssNames[i] == name) return uint8_t(i);
    return 0;
}

struct Alias {
    std::string_view name;
    std::string_view css;
};

// kitty's table (gen-key-constants.py): X11 cursor-font and legacy names.
constexpr Alias kAliases[] = {
    {"left_ptr", "default"},           {"arrow", "default"},
    {"xterm", "text"},                 {"ibeam", "text"},
    {"beam", "text"},                  {"pointing_hand", "pointer"},
    {"hand2", "pointer"},              {"hand", "pointer"},
    {"question_arrow", "help"},        {"whats_this", "help"},
    {"clock", "wait"},                 {"watch", "wait"},
    {"half-busy", "progress"},         {"left_ptr_watch", "progress"},
    {"tcross", "crosshair"},           {"plus", "cell"},
    {"cross", "cell"},                 {"fleur", "move"},
    {"pointer-move", "move"},          {"right_side", "e-resize"},
    {"top_right_corner", "ne-resize"}, {"top_left_corner", "nw-resize"},
    {"top_side", "n-resize"},          {"bottom_right_corner", "se-resize"},
    {"bottom_left_corner", "sw-resize"}, {"bottom_side", "s-resize"},
    {"left_side", "w-resize"},         {"sb_h_double_arrow", "ew-resize"},
    {"split_h", "ew-resize"},          {"sb_v_double_arrow", "ns-resize"},
    {"split_v", "ns-resize"},          {"size_bdiag", "nesw-resize"},
    {"size-bdiag", "nesw-resize"},     {"size_fdiag", "nwse-resize"},
    {"size-fdiag", "nwse-resize"},     {"zoom_in", "zoom-in"},
    {"zoom_out", "zoom-out"},          {"dnd-link", "alias"},
    {"dnd-copy", "copy"},              {"forbidden", "not-allowed"},
    {"crossed_circle", "not-allowed"}, {"dnd-no-drop", "no-drop"},
    {"openhand", "grab"},              {"hand1", "grab"},
    {"closedhand", "grabbing"},        {"dnd-none", "grabbing"},
};

// The shape a name selects: a CSS name or an alias of one; 0 if unknown.
uint8_t shape_index(std::string_view name) {
    if (const uint8_t i = css_index(name)) return i;
    for (const Alias& a : kAliases)
        if (a.name == name) return css_index(a.css);
    return 0;
}

constexpr size_t kPointerStackDepth = 16;

} // namespace

std::string_view pointer_shape_css_name(std::string_view name) noexcept { return kCssNames[shape_index(name)]; }

void Terminal::osc_pointer(std::string_view value, bool bel) {
    char op = '=';
    if (!value.empty() && (value[0] == '>' || value[0] == '<' || value[0] == '=' || value[0] == '?')) {
        op = value[0];
        value.remove_prefix(1);
    }
    std::vector<uint8_t>& stack = active_->pointer_shapes;
    auto each_name = [&](auto&& f) {
        for (;;) {
            const size_t comma = value.find(',');
            f(value.substr(0, comma));
            if (comma == std::string_view::npos) break;
            value.remove_prefix(comma + 1);
        }
    };
    if (op == '<') {
        if (!stack.empty()) stack.pop_back();
    } else if (op == '?') {
        std::string ans;
        each_name([&](std::string_view q) {
            if (!ans.empty()) ans.push_back(',');
            if (css_index(q)) ans.push_back('1');
            else if (q == "__default__") ans += pointer_shape_css_name(opts_.default_pointer_shape);
            else if (q == "__grabbed__") ans += pointer_shape_css_name(opts_.grabbed_pointer_shape);
            else if (q == "__current__") ans += pointer_shape_.empty() ? std::string_view("0") : pointer_shape_;
            else ans.push_back('0');
        });
        reply("\x1b]22;" + ans + (bel ? "\x07" : "\x1b\\"));
        return;
    } else {
        each_name([&](std::string_view name) {
            if (name.empty() && op == '>') return;
            const uint8_t s = shape_index(name);
            if (!s && !name.empty()) return;  // unknown: ignored
            if (op == '=') {
                if (stack.empty()) stack.push_back(0);
                stack.back() = s;
            } else {
                if (stack.size() >= kPointerStackDepth) stack.erase(stack.begin());
                stack.push_back(s);
            }
        });
    }
    pointer_shape_sync();
}

void Terminal::pointer_shape_sync() {
    const std::vector<uint8_t>& stack = active_->pointer_shapes;
    const std::string_view now = stack.empty() ? std::string_view() : kCssNames[stack.back()];
    if (now == pointer_shape_) return;
    pointer_shape_.assign(now);
    if (host_) host_->pointer_shape_changed(pointer_shape_);
}

} // namespace bropty
