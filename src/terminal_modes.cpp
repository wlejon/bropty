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
    case 3:
        if (modes_.allow_deccolm) set_column_mode(on);
        break;
    case 40: modes_.allow_deccolm = on; break;
    case 95: modes_.deccolm_no_clear = on; break;
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
    case 80: modes_.sixel_display_mode = on; break;
    case 1070: modes_.sixel_private_colors = on; break;
    case 8452: modes_.sixel_cursor_right = on; break;
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
    case 3: return b(modes_.deccolm);
    case 40: return b(modes_.allow_deccolm);
    case 95: return b(modes_.deccolm_no_clear);
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
    case 80: return opts_.graphics.sixel ? b(modes_.sixel_display_mode) : 0;
    case 1070: return opts_.graphics.sixel ? b(modes_.sixel_private_colors) : 0;
    case 8452: return opts_.graphics.sixel ? b(modes_.sixel_cursor_right) : 0;
    default: return 0;
    }
}

// DECCOLM under ?40, as xterm does it: the screen is cleared unless DECNCSM
// (?95) is set, the margins reset, the cursor homes, and the width becomes 132
// or 80 columns (the row count is kept). The host is told so it can resize
// its window and the pty.
void Terminal::set_column_mode(bool wide) {
    modes_.deccolm = wide;
    if (!modes_.deccolm_no_clear) erase_display(2, false);
    reset_margins();
    move_to(0, 0);
    const int cols = wide ? 132 : 80;
    if (cols != cols_) {
        resize(cols, rows_);
        reset_margins();
        move_to(0, 0);
        if (host_) host_->resized_by_application(cols_, rows_);
    }
}

namespace {
// Modes XTSAVE records: those with a set/reset state (DECRQM 1 or 2) whose
// setting is meaningful to replay. ?1048 saves the cursor rather than holding
// a state, and the alternate-screen aliases restore through ?1049 semantics.
bool xtsave_skips(int mode) { return mode == 1048 || mode == 47 || mode == 1047; }
} // namespace

// XTSAVE (CSI ? Pm s): remember the listed DEC private modes.
void Terminal::save_private_modes(const CsiSeq& s) {
    for (int i = 0; i < s.count; ++i) {
        const int mode = s.raw(i, -1);
        if (mode < 0 || xtsave_skips(mode)) continue;
        const int st = private_mode_state(mode);
        if (st == 1 || st == 2) xtsaved_[mode] = st == 1;
    }
}

// XTRESTORE (CSI ? Pm r): put back what XTSAVE recorded; a mode already in
// its saved state is left alone (so ?6 does not re-home the cursor).
void Terminal::restore_private_modes(const CsiSeq& s) {
    for (int i = 0; i < s.count; ++i) {
        const int mode = s.raw(i, -1);
        auto it = xtsaved_.find(mode);
        if (it == xtsaved_.end()) continue;
        if ((private_mode_state(mode) == 1) != it->second) set_private_mode(mode, it->second);
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
