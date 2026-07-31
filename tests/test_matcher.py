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


def test_rejects_empty_patterns():
    # An empty pattern matches everywhere, which is never what a caller means.
    with pytest.raises(ValueError, match="empty"):
        PatternMatcher(["ok", ""])


def test_no_patterns_matches_nothing():
    matcher = PatternMatcher([])

    assert len(matcher) == 0
    assert not matcher.matches("anything at all")
    assert matcher.find_all("anything at all") == []


def test_finds_overlapping_and_nested_matches():
    # The textbook case: "he" is nested inside "she", and "hers" overlaps both.
    # A loop of str.find would report only some of these.
    matcher = PatternMatcher(["he", "she", "hers"])

    found = sorted(matcher.find_all("ushers"))

    assert found == [(1, 1), (2, 0), (2, 2)]


def test_counts_overlapping_occurrences():
    matcher = PatternMatcher(["aa"])

    # "aaa" contains "aa" twice when overlaps count; str.count would say once.
    assert matcher.count("aaa") == 2


def test_matches_stops_at_the_first_hit():
    matcher = PatternMatcher(["needle"])

    assert matcher.matches("haystack needle haystack")
    assert not matcher.matches("haystack haystack")


def test_repeated_patterns_are_reported_separately():
    matcher = PatternMatcher(["ab", "ab"])

    assert len(matcher) == 2
    assert sorted(matcher.find_all("ab")) == [(0, 0), (0, 1)]


def test_len_and_state_count():
    matcher = PatternMatcher(["he", "she", "hers"])

    assert len(matcher) == 3
    # root + "he" (2) + "she" (3) + "hers" reusing the "he" prefix and adding
    # "r","s" (2) = 8. Shared prefixes are merged, which is the point of a trie.
    assert matcher.num_states == 8


def test_repr_shows_the_shape_of_the_automaton():
    matcher = PatternMatcher(["he", "she", "hers"])

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
