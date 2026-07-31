"""Record-only benchmarks. Nothing here asserts on timing.

Skipped by default; run them deliberately on a quiet machine:

    pytest --benchmark-only

Timing assertions fail on loaded shared runners, and a build that goes red for
reasons unrelated to correctness teaches you to ignore red builds. A perf gate
that did work would assert on the *ratio* between the matcher and a Python
baseline measured in the same run, which cancels machine speed.

The comparison that matters is scaling in the number of patterns: the automaton
is O(text) regardless, while every Python approach re-reads the text once per
pattern.
"""

import random
import re

import pytest

from ahocorasick_demo import PatternMatcher

PATTERN_COUNT = 10_000
TEXT_SIZE = 500_000
SEED = 20260731


@pytest.fixture(scope="module")
def corpus():
    # Fixed seed, or the numbers are not comparable across commits.
    rng = random.Random(SEED)
    alphabet = "abcdefghijklmnopqrstuvwxyz"

    patterns = set()
    while len(patterns) < PATTERN_COUNT:
        patterns.add("".join(rng.choice(alphabet) for _ in range(rng.randint(4, 9))))

    text = "".join(rng.choice(alphabet + " ") for _ in range(TEXT_SIZE))
    return sorted(patterns), text


@pytest.fixture(scope="module")
def matcher(corpus):
    patterns, _ = corpus
    return PatternMatcher(patterns)


# --- Building the automaton: the cost paid once ------------------------------


def test_build_automaton(benchmark, corpus):
    patterns, _ = corpus
    benchmark(lambda: PatternMatcher(patterns))


def test_build_regex_alternation(benchmark, corpus):
    patterns, _ = corpus
    benchmark(lambda: re.compile("|".join(map(re.escape, patterns))))


# --- Scanning: the cost paid every time --------------------------------------


def test_scan_automaton(benchmark, matcher, corpus):
    _, text = corpus
    benchmark(lambda: matcher.count(text))


def test_scan_str_count_loop(benchmark, corpus):
    # str.count is C, but re-reads the text per pattern: O(text x patterns).
    patterns, text = corpus
    benchmark(lambda: sum(text.count(pattern) for pattern in patterns))


def test_scan_regex_alternation(benchmark, corpus):
    # One pass, but the engine backtracks across every alternative at every
    # position.
    patterns, text = corpus
    combined = re.compile("|".join(map(re.escape, patterns)))
    benchmark(lambda: sum(1 for _ in combined.finditer(text)))


# --- Scaling in the number of patterns ---------------------------------------
# The actual argument for the automaton: scan cost should barely move as the
# pattern count grows, while the Python baseline tracks it linearly.

PATTERN_COUNTS = [10, 100, 1_000, 10_000]


@pytest.mark.parametrize("count", PATTERN_COUNTS)
def test_scaling_automaton(benchmark, corpus, count):
    patterns, text = corpus
    matcher = PatternMatcher(patterns[:count])
    benchmark(lambda: matcher.count(text))


@pytest.mark.parametrize("count", PATTERN_COUNTS)
def test_scaling_str_count_loop(benchmark, corpus, count):
    patterns, text = corpus
    subset = patterns[:count]
    benchmark(lambda: sum(text.count(pattern) for pattern in subset))
