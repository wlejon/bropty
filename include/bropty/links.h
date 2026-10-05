#pragma once
// Link detection: OSC 8 hyperlinks, plus URLs and file paths recognised in
// the text. Detection runs over logical lines, so a URL soft-wrapped across
// rows is found whole and its hover range spans the rows.
//
// Recognised:
//  * URLs with a scheme (`scheme://...` for any RFC 3986 scheme of two or
//    more letters, plus `mailto:`, `news:`, `tel:`), and `www.` hosts
//    (target gets `http://`). Trailing sentence punctuation and unbalanced
//    closing brackets / quotes are not part of the URL.
//  * Paths: `/abs/path`, `~/path`, `./rel`, `../rel`, `C:\dir` / `C:/dir`,
//    and relative `dir/file` with at least one slash; an optional
//    `:line[:col]` suffix (compiler diagnostics) is kept in the target.
// An OSC 8 link wins over detected text at the same cell.

#include "bropty/position.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bropty {

class Terminal;

enum class LinkKind : uint8_t { Hyperlink, Url, Path };

// A link found in a string: bytes [begin, end).
struct TextLink {
    size_t begin{0};
    size_t end{0};
    LinkKind kind{LinkKind::Url};
    std::string target;
};

// Detect URLs and paths in one line of UTF-8 text, in order.
void detect_links(std::string_view text, std::vector<TextLink>& out);

struct LinkHit {
    RowRange range;  // the link's cells, as a stream range (may span rows)
    LinkKind kind{LinkKind::Url};
    std::string target;   // URI (OSC 8 / URL) or path text
    uint32_t link_id{0};  // OSC 8 hyperlink id (Style::link), 0 otherwise
};

// The link at cell `cell` of the terminal's active buffer, if any.
std::optional<LinkHit> link_at(const Terminal& t, RowPos cell);
// Every link in the logical lines that touch rows [row0, row1).
void links_in_rows(const Terminal& t, int64_t row0, int64_t row1, std::vector<LinkHit>& out);

} // namespace bropty
