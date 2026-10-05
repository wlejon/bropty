#include "bropty/parser.h"

namespace bropty {

namespace {
constexpr uint8_t kEsc = 0x1B;
constexpr uint8_t kCan = 0x18;
constexpr uint8_t kSub = 0x1A;
constexpr uint8_t kBel = 0x07;
constexpr uint8_t kDel = 0x7F;
constexpr char32_t kReplacement = 0xFFFD;

inline bool cont(uint8_t b, uint8_t lo = 0x80, uint8_t hi = 0xBF) { return b >= lo && b <= hi; }

// Decode one complete well-formed UTF-8 sequence of 2..4 bytes at p.
inline bool decode_utf8_fast(const char* p, const char* end, size_t& len, char32_t& cp) {
    const uint8_t b0 = uint8_t(p[0]);
    const ptrdiff_t avail = end - p;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        if (avail < 2 || !cont(uint8_t(p[1]))) return false;
        cp = char32_t(b0 & 0x1F) << 6 | (uint8_t(p[1]) & 0x3F);
        len = 2;
        return true;
    }
    if (b0 >= 0xE0 && b0 <= 0xEF) {
        if (avail < 3) return false;
        const uint8_t lo = b0 == 0xE0 ? 0xA0 : 0x80, hi = b0 == 0xED ? 0x9F : 0xBF;
        if (!cont(uint8_t(p[1]), lo, hi) || !cont(uint8_t(p[2]))) return false;
        cp = char32_t(b0 & 0x0F) << 12 | char32_t(uint8_t(p[1]) & 0x3F) << 6 | (uint8_t(p[2]) & 0x3F);
        len = 3;
        return true;
    }
    if (b0 >= 0xF0 && b0 <= 0xF4) {
        if (avail < 4) return false;
        const uint8_t lo = b0 == 0xF0 ? 0x90 : 0x80, hi = b0 == 0xF4 ? 0x8F : 0xBF;
        if (!cont(uint8_t(p[1]), lo, hi) || !cont(uint8_t(p[2])) || !cont(uint8_t(p[3]))) return false;
        cp = char32_t(b0 & 0x07) << 18 | char32_t(uint8_t(p[1]) & 0x3F) << 12 | char32_t(uint8_t(p[2]) & 0x3F) << 6 |
             (uint8_t(p[3]) & 0x3F);
        len = 4;
        return true;
    }
    return false;
}

bool is_string_state(Parser::State s) {
    return s == Parser::State::OscString || s == Parser::State::SosPmApcString ||
           s == Parser::State::DcsPassthrough || s == Parser::State::DcsIgnore;
}
} // namespace

void Parser::reset() {
    state_ = State::Ground;
    utf8_need_ = 0;
    utf8_cp_ = 0;
    clear_seq();
    str_.clear();
    str_overflow_ = false;
    str_esc_ = false;
}

void Parser::clear_seq() {
    seq_ = CsiSeq{};
    param_started_ = false;
    too_many_inter_ = false;
    sep_pending_ = false;
    next_sub_ = false;
}

void Parser::feed(std::string_view bytes) {
    const char* p = bytes.data();
    const char* end = p + bytes.size();
    while (p < end) {
        if (state_ == State::Ground && utf8_need_ == 0) {
            const char* q = p;
            while (q < end && uint8_t(*q) >= 0x20 && uint8_t(*q) < 0x7F) ++q;
            if (q != p) {
                sink_->print_ascii(p, size_t(q - p));
                p = q;
                continue;
            }
            // A complete, well-formed multi-byte sequence decodes here; anything
            // else (truncated, ill-formed) goes through the byte-wise decoder.
            size_t len;
            char32_t cp;
            if (decode_utf8_fast(p, end, len, cp)) {
                sink_->print(cp);
                p += len;
                continue;
            }
        } else if (state_ == State::CsiParam || state_ == State::CsiEntry) {
            // Parameter bytes, the bulk of most CSI sequences.
            const char* q = p;
            while (q < end && uint8_t(*q) >= '0' && uint8_t(*q) <= ';') {
                uint8_t b = uint8_t(*q);
                if (b > '9') {
                    param_sep(b == ':');
                    ++q;
                    continue;
                }
                // A run of digits accumulates locally (param_digit()'s rules).
                if (!param_started_) open_param();
                uint32_t v = seq_.overflow ? 0u : seq_.params[seq_.count - 1];
                while (q < end && uint8_t(*q) >= '0' && uint8_t(*q) <= '9') {
                    v = v * 10u + uint32_t(uint8_t(*q) - '0');
                    if (v > 0xFFFF) v = 0xFFFF;
                    ++q;
                }
                if (!seq_.overflow) {
                    seq_.params[seq_.count - 1] = uint16_t(v);
                    seq_.present |= 1u << (seq_.count - 1);
                }
            }
            if (q != p) {
                state_ = State::CsiParam;
                p = q;
                continue;
            }
        } else if (!str_esc_ && (state_ == State::OscString || state_ == State::DcsPassthrough ||
                                 state_ == State::SosPmApcString)) {
            // Bulk-copy string payload up to the next byte that can end it.
            const char* q = p;
            while (q < end) {
                uint8_t b = uint8_t(*q);
                if (b == kEsc || b == kCan || b == kSub || (b < 0x20 && state_ != State::DcsPassthrough) ||
                    (b == kDel && state_ == State::DcsPassthrough))
                    break;
                ++q;
            }
            if (q != p) {
                std::string_view run(p, size_t(q - p));
                if (state_ == State::DcsPassthrough) {
                    sink_->dcs_put(run);
                } else if (!str_overflow_) {
                    if (str_.size() + run.size() > max_string_) {
                        str_overflow_ = true;
                        str_.clear();
                        str_.shrink_to_fit();
                    } else {
                        str_.append(run);
                    }
                }
                p = q;
                continue;
            }
        }
        byte(uint8_t(*p++));
    }
}

void Parser::utf8_flush_invalid() {
    utf8_need_ = 0;
    utf8_cp_ = 0;
    sink_->print(kReplacement);
}

void Parser::ground_utf8(uint8_t b) {
    if (utf8_need_ > 0) {
        if (b >= utf8_lo_ && b <= utf8_hi_) {
            utf8_cp_ = (utf8_cp_ << 6) | (b & 0x3F);
            utf8_lo_ = 0x80;
            utf8_hi_ = 0xBF;
            if (--utf8_need_ == 0) {
                char32_t cp = utf8_cp_;
                utf8_cp_ = 0;
                sink_->print(cp);
            }
            return;
        }
        // Maximal subpart ended: one U+FFFD, then reprocess b as a fresh byte.
        utf8_flush_invalid();
    }
    utf8_lo_ = 0x80;
    utf8_hi_ = 0xBF;
    if (b >= 0xC2 && b <= 0xDF) {
        utf8_need_ = 1;
        utf8_cp_ = b & 0x1F;
    } else if (b >= 0xE0 && b <= 0xEF) {
        utf8_need_ = 2;
        utf8_cp_ = b & 0x0F;
        if (b == 0xE0) utf8_lo_ = 0xA0;
        if (b == 0xED) utf8_hi_ = 0x9F;
    } else if (b >= 0xF0 && b <= 0xF4) {
        utf8_need_ = 3;
        utf8_cp_ = b & 0x07;
        if (b == 0xF0) utf8_lo_ = 0x90;
        if (b == 0xF4) utf8_hi_ = 0x8F;
    } else {
        sink_->print(kReplacement);  // stray continuation, C0/C1 lead, F5..FF
    }
}

void Parser::enter_escape() {
    clear_seq();
    state_ = State::Escape;
}

void Parser::collect(uint8_t b) {
    if (seq_.ninter < 2) {
        seq_.inter[seq_.ninter++] = char(b);
    } else {
        too_many_inter_ = true;
    }
}

void Parser::open_param() {
    if (seq_.count < CsiSeq::kMaxParams) {
        seq_.params[seq_.count] = 0;
        if (next_sub_) seq_.sub |= 1u << seq_.count;
        ++seq_.count;
    } else {
        seq_.overflow = true;
    }
    next_sub_ = false;
    sep_pending_ = false;
    param_started_ = true;
}

void Parser::param_digit(uint8_t b) {
    if (!param_started_) open_param();
    if (seq_.overflow) return;  // digits of dropped parameters
    int i = seq_.count - 1;
    uint32_t v = uint32_t(seq_.params[i]) * 10u + uint32_t(b - '0');
    seq_.params[i] = uint16_t(v > 0xFFFF ? 0xFFFF : v);
    seq_.present |= 1u << i;
}

void Parser::param_sep(bool colon) {
    if (!param_started_) open_param();
    param_started_ = false;
    sep_pending_ = true;
    next_sub_ = colon;
}

void Parser::finish_params() {
    if (sep_pending_) open_param();  // "1;" has a trailing empty parameter
}

void Parser::byte(uint8_t b) {
    // Anywhere transitions.
    if (b == kCan || b == kSub) {
        if (utf8_need_) utf8_flush_invalid();
        if (state_ == State::DcsPassthrough) sink_->dcs_unhook(true);
        if (is_string_state(state_)) abort_string();
        state_ = State::Ground;
        str_esc_ = false;
        sink_->execute(b);
        return;
    }
    if (is_string_state(state_)) {
        string_byte(b);
        return;
    }
    if (b == kEsc) {
        if (utf8_need_) utf8_flush_invalid();
        enter_escape();
        return;
    }

    switch (state_) {
    case State::Ground:
        if (b >= 0x80) {
            ground_utf8(b);
            return;
        }
        if (utf8_need_) utf8_flush_invalid();
        if (b < 0x20) {
            sink_->execute(b);
        } else if (b != kDel) {
            char c = char(b);
            sink_->print_ascii(&c, 1);
        }
        return;

    case State::Escape:
        if (b < 0x20) { sink_->execute(b); return; }
        if (b >= 0x7F) return;
        if (b <= 0x2F) { collect(b); state_ = State::EscapeIntermediate; return; }
        switch (b) {
        case '[': clear_seq(); state_ = State::CsiEntry; return;
        case ']': str_.clear(); str_overflow_ = false; state_ = State::OscString; return;
        case 'P': clear_seq(); state_ = State::DcsEntry; return;
        case 'X': str_kind_ = StringKind::Sos; break;
        case '^': str_kind_ = StringKind::Pm; break;
        case '_': str_kind_ = StringKind::Apc; break;
        default: {
            EscSeq e;
            e.final = char(b);
            state_ = State::Ground;
            sink_->esc_dispatch(e);
            return;
        }
        }
        str_.clear();
        str_overflow_ = false;
        state_ = State::SosPmApcString;
        return;

    case State::EscapeIntermediate:
        if (b < 0x20) { sink_->execute(b); return; }
        if (b >= 0x7F) return;
        if (b <= 0x2F) { collect(b); return; }
        {
            state_ = State::Ground;
            if (too_many_inter_) return;
            EscSeq e;
            e.inter[0] = seq_.inter[0];
            e.inter[1] = seq_.inter[1];
            e.ninter = seq_.ninter;
            e.final = char(b);
            sink_->esc_dispatch(e);
        }
        return;

    case State::CsiEntry:
    case State::CsiParam:
    case State::CsiIntermediate:
    case State::CsiIgnore:
        if (b < 0x20) { sink_->execute(b); return; }
        if (b >= 0x7F) return;  // DEL and 8-bit bytes are ignored inside CSI
        if (state_ == State::CsiIgnore) {
            if (b >= 0x40) state_ = State::Ground;
            return;
        }
        if (b >= 0x40) {
            state_ = State::Ground;
            if (too_many_inter_) return;
            finish_params();
            seq_.final = char(b);
            sink_->csi_dispatch(seq_);
            return;
        }
        if (b <= 0x2F) { collect(b); state_ = State::CsiIntermediate; return; }
        if (state_ == State::CsiIntermediate) { state_ = State::CsiIgnore; return; }
        if (b >= 0x3C) {  // private marker: only valid first
            if (state_ == State::CsiEntry) { seq_.prefix = char(b); state_ = State::CsiParam; }
            else state_ = State::CsiIgnore;
            return;
        }
        state_ = State::CsiParam;
        if (b <= '9') param_digit(b);
        else param_sep(b == ':');
        return;

    case State::DcsEntry:
    case State::DcsParam:
    case State::DcsIntermediate:
        if (b < 0x20 || b >= 0x7F) return;  // ignored in DCS headers
        if (b >= 0x40) {
            finish_params();
            seq_.final = char(b);
            if (too_many_inter_) { state_ = State::DcsIgnore; return; }
            state_ = State::DcsPassthrough;
            sink_->dcs_hook(seq_);
            return;
        }
        if (b <= 0x2F) { collect(b); state_ = State::DcsIntermediate; return; }
        if (state_ == State::DcsIntermediate) { state_ = State::DcsIgnore; return; }
        if (b >= 0x3C) {
            if (state_ == State::DcsEntry) { seq_.prefix = char(b); state_ = State::DcsParam; }
            else state_ = State::DcsIgnore;
            return;
        }
        state_ = State::DcsParam;
        if (b <= '9') param_digit(b);
        else param_sep(b == ':');
        return;

    default:
        return;
    }
}

void Parser::string_byte(uint8_t b) {
    if (str_esc_) {
        str_esc_ = false;
        if (b == '\\') {
            finish_string(false);
            return;
        }
        // ESC + anything else: the string is aborted and a new escape begins.
        if (state_ == State::DcsPassthrough) sink_->dcs_unhook(true);
        abort_string();
        enter_escape();
        byte(b);
        return;
    }
    if (b == kEsc) {
        str_esc_ = true;
        return;
    }
    switch (state_) {
    case State::OscString:
        if (b == kBel) { finish_string(true); return; }
        if (b < 0x20) return;
        break;
    case State::DcsPassthrough:
        if (b == kDel) return;
        {
            char c = char(b);
            sink_->dcs_put(std::string_view(&c, 1));
        }
        return;
    case State::DcsIgnore:
        return;
    default:  // SOS / PM / APC
        if (b < 0x20) return;
        break;
    }
    if (str_overflow_) return;
    if (str_.size() >= max_string_) {
        str_overflow_ = true;
        str_.clear();
        str_.shrink_to_fit();
        return;
    }
    str_.push_back(char(b));
}

void Parser::finish_string(bool bel) {
    State s = state_;
    state_ = State::Ground;
    if (s == State::DcsPassthrough) {
        sink_->dcs_unhook(false);
        return;
    }
    if (s == State::DcsIgnore) return;
    if (!str_overflow_) {
        if (s == State::OscString) sink_->osc_dispatch(str_, bel);
        else sink_->string_dispatch(str_kind_, str_);
    }
    abort_string();
}

void Parser::abort_string() {
    str_.clear();
    if (str_.capacity() > 4096) str_.shrink_to_fit();
    str_overflow_ = false;
    str_esc_ = false;
}

} // namespace bropty
