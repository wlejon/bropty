#pragma once
// Terminal modes and cursor state: what a reader of a terminal's buffer (a
// renderer, a TerminalView, a remote mirror) needs besides its rows.

#include <cstdint>

namespace bropty {

enum class CursorShape : uint8_t { Block, Underline, Bar };
enum class MouseTracking : uint8_t { None, X10, Normal, Button, Any };
enum class MouseEncoding : uint8_t { Default, Utf8, Sgr, Urxvt, SgrPixels };

struct Modes {
    bool insert{false};            // IRM (4)
    bool linefeed_newline{false};  // LNM (20)
    bool app_cursor_keys{false};   // DECCKM (?1)
    bool reverse_video{false};     // DECSCNM (?5)
    bool origin{false};            // DECOM (?6)
    bool autowrap{true};           // DECAWM (?7)
    bool cursor_blink{false};      // ?12
    bool cursor_visible{true};     // DECTCEM (?25)
    bool reverse_wrap{false};      // ?45
    bool app_keypad{false};        // DECNKM (?66) / DECKPAM
    bool backarrow_sends_bs{false};  // DECBKM (?67)
    bool left_right_margins{false};  // DECLRMM (?69)
    MouseTracking mouse_tracking{MouseTracking::None};  // ?9 ?1000 ?1002 ?1003
    MouseEncoding mouse_encoding{MouseEncoding::Default};  // ?1005 ?1006 ?1015 ?1016
    bool focus_events{false};       // ?1004
    bool alternate_scroll{false};   // ?1007
    bool meta_sends_escape{true};   // ?1036
    bool alt_sends_escape{true};    // ?1039
    bool bracketed_paste{false};    // ?2004
    bool synchronized_output{false};  // ?2026
    bool grapheme_clustering{true};   // ?2027
    bool color_scheme_updates{false};  // ?2031
    bool in_band_resize{false};        // ?2048
    bool allow_deccolm{false};         // ?40: let DECCOLM (?3) change the width
    bool deccolm{false};               // ?3: 132 columns (honoured only under ?40)
    bool deccolm_no_clear{false};      // ?95 DECNCSM: DECCOLM keeps the screen
    // xterm's key-modifier resources, set by XTMODKEYS (CSI > Pp ; Pv m),
    // disabled (-1) by CSI > Pp n, reported by XTQMODKEYS (CSI ? Pp m). The
    // initial values are xterm's. Cursor / function / keypad / other keys shape
    // the legacy encoder (see input.h); keyboard, modifier and special keys are
    // kept and reported but select nothing bropty encodes differently.
    int modify_keyboard{0};            // Pp 0
    int modify_cursor_keys{2};         // Pp 1: -1 .. 3
    int modify_function_keys{2};       // Pp 2: -1 .. 3
    int modify_keypad_keys{0};         // Pp 3: -1 .. 3
    int modify_other_keys{0};          // Pp 4: 0, 1, 2 (3 = 2)
    int modify_modifier_keys{0};       // Pp 6
    int modify_special_keys{0};        // Pp 7
    int format_other_keys{0};          // XTFMTKEYS (CSI > 4 ; Pv f): 0 = CSI 27;m;c ~, 1 = CSI c;m u
    // Sixel (xterm): ?80 DECSDM, sixel display mode (no scrolling: images at
    // the top-left, the cursor stays); ?1070, a fresh palette per image
    // (reset: registers persist between images); ?8452, the cursor ends to
    // the right of an image instead of at its left edge (both on its last row).
    bool sixel_display_mode{false};
    bool sixel_private_colors{true};
    bool sixel_cursor_right{false};
};

struct CursorState {
    int row{0};
    int col{0};
    bool pending_wrap{false};
    bool visible{true};
    CursorShape shape{CursorShape::Block};
    bool blink{true};
};

} // namespace bropty
