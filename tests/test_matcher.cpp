// Unit tests for the algorithm, with no Python anywhere in sight.
//
// The division of labour with tests/test_matcher.py is deliberate. This file
// owns the behaviour the binding layer cannot reach, plus the fixed-case core
// behaviour that does not need a Python interpreter to state. The Python suite
// owns the Hypothesis property tests against a brute-force oracle - which have
// no C++ equivalent worth having - along with the str/bytes overloads, pickle,
// and packaging.
//
// Note what is *not* here: step(), child(), build_failure_links() and scan() are
// private members, and module export controls which names an importer may spell,
// not class access. Reaching them would mean a friend declaration in
// aho_corasick.cppm - the one file whose selling point is that it exposes
// nothing, and the file that rebuilds every importer when edited. Their
// correctness is pinned through the public surface by the oracle instead.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

import aho_corasick;

using ac::PatternMatcher;
using Match = PatternMatcher::Match;
using Offsets = PatternMatcher::Offsets;

namespace {

// The constructor copies each pattern into the matcher, so the span does not
// need to outlive the call and a temporary vector is safe here.
PatternMatcher matcher_for(std::vector<std::string> patterns) {
    return PatternMatcher(patterns);
}

}  // namespace

// --- Behaviour the bindings cannot reach -------------------------------------
//
// bindings.cpp hardwires the offset unit to the input type: bytes -> Bytes,
// str -> Characters. The two cross combinations are unreachable from Python, as
// is Pattern::chars, which the bindings drop entirely.

TEST_CASE("byte offsets are available for text that is not plain ASCII") {
    // From Python this pairing requires bytes input. In C++ the unit is a
    // parameter, so multibyte text can be scanned with byte offsets directly.
    const auto matcher = matcher_for({"bar"});
    const std::string text = "caf\xc3\xa9 bar";  // "café bar"

    const auto found = matcher.find_all(text, Offsets::Bytes);

    REQUIRE(found.size() == 1);
    CHECK(found[0] == Match{6, 0});
    CHECK(text.substr(6, 3) == "bar");

    // The same text through the other unit, which is what Python's str path
    // would report: "bar" is at character 5 but byte 6.
    const auto by_char = matcher.find_all(text, Offsets::Characters);
    REQUIRE(by_char.size() == 1);
    CHECK(by_char[0] == Match{5, 0});
}

TEST_CASE("character offsets over invalid UTF-8 are meaningless but well defined") {
    // aho_corasick.cppm claims Offsets::Characters "assumes valid UTF-8; on
    // arbitrary bytes the character positions are meaningless, but harmless,
    // with no undefined behaviour". Nothing could test that from Python, because
    // arbitrary bytes always take the Bytes path there.
    const auto matcher = matcher_for({"zz"});
    const std::string text = std::string("\xff\xfe") + "zz" + "\xc0" + "zz";

    const auto by_bytes = matcher.find_all(text, Offsets::Bytes);
    const auto by_chars = matcher.find_all(text, Offsets::Characters);

    // Same scan, so the same occurrences in the same order - only the unit the
    // offsets are reported in differs.
    REQUIRE(by_bytes.size() == 2);
    REQUIRE(by_chars.size() == by_bytes.size());

    for (std::size_t i = 0; i < by_bytes.size(); ++i) {
        CHECK(by_chars[i].pattern_index == by_bytes[i].pattern_index);

        // A character offset counts UTF-8 lead bytes, which is at most one per
        // byte, so it can never exceed the byte offset however invalid the
        // input is. It also cannot underflow: the matched bytes are themselves
        // part of the text, so the lead-byte count up to the end of a match is
        // always at least the pattern's own.
        CHECK(by_chars[i].start <= by_bytes[i].start);
        CHECK(by_chars[i].start < text.size());
    }
}

TEST_CASE("a pattern records its length in code points as well as bytes") {
    // The bindings expose patterns as bytes and drop Pattern::chars, so this
    // field has no Python-visible effect at all - but find_all's character
    // offsets are computed from it.
    const auto matcher = matcher_for({"caf\xc3\xa9", "ab"});

    const auto patterns = matcher.patterns();

    REQUIRE(patterns.size() == 2);
    CHECK(patterns[0].text.size() == 5);  // "café" is five bytes
    CHECK(patterns[0].chars == 4);        // and four code points
    CHECK(patterns[1].text.size() == 2);  // ASCII: the two agree
    CHECK(patterns[1].chars == 2);
}

TEST_CASE("an empty pattern is rejected as std::invalid_argument") {
    // Python only ever sees this as ValueError, via nanobind's translation. The
    // exception type itself is part of the C++ interface and is only observable
    // here.
    CHECK_THROWS_AS(matcher_for({"ok", ""}), std::invalid_argument);
}

TEST_CASE("a matcher can be copied and moved") {
    // nanobind placement-news a PatternMatcher and holds it by pointer, so these
    // operations are never exercised through the bindings.
    const auto original = matcher_for({"he", "she"});

    SECTION("a copy scans independently and leaves the original usable") {
        const PatternMatcher copy = original;

        CHECK(copy.find_all("ushers", Offsets::Bytes) == original.find_all("ushers", Offsets::Bytes));
        CHECK(copy.num_patterns() == 2);
        CHECK(original.num_patterns() == 2);
    }

    SECTION("a move carries the automaton") {
        PatternMatcher source = matcher_for({"he", "she"});
        const PatternMatcher moved = std::move(source);

        CHECK(moved.num_patterns() == 2);
        CHECK(moved.count("ushers") == 2);
    }
}

// --- Core behaviour ----------------------------------------------------------
//
// Moved here from tests/test_matcher.py: fixed cases that need no interpreter to
// state, and that fail faster and point at C++ line numbers when they break.

TEST_CASE("overlapping and nested matches are all reported") {
    // The textbook case: "he" is nested inside "she", and "hers" overlaps both.
    // A loop of find would report only some of these.
    const auto matcher = matcher_for({"he", "she", "hers"});

    auto found = matcher.find_all("ushers", Offsets::Bytes);
    std::ranges::sort(found, [](const Match& a, const Match& b) {
        return std::tie(a.start, a.pattern_index) < std::tie(b.start, b.pattern_index);
    });

    CHECK(found == std::vector<Match>{{1, 1}, {2, 0}, {2, 2}});
}

TEST_CASE("occurrences are counted with overlaps") {
    // "aaa" contains "aa" twice when overlaps count.
    CHECK(matcher_for({"aa"}).count("aaa") == 2);
}

TEST_CASE("matches stops at the first hit") {
    const auto matcher = matcher_for({"needle"});

    CHECK(matcher.matches("haystack needle haystack"));
    CHECK_FALSE(matcher.matches("haystack haystack"));
}

TEST_CASE("shared prefixes are merged into one trie") {
    const auto matcher = matcher_for({"he", "she", "hers"});

    CHECK(matcher.num_patterns() == 3);
    // root + "he" (2) + "she" (3) + "hers" reusing the "he" prefix and adding
    // "r","s" (2) = 8. Merging shared prefixes is the point of a trie.
    CHECK(matcher.num_states() == 8);
}

TEST_CASE("a matcher with no patterns matches nothing") {
    const auto matcher = matcher_for({});

    CHECK(matcher.num_patterns() == 0);
    CHECK_FALSE(matcher.matches("anything at all"));
    CHECK(matcher.find_all("anything at all", Offsets::Bytes).empty());
}

TEST_CASE("a repeated pattern is reported once per registration") {
    const auto matcher = matcher_for({"ab", "ab"});

    auto found = matcher.find_all("ab", Offsets::Bytes);
    std::ranges::sort(found, [](const Match& a, const Match& b) {
        return a.pattern_index < b.pattern_index;
    });

    CHECK(matcher.num_patterns() == 2);
    CHECK(found == std::vector<Match>{{0, 0}, {0, 1}});
}
