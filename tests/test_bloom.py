import pickle

import pytest
from hypothesis import HealthCheck, given, settings
from hypothesis import strategies as st

from bloomdemo import BloomFilter

# Distinct enough that collisions between generated keys are not the thing under test.
keys = st.one_of(st.text(min_size=1, max_size=40), st.binary(min_size=1, max_size=40))
key_sets = st.lists(keys, min_size=1, max_size=200, unique_by=lambda k: _canonical(k))


def _canonical(key):
    return key.encode("utf-8") if isinstance(key, str) else key


# --- Tier 1: construction and core behaviour ---------------------------------


def test_rejects_nonsensical_parameters():
    with pytest.raises(ValueError):
        BloomFilter(capacity=0, error_rate=0.01)
    with pytest.raises(ValueError):
        BloomFilter(capacity=100, error_rate=0.0)
    with pytest.raises(ValueError):
        BloomFilter(capacity=100, error_rate=1.0)


def test_parameters_follow_the_standard_sizing_formulas():
    filt = BloomFilter(capacity=1000, error_rate=0.01)

    assert filt.capacity == 1000
    assert filt.error_rate == pytest.approx(0.01)
    # m = -n*ln(p)/ln(2)^2 ~= 9585 bits, k = ln(2)*m/n ~= 7 probes.
    assert filt.num_bits == pytest.approx(9585, rel=0.01)
    assert filt.num_hashes == 7
    # Bits rounded up to whole 64-bit words.
    assert filt.memory_bytes == ((filt.num_bits + 63) // 64) * 8


def test_a_new_filter_is_empty():
    filt = BloomFilter(capacity=100, error_rate=0.01)

    assert len(filt) == 0
    assert "anything" not in filt


def test_len_counts_every_add_including_duplicates():
    # Honest reporting: len() is items added, not an estimate of distinct items.
    filt = BloomFilter(capacity=100, error_rate=0.01)
    filt.add("x")
    filt.add("x")

    assert len(filt) == 2


def test_str_and_bytes_are_the_same_key():
    filt = BloomFilter(capacity=100, error_rate=0.01)
    filt.add("hello")

    assert b"hello" in filt, "str keys must hash as their UTF-8 bytes"


def test_repr_shows_the_parameters():
    filt = BloomFilter(capacity=1000, error_rate=0.01)
    filt.add("x")

    assert repr(filt) == "BloomFilter(capacity=1000, error_rate=0.01, items=1)"


@given(items=key_sets)
def test_never_reports_a_false_negative(items):
    # The correctness contract of a Bloom filter, stated directly. Example tests
    # verify this only by luck.
    filt = BloomFilter(capacity=max(len(items), 1), error_rate=0.01)
    for item in items:
        filt.add(item)

    assert all(item in filt for item in items)


def test_false_positive_rate_stays_near_the_target():
    # Never assert that one particular item is absent — that is a flaky test.
    # Measure the rate over many items, with generous tolerance.
    target = 0.01
    count = 20_000
    filt = BloomFilter(capacity=count, error_rate=target)
    for i in range(count):
        filt.add(f"present-{i}")

    false_positives = sum(f"absent-{i}" in filt for i in range(count))
    observed = false_positives / count

    assert observed < target * 3, f"observed false-positive rate {observed:.4f}"


# --- Tier 2: batch, union, pickle --------------------------------------------


@given(items=key_sets)
def test_add_all_matches_repeated_add(items):
    one_at_a_time = BloomFilter(capacity=200, error_rate=0.01)
    for item in items:
        one_at_a_time.add(item)

    batched = BloomFilter(capacity=200, error_rate=0.01)
    batched.add_all(items)

    assert batched == one_at_a_time
    assert len(batched) == len(items)


def test_add_all_accepts_a_lazy_iterable():
    filt = BloomFilter(capacity=100, error_rate=0.01)
    filt.add_all(f"item-{i}" for i in range(10))

    assert len(filt) == 10
    assert "item-7" in filt


@given(left_items=key_sets, right_items=key_sets)
@settings(suppress_health_check=[HealthCheck.too_slow])
def test_union_contains_everything_from_both_sides(left_items, right_items):
    left = BloomFilter(capacity=200, error_rate=0.01)
    left.add_all(left_items)
    right = BloomFilter(capacity=200, error_rate=0.01)
    right.add_all(right_items)

    merged = left | right

    assert all(item in merged for item in left_items)
    assert all(item in merged for item in right_items)
    assert len(merged) == len(left) + len(right)


def test_union_leaves_the_operands_untouched():
    left = BloomFilter(capacity=100, error_rate=0.01)
    left.add("only-left")
    right = BloomFilter(capacity=100, error_rate=0.01)
    right.add("only-right")

    left | right

    assert len(left) == 1
    assert len(right) == 1


def test_union_of_mismatched_filters_is_an_error():
    # Merging bit arrays of different shapes is meaningless; refuse loudly.
    left = BloomFilter(capacity=100, error_rate=0.01)
    right = BloomFilter(capacity=200, error_rate=0.01)

    with pytest.raises(ValueError, match="parameters"):
        left | right


@given(items=key_sets)
def test_pickle_round_trips_exactly(items):
    filt = BloomFilter(capacity=200, error_rate=0.01)
    filt.add_all(items)

    restored = pickle.loads(pickle.dumps(filt))

    assert restored == filt
    assert len(restored) == len(filt)
    assert restored.num_bits == filt.num_bits
    assert all(item in restored for item in items)


def test_pickled_filter_does_not_depend_on_hash_randomisation():
    # Using Python's hash() would break this across processes; splitmix64 does not.
    import subprocess
    import sys

    filt = BloomFilter(capacity=100, error_rate=0.01)
    filt.add("survives-a-restart")
    blob = pickle.dumps(filt)

    script = (
        "import pickle, sys;"
        "filt = pickle.loads(sys.stdin.buffer.read());"
        "sys.exit(0 if 'survives-a-restart' in filt else 1)"
    )
    result = subprocess.run(
        [sys.executable, "-c", script],
        input=blob,
        env={"PYTHONHASHSEED": "1", "PATH": "/usr/bin:/bin"},
    )

    assert result.returncode == 0, "filter contents did not survive a new process"


def test_equality_compares_contents_not_identity():
    left = BloomFilter(capacity=100, error_rate=0.01)
    right = BloomFilter(capacity=100, error_rate=0.01)
    assert left == right

    left.add("x")
    assert left != right

    right.add("x")
    assert left == right
