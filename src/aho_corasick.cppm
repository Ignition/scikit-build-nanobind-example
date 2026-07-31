// Aho-Corasick multi-pattern string matching.
//
// A C++20 module, and deliberately free of any Python or nanobind dependency:
// this file compiles as a plain C++ library, and `bindings.cpp` is the only file
// that knows Python exists. That separation is the point of the layout, and the
// module boundary now enforces the other half of it. Only the names marked
// `export` are visible to an importer; the trie node type, the transition
// function and the traversal are private in a way a header cannot express.
//
// The standard library is included in the global module fragment rather than
// imported. `import std;` needs a newer toolchain than this project asks for,
// and mixing the two forms is worse than picking one.

module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module aho_corasick;

export namespace ac {

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

    // Throws std::invalid_argument if any pattern is empty; an empty pattern
    // matches at every position, which is never what a caller means.
    explicit PatternMatcher(std::span<const std::string> patterns);

    // True if any pattern occurs at least once. Stops at the first hit.
    [[nodiscard]] bool matches(std::string_view text) const noexcept;

    // Total occurrences of all patterns, counting overlaps.
    [[nodiscard]] std::size_t count(std::string_view text) const noexcept;

    // Every occurrence, in the order encountered. Overlapping and nested matches
    // are all reported: searching "he", "she", "hers" in "shers" yields all three.
    //
    // Offsets::Characters assumes valid UTF-8; on arbitrary bytes the character
    // positions are meaningless, but harmless, with no undefined behaviour.
    [[nodiscard]] std::vector<Match> find_all(std::string_view text, Offsets offsets) const;

    [[nodiscard]] std::size_t num_patterns() const noexcept { return patterns_.size(); }
    [[nodiscard]] std::size_t num_states() const noexcept { return nodes_.size(); }
    [[nodiscard]] std::span<const Pattern> patterns() const noexcept { return patterns_; }

private:
    using Child = std::pair<std::uint8_t, std::int32_t>;  // (byte, next state)

    struct Node {
        // Sorted by byte, so the lookup below can stop early. Kept as a small
        // vector rather than a dense table indexed by byte: dense would be one
        // load instead of a loop, but costs a kilobyte per state, which dominates
        // memory once there are many patterns. It was also no faster in practice,
        // because the lookup it replaces is already short.
        std::vector<Child> children;
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

    // The innermost operation of every scan. Two deliberate choices, both against
    // the more idiomatic spelling:
    //
    //   - A linear scan, not a binary search. Child lists are short, and a binary
    //     search branches on data the predictor cannot learn, so it mispredicts
    //     roughly every other lookup.
    //   - Written out, not std::ranges::find_if. find_if must locate the first
    //     entry not below the byte and then re-test it for equality, where this
    //     returns the instant it matches.
    //
    // Everything else in this file uses the ranges algorithms. This is the only
    // loop hot enough to be worth hand-writing.
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

// Implementation. Not exported: an importer sees the declarations above and
// nothing here.
namespace ac {
namespace {

std::size_t count_code_points(std::string_view text) noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(text, [](char c) {
        return (static_cast<std::uint8_t>(c) & 0xC0) != 0x80;
    }));
}

}  // namespace

std::int32_t PatternMatcher::step(std::int32_t state, std::uint8_t byte) const noexcept {
    // One child() lookup per iteration. Testing the transition in the loop
    // condition and then repeating it to get the result would do the work twice
    // for every byte that does match, which is most of them.
    for (;;) {
        const std::int32_t next = child(state, byte);
        if (next >= 0) {
            return next;
        }
        if (state == 0) {
            return 0;  // no transition from the root: stay put
        }
        state = node(state).fail;
    }
}

PatternMatcher::PatternMatcher(std::span<const std::string> patterns) {
    // std::transform_reduce, unlike the ranges algorithms, invokes its callable
    // with () rather than std::invoke, so a pointer-to-member will not do here.
    const std::size_t total_bytes =
        std::transform_reduce(patterns.begin(), patterns.end(), std::size_t{0}, std::plus{},
                              [](const std::string& pattern) { return pattern.size(); });

    // Upper bound on the node count: every pattern byte adds at most one state.
    nodes_.reserve(total_bytes + 1);
    nodes_.emplace_back();  // root
    patterns_.reserve(patterns.size());

    for (const std::string& pattern : patterns) {
        if (pattern.empty()) {
            throw std::invalid_argument("patterns must not be empty");
        }
        const auto index = static_cast<std::int32_t>(patterns_.size());
        patterns_.push_back({pattern, count_code_points(pattern)});

        std::int32_t state = 0;
        for (const char c : pattern) {
            const auto byte = static_cast<std::uint8_t>(c);
            std::int32_t next = child(state, byte);
            if (next < 0) {
                next = static_cast<std::int32_t>(nodes_.size());
                nodes_.emplace_back();
                auto& children = node(state).children;
                // Projection instead of a comparator lambda: compare on the byte,
                // keeping children sorted so child() can stop early.
                const auto at = std::ranges::lower_bound(children, byte, {}, &Child::first);
                children.insert(at, {byte, next});
            }
            state = next;
        }
        // Duplicate patterns land on the same node; both indices are recorded so
        // find_all can report each one the caller asked about.
        node(state).outputs.push_back(index);
    }

    build_failure_links();
}

void PatternMatcher::build_failure_links() {
    // Breadth-first, so a node's failure link is always resolved before its
    // children need it.
    std::deque<std::int32_t> queue;

    for (const auto& [byte, child_state] : nodes_[0].children) {
        (void)byte;
        node(child_state).fail = 0;
        queue.push_back(child_state);
    }

    while (!queue.empty()) {
        const std::int32_t state = queue.front();
        queue.pop_front();

        // No node is added here, the trie is complete before this runs, so a
        // reference is safe and avoids copying a vector per state.
        const auto& children = node(state).children;
        for (const auto& [byte, next] : children) {
            std::int32_t fail = node(state).fail;
            while (fail != 0 && child(fail, byte) < 0) {
                fail = node(fail).fail;
            }
            const std::int32_t candidate = child(fail, byte);
            // `candidate == next` happens at depth 1, where the only match for the
            // byte is the node itself; its failure link is the root.
            const std::int32_t target = (candidate >= 0 && candidate != next) ? candidate : 0;

            node(next).fail = target;
            node(next).output_link =
                node(target).outputs.empty() ? node(target).output_link : target;

            queue.push_back(next);
        }
    }
}

bool PatternMatcher::matches(std::string_view text) const noexcept {
    bool found = false;
    scan(text, [&](std::size_t, std::size_t, std::size_t) {
        found = true;
        return false;  // stop at the first hit
    });
    return found;
}

std::size_t PatternMatcher::count(std::string_view text) const noexcept {
    std::size_t total = 0;
    scan(text, [&](std::size_t, std::size_t, std::size_t) {
        ++total;
        return true;
    });
    return total;
}

std::vector<PatternMatcher::Match> PatternMatcher::find_all(std::string_view text,
                                                            Offsets offsets) const {
    std::vector<Match> found;
    const bool by_char = offsets == Offsets::Characters;

    scan(text, [&](std::size_t pattern, std::size_t byte_end, std::size_t char_end) {
        const Pattern& info = patterns_[pattern];
        const std::size_t end = by_char ? char_end : byte_end;
        const std::size_t length = by_char ? info.chars : info.text.size();
        found.push_back({end - length, pattern});
        return true;
    });
    return found;
}

}  // namespace ac
