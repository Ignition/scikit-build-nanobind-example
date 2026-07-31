// A module implementation unit: it names the module without exporting, so it
// sees the interface's private members while contributing nothing to the
// compiled interface. Editing this file recompiles only this file.
//
// The interface's global module fragment is not inherited, hence the includes.

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

module aho_corasick;

namespace ac {

// Single shared traversal, defined here because it is only instantiated here.
template <typename OnMatch>
void PatternMatcher::scan(std::string_view text, OnMatch on_match) const {
    std::int32_t state = 0;
    std::size_t chars = 0;

    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<std::uint8_t>(text[i]);

        // Only continuation bytes look like 10xxxxxx, so anything else starts a
        // code point. Counting as we go is near-free, and a match can only end on
        // a character boundary.
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

namespace {

std::size_t count_code_points(std::string_view text) noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(text, [](char c) {
        return (static_cast<std::uint8_t>(c) & 0xC0) != 0x80;
    }));
}

}  // namespace

std::int32_t PatternMatcher::step(std::int32_t state, std::uint8_t byte) const noexcept {
    // One child() lookup per iteration: testing the transition in the loop
    // condition and repeating it for the result would do the work twice for every
    // byte that matches, which is most of them.
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
    // transform_reduce invokes with () rather than std::invoke, unlike the ranges
    // algorithms, so a pointer-to-member will not do here.
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
                // Projection rather than a comparator: keeps children sorted by
                // byte so child() can stop early.
                const auto at = std::ranges::lower_bound(children, byte, {}, &Child::first);
                children.insert(at, {byte, next});
            }
            state = next;
        }
        // Duplicate patterns land on the same node; both indices are recorded so
        // find_all reports each registration.
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

        // The trie is complete before this runs, so a reference is safe and
        // avoids copying a vector per state.
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
