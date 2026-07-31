#include "aho_corasick.hpp"

#include <algorithm>
#include <deque>
#include <stdexcept>

namespace ac {
namespace {

std::size_t count_code_points(std::string_view text) noexcept {
    std::size_t chars = 0;
    for (const char c : text) {
        if ((static_cast<std::uint8_t>(c) & 0xC0) != 0x80) {
            ++chars;
        }
    }
    return chars;
}

}  // namespace

std::int32_t PatternMatcher::child(std::int32_t state, std::uint8_t byte) const noexcept {
    const auto& children = nodes_[static_cast<std::size_t>(state)].children;
    const auto it = std::lower_bound(
        children.begin(), children.end(), byte,
        [](const auto& entry, std::uint8_t value) { return entry.first < value; });
    return (it != children.end() && it->first == byte) ? it->second : -1;
}

PatternMatcher::PatternMatcher(std::vector<std::string> patterns)
    : patterns_(std::move(patterns)) {
    nodes_.emplace_back();  // root

    pattern_bytes_.reserve(patterns_.size());
    pattern_chars_.reserve(patterns_.size());

    for (std::size_t index = 0; index < patterns_.size(); ++index) {
        const std::string& pattern = patterns_[index];
        if (pattern.empty()) {
            throw std::invalid_argument("patterns must not be empty");
        }
        pattern_bytes_.push_back(pattern.size());
        pattern_chars_.push_back(count_code_points(pattern));

        std::int32_t state = 0;
        for (const char c : pattern) {
            const auto byte = static_cast<std::uint8_t>(c);
            std::int32_t next = child(state, byte);
            if (next < 0) {
                next = static_cast<std::int32_t>(nodes_.size());
                nodes_.emplace_back();
                auto& children = nodes_[static_cast<std::size_t>(state)].children;
                const auto at = std::lower_bound(
                    children.begin(), children.end(), byte,
                    [](const auto& entry, std::uint8_t value) { return entry.first < value; });
                children.insert(at, {byte, next});
            }
            state = next;
        }
        // Duplicate patterns land on the same node; both indices are recorded so
        // find_all can report each one the caller asked about.
        nodes_[static_cast<std::size_t>(state)].outputs.push_back(
            static_cast<std::int32_t>(index));
    }

    build_failure_links();
}

void PatternMatcher::build_failure_links() {
    // Breadth-first, so a node's failure link is always resolved before its
    // children need it.
    std::deque<std::int32_t> queue;

    for (const auto& [byte, node] : nodes_[0].children) {
        (void)byte;
        nodes_[static_cast<std::size_t>(node)].fail = 0;
        queue.push_back(node);
    }

    while (!queue.empty()) {
        const std::int32_t state = queue.front();
        queue.pop_front();

        // Copy: inserting into nodes_ during the loop would invalidate a reference.
        const auto children = nodes_[static_cast<std::size_t>(state)].children;
        for (const auto& [byte, next] : children) {
            std::int32_t fail = nodes_[static_cast<std::size_t>(state)].fail;
            while (fail != 0 && child(fail, byte) < 0) {
                fail = nodes_[static_cast<std::size_t>(fail)].fail;
            }
            const std::int32_t candidate = child(fail, byte);
            // `candidate == next` happens at depth 1, where the only match for the
            // byte is the node itself; its failure link is the root.
            nodes_[static_cast<std::size_t>(next)].fail =
                (candidate >= 0 && candidate != next) ? candidate : 0;

            const auto& target = nodes_[static_cast<std::size_t>(
                nodes_[static_cast<std::size_t>(next)].fail)];
            nodes_[static_cast<std::size_t>(next)].output_link =
                target.outputs.empty() ? target.output_link
                                       : nodes_[static_cast<std::size_t>(next)].fail;

            queue.push_back(next);
        }
    }
}

bool PatternMatcher::matches(std::string_view text) const {
    bool found = false;
    scan(text, [&](std::size_t, std::size_t, std::size_t) {
        found = true;
        return false;  // stop at the first hit
    });
    return found;
}

std::size_t PatternMatcher::count(std::string_view text) const {
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
        const std::size_t end = by_char ? char_end : byte_end;
        const std::size_t length = by_char ? pattern_chars_[pattern] : pattern_bytes_[pattern];
        found.push_back({end - length, pattern});
        return true;
    });
    return found;
}

}  // namespace ac
