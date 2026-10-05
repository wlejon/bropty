#include "bropty/search_regex.h"

#include <brosearch/regex.h>

#include <utility>

namespace bropty {

std::shared_ptr<RegexMatcher> RegexMatcher::create(std::string_view pattern, const RegexSearchOptions& options,
                                                   std::string* error) {
    bro::search::RegexOptions ro;
    ro.case_insensitive = options.case_mode == SearchCase::Insensitive;
    ro.smart_case = options.case_mode == SearchCase::Smart;
    ro.word = options.whole_word;
    ro.literal = options.literal;
    auto re = bro::search::Regex::compile(pattern, ro, error);
    if (!re) return nullptr;
    return std::make_shared<RegexMatcher>(std::move(re));
}

RegexMatcher::RegexMatcher(std::shared_ptr<const bro::search::Regex> regex) : regex_(std::move(regex)) {}

bool RegexMatcher::case_insensitive() const noexcept { return regex_->case_insensitive(); }

const std::string& RegexMatcher::pattern() const noexcept { return regex_->pattern(); }

void RegexMatcher::find(std::string_view line, std::vector<std::pair<size_t, size_t>>& out) {
    size_t pos = 0;
    while (pos <= line.size()) {
        const auto m = regex_->find(line, pos);
        if (!m) break;
        if (m->end > m->start) {
            out.emplace_back(m->start, m->end);
            pos = m->end;
            continue;
        }
        // An empty match: no highlight; resume after the code point it sits on.
        pos = m->end + 1;
        while (pos < line.size() && (static_cast<unsigned char>(line[pos]) & 0xC0) == 0x80) ++pos;
    }
}

} // namespace bropty
