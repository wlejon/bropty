#pragma once
// VT parser: a Paul Williams-style state machine (vt100.net/emu/dec_ansi_parser)
// running on a UTF-8 byte stream, as modern terminals do:
//  - UTF-8 is decoded in the ground state only, strictly (no overlongs,
//    surrogates or > U+10FFFF); each maximal ill-formed subsequence becomes one
//    U+FFFD. Raw 8-bit C1 bytes are therefore ill-formed UTF-8, never controls.
//  - A run of printable ASCII is delivered in one print_ascii() call.
//  - CSI parameters are bounded (kMaxParams, values saturate at 65535) and
//    colon sub-parameters are kept.
//  - OSC ends at BEL or ST; DCS/SOS/PM/APC only at ST. ESC followed by
//    anything other than '\' inside a string aborts the string (no dispatch)
//    and starts the new escape sequence. CAN/SUB abort anything.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace bropty {

struct CsiSeq {
    static constexpr int kMaxParams = 32;

    uint16_t params[kMaxParams]{};
    uint32_t present{0};  // bit i: param i had digits (not empty)
    uint32_t sub{0};      // bit i: param i is a ':' sub-parameter of the previous one
    uint8_t count{0};
    char prefix{0};       // one of < = > ? or 0
    char inter[2]{};
    uint8_t ninter{0};
    char final{0};
    bool overflow{false};  // more than kMaxParams were given; the rest were dropped

    // Parameter i, with "missing or 0" meaning `def` (the usual VT convention).
    [[nodiscard]] int arg(int i, int def) const noexcept {
        return (i < count && params[i] != 0) ? int(params[i]) : def;
    }
    // Parameter i, with only "missing" meaning `def` (explicit 0 stays 0).
    [[nodiscard]] int raw(int i, int def) const noexcept {
        return (i < count && (present >> i & 1u)) ? int(params[i]) : def;
    }
    [[nodiscard]] bool has_subparams() const noexcept { return sub != 0; }
    [[nodiscard]] char intermediate() const noexcept { return ninter ? inter[0] : 0; }
};

struct EscSeq {
    char inter[2]{};
    uint8_t ninter{0};
    char final{0};
    [[nodiscard]] char intermediate() const noexcept { return ninter ? inter[0] : 0; }
};

enum class StringKind : uint8_t { Sos, Pm, Apc };

class ParserSink {
public:
    virtual ~ParserSink() = default;
    virtual void print(char32_t cp) = 0;
    virtual void print_ascii(const char* s, size_t n) = 0;  // bytes 0x20..0x7E
    virtual void execute(uint8_t c0) = 0;
    virtual void esc_dispatch(const EscSeq& seq) = 0;
    virtual void csi_dispatch(const CsiSeq& seq) = 0;
    // Complete OSC payload ("Ps ; Pt"). `bel` reports the terminator so replies can mirror it.
    virtual void osc_dispatch(std::string_view payload, bool bel) = 0;
    virtual void dcs_hook(const CsiSeq& seq) = 0;
    virtual void dcs_put(std::string_view data) = 0;
    virtual void dcs_unhook(bool aborted) = 0;
    virtual void string_dispatch(StringKind kind, std::string_view payload) = 0;
};

class Parser {
public:
    explicit Parser(ParserSink* sink) noexcept : sink_(sink) {}

    void feed(std::string_view bytes);
    void feed(const uint8_t* data, size_t n) { feed(std::string_view(reinterpret_cast<const char*>(data), n)); }
    void reset();

    // OSC / SOS / PM / APC payloads longer than this are discarded whole.
    void set_max_string_bytes(size_t n) noexcept { max_string_ = n; }

    enum class State : uint8_t {
        Ground, Escape, EscapeIntermediate,
        CsiEntry, CsiParam, CsiIntermediate, CsiIgnore,
        DcsEntry, DcsParam, DcsIntermediate, DcsPassthrough, DcsIgnore,
        OscString, SosPmApcString,
    };
    [[nodiscard]] State state() const noexcept { return state_; }

private:
    void byte(uint8_t b);
    void ground_utf8(uint8_t b);
    void utf8_flush_invalid();
    void clear_seq();
    void param_digit(uint8_t b);
    void param_sep(bool colon);
    void open_param();
    void finish_params();
    void collect(uint8_t b);
    void enter_escape();
    void string_byte(uint8_t b);
    void finish_string(bool bel);
    void abort_string();

    ParserSink* sink_;
    State state_{State::Ground};

    // UTF-8 decoder state (ground only).
    uint32_t utf8_cp_{0};
    uint8_t utf8_need_{0};
    uint8_t utf8_lo_{0x80};
    uint8_t utf8_hi_{0xBF};

    CsiSeq seq_;
    bool param_started_{false};
    bool sep_pending_{false};
    bool next_sub_{false};
    bool too_many_inter_{false};

    // String states (OSC / SOS / PM / APC / DCS passthrough).
    std::string str_;
    bool str_overflow_{false};
    bool str_esc_{false};  // saw ESC inside a string; next byte decides ST vs abort
    StringKind str_kind_{StringKind::Apc};
    size_t max_string_{8u << 20};
};

} // namespace bropty
