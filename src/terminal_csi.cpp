// CSI and ESC dispatch, SGR, and the reports that answer them.
#include "bropty/terminal.h"
#include "bropty/version.h"

#include <algorithm>
#include <string>

namespace bropty {

namespace {
constexpr uint32_t key(char prefix, char inter, char final_char) {
    return (uint32_t(uint8_t(prefix)) << 16) | (uint32_t(uint8_t(inter)) << 8) | uint32_t(uint8_t(final_char));
}
std::string num(int v) { return std::to_string(v); }
} // namespace

void Terminal::csi_dispatch(const CsiSeq& s) {
    if (s.ninter > 1) return;
    if (s.final != 'm' && s.has_subparams()) return;  // colons are only meaningful in SGR
    Cursor& c = cur();
    int n1 = s.arg(0, 1);
    if (s.final != 'm') invalidate_print();

    switch (key(s.prefix, s.intermediate(), s.final)) {
    case key(0, 0, '@'): insert_chars(n1); break;
    case key(0, 0, 'A'): move_cursor_rel(-n1, 0); break;
    case key(0, 0, 'B'):
    case key(0, 0, 'e'): move_cursor_rel(n1, 0); break;
    case key(0, 0, 'C'):
    case key(0, 0, 'a'): move_cursor_rel(0, n1); break;
    case key(0, 0, 'D'): move_cursor_rel(0, -n1); break;
    case key(0, 0, 'E'): move_cursor_rel(n1, 0); carriage_return(); break;
    case key(0, 0, 'F'): move_cursor_rel(-n1, 0); carriage_return(); break;
    case key(0, 0, 'G'):
    case key(0, 0, '`'):
        move_to(modes_.origin ? c.row - top_ : c.row, n1 - 1);
        break;
    case key(0, 0, 'H'):
    case key(0, 0, 'f'): move_to(s.arg(0, 1) - 1, s.arg(1, 1) - 1); break;
    case key(0, 0, 'I'): tab_forward(n1); break;
    case key(0, 0, 'J'): erase_display(s.raw(0, 0), false); break;
    case key('?', 0, 'J'): erase_display(s.raw(0, 0), true); break;
    case key(0, 0, 'K'): erase_line(s.raw(0, 0), false); break;
    case key('?', 0, 'K'): erase_line(s.raw(0, 0), true); break;
    case key(0, 0, 'L'): insert_lines(n1); break;
    case key(0, 0, 'M'): delete_lines(n1); break;
    case key(0, 0, 'P'): delete_chars(n1); break;
    case key(0, 0, 'S'): scroll_up(n1); break;
    case key(0, 0, 'T'):
        if (s.count <= 1) scroll_down(n1);
        break;
    case key(0, 0, 'X'): erase_chars(n1); break;
    case key(0, 0, 'Z'): tab_backward(n1); break;
    case key(0, 0, 'b'): repeat_last(n1); break;
    case key(0, 0, 'c'):
        if (s.raw(0, 0) == 0) reply("\x1b[?62;22;52c");
        break;
    case key('>', 0, 'c'):
        if (s.raw(0, 0) == 0) reply("\x1b[>1;10;0c");
        break;
    case key('=', 0, 'c'):
        if (s.raw(0, 0) == 0) reply("\x1bP!|00000000\x1b\\");
        break;
    case key(0, 0, 'd'): move_to(n1 - 1, modes_.origin ? c.col - left_ : c.col); break;
    case key(0, 0, 'g'):
        if (s.raw(0, 0) == 0) tabs_[size_t(c.col)] = 0;
        else if (s.raw(0, 0) == 3) std::fill(tabs_.begin(), tabs_.end(), uint8_t(0));
        break;
    case key(0, 0, 'h'):
    case key(0, 0, 'l'):
        for (int i = 0; i < s.count; ++i) set_mode(s.raw(i, 0), s.final == 'h');
        break;
    case key('?', 0, 'h'):
    case key('?', 0, 'l'):
        for (int i = 0; i < s.count; ++i) set_private_mode(s.raw(i, 0), s.final == 'h');
        break;
    case key('?', 0, 's'): save_private_modes(s); break;     // XTSAVE
    case key('?', 0, 'r'): restore_private_modes(s); break;  // XTRESTORE
    case key(0, 0, 'm'): sgr(s); break;
    case key(0, 0, 'n'):
    case key('?', 0, 'n'): device_status(s); break;
    case key(0, 0, 'r'): set_margins(s.arg(0, 1), s.arg(1, rows_)); break;
    case key(0, 0, 's'):
        if (modes_.left_right_margins) set_lr_margins(s.arg(0, 1), s.arg(1, cols_));
        else save_cursor();
        break;
    case key(0, 0, 't'): window_op(s); break;
    case key(0, 0, 'u'):
        if (s.count == 0) restore_cursor();
        break;
    case key('?', 0, 'u'):
    case key('>', 0, 'u'):
    case key('<', 0, 'u'):
    case key('=', 0, 'u'): kitty_keyboard(s); break;
    case key('>', 0, 'm'):  // XTMODKEYS
    case key('>', 0, 'n'):  // XTMODKEYS disable
    case key('?', 0, 'm'): xterm_modkeys(s); break;  // XTQMODKEYS
    case key('>', 0, 'f'):  // XTFMTKEYS
    case key('?', 0, 'f'): xterm_fmtkeys(s); break;  // XTQFMTKEYS
    case key('>', 0, 'q'):
        if (s.raw(0, 0) == 0) reply(std::string("\x1bP>|bropty(") + std::string(version_string()) + ")\x1b\\");
        break;
    case key('?', 0, 'W'):
        if (s.raw(0, 0) == 5) {
            std::fill(tabs_.begin(), tabs_.end(), uint8_t(0));
            for (int x = 8; x < cols_; x += 8) tabs_[size_t(x)] = 1;
        }
        break;
    case key(0, '$', 'p'):
    case key('?', '$', 'p'): request_mode(s); break;
    case key(0, ' ', 'q'): set_cursor_style(s.raw(0, 0)); break;
    case key(0, ' ', '@'):  // SL: scroll left
        if (cursor_in_margins()) scroll_columns(top_, bottom_, left_, right_, n1);
        break;
    case key(0, ' ', 'A'):  // SR: scroll right
        if (cursor_in_margins()) scroll_columns(top_, bottom_, left_, right_, -n1);
        break;
    case key(0, '\'', '}'):  // DECIC
        if (cursor_in_margins()) scroll_columns(top_, bottom_, c.col, right_, -n1);
        break;
    case key(0, '\'', '~'):  // DECDC
        if (cursor_in_margins()) scroll_columns(top_, bottom_, c.col, right_, n1);
        break;
    case key(0, '!', 'p'): soft_reset(); break;
    case key(0, '"', 'q'):  // DECSCA
        c.protect = s.raw(0, 0) == 1;
        break;
    case key(0, '$', 'x'): rect_op(s, RectOp::Fill); break;
    case key(0, '$', 'z'): rect_op(s, RectOp::Erase); break;
    case key(0, '$', '{'): rect_op(s, RectOp::SelectiveErase); break;
    case key(0, '$', 'v'): rect_op(s, RectOp::Copy); break;
    default: break;
    }
}

void Terminal::esc_dispatch(const EscSeq& e) {
    invalidate_print();
    Cursor& c = cur();
    if (e.ninter == 0) {
        switch (e.final) {
        case 'D': index(); break;
        case 'E': index(); carriage_return(); break;
        case 'H': tabs_[size_t(c.col)] = 1; break;
        case 'M': reverse_index(); break;
        case 'N': c.cs.single_shift = 2; break;
        case 'O': c.cs.single_shift = 3; break;
        case '7': save_cursor(); break;
        case '8': restore_cursor(); break;
        case 'c': reset(); break;
        case '=': modes_.app_keypad = true; break;
        case '>': modes_.app_keypad = false; break;
        case 'n': c.cs.gl = 2; break;
        case 'o': c.cs.gl = 3; break;
        case 'Z': reply("\x1b[?62;22;52c"); break;
        case 'V': c.protect = true; break;   // SPA
        case 'W': c.protect = false; break;  // EPA
        case '6':  // DECBI
            if (c.col == left_ && cursor_in_margins()) scroll_columns(top_, bottom_, left_, right_, -1);
            else if (c.col > 0) move_cursor_rel(0, -1);
            break;
        case '9':  // DECFI
            if (c.col == right_ && cursor_in_margins()) scroll_columns(top_, bottom_, left_, right_, 1);
            else if (c.col < cols_ - 1) move_cursor_rel(0, 1);
            break;
        default: break;
        }
        return;
    }
    char i0 = e.inter[0];
    if (e.ninter == 1) {
        switch (i0) {
        case '(': designate(0, e.final, 0); return;
        case ')': designate(1, e.final, 0); return;
        case '*': designate(2, e.final, 0); return;
        case '+': designate(3, e.final, 0); return;
        case '#':
            switch (e.final) {
            case '8': fill_alignment(); return;
            case '3': grid().set_flag(c.row, Row_LineAttrMask, false); grid().set_flag(c.row, Row_DoubleTop, true); break;
            case '4': grid().set_flag(c.row, Row_LineAttrMask, false); grid().set_flag(c.row, Row_DoubleBottom, true); break;
            case '5': grid().set_flag(c.row, Row_LineAttrMask, false); break;
            case '6': grid().set_flag(c.row, Row_LineAttrMask, false); grid().set_flag(c.row, Row_DoubleWidth, true); break;
            default: return;
            }
            grid().mark_dirty(c.row);
            return;
        default: return;
        }
    }
    if (e.ninter == 2 && i0 >= '(' && i0 <= '+') designate(i0 - '(', e.final, e.inter[1]);
}

void Terminal::designate(int slot, char final_char, char inter2) {
    char set = 'B';
    if (inter2 == 0) {
        if (final_char == '0') set = '0';
        else if (final_char == 'A') set = 'A';
    }
    cur().cs.g[slot] = set;
}

void Terminal::sgr(const CsiSeq& s) {
    Style& p = cur().pen;
    if (s.count == 0) {
        uint32_t link = p.link;
        p = Style{};
        p.link = link;
        update_pen();
        return;
    }
    int i = 0;
    while (i < s.count) {
        int nsub = 0;
        while (i + 1 + nsub < s.count && (s.sub >> (i + 1 + nsub) & 1u)) ++nsub;
        int v = s.raw(i, 0);
        int next = i + 1 + nsub;

        auto ext_color = [&](Color& out) -> bool {
            // Returns false when the sequence is malformed and SGR must stop.
            if (nsub > 0) {
                int t = s.raw(i + 1, -1);
                if (t == 5 && nsub >= 2) {
                    int idx = s.raw(i + 2, 0);
                    if (idx <= 255) out = Color::indexed(uint8_t(idx));
                } else if (t == 2 && nsub >= 4) {
                    int b0 = nsub >= 5 ? i + 3 : i + 2;  // 38:2:CS:r:g:b or 38:2:r:g:b
                    int r = s.raw(b0, 0), g = s.raw(b0 + 1, 0), b = s.raw(b0 + 2, 0);
                    if (r <= 255 && g <= 255 && b <= 255) out = Color::rgb(uint8_t(r), uint8_t(g), uint8_t(b));
                }
                return true;
            }
            int t = s.raw(i + 1, -1);
            if (t == 5) {
                if (i + 2 >= s.count) return false;
                int idx = s.raw(i + 2, 0);
                if (idx <= 255) out = Color::indexed(uint8_t(idx));
                next = i + 3;
                return true;
            }
            if (t == 2) {
                if (i + 4 >= s.count) return false;
                int r = s.raw(i + 2, 0), g = s.raw(i + 3, 0), b = s.raw(i + 4, 0);
                if (r <= 255 && g <= 255 && b <= 255) out = Color::rgb(uint8_t(r), uint8_t(g), uint8_t(b));
                next = i + 5;
                return true;
            }
            return false;
        };

        switch (v) {
        case 0: {
            uint32_t link = p.link;
            p = Style{};
            p.link = link;
            break;
        }
        case 1: p.attrs |= Attr_Bold; break;
        case 2: p.attrs |= Attr_Dim; break;
        case 3: p.attrs |= Attr_Italic; break;
        case 4:
            if (nsub > 0) {
                int u = s.raw(i + 1, 1);
                if (u >= 0 && u <= 5) p.underline = Underline(u);
            } else {
                p.underline = Underline::Single;
            }
            break;
        case 5: p.attrs |= Attr_Blink; break;
        case 6: p.attrs |= Attr_RapidBlink; break;
        case 7: p.attrs |= Attr_Inverse; break;
        case 8: p.attrs |= Attr_Invisible; break;
        case 9: p.attrs |= Attr_Strike; break;
        case 21: p.underline = Underline::Double; break;
        case 22: p.attrs &= uint16_t(~(Attr_Bold | Attr_Dim)); break;
        case 23: p.attrs &= uint16_t(~Attr_Italic); break;
        case 24: p.underline = Underline::None; break;
        case 25: p.attrs &= uint16_t(~(Attr_Blink | Attr_RapidBlink)); break;
        case 27: p.attrs &= uint16_t(~Attr_Inverse); break;
        case 28: p.attrs &= uint16_t(~Attr_Invisible); break;
        case 29: p.attrs &= uint16_t(~Attr_Strike); break;
        case 38: if (!ext_color(p.fg)) i = s.count; break;
        case 39: p.fg = Color{}; break;
        case 48: if (!ext_color(p.bg)) i = s.count; break;
        case 49: p.bg = Color{}; break;
        case 53: p.attrs |= Attr_Overline; break;
        case 55: p.attrs &= uint16_t(~Attr_Overline); break;
        case 58: if (!ext_color(p.underline_color)) i = s.count; break;
        case 59: p.underline_color = Color{}; break;
        default:
            if (v >= 30 && v <= 37) p.fg = Color::indexed(uint8_t(v - 30));
            else if (v >= 40 && v <= 47) p.bg = Color::indexed(uint8_t(v - 40));
            else if (v >= 90 && v <= 97) p.fg = Color::indexed(uint8_t(v - 90 + 8));
            else if (v >= 100 && v <= 107) p.bg = Color::indexed(uint8_t(v - 100 + 8));
            break;
        }
        if (i >= s.count) break;
        i = next;
    }
    update_pen();
}

void Terminal::set_margins(int top, int bottom) {
    top = std::max(1, top) - 1;
    bottom = std::min(bottom, rows_) - 1;
    if (top >= bottom) return;
    top_ = top;
    bottom_ = bottom;
    move_to(0, 0);
}

void Terminal::set_lr_margins(int left, int right) {
    left = std::max(1, left) - 1;
    right = std::min(right, cols_) - 1;
    if (left >= right) return;
    left_ = left;
    right_ = right;
    move_to(0, 0);
}

void Terminal::set_cursor_style(int ps) {
    switch (ps) {
    case 0:
    case 1: cursor_shape_ = CursorShape::Block; cursor_shape_blink_ = true; break;
    case 2: cursor_shape_ = CursorShape::Block; cursor_shape_blink_ = false; break;
    case 3: cursor_shape_ = CursorShape::Underline; cursor_shape_blink_ = true; break;
    case 4: cursor_shape_ = CursorShape::Underline; cursor_shape_blink_ = false; break;
    case 5: cursor_shape_ = CursorShape::Bar; cursor_shape_blink_ = true; break;
    case 6: cursor_shape_ = CursorShape::Bar; cursor_shape_blink_ = false; break;
    default: break;
    }
}

void Terminal::kitty_keyboard(const CsiSeq& s) {
    auto& st = active_->kitty_flags;
    switch (s.prefix) {
    case '?':
        reply("\x1b[?" + num(int(kitty_keyboard_flags())) + "u");
        break;
    case '>':
        if (st.size() >= 16) st.erase(st.begin());
        st.push_back(uint32_t(s.raw(0, 0)) & 0x1F);
        break;
    case '<': {
        size_t n = size_t(s.arg(0, 1));
        st.resize(st.size() > n ? st.size() - n : 0);
        break;
    }
    case '=': {
        uint32_t f = uint32_t(s.raw(0, 0)) & 0x1F;
        int mode = s.arg(1, 1);
        if (st.empty()) st.push_back(0);
        if (mode == 1) st.back() = f;
        else if (mode == 2) st.back() |= f;
        else if (mode == 3) st.back() &= ~f;
        break;
    }
    default: break;
    }
}

namespace {
// The XTMODKEYS resource Pp, or null for an unknown one.
int* modkey_resource(Modes& m, int pp) {
    switch (pp) {
    case 0: return &m.modify_keyboard;
    case 1: return &m.modify_cursor_keys;
    case 2: return &m.modify_function_keys;
    case 3: return &m.modify_keypad_keys;
    case 4: return &m.modify_other_keys;
    case 6: return &m.modify_modifier_keys;
    case 7: return &m.modify_special_keys;
    default: return nullptr;
    }
}
// Highest value each resource accepts (xterm ignores a value out of range).
int modkey_max(int pp) {
    switch (pp) {
    case 0: return 15;  // modifyKeyboard is a bit mask
    case 4: return 3;
    case 6: return 3;
    case 7: return 1;
    default: return 3;
    }
}
} // namespace

void Terminal::reset_modkeys() {
    const Modes initial;
    modes_.modify_keyboard = initial.modify_keyboard;
    modes_.modify_cursor_keys = initial.modify_cursor_keys;
    modes_.modify_function_keys = initial.modify_function_keys;
    modes_.modify_keypad_keys = initial.modify_keypad_keys;
    modes_.modify_other_keys = initial.modify_other_keys;
    modes_.modify_modifier_keys = initial.modify_modifier_keys;
    modes_.modify_special_keys = initial.modify_special_keys;
    modes_.format_other_keys = initial.format_other_keys;
}

// XTMODKEYS (CSI > Pp ; Pv m), its disable form (CSI > Pp n, Pp omitted = 2,
// value -1) and XTQMODKEYS (CSI ? Pp m), following xterm's set_mod_fkeys():
// omitting Pv resets the resource to its initial value, omitting every
// parameter resets them all.
void Terminal::xterm_modkeys(const CsiSeq& s) {
    if (s.has_subparams()) return;
    Modes initial;
    if (s.prefix == '?') {
        for (int i = 0; i < s.count; ++i) {
            const int pp = s.raw(i, -1);
            if (const int* r = modkey_resource(modes_, pp)) reply("\x1b[>" + num(pp) + ";" + num(*r) + "m");
        }
        return;
    }
    if (s.final == 'n') {
        if (int* r = modkey_resource(modes_, s.raw(0, 2))) *r = -1;
        return;
    }
    if (s.count == 0) {
        const int fmt = modes_.format_other_keys;  // XTFMTKEYS is a separate resource
        reset_modkeys();
        modes_.format_other_keys = fmt;
        return;
    }
    const int pp = s.raw(0, -1);
    int* r = modkey_resource(modes_, pp);
    if (!r) return;
    const int v = s.raw(1, -2);
    if (v == -2) {
        *r = *modkey_resource(initial, pp);
    } else if (v >= 0 && v <= modkey_max(pp)) {
        *r = v;
    }
}

// XTFMTKEYS (CSI > 4 ; Pv f) and its query XTQFMTKEYS (CSI ? 4 f): only
// formatOtherKeys (Pp 4) exists; omitting Pv resets it to 0.
void Terminal::xterm_fmtkeys(const CsiSeq& s) {
    if (s.has_subparams()) return;
    if (s.prefix == '?') {
        for (int i = 0; i < s.count; ++i) {
            if (s.raw(i, -1) == 4) reply("\x1b[>4;" + num(modes_.format_other_keys) + "f");
        }
        return;
    }
    if (s.raw(0, -1) != 4) return;
    const int v = s.raw(1, 0);
    if (v == 0 || v == 1) modes_.format_other_keys = v;
}

void Terminal::window_op(const CsiSeq& s) {
    switch (s.raw(0, 0)) {
    case 14:
        if (cell_w_ > 0 && cell_h_ > 0) reply("\x1b[4;" + num(rows_ * cell_h_) + ";" + num(cols_ * cell_w_) + "t");
        break;
    case 16:
        if (cell_w_ > 0 && cell_h_ > 0) reply("\x1b[6;" + num(cell_h_) + ";" + num(cell_w_) + "t");
        break;
    case 18: reply("\x1b[8;" + num(rows_) + ";" + num(cols_) + "t"); break;
    case 19: reply("\x1b[9;" + num(rows_) + ";" + num(cols_) + "t"); break;
    case 22:
        if (title_stack_.size() >= 10) title_stack_.erase(title_stack_.begin());
        title_stack_.emplace_back(title_, icon_name_);
        break;
    case 23:
        if (!title_stack_.empty()) {
            int which = s.raw(1, 0);
            auto [t, icon] = title_stack_.back();
            title_stack_.pop_back();
            if (which == 0 || which == 2) {
                title_ = t;
                if (host_) host_->title_changed(title_);
            }
            if (which == 0 || which == 1) {
                icon_name_ = icon;
                if (host_) host_->icon_name_changed(icon_name_);
            }
        }
        break;
    default: break;
    }
}

void Terminal::device_status(const CsiSeq& s) {
    const Cursor& c = cur();
    int row = modes_.origin ? c.row - top_ : c.row;
    int col = modes_.origin ? c.col - left_ : c.col;
    int ps = s.raw(0, 0);
    if (s.prefix == 0) {
        if (ps == 5) reply("\x1b[0n");
        else if (ps == 6) reply("\x1b[" + num(row + 1) + ";" + num(col + 1) + "R");
        return;
    }
    switch (ps) {
    case 6: reply("\x1b[?" + num(row + 1) + ";" + num(col + 1) + "R"); break;
    case 15: reply("\x1b[?13n"); break;
    case 25: reply("\x1b[?20n"); break;
    case 26: reply("\x1b[?27;1;0;0n"); break;
    case 53:
    case 55: reply("\x1b[?50n"); break;
    default: break;
    }
}

void Terminal::soft_reset() {
    Cursor& c = cur();
    modes_.cursor_visible = true;
    modes_.insert = false;
    modes_.origin = false;
    modes_.autowrap = true;
    modes_.app_keypad = false;
    modes_.app_cursor_keys = false;
    reset_modkeys();  // xterm's ReallyReset restores the modifier resources on DECSTR too
    reset_margins();
    c.cs = Charsets{};
    c.pen = Style{};
    c.protect = false;
    update_pen();
    active_->saved = Saved{};
    c.pending_wrap = false;
}

void Terminal::scroll_columns(int top, int bottom, int left, int right, int n) {
    if (n == 0 || left > right) return;
    Grid& g = grid();
    Cell blank = Cell::blank(cur().bce_id);
    int width = right - left + 1;
    int k = std::min(std::abs(n), width);
    for (int y = top; y <= bottom; ++y) {
        if (n > 0) {
            if (k < width) g.copy_span(y, left + k, right + 1, y, left);
            g.fill(y, right + 1 - k, right + 1, blank);
        } else {
            if (k < width) g.copy_span(y, left, right + 1 - k, y, left + k);
            g.fill(y, left, left + k, blank);
        }
        sanitize_row(y, left, right + 1);
    }
    invalidate_print();
}

void Terminal::rect_op(const CsiSeq& s, RectOp op) {
    // DECFRA has the fill character first; DECCRA has a source page and destination.
    int base = op == RectOp::Fill ? 1 : 0;
    int t = s.arg(base + 0, 1) - 1, l = s.arg(base + 1, 1) - 1;
    int b = s.arg(base + 2, rows_) - 1, r = s.arg(base + 3, cols_) - 1;
    if (modes_.origin) {
        t += top_; b += top_; l += left_; r += left_;
        b = std::min(b, bottom_);
        r = std::min(r, right_);
    }
    b = std::min(b, rows_ - 1);
    r = std::min(r, cols_ - 1);
    if (t > b || l > r) return;
    Grid& g = grid();
    switch (op) {
    case RectOp::Fill: {
        int ch = s.raw(0, 0);
        if (!((ch >= 32 && ch <= 126) || (ch >= 160 && ch <= 255))) return;
        Cell fill = Cell::make(char32_t(ch), cur().pen_id);
        for (int y = t; y <= b; ++y) {
            g.fill(y, l, r + 1, fill);
            sanitize_row(y, l, r + 1);
        }
        break;
    }
    case RectOp::Erase:
    case RectOp::SelectiveErase:
        for (int y = t; y <= b; ++y) erase_cells(y, l, r + 1, op == RectOp::SelectiveErase);
        break;
    case RectOp::Copy: {
        int dt = s.arg(5, 1) - 1, dl = s.arg(6, 1) - 1;
        if (modes_.origin) { dt += top_; dl += left_; }
        if (dt >= rows_ || dl >= cols_) return;
        int h = std::min(b - t + 1, rows_ - dt);
        int w = std::min(r - l + 1, cols_ - dl);
        // Snapshot the source (cells and cluster tails) first: the areas may overlap.
        std::vector<Cell> tmp(size_t(h) * size_t(w));
        std::vector<std::pair<int, std::u32string>> tails;  // (y * w + x, tail)
        for (int y = 0; y < h; ++y) {
            const ClusterMap* m = g.clusters(t + y);
            for (int x = 0; x < w; ++x) {
                const Cell& cell = g.at(t + y, l + x);
                tmp[size_t(y * w + x)] = cell;
                if (cell.has_cluster() && m) {
                    std::u32string_view tail = m->find(l + x);
                    if (!tail.empty()) tails.emplace_back(y * w + x, std::u32string(tail));
                }
            }
        }
        size_t ti = 0;
        for (int y = 0; y < h; ++y) {
            if (const ClusterMap* m = g.clusters(dt + y); m && !m->empty()) g.clusters_mut(dt + y).erase_range(dl, dl + w);
            for (int x = 0; x < w; ++x) {
                Cell cell = tmp[size_t(y * w + x)];
                const bool has_tail = ti < tails.size() && tails[ti].first == y * w + x;
                cell.set_cluster(has_tail);
                g.at(dt + y, dl + x) = cell;
                if (has_tail) g.clusters_mut(dt + y).set(dl + x, tails[ti++].second);
            }
            sanitize_row(dt + y, dl, dl + w);
            g.mark_dirty(dt + y);
        }
        break;
    }
    }
    invalidate_print();
}

} // namespace bropty
