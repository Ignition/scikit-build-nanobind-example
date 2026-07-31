// Unit tests for the algorithm, with no Python involved.
//
// This file owns what the binding layer cannot reach, plus fixed-case behaviour
// that needs no interpreter to state. The Python suite keeps the property tests
// against a brute-force oracle, the str/bytes overloads, pickle and packaging.
//
// The automaton's internals are deliberately untested here: step(), child(),
// build_failure_links() and scan() are private, and module export governs
// spelling rather than access, so reaching them would need a friend declaration
// in the one file whose point is that it exposes nothing. The oracle pins their
// correctness through the public surface instead.

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

// The constructor copies each pattern, so a temporary vector is safe here.
PatternMatcher matcher_for(std::vector<std::string> patterns) {
    return PatternMatcher(patterns);
}

}  // namespace

// --- Behaviour the bindings cannot reach -------------------------------------
//
// bindings.cpp hardwires the offset unit to the input type, so both cross
// combinations are unreachable from Python - as is Pattern::chars, which the
// bindings drop entirely.

TEST_CASE("byte offsets are available for text that is not plain ASCII") {
    // In C++ the unit is a parameter, so multibyte text can be scanned with byte
    // offsets directly; from Python this pairing requires bytes input.
    const auto matcher = matcher_for({"bar"});
    const std::string text = "caf\xc3\xa9 bar";  // "café bar"

    const auto found = matcher.find_all(text, Offsets::Bytes);

    REQUIRE(found.size() == 1);
    CHECK(found[0] == Match{6, 0});
    CHECK(text.substr(6, 3) == "bar");

    // The other unit, which is what Python's str path reports.
    const auto by_char = matcher.find_all(text, Offsets::Characters);
    REQUIRE(by_char.size() == 1);
    CHECK(by_char[0] == Match{5, 0});
}

TEST_CASE("character offsets over invalid UTF-8 are meaningless but well defined") {
    // Character positions over arbitrary bytes are meaningless but harmless.
    // Unreachable from Python, where arbitrary bytes always take the Bytes path.
    const auto matcher = matcher_for({"zz"});
    const std::string text = std::string("\xff\xfe") + "zz" + "\xc0" + "zz";

    const auto by_bytes = matcher.find_all(text, Offsets::Bytes);
    const auto by_chars = matcher.find_all(text, Offsets::Characters);

    // Same scan, so the same occurrences in order; only the unit differs.
    REQUIRE(by_bytes.size() == 2);
    REQUIRE(by_chars.size() == by_bytes.size());

    for (std::size_t i = 0; i < by_bytes.size(); ++i) {
        CHECK(by_chars[i].pattern_index == by_bytes[i].pattern_index);

        // Lead bytes are at most one per byte, so a character offset can never
        // exceed the byte offset however invalid the input. Nor can it underflow:
        // the matched bytes are part of the text, so the lead-byte count to the
        // end of a match is at least the pattern's own.
        CHECK(by_chars[i].start <= by_bytes[i].start);
        CHECK(by_chars[i].start < text.size());
    }
}

TEST_CASE("a pattern records its length in code points as well as bytes") {
    // Invisible from Python, which drops this field - but find_all's character
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
    // Python only sees nanobind's translation of this into ValueError.
    CHECK_THROWS_AS(matcher_for({"ok", ""}), std::invalid_argument);
}

TEST_CASE("a matcher can be copied and moved") {
    // nanobind placement-news a matcher and holds it by pointer, so neither
    // operation is exercised through the bindings.
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
// Fixed cases that need no interpreter, and that point at C++ lines when broken.

TEST_CASE("overlapping and nested matches are all reported") {
    // "he" is nested inside "she" and "hers" overlaps both; a loop of find would
    // report only some of them.
    const auto matcher = matcher_for({"he", "she", "hers"});

    auto found = matcher.find_all("ushers", Offsets::Bytes);
    std::ranges::sort(found, [](const Match& a, const Match& b) {
        return std::tie(a.start, a.pattern_index) < std::tie(b.start, b.pattern_index);
    });

    CHECK(found == std::vector<Match>{{1, 1}, {2, 0}, {2, 2}});
}

TEST_CASE("occurrences are counted with overlaps") {
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
    // "r","s" (2) = 8.
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
