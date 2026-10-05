#pragma once
// Regular-expression scrollback search: a SearchMatcher backed by brosearch's
// linear-time engine (Rust `regex` / ripgrep syntax, Unicode-aware; see
// brosearch/regex.h). Matching time is O(line x pattern) for every pattern,
// so a pathological search cannot stall the terminal's thread, and Search's
// time-budgeted step() stays meaningful.
//
// Each logical line is matched on its own (search.h), so ^ and $ anchor to
// the start and end of a logical line — a match can still span the rows of
// a soft-wrapped line, but never a hard line break. Empty matches are
// dropped (a highlight needs at least one cell).

#include "bropty/search.h"

#include <memory>
#include <string>
#include <string_view>

namespace bro::search {
class Regex;
}

namespace bropty {

enum class SearchCase {
    Sensitive,
    Insensitive,  // Unicode simple case folding
    Smart,        // insensitive unless the pattern has an uppercase literal (as rg -S)
};

struct RegexSearchOptions {
    SearchCase case_mode = SearchCase::Smart;
    bool whole_word = false;  // the match must not touch word characters on either side
    bool literal = false;     // the pattern is a fixed string (regex features off)
};

class RegexMatcher final : public SearchMatcher {
public:
    // nullptr on a syntax error (or a pattern too large to compile), with the
    // message in *error.
    [[nodiscard]] static std::shared_ptr<RegexMatcher> create(std::string_view pattern,
                                                              const RegexSearchOptions& options,
                                                              std::string* error = nullptr);
    [[nodiscard]] static std::shared_ptr<RegexMatcher> create(std::string_view pattern, std::string* error = nullptr) {
        return create(pattern, RegexSearchOptions(), error);
    }

    explicit RegexMatcher(std::shared_ptr<const bro::search::Regex> regex);
    void find(std::string_view line, std::vector<std::pair<size_t, size_t>>& out) override;

    // Whether matching ignores case (chosen explicitly or by smart case).
    [[nodiscard]] bool case_insensitive() const noexcept;
    [[nodiscard]] const std::string& pattern() const noexcept;

private:
    std::shared_ptr<const bro::search::Regex> regex_;
};

} // namespace bropty
