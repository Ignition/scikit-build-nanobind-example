"""Record-only benchmarks. Nothing here asserts on timing.

Skipped by default (see `addopts` in pyproject.toml). Run them deliberately, on a
quiet machine:

    pytest --benchmark-only

Timing assertions in CI fail on loaded shared runners, and a build that goes red
for reasons unrelated to correctness teaches you to ignore red builds. If you
ever do want a perf gate, benchmark the filter and the `set` in the same run and
assert on their *ratio* — that self-calibrates and cancels out machine speed.
"""

import pytest

from bloomdemo import BloomFilter

ITEMS = 100_000
ERROR_RATE = 0.01


@pytest.fixture(scope="module")
def keys():
    return [f"key-{i}" for i in range(ITEMS)]


@pytest.fixture(scope="module")
def populated(keys):
    filt = BloomFilter(capacity=ITEMS, error_rate=ERROR_RATE)
    filt.add_all(keys)
    return filt


def test_bloom_insert(benchmark, keys):
    def build():
        filt = BloomFilter(capacity=ITEMS, error_rate=ERROR_RATE)
        filt.add_all(keys)
        return filt

    benchmark(build)


def test_set_insert(benchmark, keys):
    benchmark(lambda: set(keys))


def test_bloom_lookup(benchmark, populated, keys):
    benchmark(lambda: sum(key in populated for key in keys))


def test_set_lookup(benchmark, keys):
    reference = set(keys)
    benchmark(lambda: sum(key in reference for key in keys))
