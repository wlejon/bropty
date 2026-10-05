#pragma once
// Terminal: a headless xterm-compatible emulator. Feed it the bytes an
// application writes to its PTY; read the screen, history and state back.
// Everything the terminal says back to the application (device-attribute and
// status replies, OSC color/clipboard query answers) and every side effect
// meant for the embedder (bell, title, clipboard, notifications) goes through
// a TerminalHost. Terminal itself never touches a PTY, so it is fully
// deterministic and testable; Session (session.h) glues one to a PTY.
//
// Not thread-safe: feed(), resize() and the read accessors must be called from
// one thread (or under the embedder's lock). Row views stay valid until the
// next mutating call.

#include "bropty/cell.h"
#include "bropty/color.h"
#include "bropty/grid.h"
#include "bropty/parser.h"
#include "bropty/scrollback.h"
#include "bropty/style.h"
#include "bropty/unicode.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bropty {

class TerminalHost {
public:
    virtual ~TerminalHost() = default;
    // Bytes for the application (replies to queries). Write them to the PTY.
    virtual void write_to_pty(std::string_view bytes) { (void)bytes; }
    virtual void bell() {}
    virtual void title_changed(std::string_view title) { (void)title; }
    virtual void icon_name_changed(std::string_view name) { (void)name; }
    // OSC 7: the shell's working directory as a file:// URI.
    virtual void cwd_changed(std::string_view uri) { (void)uri; }
    // OSC 52 set. `selection` is the raw Pc field ("c", "p", "s", "0".."7"
    // or several of them; empty means the xterm default "s0"). `data` is decoded.
    virtual void clipboard_write(std::string_view selection, std::string_view data) {
        (void)selection;
        (void)data;
    }
    // OSC 52 query. Return the clipboard contents to answer, or nullopt to
    // refuse (the default: reading the clipboard leaks data to the program).
    virtual std::optional<std::string> clipboard_read(std::string_view selection) {
        (void)selection;
        return std::nullopt;
    }
    // OSC 9 / OSC 777 desktop notification.
    virtual void notification(std::string_view title, std::string_view body) {
        (void)title;
        (void)body;
    }
    // OSC 9;4 progress (ConEmu / Windows Terminal): state 0 clear, 1 normal,
    // 2 error, 3 indeterminate, 4 paused; value 0..100.
    virtual void progress(int state, int value) {
        (void)state;
        (void)value;
    }
    // OSC 133 shell integration mark ('A' prompt, 'B' input, 'C' output, 'D' done).
    virtual void semantic_mark(char kind, std::string_view params) {
        (void)kind;
        (void)params;
    }
    // A palette entry or default color changed (OSC 4/10/11/12/104/110..112).
    virtual void palette_changed() {}
    // APC payload (e.g. kitty graphics protocol), raw.
    virtual void apc(std::string_view payload) { (void)payload; }
    // The application changed the terminal's size itself (DECCOLM, when
    // ?40 allows it): the Terminal has already resized; the host should make
    // its window (and the pty) match.
    virtual void resized_by_application(int cols, int rows) {
        (void)cols;
        (void)rows;
    }
};

struct TerminalOptions {
    int cols{80};
    int rows{24};
    size_t scrollback_rows{10000};
    // Mode 2027: cluster with UAX #29 and use cluster widths (VS16, ZWJ, flags).
    // Off = per-code-point wcwidth semantics (zero-width marks still combine).
    bool grapheme_clustering{true};
    bool ambiguous_wide{false};
    std::string answerback;                 // ENQ reply
    std::string term_name{"xterm-256color"};  // XTGETTCAP TN
    size_t max_string_bytes{8u << 20};      // OSC / APC payload cap
};

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
};

struct CursorState {
    int row{0};
    int col{0};
    bool pending_wrap{false};
    bool visible{true};
    CursorShape shape{CursorShape::Block};
    bool blink{true};
};

struct Hyperlink {
    std::string id;   // the OSC 8 id= parameter (may be empty)
    std::string uri;
};

class Terminal final : private ParserSink {
public:
    Terminal();
    explicit Terminal(const TerminalOptions& options);
    Terminal(int cols, int rows, size_t scrollback_rows = 10000);
    ~Terminal() override;
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    void set_host(TerminalHost* host) noexcept { host_ = host; }

    // Process application output.
    void feed(std::string_view bytes);
    void feed(const uint8_t* data, size_t n) { feed(std::string_view(reinterpret_cast<const char*>(data), n)); }

    // Resize; the primary screen and its history reflow, the alternate screen is cropped.
    void resize(int cols, int rows);
    // Cell size in pixels, used to answer XTWINOPS 14/16 and SGR-pixel mouse.
    void set_cell_pixel_size(int width, int height) noexcept { cell_w_ = width; cell_h_ = height; }
    [[nodiscard]] int cell_pixel_width() const noexcept { return cell_w_; }
    [[nodiscard]] int cell_pixel_height() const noexcept { return cell_h_; }
    void reset();  // RIS

    // ---- reading -----------------------------------------------------------
    [[nodiscard]] int cols() const noexcept { return cols_; }
    [[nodiscard]] int rows() const noexcept { return rows_; }
    [[nodiscard]] CursorState cursor() const noexcept;
    [[nodiscard]] const Modes& modes() const noexcept { return modes_; }
    [[nodiscard]] bool alt_screen_active() const noexcept { return active_ == &alt_; }
    [[nodiscard]] uint32_t kitty_keyboard_flags() const noexcept;

    // Screen row y of the active screen, 0 <= y < rows().
    [[nodiscard]] RowView row(int y) const noexcept;
    // History of the primary screen: rows scrolled off the top, 0 = oldest.
    [[nodiscard]] size_t history_rows() const noexcept { return scrollback_.rows(); }
    [[nodiscard]] RowView history_row(size_t i) const { return scrollback_.row(i); }
    [[nodiscard]] const Scrollback& scrollback() const noexcept { return scrollback_; }
    // Primary screen row even while the alternate screen is active.
    [[nodiscard]] RowView primary_row(int y) const noexcept;

    [[nodiscard]] const Style& style(uint32_t id) const noexcept { return styles_.get(id); }
    [[nodiscard]] const StyleTable& styles() const noexcept { return styles_; }
    [[nodiscard]] const Palette& palette() const noexcept { return palette_; }
    [[nodiscard]] const Hyperlink* hyperlink(uint32_t id) const noexcept;

    [[nodiscard]] const std::string& title() const noexcept { return title_; }
    [[nodiscard]] const std::string& icon_name() const noexcept { return icon_name_; }
    [[nodiscard]] const std::string& cwd() const noexcept { return cwd_; }

    // Damage: rows changed since the last clear_dirty().
    [[nodiscard]] bool row_dirty(int y) const noexcept { return active_->grid.dirty(y); }
    void clear_dirty() noexcept { active_->grid.clear_dirty(); }

    // Convenience (tests, debugging): UTF-8 text of a row, trailing spaces trimmed.
    [[nodiscard]] std::string row_text(int y) const { return row(y).text(); }
    [[nodiscard]] std::string history_text(size_t i) const { return history_row(i).text(); }

private:
    struct Charsets {
        char g[4]{'B', 'B', 'B', 'B'};
        uint8_t gl{0};
        int8_t single_shift{-1};
    };
    struct Cursor {
        int row{0};
        int col{0};
        bool pending_wrap{false};
        Style pen;
        uint32_t pen_id{0};
        uint32_t bce_id{0};  // style for erased cells: the pen's background only
        Color bce_bg;        // the background bce_id was made from (when bce_valid)
        bool bce_valid{false};
        bool protect{false};
        Charsets cs;
    };
    struct Saved {
        bool valid{false};
        int row{0};
        int col{0};
        bool pending_wrap{false};
        Style pen;
        bool protect{false};
        bool origin{false};
        Charsets cs;
    };
    struct Screen {
        Grid grid;
        Cursor cur;
        Saved saved;
        std::vector<uint32_t> kitty_flags;  // stack; back() is current
        Screen(int c, int r) : grid(c, r) {}
    };

    // ParserSink
    void print(char32_t cp) override;
    void print_ascii(const char* s, size_t n) override;
    void execute(uint8_t c0) override;
    void esc_dispatch(const EscSeq& seq) override;
    void csi_dispatch(const CsiSeq& seq) override;
    void osc_dispatch(std::string_view payload, bool bel) override;
    void dcs_hook(const CsiSeq& seq) override;
    void dcs_put(std::string_view data) override;
    void dcs_unhook(bool aborted) override;
    void string_dispatch(StringKind kind, std::string_view payload) override;

    // --- terminal.cpp: cursor, scrolling, erasing
    void init(const TerminalOptions& o);
    Grid& grid() noexcept { return active_->grid; }
    Cursor& cur() noexcept { return active_->cur; }
    void update_pen();
    // --- terminal_print.cpp: printing
    void write_cell(int y, int x, char32_t cp, Wide w);
    void clear_wide_at(int y, int x);
    // `seg`: the segmenter state after cp, when the caller already has it;
    // `p`: cp's unicode::properties().
    void print_cluster_start(char32_t cp, int width, const unicode::GraphemeSegmenter* seg, unicode::Props p);
    bool try_extend_cluster(char32_t cp, unicode::Props p, unicode::GraphemeSegmenter& seg, bool& seg_valid);
    void attach_zero_width(char32_t cp);
    void widen_last_cluster();
    void narrow_last_cluster();
    void wrap_line();
    int right_edge() const noexcept;  // last column printing may use
    int left_edge() const noexcept;
    void linefeed();
    void index();
    void reverse_index();
    void carriage_return();
    void backspace();
    void tab_forward(int n);
    void tab_backward(int n);
    void move_to(int row, int col);           // absolute, clamped, honours DECOM
    void move_cursor_rel(int drow, int dcol);  // relative, stays inside margins
    void scroll_up(int n);                     // within the scroll region
    void scroll_down(int n);
    void scroll_region_up(int top, int bottom, int left, int right, int n, bool to_history);
    void scroll_region_down(int top, int bottom, int left, int right, int n);
    void erase_cells(int y, int x0, int x1, bool selective);
    void erase_display(int mode, bool selective);
    void erase_line(int mode, bool selective);
    void insert_chars(int n);
    void delete_chars(int n);
    void insert_lines(int n);
    void delete_lines(int n);
    void erase_chars(int n);
    void repeat_last(int n);
    void fill_alignment();
    void sanitize_row(int y, int x0, int x1);
    void scroll_columns(int top, int bottom, int left, int right, int n);  // n > 0: left
    void soft_reset();
    enum class RectOp : uint8_t { Fill, Erase, SelectiveErase, Copy };
    void rect_op(const CsiSeq& s, RectOp op);
    void save_cursor();
    void restore_cursor();
    void switch_screen(bool alt, bool clear_alt, bool save_restore);
    void reset_margins();
    bool cursor_in_margins() const noexcept;
    void invalidate_print() noexcept { ++epoch_; }
    void maybe_collect_garbage();
    void collect_links();

    // --- terminal_csi.cpp
    void csi_private(const CsiSeq& s);
    void csi_standard(const CsiSeq& s);
    void sgr(const CsiSeq& s);
    void designate(int slot, char final_char, char inter2);
    void kitty_keyboard(const CsiSeq& s);
    void xterm_modkeys(const CsiSeq& s);
    void xterm_fmtkeys(const CsiSeq& s);
    void reset_modkeys();
    void window_op(const CsiSeq& s);
    void device_status(const CsiSeq& s);
    void set_margins(int top, int bottom);
    void set_lr_margins(int left, int right);
    void set_cursor_style(int ps);

    // --- terminal_modes.cpp
    void set_mode(int mode, bool on);
    void set_private_mode(int mode, bool on);
    int mode_state(int mode) const;          // DECRQM Pm value
    int private_mode_state(int mode) const;
    void request_mode(const CsiSeq& s);
    void save_private_modes(const CsiSeq& s);     // XTSAVE
    void restore_private_modes(const CsiSeq& s);  // XTRESTORE
    void set_column_mode(bool wide);              // DECCOLM

    // --- terminal_osc.cpp
    void reply(std::string_view bytes);
    void osc_color(int which, std::string_view spec, bool bel);
    void osc_palette(std::string_view payload, bool bel);
    void osc_hyperlink(std::string_view payload);
    void osc_clipboard(std::string_view payload, bool bel);
    void osc_semantic(std::string_view payload);
    void decrqss(std::string_view request);
    void xtgettcap(std::string_view request);

    // --- terminal_reflow.cpp
    void reflow_primary(int cols, int rows);

    TerminalOptions opts_;
    TerminalHost* host_{nullptr};
    Parser parser_;
    int cols_;
    int rows_;
    Screen primary_;
    Screen alt_;
    Screen* active_;
    Scrollback scrollback_;
    StyleTable styles_;
    Palette palette_;
    Modes modes_;
    std::unordered_map<int, bool> xtsaved_;  // XTSAVE: DEC private mode -> set

    int top_{0};
    int bottom_{0};
    int left_{0};
    int right_{0};
    std::vector<uint8_t> tabs_;

    // Last printed cluster, for combining marks / grapheme continuation and REP.
    struct LastPrint {
        bool valid{false};
        int row{0};
        int col{0};       // column of the cluster's lead cell
        int cur_row{0};   // cursor right after the print
        int cur_col{0};
        bool cur_pending{false};
        uint64_t epoch{0};
        char32_t cp{0};   // base code point (for REP)
        unicode::GraphemeSegmenter seg;
    } last_;
    uint64_t epoch_{1};

    std::unordered_map<uint32_t, Hyperlink> links_;
    std::unordered_map<std::string, uint32_t> link_by_id_;
    uint32_t next_link_{1};
    size_t link_sweep_threshold_{1024};

    std::string title_;
    std::string icon_name_;
    std::string cwd_;
    std::vector<std::pair<std::string, std::string>> title_stack_;

    CursorShape cursor_shape_{CursorShape::Block};
    bool cursor_shape_blink_{true};
    int cell_w_{0};
    int cell_h_{0};

    // DCS in progress.
    enum class Dcs : uint8_t { None, Decrqss, Xtgettcap, Ignore } dcs_{Dcs::None};
    std::string dcs_data_;
};

} // namespace bropty
