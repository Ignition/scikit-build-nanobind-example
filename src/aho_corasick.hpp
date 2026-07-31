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
    };

    // Text arriving from a Python `str` is UTF-8, where a byte offset is a
    // footgun: it disagrees with every offset Python itself would report.
    // Scanning tracks both, and the caller picks the unit that matches its input.
    enum class Offsets { Bytes, Characters };

    // Throws std::invalid_argument if any pattern is empty — an empty pattern
    // matches at every position, which is never what a caller means.
    explicit PatternMatcher(std::vector<std::string> patterns);

    // True if any pattern occurs at least once. Stops at the first hit.
    bool matches(std::string_view text) const;

    // Total occurrences of all patterns, counting overlaps.
    std::size_t count(std::string_view text) const;

    // Every occurrence, in the order encountered. Overlapping and nested matches
    // are all reported: searching "he", "she", "hers" in "shers" yields all three.
    std::vector<Match> find_all(std::string_view text, Offsets offsets) const;

    std::size_t num_patterns() const noexcept { return patterns_.size(); }
    std::size_t num_states() const noexcept { return nodes_.size(); }
    const std::vector<std::string>& patterns() const noexcept { return patterns_; }

private:
    struct Node {
        // Sorted by byte so lookup is a binary search. A dense 256-entry table per
        // node would scan faster but costs 1 KB per state, which at realistic
        // pattern counts runs to hundreds of megabytes.
        std::vector<std::pair<std::uint8_t, std::int32_t>> children;
        std::int32_t fail = 0;
        std::int32_t output_link = -1;       // nearest proper suffix that is terminal
        std::vector<std::int32_t> outputs;   // patterns ending exactly here
    };

    std::int32_t child(std::int32_t state, std::uint8_t byte) const noexcept;
    void build_failure_links();

    // Single shared traversal. `on_match(pattern_index, byte_end, char_end)`
    // returns false to stop early, which is how `matches` avoids scanning the
    // rest of the text.
    template <typename OnMatch>
    void scan(std::string_view text, OnMatch&& on_match) const;

    std::vector<Node> nodes_;
    std::vector<std::string> patterns_;
    std::vector<std::size_t> pattern_bytes_;
    std::vector<std::size_t> pattern_chars_;
};

template <typename OnMatch>
void PatternMatcher::scan(std::string_view text, OnMatch&& on_match) const {
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

        while (state != 0 && child(state, byte) < 0) {
            state = nodes_[static_cast<std::size_t>(state)].fail;
        }
        const std::int32_t next = child(state, byte);
        state = next < 0 ? 0 : next;

        // Walk the suffix chain: a match here may also complete shorter patterns.
        for (std::int32_t out = state; out != -1;
             out = nodes_[static_cast<std::size_t>(out)].output_link) {
            for (const std::int32_t pattern : nodes_[static_cast<std::size_t>(out)].outputs) {
                if (!on_match(static_cast<std::size_t>(pattern), i + 1, chars)) {
                    return;
                }
            }
        }
    }
}

}  // namespace ac
