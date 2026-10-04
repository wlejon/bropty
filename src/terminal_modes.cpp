// ANSI (SM/RM) and DEC private (DECSET/DECRST) modes, and DECRQM reports.
#include "bropty/terminal.h"

#include <string>

namespace bropty {

void Terminal::set_mode(int mode, bool on) {
    switch (mode) {
    case 4: modes_.insert = on; break;
    case 20: modes_.linefeed_newline = on; break;
    default: break;
    }
}

namespace {
void set_tracking(Modes& m, MouseTracking t, bool on) {
    if (on) m.mouse_tracking = t;
    else if (m.mouse_tracking == t) m.mouse_tracking = MouseTracking::None;
}
void set_encoding(Modes& m, MouseEncoding e, bool on) {
    if (on) m.mouse_encoding = e;
    else if (m.mouse_encoding == e) m.mouse_encoding = MouseEncoding::Default;
}
} // namespace

void Terminal::set_private_mode(int mode, bool on) {
    switch (mode) {
    case 1: modes_.app_cursor_keys = on; break;
    case 5:
        if (modes_.reverse_video != on) {
            modes_.reverse_video = on;
            grid().mark_all_dirty();
        }
        break;
    case 6:
        modes_.origin = on;
        move_to(0, 0);
        break;
    case 7:
        modes_.autowrap = on;
        if (!on) cur().pending_wrap = false;
        break;
    case 9: set_tracking(modes_, MouseTracking::X10, on); break;
    case 12: modes_.cursor_blink = on; break;
    case 25: modes_.cursor_visible = on; break;
    case 45: modes_.reverse_wrap = on; break;
    case 47: switch_screen(on, false, false); break;
    case 66: modes_.app_keypad = on; break;
    case 67: modes_.backarrow_sends_bs = on; break;
    case 69:
        modes_.left_right_margins = on;
        if (!on) {
            left_ = 0;
            right_ = cols_ - 1;
        }
        break;
    case 1000: set_tracking(modes_, MouseTracking::Normal, on); break;
    case 1002: set_tracking(modes_, MouseTracking::Button, on); break;
    case 1003: set_tracking(modes_, MouseTracking::Any, on); break;
    case 1004: modes_.focus_events = on; break;
    case 1005: set_encoding(modes_, MouseEncoding::Utf8, on); break;
    case 1006: set_encoding(modes_, MouseEncoding::Sgr, on); break;
    case 1015: set_encoding(modes_, MouseEncoding::Urxvt, on); break;
    case 1016: set_encoding(modes_, MouseEncoding::SgrPixels, on); break;
    case 1007: modes_.alternate_scroll = on; break;
    case 1036: modes_.meta_sends_escape = on; break;
    case 1039: modes_.alt_sends_escape = on; break;
    case 1047: switch_screen(on, !on, false); break;
    case 1048:
        if (on) save_cursor();
        else restore_cursor();
        break;
    case 1049: switch_screen(on, on, true); break;
    case 2004: modes_.bracketed_paste = on; break;
    case 2026: modes_.synchronized_output = on; break;
    case 2027:
        modes_.grapheme_clustering = on;
        invalidate_print();
        break;
    case 2031: modes_.color_scheme_updates = on; break;
    case 2048:
        modes_.in_band_resize = on;
        if (on) {
            reply("\x1b[48;" + std::to_string(rows_) + ";" + std::to_string(cols_) + ";" +
                  std::to_string(rows_ * cell_h_) + ";" + std::to_string(cols_ * cell_w_) + "t");
        }
        break;
    default: break;
    }
}

int Terminal::mode_state(int mode) const {
    switch (mode) {
    case 4: return modes_.insert ? 1 : 2;
    case 20: return modes_.linefeed_newline ? 1 : 2;
    case 2: return 4;   // KAM: keyboard never locked
    case 12: return 4;  // SRM: local echo never on
    default: return 0;
    }
}

int Terminal::private_mode_state(int mode) const {
    auto b = [](bool v) { return v ? 1 : 2; };
    switch (mode) {
    case 1: return b(modes_.app_cursor_keys);
    case 3: return 4;  // DECCOLM is not supported
    case 5: return b(modes_.reverse_video);
    case 6: return b(modes_.origin);
    case 7: return b(modes_.autowrap);
    case 8: return 3;  // DECARM: autorepeat is the host's business
    case 9: return b(modes_.mouse_tracking == MouseTracking::X10);
    case 12: return b(modes_.cursor_blink);
    case 25: return b(modes_.cursor_visible);
    case 45: return b(modes_.reverse_wrap);
    case 47:
    case 1047:
    case 1049: return b(alt_screen_active());
    case 66: return b(modes_.app_keypad);
    case 67: return b(modes_.backarrow_sends_bs);
    case 69: return b(modes_.left_right_margins);
    case 1000: return b(modes_.mouse_tracking == MouseTracking::Normal);
    case 1002: return b(modes_.mouse_tracking == MouseTracking::Button);
    case 1003: return b(modes_.mouse_tracking == MouseTracking::Any);
    case 1004: return b(modes_.focus_events);
    case 1005: return b(modes_.mouse_encoding == MouseEncoding::Utf8);
    case 1006: return b(modes_.mouse_encoding == MouseEncoding::Sgr);
    case 1015: return b(modes_.mouse_encoding == MouseEncoding::Urxvt);
    case 1016: return b(modes_.mouse_encoding == MouseEncoding::SgrPixels);
    case 1007: return b(modes_.alternate_scroll);
    case 1036: return b(modes_.meta_sends_escape);
    case 1039: return b(modes_.alt_sends_escape);
    case 1048: return 1;
    case 2004: return b(modes_.bracketed_paste);
    case 2026: return b(modes_.synchronized_output);
    case 2027: return b(modes_.grapheme_clustering);
    case 2031: return b(modes_.color_scheme_updates);
    case 2048: return b(modes_.in_band_resize);
    default: return 0;
    }
}

void Terminal::request_mode(const CsiSeq& s) {
    int mode = s.raw(0, 0);
    if (s.prefix == '?') {
        reply("\x1b[?" + std::to_string(mode) + ";" + std::to_string(private_mode_state(mode)) + "$y");
    } else {
        reply("\x1b[" + std::to_string(mode) + ";" + std::to_string(mode_state(mode)) + "$y");
    }
}

} // namespace bropty
