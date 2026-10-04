// gen_unicode: generates src/unicode_tables.inc from the Unicode Character Database.
//
// Download these files of the UCD 17.0.0 from https://www.unicode.org/Public/17.0.0/ucd/
// into one flat directory (they are not committed):
//
//   EastAsianWidth.txt
//   extracted/DerivedGeneralCategory.txt     -> DerivedGeneralCategory.txt
//   DerivedCoreProperties.txt                (InCB, Default_Ignorable_Code_Point)
//   emoji/emoji-data.txt                     -> emoji-data.txt
//   auxiliary/GraphemeBreakProperty.txt      -> GraphemeBreakProperty.txt
//
// e.g.
//   B=https://www.unicode.org/Public/17.0.0/ucd
//   F="EastAsianWidth.txt extracted/DerivedGeneralCategory.txt DerivedCoreProperties.txt"
//   F="$F emoji/emoji-data.txt auxiliary/GraphemeBreakProperty.txt"
//   for f in $F; do curl -sSfLO $B/$f; done
//
// Then build this single file (no dependencies) and run it:
//
//   c++ -std=c++20 -O2 -o gen_unicode tools/gen_unicode.cpp
//   ./gen_unicode <ucd-dir> src/unicode_tables.inc
//
// The test fixture tests/data/GraphemeBreakTest.txt comes from auxiliary/ of the same
// version; update it together with the tables, and bump kUnicode* in unicode.h.
//
// East_Asian_Width defaults: unlisted code points are N (the @missing line), and the
// unassigned code points of the CJK blocks below plus all of planes 2 and 3 default to W.
// The UCD lists those explicitly today; the generator also applies them itself so that
// it stays correct if a future file reverts to @missing-only defaults.
//
// Packed per-code-point properties (uint16_t, see unicode.cpp):
//   bits 0-1  width class: 0 zero, 1 narrow, 2 wide, 3 East Asian ambiguous
//   bits 2-5  Grapheme_Cluster_Break (bropty::unicode::GraphemeBreak order)
//   bit  6    Extended_Pictographic
//   bit  7    Emoji
//   bits 8-9  Indic_Conjunct_Break: 0 None, 1 Linker, 2 Consonant, 3 Extend
//   bit  10   control: C0, C1 or DEL
//
// Layout: props = kProps[kStage2[(kStage1[cp >> kShift] << kShift) | (cp & mask)]].
// The shift is chosen to minimise the total table size.

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace gen {

constexpr uint32_t kMaxCp = 0x110000;

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(trim(cur));
    return out;
}

bool parse_range(const std::string& f, uint32_t& lo, uint32_t& hi) {
    size_t dots = f.find("..");
    try {
        if (dots == std::string::npos) {
            lo = hi = static_cast<uint32_t>(std::stoul(f, nullptr, 16));
        } else {
            lo = static_cast<uint32_t>(std::stoul(f.substr(0, dots), nullptr, 16));
            hi = static_cast<uint32_t>(std::stoul(f.substr(dots + 2), nullptr, 16));
        }
    } catch (...) {
        return false;
    }
    return lo <= hi && hi < kMaxCp;
}

using LineFn = std::function<void(uint32_t lo, uint32_t hi, const std::vector<std::string>& fields)>;

// Calls fn for every data line, and missing_fn (if given) for every "# @missing:" line.
bool parse_file(const std::string& path, const LineFn& fn, const LineFn& missing_fn = nullptr) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "gen_unicode: cannot open %s\n", path.c_str());
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        bool missing = false;
        if (line.rfind("# @missing:", 0) == 0) {
            if (!missing_fn) continue;
            line = line.substr(11);
            missing = true;
        }
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        line = trim(line);
        if (line.empty()) continue;
        auto fields = split(line, ';');
        uint32_t lo, hi;
        if (fields.size() < 2 || !parse_range(fields[0], lo, hi)) {
            std::fprintf(stderr, "gen_unicode: bad line in %s: %s\n", path.c_str(), line.c_str());
            return false;
        }
        (missing ? missing_fn : fn)(lo, hi, fields);
    }
    return true;
}

enum class Eaw : uint8_t { N, Na, A, W, F, H };

struct Ucd {
    std::vector<Eaw> eaw = std::vector<Eaw>(kMaxCp, Eaw::N);
    std::vector<std::string> gc = std::vector<std::string>(kMaxCp, "Cn");
    std::vector<uint8_t> gcb = std::vector<uint8_t>(kMaxCp, 0);
    std::vector<uint8_t> incb = std::vector<uint8_t>(kMaxCp, 0);
    std::vector<bool> di = std::vector<bool>(kMaxCp, false);
    std::vector<bool> emoji = std::vector<bool>(kMaxCp, false);
    std::vector<bool> emoji_pres = std::vector<bool>(kMaxCp, false);
    std::vector<bool> ext_pict = std::vector<bool>(kMaxCp, false);
};

bool parse_eaw_value(const std::string& v, Eaw& out) {
    static const std::map<std::string, Eaw> kNames = {
        {"N", Eaw::N}, {"Na", Eaw::Na}, {"A", Eaw::A}, {"W", Eaw::W}, {"F", Eaw::F}, {"H", Eaw::H}};
    auto it = kNames.find(v);
    if (it == kNames.end()) return false;
    out = it->second;
    return true;
}

bool load(const std::string& dir, Ucd& u) {
    bool ok = true;
    auto set_eaw = [&](uint32_t lo, uint32_t hi, const std::vector<std::string>& f) {
        Eaw v;
        if (!parse_eaw_value(f[1], v)) {
            std::fprintf(stderr, "gen_unicode: unknown East_Asian_Width %s\n", f[1].c_str());
            ok = false;
            return;
        }
        for (uint32_t c = lo; c <= hi; ++c) u.eaw[c] = v;
    };
    // @missing lines first (they appear before the data), then the documented W defaults,
    // then the explicit data, which overrides both.
    std::vector<std::array<uint32_t, 2>> wide_defaults = {
        {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xF900, 0xFAFF}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD}};
    bool defaults_applied = false;
    auto apply_defaults = [&] {
        if (defaults_applied) return;
        defaults_applied = true;
        for (auto& r : wide_defaults)
            for (uint32_t c = r[0]; c <= r[1]; ++c) u.eaw[c] = Eaw::W;
    };
    ok &= parse_file(
        dir + "/EastAsianWidth.txt",
        [&](uint32_t lo, uint32_t hi, const std::vector<std::string>& f) { apply_defaults(); set_eaw(lo, hi, f); },
        set_eaw);

    ok &= parse_file(dir + "/DerivedGeneralCategory.txt",
                     [&](uint32_t lo, uint32_t hi, const std::vector<std::string>& f) {
                         for (uint32_t c = lo; c <= hi; ++c) u.gc[c] = f[1];
                     });

    ok &= parse_file(dir + "/DerivedCoreProperties.txt",
                     [&](uint32_t lo, uint32_t hi, const std::vector<std::string>& f) {
                         if (f[1] == "Default_Ignorable_Code_Point") {
                             for (uint32_t c = lo; c <= hi; ++c) u.di[c] = true;
                         } else if (f[1] == "InCB" && f.size() >= 3) {
                             uint8_t v = f[2] == "Linker" ? 1 : f[2] == "Consonant" ? 2 : f[2] == "Extend" ? 3 : 0;
                             if (v == 0) {
                                 std::fprintf(stderr, "gen_unicode: unknown InCB %s\n", f[2].c_str());
                                 ok = false;
                             }
                             for (uint32_t c = lo; c <= hi; ++c) u.incb[c] = v;
                         }
                     });

    ok &= parse_file(dir + "/emoji-data.txt", [&](uint32_t lo, uint32_t hi, const std::vector<std::string>& f) {
        std::vector<bool>* target = f[1] == "Emoji"                   ? &u.emoji
                                    : f[1] == "Emoji_Presentation"    ? &u.emoji_pres
                                    : f[1] == "Extended_Pictographic" ? &u.ext_pict
                                                                      : nullptr;
        if (target)
            for (uint32_t c = lo; c <= hi; ++c) (*target)[c] = true;
    });

    static const std::map<std::string, int> kGcb = {
        {"Other", 0}, {"CR", 1}, {"LF", 2}, {"Control", 3}, {"Extend", 4}, {"ZWJ", 5},
        {"Regional_Indicator", 6}, {"Prepend", 7}, {"SpacingMark", 8}, {"L", 9}, {"V", 10},
        {"T", 11}, {"LV", 12}, {"LVT", 13}};
    ok &= parse_file(dir + "/GraphemeBreakProperty.txt",
                     [&](uint32_t lo, uint32_t hi, const std::vector<std::string>& f) {
                         auto it = kGcb.find(f[1]);
                         if (it == kGcb.end()) {
                             std::fprintf(stderr, "gen_unicode: unknown GCB %s\n", f[1].c_str());
                             ok = false;
                             return;
                         }
                         for (uint32_t c = lo; c <= hi; ++c) u.gcb[c] = static_cast<uint8_t>(it->second);
                     });
    return ok;
}

bool is_control(uint32_t c) { return c < 0x20 || (c >= 0x7F && c <= 0x9F); }

bool is_visible_format(uint32_t c) {
    return c == 0x00AD || (c >= 0x0600 && c <= 0x0605) || c == 0x06DD || c == 0x070F ||
           c == 0x0890 || c == 0x0891 || c == 0x08E2 || c == 0x110BD || c == 0x110CD;
}

// Width class: 0 zero, 1 narrow, 2 wide, 3 ambiguous. The reference implementation of the
// rules documented in include/bropty/unicode.h.
uint8_t width_class(const Ucd& u, uint32_t c) {
    if (is_control(c)) return 0;
    const std::string& gc = u.gc[c];
    if ((gc == "Mn" || gc == "Me" || gc == "Cf") && !is_visible_format(c)) return 0;
    if (u.di[c] && c != 0x115F && !is_visible_format(c)) return 0;  // U+00AD is also DI
    if ((c >= 0x1160 && c <= 0x11FF) || (c >= 0xD7B0 && c <= 0xD7FF)) return 0;
    if (c >= 0x1F1E6 && c <= 0x1F1FF) return 1;
    if (u.eaw[c] == Eaw::W || u.eaw[c] == Eaw::F || u.emoji_pres[c]) return 2;
    if (u.eaw[c] == Eaw::A) return 3;
    return 1;
}

uint16_t packed(const Ucd& u, uint32_t c) {
    return static_cast<uint16_t>(width_class(u, c) | (u.gcb[c] << 2) | (u.ext_pict[c] ? 1u << 6 : 0u) |
                                 (u.emoji[c] ? 1u << 7 : 0u) | (u.incb[c] << 8) | (is_control(c) ? 1u << 10 : 0u));
}

struct Tables {
    int shift = 0;
    std::vector<uint16_t> props;   // distinct packed values
    std::vector<uint32_t> stage1;  // block number per cp >> shift
    std::vector<uint8_t> stage2;   // concatenated distinct blocks of props indices
    size_t stage1_elem = 1;
    size_t bytes() const { return stage1.size() * stage1_elem + stage2.size() + props.size() * 2; }
};

bool build_tables(const std::vector<uint16_t>& all, int shift, Tables& t) {
    t = Tables{};
    t.shift = shift;
    std::map<uint16_t, uint8_t> prop_index;
    std::vector<uint8_t> idx(kMaxCp);
    for (uint32_t c = 0; c < kMaxCp; ++c) {
        auto it = prop_index.find(all[c]);
        if (it == prop_index.end()) {
            if (t.props.size() == 256) return false;
            it = prop_index.emplace(all[c], static_cast<uint8_t>(t.props.size())).first;
            t.props.push_back(all[c]);
        }
        idx[c] = it->second;
    }
    const uint32_t block = 1u << shift;
    std::map<std::vector<uint8_t>, uint32_t> blocks;
    for (uint32_t b = 0; b < kMaxCp / block; ++b) {
        std::vector<uint8_t> key(idx.begin() + b * block, idx.begin() + (b + 1) * block);
        auto it = blocks.find(key);
        if (it == blocks.end()) {
            it = blocks.emplace(key, static_cast<uint32_t>(blocks.size())).first;
            t.stage2.insert(t.stage2.end(), key.begin(), key.end());
        }
        t.stage1.push_back(it->second);
    }
    t.stage1_elem = blocks.size() <= 256 ? 1 : 2;
    return blocks.size() <= 65536;
}

template <typename T>
void emit_array(std::ostream& os, const char* type, const char* name, const std::vector<T>& v) {
    os << "inline constexpr " << type << ' ' << name << '[' << v.size() << "] = {\n";
    std::string line;
    for (size_t i = 0; i < v.size(); ++i) {
        std::string item = std::to_string(static_cast<unsigned>(v[i])) + ',';
        if (line.size() + item.size() > 198) {
            os << line << '\n';
            line.clear();
        }
        line += item;
    }
    if (!line.empty()) os << line << '\n';
    os << "};\n";
}

bool generate(const std::string& ucd_dir, std::vector<uint16_t>& all, Tables& best) {
    Ucd u;
    if (!load(ucd_dir, u)) return false;
    all.resize(kMaxCp);
    for (uint32_t c = 0; c < kMaxCp; ++c) all[c] = packed(u, c);
    best = Tables{};
    for (int shift = 4; shift <= 10; ++shift) {
        Tables t;
        if (build_tables(all, shift, t) && (best.stage1.empty() || t.bytes() < best.bytes())) best = std::move(t);
    }
    return !best.stage1.empty();
}

bool write_inc(const Tables& t, const std::string& out_path) {
    std::ostringstream os;
    os << "// GENERATED by tools/gen_unicode.cpp from the Unicode 17.0.0 UCD. Do not edit.\n"
       << "// props = kProps[kStage2[(kStage1[cp >> kShift] << kShift) | (cp & kMask)]]\n"
       << "// Sizes: stage1 " << t.stage1.size() * t.stage1_elem << " B, stage2 " << t.stage2.size()
       << " B, props " << t.props.size() * 2 << " B, total " << t.bytes() << " B.\n"
       << "inline constexpr unsigned kShift = " << t.shift << ";\n"
       << "inline constexpr unsigned kMask = " << ((1u << t.shift) - 1) << ";\n";
    emit_array(os, "uint16_t", "kProps", t.props);
    emit_array(os, t.stage1_elem == 1 ? "uint8_t" : "uint16_t", "kStage1", t.stage1);
    emit_array(os, "uint8_t", "kStage2", t.stage2);
    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "gen_unicode: cannot write %s\n", out_path.c_str());
        return false;
    }
    out << os.str();
    std::printf("gen_unicode: shift %d, stage1 %zu B, stage2 %zu B, props %zu B (%zu values), total %zu B\n",
                t.shift, t.stage1.size() * t.stage1_elem, t.stage2.size(), t.props.size() * 2, t.props.size(),
                t.bytes());
    return static_cast<bool>(out);
}

} // namespace gen

#ifndef GEN_UNICODE_NO_MAIN
int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: gen_unicode <ucd-dir> <out unicode_tables.inc>\n");
        return 2;
    }
    std::vector<uint16_t> all;
    gen::Tables t;
    if (!gen::generate(argv[1], all, t)) return 1;
    return gen::write_inc(t, argv[2]) ? 0 : 1;
}
#endif
