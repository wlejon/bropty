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
// next mutating call. A renderer on another thread reads Frames instead
// (TerminalView, view.h / frame.h): immutable snapshots handed over through a
// lock-free FrameChannel, so it never touches the Terminal or stalls it.

#include "bropty/cell.h"
#include "bropty/color.h"
#include "bropty/graphics.h"
#include "bropty/grid.h"
#include "bropty/modes.h"
#include "bropty/parser.h"
#include "bropty/position.h"
#include "bropty/row_source.h"
#include "bropty/scrollback.h"
#include "bropty/style.h"
#include "bropty/unicode.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bropty {

namespace detail {
class Graphics;
struct KittyCommand;
}

// A desktop notification a program asked for (TerminalHost::notification_ex).
struct Notification {
    std::string id;     // OSC 99 i= (sanitised to [A-Za-z0-9_+.-]); empty otherwise
    std::string title;  // OSC 99: the body when no title was sent; OSC 9: empty
    std::string body;
    int urgency{-1};    // OSC 99 u=: 0 low, 1 normal, 2 critical; -1 not given
    std::string source; // "osc9", "osc777" or "osc99"
};

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
    // Under ConPTY a native console program's query never arrives, and no
    // OSC reply would reach it (see "Replies under ConPTY" in pty.h).
    virtual std::optional<std::string> clipboard_read(std::string_view selection) {
        (void)selection;
        return std::nullopt;
    }
    // OSC 52 query, answered later (the clipboard is behind a permission
    // prompt, another process, or a network round trip). Asked first; return
    // true to take the query, then answer it with
    // Terminal::answer_clipboard(request, data) -- or drop it with
    // Terminal::cancel_clipboard(request) -- whenever the answer is known,
    // from the terminal's thread. The reply is written then, terminated as
    // the query was. Return false (the default) to have clipboard_read()
    // answer at once instead. `selection` is the raw Pc field (empty: "s0").
    virtual bool clipboard_read_async(uint64_t request, std::string_view selection) {
        (void)request;
        (void)selection;
        return false;
    }
    // OSC 9 / OSC 777 desktop notification. (Called by notification_ex's
    // default; a host overriding notification_ex does not get these.)
    virtual void notification(std::string_view title, std::string_view body) {
        (void)title;
        (void)body;
    }
    // Every desktop notification: OSC 9, OSC 777 and kitty's OSC 99 (once
    // all its chunks have arrived). The default passes title and body to
    // notification(), so a host overriding only that one sees all three.
    virtual void notification_ex(const Notification& n) { notification(n.title, n.body); }
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
    // APC payload, raw. Kitty graphics commands (payloads starting with 'G')
    // are handled by the terminal and not passed on, unless
    // GraphicsOptions::kitty is off.
    virtual void apc(std::string_view payload) { (void)payload; }
    // Decode a compressed image: PNG for kitty f=100, any format the host
    // supports for iTerm2 inline images. Fill `out` (RGBA, one frame per
    // animation frame) within `limits` and return true, or return false to
    // refuse (the default: bropty links no image codecs). Called on the
    // terminal's thread, inside feed().
    virtual bool decode_image(std::string_view data, const ImageLimits& limits, DecodedImage& out) {
        (void)data;
        (void)limits;
        (void)out;
        return false;
    }
    // The application changed the terminal's size itself (DECCOLM, when
    // ?40 allows it): the Terminal has already resized; the host should make
    // its window (and the pty) match.
    virtual void resized_by_application(int cols, int rows) {
        (void)cols;
        (void)rows;
    }
    // OSC 22: the mouse pointer shape the application asks for over the
    // terminal changed (also on a switch between the screens, which keep
    // separate shape stacks, and on RIS). `name` is a CSS cursor name
    // ("text", "pointer", "crosshair", "ew-resize", ...; X11 aliases arrive
    // already mapped), or empty for the host's default. Terminal::pointer_shape()
    // holds the same.
    virtual void pointer_shape_changed(std::string_view name) { (void)name; }
};

// The CSS cursor name an OSC 22 pointer shape name stands for: the name
// itself, or the CSS name an X11 / legacy alias (kitty's table: "left_ptr",
// "fleur", "hand2", ...) maps to. Empty for an unknown name.
[[nodiscard]] std::string_view pointer_shape_css_name(std::string_view name) noexcept;

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
    // OSC / APC payload cap. It also bounds an iTerm2 File= image sent in one
    // OSC (multipart transfers and kitty chunks are not affected).
    size_t max_string_bytes{8u << 20};
    GraphicsOptions graphics;  // inline images (graphics.h)
    // OSC 22 queries ?__default__ / ?__grabbed__ answer with these (CSS
    // names): the host's pointer over text, and while the program grabs the
    // mouse (mouse reporting on). kitty's defaults.
    std::string default_pointer_shape{"text"};
    std::string grabbed_pointer_shape{"default"};
};

// One shell command, from OSC 133 shell-integration marks on the primary
// screen (Terminal::commands()). Positions are absolute (position.h): the
// cursor where each mark arrived. They stay on their text as it scrolls into
// history, and a resize carries them through the reflow to the same
// characters.
struct CommandRecord {
    RowPos prompt;                 // 'A': the prompt starts
    std::optional<RowPos> input;   // 'B': the command line starts (the prompt ends)
    std::optional<RowPos> output;  // 'C': the command runs; its output starts
    std::optional<RowPos> end;     // 'D': it finished
    std::optional<int> exit_code;  // 'D;<code>'
    // Done: 'D' arrived, or the next prompt began without one (no exit code).
    bool finished{false};
    // The front of the record fell off history (capacity, ED 3): positions
    // that were evicted read as history_first_row(), column 0.
    bool trimmed{false};
    // The command line, when the shell says it: OSC 133;C;cmdline=<text> or
    // cmdline_url=<percent-encoded> (kitty), or OSC 633;E;<escaped> (VS Code).
    std::string command_line;
};

struct Hyperlink {
    std::string id;   // the OSC 8 id= parameter (may be empty)
    std::string uri;
};

// A Terminal is a RowSource (row_source.h): Selection, Search, link
// detection and TerminalView read it through that interface. (ParserSink
// stays the first base: the parser's calls into the terminal, the hot path,
// then need no this-adjusting thunks.)
class Terminal final : private ParserSink, public RowSource {
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
    [[nodiscard]] int cols() const noexcept override { return cols_; }
    [[nodiscard]] int rows() const noexcept override { return rows_; }
    [[nodiscard]] CursorState cursor() const noexcept override;
    [[nodiscard]] const Modes& modes() const noexcept override { return modes_; }
    [[nodiscard]] bool alt_screen_active() const noexcept override { return active_ == &alt_; }
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
    [[nodiscard]] const Palette& palette() const noexcept override { return palette_; }
    // The host's theme: what OSC 104 (all colors or some), OSC 110 / 111 /
    // 112 and RIS reset to. Setting it replaces the live palette entirely,
    // dropping what programs set with OSC 4 / 10 / 11 / 12 (as kitty does on
    // a theme change), and calls TerminalHost::palette_changed(). Until set:
    // Palette::standard().
    void set_base_palette(const Palette& p);
    [[nodiscard]] const Palette& base_palette() const noexcept { return base_palette_; }
    [[nodiscard]] const Hyperlink* hyperlink(uint32_t id) const noexcept;
    // RowSource: the terminal's hyperlink ids are one space for every row.
    [[nodiscard]] const std::string* hyperlink_uri(int64_t row, uint32_t id) const noexcept override {
        (void)row;
        const Hyperlink* h = hyperlink(id);
        return h ? &h->uri : nullptr;
    }

    // ---- OSC 52 queries taken by TerminalHost::clipboard_read_async() --------
    // Answer one with the clipboard's contents: the reply is written through
    // the host, terminated as the query was. False when `request` is not
    // pending (answered or cancelled already, or dropped: at most
    // kMaxClipboardRequests wait, the oldest go first).
    bool answer_clipboard(uint64_t request, std::string_view data);
    // Drop one unanswered (the program gets no reply, as when refused).
    bool cancel_clipboard(uint64_t request);
    [[nodiscard]] size_t pending_clipboard_requests() const noexcept { return clip_pending_.size(); }
    static constexpr size_t kMaxClipboardRequests = 16;

    // OSC 99 bounds: notifications receiving chunks at once (the oldest
    // incomplete one is dropped beyond), and the title + body bytes of one
    // (also at most TerminalOptions::max_string_bytes; one that grows past
    // it is dropped).
    static constexpr size_t kMaxPendingNotifications = 16;
    static constexpr size_t kMaxNotificationBytes = 64u << 10;

    [[nodiscard]] const std::string& title() const noexcept { return title_; }
    [[nodiscard]] const std::string& icon_name() const noexcept { return icon_name_; }
    [[nodiscard]] const std::string& cwd() const noexcept { return cwd_; }
    // OSC 22: the active screen's current pointer shape, a CSS cursor name;
    // empty means the host's default (TerminalHost::pointer_shape_changed).
    [[nodiscard]] const std::string& pointer_shape() const noexcept { return pointer_shape_; }

    // Damage: rows changed since the last clear_dirty().
    [[nodiscard]] bool row_dirty(int y) const noexcept { return active_->grid.dirty(y); }
    void clear_dirty() noexcept { active_->grid.clear_dirty(); }

    // Convenience (tests, debugging): UTF-8 text of a row, trailing spaces trimmed.
    [[nodiscard]] std::string row_text(int y) const { return row(y).text(); }
    [[nodiscard]] std::string history_text(size_t i) const { return history_row(i).text(); }

    // ---- absolute rows (position.h) ------------------------------------------
    // The oldest row still held: history row 0, or (on the alternate screen,
    // which has no history) screen row 0.
    [[nodiscard]] int64_t first_row() const noexcept override {
        return alt_screen_active() ? screen_top_row() : history_first_row();
    }
    // Screen row 0 of the active screen.
    [[nodiscard]] int64_t screen_top_row() const noexcept override {
        return int64_t(scrollback_.dropped_rows() + scrollback_.rows());
    }
    // One past the last row.
    [[nodiscard]] int64_t end_row() const noexcept { return screen_top_row() + rows_; }
    // Row `abs` (first_row() <= abs < end_row()); an empty view otherwise.
    // History views share the scrollback's decode cache: read one at a time.
    [[nodiscard]] RowView row_at(int64_t abs) const override;
    // The primary screen's history as absolute rows, also while the
    // alternate screen is active: history_row(i) is row history_first_row() + i.
    // Rows evicted from the front of history (capacity, ED 3) never give
    // their numbers to other text: history_first_row() only grows -- until
    // row_numbering() changes.
    [[nodiscard]] int64_t history_first_row() const noexcept { return int64_t(scrollback_.dropped_rows()); }
    // Bumped whenever rows that already had numbers get new ones: a resize
    // (the primary screen and its history reflow). While it stays the same,
    // an absolute row keeps its number, and a history row its content (except
    // the newest history row's wrap flag while its line continues on the
    // screen: when the next row turns out not to continue it, it unwraps).
    // A reader that caches rows by number drops its cache when this changes.
    [[nodiscard]] uint64_t row_numbering() const noexcept { return numbering_; }
    // RowSource: screen rows' stamps (below); 0 for history rows.
    [[nodiscard]] uint64_t row_serial(int64_t abs) const noexcept override {
        const int64_t y = abs - screen_top_row();
        return y >= 0 && y < rows_ ? row_stamp(int(y)) : 0;
    }
    [[nodiscard]] const Terminal* terminal() const noexcept override { return this; }
    // The cursor as an absolute position.
    [[nodiscard]] RowPos cursor_pos() const noexcept {
        return RowPos{screen_top_row() + active_->cur.row, active_->cur.col};
    }
    // The OSC 133 zone newly printed text gets.
    [[nodiscard]] Zone zone() const noexcept { return zone_; }

    // ---- shell commands (OSC 133) ----------------------------------------------
    // The commands run on the primary screen, oldest first; the last may be
    // in progress (!finished). 'A' starts a record (a prompt that never got
    // to 'C' -- an empty Enter, Ctrl+C at the prompt, a redraw -- is moved to
    // the new prompt instead of kept as a command; secondary prompts, k=s /
    // k=c / k=r, start none); 'B' / 'C' set the input / output starts of the
    // record in progress; 'D[;code]' ends it. Marks while the alternate
    // screen is showing are ignored. Records whose every position left
    // history are dropped; erasing the whole screen (ED 2) drops the finished
    // ones that started on it; RIS drops all. At most kMaxCommands, the
    // oldest dropped first.
    [[nodiscard]] const std::vector<CommandRecord>& commands() const noexcept { return commands_; }
    // Bumped by every change to commands(), positions included.
    [[nodiscard]] uint64_t commands_version() const noexcept { return commands_version_; }
    static constexpr size_t kMaxCommands = 10000;

    // ---- change tracking for readers on the terminal's thread ----------------
    // Bumped by every feed(), resize() and reset(): nothing a reader sees
    // changed while it stays the same.
    [[nodiscard]] uint64_t change_count() const noexcept override { return change_count_; }
    // Per screen row: a content stamp. It changes whenever the row's content
    // does, travels with the row when the screen scrolls, and is a serial:
    // two rows with equal stamps hold equal content, across both screens,
    // resizes and the terminal's whole life (never 0). That holds as long as
    // a reader that compares stamps calls advance_generation() after reading
    // them (and the row content it keeps), so later writes get stamps it has
    // not seen. Several readers may share one terminal: each one advancing
    // after its read keeps it true for all.
    [[nodiscard]] uint64_t row_stamp(int y) const noexcept { return active_->grid.stamp(y); }
    // Which storage row (0 .. rows()-1) screen row y is; follows the row as
    // it scrolls. Storage is per screen and rebuilt by a resize (grid_id()).
    [[nodiscard]] uint32_t row_storage(int y) const noexcept { return active_->grid.storage(y); }
    [[nodiscard]] uint64_t grid_id() const noexcept { return active_->grid.id(); }
    void advance_generation() noexcept override {
        primary_.grid.set_generation(reserve_stamps());
        alt_.grid.set_generation(reserve_stamps());
    }

    // ---- inline images (graphics.h) -------------------------------------------
    // The images and kitty placements of the active screen / of either one.
    [[nodiscard]] const ImageLayer& images() const noexcept { return images(alt_screen_active()); }
    [[nodiscard]] const ImageLayer& images(bool alternate) const noexcept;
    // Decoded bytes held for both screens (bounded by GraphicsOptions::storage_limit).
    [[nodiscard]] size_t image_bytes() const noexcept;
    // Changes whenever an image, a placement or an animation frame does.
    [[nodiscard]] uint64_t images_version() const noexcept;
    [[nodiscard]] const GraphicsOptions& graphics_options() const noexcept { return opts_.graphics; }
    // Change GraphicsOptions::storage_limit at run time. Lowering it evicts
    // images now, as an incoming image would (those without placements
    // first, then the least recently used), until the rest fit.
    void set_image_storage_limit(size_t bytes);
    // The cell size images are laid out with: set_cell_pixel_size()'s, or
    // GraphicsOptions' fallback while none is set.
    [[nodiscard]] int image_cell_width() const noexcept {
        return cell_w_ > 0 ? cell_w_ : std::max(1, opts_.graphics.fallback_cell_width);
    }
    [[nodiscard]] int image_cell_height() const noexcept {
        return cell_h_ > 0 ? cell_h_ : std::max(1, opts_.graphics.fallback_cell_height);
    }
    // Run terminal-driven animations (kitty a=a, animated iTerm2 images) up
    // to `now_ms` on the host's monotonic clock. Returns the milliseconds
    // until the next frame change (UINT64_MAX when nothing is animating);
    // the host calls it again then. A frame change counts as a change
    // (change_count()), so the next frame shows it.
    uint64_t advance_animations(uint64_t now_ms);
    // Whether any cell may hold an image placeholder (U+10EEEE): one was
    // printed or a sixel / iTerm2 image drawn. Sticky; lets readers skip
    // looking for image cells in terminals that never had any.
    [[nodiscard]] bool may_have_image_cells() const noexcept { return image_cells_; }

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
        std::vector<uint8_t> pointer_shapes;  // OSC 22 stack; back() is current (0: default)
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
    // --- terminal_commands.cpp: OSC 133 command records
    void command_mark(char kind, std::string_view params);
    void command_line_633(std::string_view rest);  // OSC 633 ; E ; <command line>
    void commands_trim();          // drop / trim what left history
    void commands_screen_erased();  // ED 2 on the primary screen
    void commands_resize_begin();
    void commands_resize_end();
    void decrqss(std::string_view request);
    void xtgettcap(std::string_view request);
    // --- terminal_pointer.cpp: OSC 22
    void osc_pointer(std::string_view value, bool bel);
    void pointer_shape_sync();  // re-read the active screen's shape; tell the host if it changed
    // --- terminal_notify.cpp: OSC 9 / 777 / 99 notifications
    void osc_notify99(std::string_view rest, bool bel);
    void notify(Notification n);

    // --- terminal_reflow.cpp
    void reflow_primary(int cols, int rows);

    // --- graphics_kitty.cpp, graphics_kitty_anim.cpp: the kitty graphics protocol
    void kitty_command(std::string_view payload);
    void kitty_execute(detail::KittyCommand& c, std::vector<uint8_t>& data);
    void kitty_respond(const detail::KittyCommand& c, uint32_t id, std::string_view result);
    std::string kitty_transmit(const detail::KittyCommand& c, std::vector<uint8_t>& data, uint64_t& key);
    std::string kitty_put(const detail::KittyCommand& c, uint64_t key);
    void kitty_delete(const detail::KittyCommand& c);
    // Sets c.rows to the frame it wrote (the reply's r=).
    std::string kitty_frame(detail::KittyCommand& c, std::vector<uint8_t>& data);
    std::string kitty_animate(const detail::KittyCommand& c);
    std::string kitty_compose(const detail::KittyCommand& c);
    // --- graphics_sixel.cpp: sixel DCS, XTSMGRAPHICS
    void sixel_start(const CsiSeq& seq);
    void sixel_finish();
    void xtsmgraphics(const CsiSeq& s);
    // --- graphics_iterm.cpp: OSC 1337 inline images
    void osc_iterm(std::string_view rest);
    // --- graphics_cells.cpp: images drawn as placeholder cells, and the hooks
    // that keep kitty placements anchored to the text.
    enum class CellCursor : uint8_t { Sixel, SixelRight, Iterm, Stay };
    void place_cell_image(std::unique_ptr<Image> img, CellCursor cursor, bool at_origin);
    void sweep_cell_images();
    void graphics_scrolled(int top, int bottom, int left, int right, int n, bool to_history);
    void graphics_after_feed();
    void graphics_resize_begin();
    void graphics_resize_end(int64_t old_screen_top);

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
    Palette base_palette_;
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

    Zone zone_{Zone::None};
    uint64_t change_count_{0};
    uint64_t next_stamp_{1};  // row stamps are drawn from here (Grid::set_generation)
    uint64_t numbering_{0};
    uint64_t reserve_stamps() noexcept {
        const uint64_t base = next_stamp_;
        next_stamp_ += uint64_t(rows_);
        return base;
    }

    // OSC 52 queries waiting for answer_clipboard().
    struct ClipQuery {
        uint64_t id{0};
        std::string selection;
        bool bel{false};
    };
    std::vector<ClipQuery> clip_pending_;
    uint64_t next_clip_{1};

    // OSC 99 notifications still receiving chunks (d=0), oldest first.
    struct PendingNote {
        Notification n;
        bool overflow{false};  // grew past the bound: chunks are dropped until the last
    };
    std::vector<PendingNote> notes_pending_;

    std::string title_;
    std::string icon_name_;
    std::string cwd_;
    std::string pointer_shape_;
    std::vector<std::pair<std::string, std::string>> title_stack_;

    CursorShape cursor_shape_{CursorShape::Block};
    bool cursor_shape_blink_{true};
    int cell_w_{0};
    int cell_h_{0};

    // DCS in progress.
    enum class Dcs : uint8_t { None, Decrqss, Xtgettcap, Sixel, Ignore } dcs_{Dcs::None};
    std::string dcs_data_;

    // Inline images (graphics_state.h).
    std::unique_ptr<detail::Graphics> gfx_;
    struct CarriedAnchor {
        int64_t line{0};
        size_t offset{0};
        bool valid{false};
    };
    std::vector<CarriedAnchor> carried_anchors_;
    bool image_cells_{false};

    // OSC 133 command records (terminal_commands.cpp).
    std::vector<CommandRecord> commands_;
    uint64_t commands_version_{0};
    int64_t commands_first_row_{0};  // history_first_row() when last trimmed
    std::vector<CarriedAnchor> carried_commands_;
};

} // namespace bropty
