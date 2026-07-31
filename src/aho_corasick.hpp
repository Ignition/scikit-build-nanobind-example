// Aho-Corasick multi-pattern string matching.
//
// Deliberately free of any Python or nanobind dependency: this header and its
// .cpp compile as a plain C++20 library, and `bindings.cpp` is the only file
// that knows Python exists. That separation is the point of the layout — the
// algorithm can be tested, reused, or linked into a non-Python program without
// dragging the interpreter along.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ac {

// Builds an automaton from a fixed set of patterns, then finds every occurrence
// of every pattern in a text in a single pass.
//
// Construction is the expensive part and happens once; scanning is O(text
// length) and, crucially, independent of how many patterns were registered.
// That asymmetry is the whole reason this type exists.
class PatternMatcher {
public:
    struct Match {
        std::size_t start;          // offset of the occurrence within the text
        std::size_t pattern_index;  // index into the pattern list

        bool operator==(const Match&) const = default;
    };

    struct Pattern {
        std::string text;
        std::size_t chars;  // length in code points; text.size() is the byte length
    };

    // Text arriving from a Python `str` is UTF-8, where a byte offset is a
    // footgun: it disagrees with every offset Python itself would report.
    // Scanning tracks both, and the caller picks the unit that matches its input.
    enum class Offsets { Bytes, Characters };

    // Throws std::invalid_argument if any pattern is empty — an empty pattern
    // matches at every position, which is never what a caller means.
    explicit PatternMatcher(const std::vector<std::string>& patterns);

    // True if any pattern occurs at least once. Stops at the first hit.
    [[nodiscard]] bool matches(std::string_view text) const noexcept;

    // Total occurrences of all patterns, counting overlaps.
    [[nodiscard]] std::size_t count(std::string_view text) const noexcept;

    // Every occurrence, in the order encountered. Overlapping and nested matches
    // are all reported: searching "he", "she", "hers" in "shers" yields all three.
    //
    // Offsets::Characters assumes valid UTF-8; on arbitrary bytes the character
    // positions are meaningless (but harmless — no undefined behaviour).
    [[nodiscard]] std::vector<Match> find_all(std::string_view text, Offsets offsets) const;

    [[nodiscard]] std::size_t num_patterns() const noexcept { return patterns_.size(); }
    [[nodiscard]] std::size_t num_states() const noexcept { return nodes_.size(); }
    [[nodiscard]] const std::vector<Pattern>& patterns() const noexcept { return patterns_; }

private:
    struct Node {
        // Sorted by byte, so the scan below can stop early. Kept as a small vector
        // rather than a dense 256-entry table: dense would be one load instead of
        // a loop, but costs 1 KB per state — hundreds of megabytes at realistic
        // pattern counts. Measured, it also gains nothing here (see README).
        std::vector<std::pair<std::uint8_t, std::int32_t>> children;
        std::int32_t fail = 0;
        std::int32_t output_link = -1;       // nearest proper suffix that is terminal
        std::vector<std::int32_t> outputs;   // patterns ending exactly here
    };

    // States are held as int32_t indices, so every access would otherwise need a
    // static_cast. One named accessor is worth fifteen casts inline.
    [[nodiscard]] const Node& node(std::int32_t state) const noexcept {
        return nodes_[static_cast<std::size_t>(state)];
    }
    [[nodiscard]] Node& node(std::int32_t state) noexcept {
        return nodes_[static_cast<std::size_t>(state)];
    }

    // The innermost operation of every scan, so it lives in the header where the
    // compiler can inline it. A linear scan rather than std::lower_bound: child
    // lists are short, and a binary search branches unpredictably on data. Both
    // were measured — the linear version executes 20% fewer instructions and
    // takes 40% fewer branch mispredictions (see README).
    [[nodiscard]] std::int32_t child(std::int32_t state, std::uint8_t byte) const noexcept {
        for (const auto& [key, next] : node(state).children) {
            if (key == byte) {
                return next;
            }
            if (key > byte) {
                break;  // sorted, so no later entry can match
            }
        }
        return -1;
    }

    // The automaton's transition function: follow failure links until the byte
    // can be consumed, or fall back to the root.
    [[nodiscard]] std::int32_t step(std::int32_t state, std::uint8_t byte) const noexcept;

    void build_failure_links();

    // Single shared traversal. `on_match(pattern_index, byte_end, char_end)`
    // returns false to stop early, which is how `matches` avoids scanning the
    // rest of the text.
    template <typename OnMatch>
    void scan(std::string_view text, OnMatch on_match) const;

    std::vector<Node> nodes_;
    std::vector<Pattern> patterns_;
};

template <typename OnMatch>
void PatternMatcher::scan(std::string_view text, OnMatch on_match) const {
    std::int32_t state = 0;
    std::size_t chars = 0;

    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<std::uint8_t>(text[i]);

        // In UTF-8 exactly the continuation bytes look like 10xxxxxx, so anything
        // else starts a new code point. Counting them as we go is close to free,
        // and a match can only ever end on a character boundary.
        if ((byte & 0xC0) != 0x80) {
            ++chars;
        }

        state = step(state, byte);

        // Walk the suffix chain: a match here may also complete shorter patterns.
        for (std::int32_t out = state; out != -1; out = node(out).output_link) {
            for (const std::int32_t pattern : node(out).outputs) {
                if (!on_match(static_cast<std::size_t>(pattern), i + 1, chars)) {
                    return;
                }
            }
        }
    }
}

}  // namespace ac
