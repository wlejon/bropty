#include "bropty/csi_handler.h"
#include <string>

namespace bropty {

CsiHandler::CsiHandler(Grid& primary_grid,
                       Grid& alt_grid,
                       ScrollbackBuffer& scrollback,
                       TerminalModes& modes)
    : primary_grid_(primary_grid),
      alt_grid_(alt_grid),
      scrollback_(scrollback),
      modes_(modes) {}

Grid& CsiHandler::active_grid() noexcept {
    return modes_.alt_screen ? alt_grid_ : primary_grid_;
}

const Grid& CsiHandler::active_grid() const noexcept {
    return modes_.alt_screen ? alt_grid_ : primary_grid_;
}

int CsiHandler::get_param(const std::vector<CsiParamValue>& params, size_t index, int default_val) {
    if (index >= params.size() || params[index].value <= 0) {
        return default_val;
    }
    return params[index].value;
}

void CsiHandler::handle_csi(char final_char,
                            const std::vector<CsiParamValue>& params,
                            const std::vector<char>& intermediates,
                            char prefix) {
    Grid& grid = active_grid();

    // Check for intermediate characters (e.g. DECSTR CSI ! p)
    if (!intermediates.empty()) {
        if (intermediates.size() == 1 && intermediates[0] == '!' && final_char == 'p') {
            handle_soft_reset();
            return;
        }
    }

    // Check for private mode sequences (prefix '?')
    if (prefix == '?') {
        if (final_char == 'h' || final_char == 'l') {
            bool enable = (final_char == 'h');
            for (const auto& p : params) {
                if (p.value > 0) {
                    handle_private_mode(p.value, enable);
                }
            }
        }
        return;
    }

    switch (final_char) {
        case 'A': { // CUU - Cursor Up
            int count = get_param(params, 0, 1);
            grid.move_cursor_rel(-count, 0);
            break;
        }
        case 'B': { // CUD - Cursor Down
            int count = get_param(params, 0, 1);
            grid.move_cursor_rel(count, 0);
            break;
        }
        case 'C': { // CUF - Cursor Forward
            int count = get_param(params, 0, 1);
            grid.move_cursor_rel(0, count);
            break;
        }
        case 'D': { // CUB - Cursor Back
            int count = get_param(params, 0, 1);
            grid.move_cursor_rel(0, -count);
            break;
        }
        case 'E': { // CNL - Cursor Next Line
            int count = get_param(params, 0, 1);
            grid.set_cursor_pos(grid.cursor().row + count, 0);
            break;
        }
        case 'F': { // CPL - Cursor Previous Line
            int count = get_param(params, 0, 1);
            grid.set_cursor_pos(grid.cursor().row - count, 0);
            break;
        }
        case 'G': { // CHA - Cursor Character Absolute
            int col = get_param(params, 0, 1) - 1;
            grid.set_cursor_pos(grid.cursor().row, col);
            break;
        }
        case 'H':   // CUP - Cursor Position
        case 'f': { // HVP - Horizontal and Vertical Position
            int row = get_param(params, 0, 1) - 1;
            int col = get_param(params, 1, 1) - 1;
            grid.set_cursor_pos(row, col);
            break;
        }
        case 'd': { // VPA - Line Position Absolute
            int row = get_param(params, 0, 1) - 1;
            grid.set_cursor_pos(row, grid.cursor().col);
            break;
        }
        case 'J': { // ED - Erase in Display
            int mode = (params.empty() || params[0].value < 0) ? 0 : params[0].value;
            if (mode == 3) {
                // Clear scrollback
                scrollback_.clear();
                if (clear_scrollback_cb_) clear_scrollback_cb_();
                grid.erase_in_display(2);
            } else {
                grid.erase_in_display(mode);
            }
            break;
        }
        case 'K': { // EL - Erase in Line
            int mode = (params.empty() || params[0].value < 0) ? 0 : params[0].value;
            grid.erase_in_line(mode);
            break;
        }
        case 'L': { // IL - Insert Lines
            int count = get_param(params, 0, 1);
            grid.insert_lines(count);
            break;
        }
        case 'M': { // DL - Delete Lines
            int count = get_param(params, 0, 1);
            grid.delete_lines(count);
            break;
        }
        case 'P': { // DCH - Delete Characters
            int count = get_param(params, 0, 1);
            grid.delete_chars(count);
            break;
        }
        case '@': { // ICH - Insert Characters
            int count = get_param(params, 0, 1);
            grid.insert_chars(count);
            break;
        }
        case 'X': { // ECH - Erase Characters
            int count = get_param(params, 0, 1);
            grid.erase_chars(count);
            break;
        }
        case 'S': { // SU - Scroll Up
            int count = get_param(params, 0, 1);
            grid.scroll_up(count, [this](std::vector<Cell> cells, bool wrapped) {
                if (!modes_.alt_screen) {
                    scrollback_.push_line(std::move(cells), wrapped);
                }
            });
            break;
        }
        case 'T': { // SD - Scroll Down
            int count = get_param(params, 0, 1);
            grid.scroll_down(count);
            break;
        }
        case 'r': { // DECSTBM - Set Scrolling Margins
            int top = (params.empty() || params[0].value <= 0) ? 0 : params[0].value - 1;
            int bottom = (params.size() < 2 || params[1].value <= 0) ? grid.rows() - 1 : params[1].value - 1;
            grid.set_margins(top, bottom);
            break;
        }
        case 'm': { // SGR - Select Graphic Rendition
            handle_sgr(params);
            break;
        }
        case 's': { // Save cursor
            grid.save_cursor();
            break;
        }
        case 'u': { // Restore cursor
            grid.restore_cursor();
            break;
        }
        case 'n': { // DSR - Device Status Report
            int mode = get_param(params, 0, 0);
            if (mode == 6 && response_cb_) {
                // CPR: Cursor Position Report \x1b[row;colR
                std::string resp = "\x1b[" + std::to_string(grid.cursor().row + 1) + ";" +
                                   std::to_string(grid.cursor().col + 1) + "R";
                response_cb_(resp);
            }
            break;
        }
        default:
            break;
    }
}

namespace {

void parse_color(const std::vector<CsiParamValue>& params, size_t& i, Color& out_color) {
    const auto& p = params[i];
    // Check if subparams exist: 38:5:n or 38:2::r:g:b or 38:2:cs:r:g:b
    if (!p.subparams.empty()) {
        int color_type = p.subparams[0];
        if (color_type == 5 && p.subparams.size() >= 2) {
            out_color = Color::from_index(static_cast<uint8_t>(p.subparams[1]));
            return;
        }
        if (color_type == 2) {
            // subparams could be: [2, r, g, b] or [2, -1, r, g, b] or [2, cs, r, g, b]
            if (p.subparams.size() >= 5) {
                int r = p.subparams[2] >= 0 ? p.subparams[2] : 0;
                int g = p.subparams[3] >= 0 ? p.subparams[3] : 0;
                int b = p.subparams[4] >= 0 ? p.subparams[4] : 0;
                out_color = Color::from_rgb(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b));
                return;
            } else if (p.subparams.size() >= 4) {
                int r = p.subparams[1] >= 0 ? p.subparams[1] : 0;
                int g = p.subparams[2] >= 0 ? p.subparams[2] : 0;
                int b = p.subparams[3] >= 0 ? p.subparams[3] : 0;
                out_color = Color::from_rgb(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b));
                return;
            }
        }
        return;
    }

    // Semicolon format: 38;5;n or 38;2;r;g;b
    if (i + 1 < params.size()) {
        int color_type = params[i + 1].value;
        if (color_type == 5 && i + 2 < params.size()) {
            int idx = params[i + 2].value;
            out_color = Color::from_index(static_cast<uint8_t>(idx >= 0 ? idx : 0));
            i += 2;
            return;
        }
        if (color_type == 2 && i + 4 < params.size()) {
            int r = params[i + 2].value >= 0 ? params[i + 2].value : 0;
            int g = params[i + 3].value >= 0 ? params[i + 3].value : 0;
            int b = params[i + 4].value >= 0 ? params[i + 4].value : 0;
            out_color = Color::from_rgb(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b));
            i += 4;
            return;
        }
    }
}

} // namespace

void CsiHandler::handle_sgr(const std::vector<CsiParamValue>& params) {
    Grid& grid = active_grid();
    Cell& pen = grid.cursor().pen;

    if (params.empty()) {
        pen.reset();
        return;
    }

    for (size_t i = 0; i < params.size(); ++i) {
        int val = params[i].value;
        if (val <= 0) { // 0 or omitted
            pen.reset();
            continue;
        }

        switch (val) {
            case 1: pen.set_flag(CellFlag_Bold, true); break;
            case 2: pen.set_flag(CellFlag_Dim, true); break;
            case 3: pen.set_flag(CellFlag_Italic, true); break;
            case 4: {
                // Check for underline style subparameter: 4:0 (none), 4:1 (single), 4:2 (double), 4:3 (curly), 4:4 (dotted), 4:5 (dashed)
                pen.flags &= ~CellFlag_UnderlineMask;
                if (!params[i].subparams.empty()) {
                    int sub = params[i].subparams[0];
                    if (sub == 1) pen.set_flag(CellFlag_UnderlineSingle, true);
                    else if (sub == 2) pen.set_flag(CellFlag_UnderlineDouble, true);
                    else if (sub == 3) pen.set_flag(CellFlag_UnderlineCurly, true);
                    else if (sub == 4) pen.set_flag(CellFlag_UnderlineDotted, true);
                    else if (sub == 5) pen.set_flag(CellFlag_UnderlineDashed, true);
                } else {
                    pen.set_flag(CellFlag_UnderlineSingle, true);
                }
                break;
            }
            case 5:
            case 6: pen.set_flag(CellFlag_Blink, true); break;
            case 7: pen.set_flag(CellFlag_Inverse, true); break;
            case 8: pen.set_flag(CellFlag_Hidden, true); break;
            case 9: pen.set_flag(CellFlag_Strikethrough, true); break;
            case 21: {
                pen.flags &= ~CellFlag_UnderlineMask;
                pen.set_flag(CellFlag_UnderlineDouble, true);
                break;
            }
            case 22:
                pen.set_flag(CellFlag_Bold, false);
                pen.set_flag(CellFlag_Dim, false);
                break;
            case 23: pen.set_flag(CellFlag_Italic, false); break;
            case 24: pen.flags &= ~CellFlag_UnderlineMask; break;
            case 25: pen.set_flag(CellFlag_Blink, false); break;
            case 27: pen.set_flag(CellFlag_Inverse, false); break;
            case 28: pen.set_flag(CellFlag_Hidden, false); break;
            case 29: pen.set_flag(CellFlag_Strikethrough, false); break;

            // 16-color FG
            case 30: case 31: case 32: case 33:
            case 34: case 35: case 36: case 37:
                pen.fg = Color::from_index(static_cast<uint8_t>(val - 30));
                break;
            case 38:
                parse_color(params, i, pen.fg);
                break;
            case 39:
                pen.fg = Color::default_color();
                break;

            // 16-color BG
            case 40: case 41: case 42: case 43:
            case 44: case 45: case 46: case 47:
                pen.bg = Color::from_index(static_cast<uint8_t>(val - 40));
                break;
            case 48:
                parse_color(params, i, pen.bg);
                break;
            case 49:
                pen.bg = Color::default_color();
                break;

            // Underline color
            case 58:
                parse_color(params, i, pen.underline_color);
                break;
            case 59:
                pen.underline_color = Color::default_color();
                break;

            // Bright FG (8-15)
            case 90: case 91: case 92: case 93:
            case 94: case 95: case 96: case 97:
                pen.fg = Color::from_index(static_cast<uint8_t>(val - 90 + 8));
                break;

            // Bright BG (8-15)
            case 100: case 101: case 102: case 103:
            case 104: case 105: case 106: case 107:
                pen.bg = Color::from_index(static_cast<uint8_t>(val - 100 + 8));
                break;

            default:
                break;
        }
    }
}

void CsiHandler::handle_private_mode(int mode, bool enable) {
    switch (mode) {
        case 1: // DECCKM - Application Cursor Keys
            modes_.application_cursor_keys = enable;
            break;
        case 7: // DECAWM - Auto-wrap
            modes_.auto_wrap = enable;
            primary_grid_.set_auto_wrap(enable);
            alt_grid_.set_auto_wrap(enable);
            break;
        case 12:
        case 13: // Cursor blink
            modes_.cursor_blinking = enable;
            primary_grid_.cursor().blinking = enable;
            alt_grid_.cursor().blinking = enable;
            break;
        case 25: // DECTCEM - Cursor visibility
            modes_.cursor_visible = enable;
            primary_grid_.cursor().visible = enable;
            alt_grid_.cursor().visible = enable;
            break;
        case 47:
        case 1047: { // Alternate Screen Buffer
            if (modes_.alt_screen != enable) {
                modes_.alt_screen = enable;
                if (alt_screen_cb_) alt_screen_cb_(enable);
                active_grid().mark_all_dirty();
            }
            break;
        }
        case 1049: { // Alternate Screen Buffer + save/restore cursor & clear
            if (enable) {
                primary_grid_.save_cursor();
                modes_.alt_screen = true;
                alt_grid_.erase_in_display(2);
                alt_grid_.set_cursor_pos(0, 0);
                if (alt_screen_cb_) alt_screen_cb_(true);
                alt_grid_.mark_all_dirty();
            } else {
                modes_.alt_screen = false;
                primary_grid_.restore_cursor();
                if (alt_screen_cb_) alt_screen_cb_(false);
                primary_grid_.mark_all_dirty();
            }
            break;
        }
        case 1000:
            modes_.mouse_mode = enable ? MouseMode::Click : MouseMode::None;
            break;
        case 1002:
            modes_.mouse_mode = enable ? MouseMode::Button : MouseMode::None;
            break;
        case 1003:
            modes_.mouse_mode = enable ? MouseMode::Any : MouseMode::None;
            break;
        case 1004:
            modes_.focus_reporting = enable;
            break;
        case 1006:
            modes_.sgr_mouse = enable;
            break;
        case 2004: // Bracketed paste
            modes_.bracketed_paste = enable;
            break;
        default:
            break;
    }
}

void CsiHandler::handle_soft_reset() {
    Grid& grid = active_grid();
    grid.cursor().pen.reset();
    grid.cursor().visible = true;
    grid.reset_margins();
    grid.set_origin_mode(false);
    modes_.auto_wrap = true;
    grid.set_auto_wrap(true);
    modes_.application_cursor_keys = false;
    modes_.bracketed_paste = false;
    modes_.mouse_mode = MouseMode::None;
    modes_.sgr_mouse = false;
}

} // namespace bropty
