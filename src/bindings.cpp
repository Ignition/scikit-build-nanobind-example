// The Python binding layer, and nothing else: overload resolution, iterable
// conversion, pickling, docstrings. The aho_corasick module stays a plain C++
// library that could link into a program that has never heard of Python.

#include <nanobind/nanobind.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

import aho_corasick;

namespace nb = nanobind;
using namespace nb::literals;
using ac::PatternMatcher;

namespace {

std::string_view view_of(const nb::bytes& value) {
    return {value.c_str(), value.size()};
}

// str is encoded to UTF-8, so a pattern registered either way matches text given
// either way.
std::vector<std::string> collect_patterns(const nb::iterable& items) {
    std::vector<std::string> patterns;
    for (nb::handle item : items) {
        if (nb::isinstance<nb::bytes>(item)) {
            const auto text = view_of(nb::cast<nb::bytes>(item));
            patterns.emplace_back(text);
        } else {
            patterns.emplace_back(nb::cast<std::string_view>(item));
        }
    }
    return patterns;
}

// (start, pattern_index) pairs — offsets in code points for str, bytes for bytes.
using Found = std::vector<std::pair<std::size_t, std::size_t>>;

Found to_pairs(const std::vector<PatternMatcher::Match>& matches) {
    Found pairs;
    pairs.reserve(matches.size());
    for (const auto& match : matches) {
        pairs.emplace_back(match.start, match.pattern_index);
    }
    return pairs;
}

std::vector<nb::bytes> patterns_as_bytes(const PatternMatcher& matcher) {
    std::vector<nb::bytes> out;
    out.reserve(matcher.patterns().size());
    for (const PatternMatcher::Pattern& pattern : matcher.patterns()) {
        out.emplace_back(pattern.text.data(), pattern.text.size());
    }
    return out;
}

// Rebuilding from the pattern list is simpler than serialising the trie, and the
// automaton is deterministic, so reconstruction is exact.
//
// nb::bytes, not std::string: nanobind maps std::string to Python str, which
// decodes as UTF-8 and would raise on patterns that are arbitrary binary.
using State = std::tuple<std::vector<nb::bytes>>;

}  // namespace

NB_MODULE(_core, m) {
    m.doc() = "Aho-Corasick multi-pattern matching, implemented in C++20.";
    m.attr("__version__") = AHOCORASICK_DEMO_VERSION;

    nb::class_<PatternMatcher> matcher(m, "PatternMatcher");

    matcher.def(
        "__init__",
        [](PatternMatcher* self, const nb::iterable& patterns) {
            // Placement-new: the automaton is built from a converted vector
            // rather than from the Python object directly.
            new (self) PatternMatcher(collect_patterns(patterns));
        },
        "patterns"_a,
        "Build an automaton over the given patterns. This is the expensive step; "
        "scanning afterwards costs the same no matter how many patterns there are. "
        "Raises ValueError if any pattern is empty.");

    // bytes before str in every pair: registration order is the order nanobind
    // tries, and bytes is the exact match rather than a str conversion.
    matcher.def("matches",
                [](const PatternMatcher& self, const nb::bytes& text) {
                    return self.matches(view_of(text));
                },
                "text"_a, "True if any pattern occurs in the text. Stops at the first hit.");
    matcher.def("matches",
                [](const PatternMatcher& self, std::string_view text) {
                    return self.matches(text);
                },
                "text"_a);

    matcher.def("count",
                [](const PatternMatcher& self, const nb::bytes& text) {
                    return self.count(view_of(text));
                },
                "text"_a, "Total occurrences of all patterns, counting overlaps.");
    matcher.def("count",
                [](const PatternMatcher& self, std::string_view text) {
                    return self.count(text);
                },
                "text"_a);

    matcher.def("find_all",
                [](const PatternMatcher& self, const nb::bytes& text) {
                    return to_pairs(self.find_all(view_of(text), PatternMatcher::Offsets::Bytes));
                },
                "text"_a,
                "Every occurrence as (start, pattern_index), including overlapping "
                "and nested matches. Offsets are byte positions for bytes input and "
                "code point positions for str.");
    matcher.def("find_all",
                [](const PatternMatcher& self, std::string_view text) {
                    return to_pairs(
                        self.find_all(text, PatternMatcher::Offsets::Characters));
                },
                "text"_a);

    matcher.def("__len__", &PatternMatcher::num_patterns, "Number of registered patterns.");

    matcher.def_prop_ro("num_states", &PatternMatcher::num_states,
                        "States in the automaton — its memory footprint in practice.");
    matcher.def_prop_ro(
        "patterns", &patterns_as_bytes,
        // bytes, because str patterns were encoded on the way in and a decode on
        // the way out would fail on any pattern that is not valid UTF-8.
        "The registered patterns, as bytes, in their original order.");

    matcher.def("__repr__", [](const PatternMatcher& self) {
        return "PatternMatcher(patterns=" + std::to_string(self.num_patterns()) +
               ", states=" + std::to_string(self.num_states()) + ")";
    });

    matcher.def("__getstate__",
                [](const PatternMatcher& self) { return State{patterns_as_bytes(self)}; });
    matcher.def("__setstate__", [](PatternMatcher& self, const State& state) {
        std::vector<std::string> patterns;
        for (const nb::bytes& pattern : std::get<0>(state)) {
            patterns.emplace_back(view_of(pattern));
        }
        new (&self) PatternMatcher(patterns);
    });
}
