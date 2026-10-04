#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace vdiff {

struct Rng {
    uint64_t s;
    uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return uint32_t(s >> 33);
    }
    int below(int n) { return n <= 0 ? 0 : int(next() % uint32_t(n)); }
    bool chance(int pct) { return below(100) < pct; }
};

// A random stream as a list of tokens (each a complete sequence or text run;
// combining marks and REP are glued to the print they follow).
std::vector<std::string> generate(uint64_t seed, int cols, int rows);

// Whether a token list (e.g. a minimizer trial) still respects the
// generator's exclusions: DECRC / 1049l only when charsets and DECOM are
// unchanged since the matching save.
bool valid(const std::vector<std::string>& toks);

} // namespace vdiff
