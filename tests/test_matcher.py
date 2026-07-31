import pickle

import pytest
from hypothesis import given, settings
from hypothesis import strategies as st

from ahocorasick_demo import PatternMatcher

# A small alphabet makes collisions, overlaps, and nested patterns common, which
# is exactly where an automaton is easy to get wrong.
small_alphabet = st.text(alphabet="abc", min_size=1, max_size=6)
pattern_lists = st.lists(small_alphabet, min_size=1, max_size=8, unique=True)
texts = st.text(alphabet="abc", max_size=60)


def reference_find_all(patterns, text):
    """Obviously-correct O(n*k) oracle, including overlaps."""
    found = []
    for start in range(len(text)):
        for index, pattern in enumerate(patterns):
            if text.startswith(pattern, start):
                found.append((start, index))
    return sorted(found)


# --- Core behaviour ----------------------------------------------------------
#
# The fixed-case core behaviour - overlaps, nesting, counting, state counts, the
# empty pattern list, repeated patterns - lives in tests/test_matcher.cpp. It
# needs no interpreter to state, and stating it in C++ means a failure points at
# the algorithm rather than arriving through the binding layer.
#
# What stays here is what genuinely exercises the whole stack: the binding
# layer's own behaviour, and the property tests against a brute-force oracle.


def test_rejects_empty_patterns():
    # An empty pattern matches everywhere, which is never what a caller means.
    # The C++ suite pins the exception *type*; this pins nanobind's translation
    # of it into ValueError, which is the part Python callers depend on.
    with pytest.raises(ValueError, match="empty"):
        PatternMatcher(["ok", ""])


def test_dunder_len_and_repr_are_wired_to_the_automaton():
    # These two have no C++ counterpart to move to: __len__ and __repr__ are
    # defined in bindings.cpp, so the mapping from num_patterns/num_states onto
    # Python's protocols is only observable from here.
    matcher = PatternMatcher(["he", "she", "hers"])

    assert len(matcher) == 3
    assert matcher.num_states == 8
    assert repr(matcher) == "PatternMatcher(patterns=3, states=8)"


# --- str / bytes and offsets -------------------------------------------------


def test_str_and_bytes_patterns_are_interchangeable():
    from_str = PatternMatcher(["café"])
    from_bytes = PatternMatcher(["café".encode()])

    assert from_str.patterns == from_bytes.patterns == [b"caf\xc3\xa9"]


def test_str_offsets_are_code_points_not_bytes():
    # "é" is two bytes in UTF-8. Reporting byte offsets for str input would
    # disagree with every offset Python itself produces.
    text = "café bar"
    matcher = PatternMatcher(["bar"])

    assert matcher.find_all(text) == [(5, 0)]
    assert text[5:8] == "bar"


def test_bytes_offsets_are_byte_positions():
    matcher = PatternMatcher([b"bar"])
    text = "café bar".encode()

    assert matcher.find_all(text) == [(6, 0)]
    assert text[6:9] == b"bar"


def test_patterns_are_returned_as_bytes_in_order():
    matcher = PatternMatcher(["b", "a"])

    assert matcher.patterns == [b"b", b"a"]


# --- Properties, against a brute-force oracle --------------------------------


@given(patterns=pattern_lists, text=texts)
@settings(max_examples=300)
def test_find_all_agrees_with_brute_force(patterns, text):
    matcher = PatternMatcher(patterns)

    assert sorted(matcher.find_all(text)) == reference_find_all(patterns, text)


@given(patterns=pattern_lists, text=texts)
def test_count_agrees_with_find_all(patterns, text):
    matcher = PatternMatcher(patterns)

    assert matcher.count(text) == len(matcher.find_all(text))


@given(patterns=pattern_lists, text=texts)
def test_matches_agrees_with_count(patterns, text):
    matcher = PatternMatcher(patterns)

    assert matcher.matches(text) == (matcher.count(text) > 0)


@given(patterns=pattern_lists, text=texts)
def test_str_and_bytes_scanning_find_the_same_occurrences(patterns, text):
    matcher = PatternMatcher(patterns)

    # ASCII-only here, so code point offsets and byte offsets coincide.
    assert matcher.find_all(text) == matcher.find_all(text.encode())


# --- Pickle ------------------------------------------------------------------


@given(patterns=pattern_lists, text=texts)
def test_pickle_round_trips(patterns, text):
    matcher = PatternMatcher(patterns)

    restored = pickle.loads(pickle.dumps(matcher))

    assert restored.patterns == matcher.patterns
    assert restored.num_states == matcher.num_states
    assert restored.find_all(text) == matcher.find_all(text)


def test_handles_patterns_that_are_not_valid_utf8():
    # Arbitrary binary is a legitimate pattern: the matcher works on bytes, and
    # nothing should try to decode them as text on the way in or out.
    matcher = PatternMatcher([b"\xff\xfe", b"\x00\x01"])

    assert matcher.patterns == [b"\xff\xfe", b"\x00\x01"]
    assert matcher.find_all(b"zz\xff\xfezz") == [(2, 0)]

    restored = pickle.loads(pickle.dumps(matcher))
    assert restored.patterns == matcher.patterns


def test_pickle_carries_patterns_not_the_trie():
    # The automaton is rebuilt on load: deterministic, and far simpler than
    # serialising failure links. Cheap because construction is the only cost.
    matcher = PatternMatcher(["he", "she", "hers"])

    blob = pickle.dumps(matcher)

    assert b"hers" in blob
