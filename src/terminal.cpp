#include "bropty/terminal.h"
#include <algorithm>
#include <vector>

namespace bropty {

int codepoint_width(uint32_t cp) noexcept {
    if (cp == 0 || (cp >= 0x01 && cp <= 0x1F) || (cp >= 0x7F && cp <= 0x9F)) {
        return 0;
    }
    // Combining characters
    if ((cp >= 0x0300 && cp <= 0x036F) ||
        (cp >= 0x1DC0 && cp <= 0x1DFF) ||
        (cp >= 0x20D0 && cp <= 0x20FF) ||
        (cp >= 0xFE20 && cp <= 0xFE2F)) {
        return 0;
    }
    // East Asian Wide & Fullwidth & Emoji
    if ((cp >= 0x1100 && cp <= 0x115F) ||
        (cp == 0x2329 || cp == 0x232A) ||
        (cp >= 0x2E80 && cp <= 0x303E) ||
        (cp >= 0x3040 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE10 && cp <= 0xFE19) ||
        (cp >= 0xFE30 && cp <= 0xFE6F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x1F300 && cp <= 0x1F64F) ||
        (cp >= 0x1F680 && cp <= 0x1F6FF) ||
        (cp >= 0x1F900 && cp <= 0x1F9FF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD)) {
        return 2;
    }
    return 1;
}

Terminal::Terminal(int cols, int rows, size_t max_scrollback_lines)
    : cols_(std::max(1, cols)),
      rows_(std::max(1, rows)),
      primary_grid_(cols_, rows_),
      alt_grid_(cols_, rows_),
      scrollback_(max_scrollback_lines),
      parser_(this),
      csi_handler_(primary_grid_, alt_grid_, scrollback_, modes_) {

    csi_handler_.set_response_callback([this](std::string_view resp) {
        send_to_pty(resp);
    });

    osc_handler_.set_response_callback([this](std::string_view resp) {
        send_to_pty(resp);
    });
}

void Terminal::attach_pty(std::shared_ptr<IPtyProcess> pty) {
    pty_ = std::move(pty);
    if (pty_) {
        pty_->resize(cols_, rows_);
    }
}

void Terminal::process_input(std::string_view data) {
    parser_.feed(data);
}

void Terminal::process_input(const uint8_t* data, size_t length) {
    parser_.feed(data, length);
}

size_t Terminal::update() {
    if (!pty_) return 0;

    uint8_t buffer[16384];
    size_t total_processed = 0;

    while (true) {
        size_t n = pty_->read_nonblocking(buffer, sizeof(buffer));
        if (n == 0) break;
        process_input(buffer, n);
        total_processed += n;
    }

    return total_processed;
}

void Terminal::send_key(Key key, uint32_t codepoint, uint8_t modifiers, KeyEventType event_type) {
    std::string encoded = KeyEncoder::encode_key(
        key,
        codepoint,
        modifiers,
        modes_.application_cursor_keys,
        false, // Standard protocol by default
        event_type);

    if (!encoded.empty()) {
        send_to_pty(encoded);
    }
}

void Terminal::send_text(std::string_view text) {
    std::string encoded = KeyEncoder::encode_paste(text, modes_.bracketed_paste);
    send_to_pty(encoded);
}

void Terminal::send_mouse(MouseButton button,
                          MouseAction action,
                          uint8_t modifiers,
                          int col,
                          int row) {
    if (modes_.mouse_mode == MouseMode::None) return;

    if (modes_.mouse_mode == MouseMode::Click && (action == MouseAction::Move || action == MouseAction::Drag)) {
        return;
    }
    if (modes_.mouse_mode == MouseMode::Button && action == MouseAction::Move) {
        return;
    }

    if (modes_.sgr_mouse) {
        std::string sgr = KeyEncoder::encode_mouse_sgr(button, action, modifiers, col, row);
        send_to_pty(sgr);
    }
}

void Terminal::resize(int cols, int rows) {
    cols = std::max(1, cols);
    rows = std::max(1, rows);

    if (cols == cols_ && rows == rows_) return;

    cols_ = cols;
    rows_ = rows;

    primary_grid_.resize(cols, rows);
    alt_grid_.resize(cols, rows);
    scrollback_.reflow(cols);

    if (pty_) {
        pty_->resize(cols, rows);
    }
}

void Terminal::reset() {
    parser_.reset();
    primary_grid_ = Grid(cols_, rows_);
    alt_grid_ = Grid(cols_, rows_);
    scrollback_.clear();
    modes_ = TerminalModes{};
}

Grid& Terminal::active_grid() noexcept {
    return modes_.alt_screen ? alt_grid_ : primary_grid_;
}

const Grid& Terminal::active_grid() const noexcept {
    return modes_.alt_screen ? alt_grid_ : primary_grid_;
}

void Terminal::set_title_callback(TitleCallback cb) {
    osc_handler_.set_title_callback(std::move(cb));
}

void Terminal::set_cwd_callback(CwdCallback cb) {
    osc_handler_.set_cwd_callback(std::move(cb));
}

void Terminal::set_shell_marker_callback(ShellMarkerCallback cb) {
    osc_handler_.set_shell_marker_callback(std::move(cb));
}

void Terminal::send_to_pty(std::string_view data) {
    if (pty_) {
        pty_->write(data);
    }
    if (output_cb_) {
        output_cb_(data);
    }
}

void Terminal::on_print(uint32_t codepoint) {
    int w = codepoint_width(codepoint);
    if (w <= 0) return;

    Grid& grid = active_grid();
    grid.cursor().pen.hyperlink_id = osc_handler_.current_hyperlink_id();

    grid.write_char(codepoint, static_cast<uint8_t>(w), [this](std::vector<Cell> cells, bool wrapped) {
        if (!modes_.alt_screen) {
            scrollback_.push_line(std::move(cells), wrapped);
        }
    });
}

void Terminal::on_execute(uint8_t byte) {
    Grid& grid = active_grid();

    switch (byte) {
        case 0x07: // BEL
            if (bell_cb_) bell_cb_();
            break;
        case 0x08: // BS (Backspace)
            grid.move_cursor_rel(0, -1);
            break;
        case 0x09: { // TAB (Horizontal Tab)
            int current_col = grid.cursor().col;
            int next_tab = ((current_col / 8) + 1) * 8;
            next_tab = std::min(next_tab, cols_ - 1);
            grid.set_cursor_pos(grid.cursor().row, next_tab);
            break;
        }
        case 0x0A: // LF (Linefeed)
        case 0x0B: // VT
        case 0x0C: { // FF
            if (grid.cursor().row == grid.bottom_margin()) {
                grid.scroll_up(1, [this](std::vector<Cell> cells, bool wrapped) {
                    if (!modes_.alt_screen) {
                        scrollback_.push_line(std::move(cells), wrapped);
                    }
                });
            } else if (grid.cursor().row + 1 < rows_) {
                grid.set_cursor_pos(grid.cursor().row + 1, grid.cursor().col);
            }
            break;
        }
        case 0x0D: // CR (Carriage Return)
            grid.set_cursor_pos(grid.cursor().row, 0);
            break;
        default:
            break;
    }
}

void Terminal::on_csi(char final_char,
                      const std::vector<CsiParamValue>& params,
                      const std::vector<char>& intermediates,
                      char prefix) {
    csi_handler_.handle_csi(final_char, params, intermediates, prefix);
}

void Terminal::on_osc(int command, std::string_view payload) {
    osc_handler_.handle_osc(command, payload);
}

void Terminal::on_esc(char final_char, const std::vector<char>& intermediates) {
    Grid& grid = active_grid();

    if (intermediates.empty()) {
        switch (final_char) {
            case '7': // DECSC - Save Cursor
                grid.save_cursor();
                break;
            case '8': // DECRC - Restore Cursor
                grid.restore_cursor();
                break;
            case 'c': // RIS - Reset Initial State
                reset();
                break;
            default:
                break;
        }
    }
}

} // namespace bropty
