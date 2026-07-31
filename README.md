# bloomdemo

A Bloom filter written in C++20 and bound to Python with
[nanobind](https://nanobind.readthedocs.io), packaged with
**scikit-build-core + CMake + Ninja**.

The filter is the pretext. The subject is the build and binding stack: this is a
small, complete, deliberately legible template to copy when starting a real
extension module.

## What it demonstrates

| Concern | Where to look |
| --- | --- |
| Backend wiring, dependency pinning, editable rebuilds | `pyproject.toml` |
| Finding Python and nanobind, build flags, install rules | `CMakeLists.txt` |
| Free functions, classes, dunders, properties, operators | `src/bloom.cpp` |
| Overload resolution (`str` and `bytes`) | `BloomFilter::add` bindings |
| Translating C++ exceptions to Python ones | `BloomFilter::union_with` |
| Pickle support for a C++ type | `nb::pickle` in `src/bloom.cpp` |
| Type stubs generated at build time | `nanobind_add_stub` in `CMakeLists.txt` |
| Property-based testing of an invariant | `tests/test_bloom.py` |

## Decisions, and why

The choices worth knowing about before you copy this:

| Decision | Choice | Why |
| --- | --- | --- |
| Binding layer | nanobind 2.x | Best scikit-build-core integration, fast compiles, small binaries |
| nanobind source | pip dep + `-m nanobind --cmake_dir` probe | Version pin lives in `pyproject.toml`, where a Python reader looks |
| Dev loop | editable + `editable.rebuild` + persistent `build-dir` | One build path, no drift; Ninja does the incremental work |
| Layout | `src/` layout, package wrapping a private `_core` | Somewhere to put stubs, `py.typed`, and future pure-Python code |
| Keys | `str` + `bytes` overloads | `str` is what people reach for; `bytes` is what is correct |
| Hashing | FNV-1a + splitmix64, not Python's `hash()` | `PYTHONHASHSEED` would break pickled filters across processes |
| Tests | pytest + Hypothesis | No-false-negatives is a property, not an example |
| Stubs | `nanobind_add_stub` at build time | Cannot drift from the bindings; the fiddly bit worth recording |
| C++ standard | C++20 | `<bit>`, `std::span`, `std::to_chars` |
| Free-threading | On | No mutable module state; instances follow normal Python sharing rules |
| Atomics | None | Instances are single-threaded by contract; `fetch_or` would tax the hot path |
| Benchmarks | Record-only, skipped in CI | Numbers on demand; CI stays a pure correctness gate |
| Version | `pyproject.toml` → CMake → C++ | One source of truth via `SKBUILD_PROJECT_VERSION` |
| Python range | `>=3.9`, CI on 3.9 / 3.12 / 3.13 | Claim only what is tested |

## Quick start

```bash
python3 -m venv .venv
.venv/bin/python -m pip install -e ".[test]" --no-build-isolation
.venv/bin/python -m pytest
```

`--no-build-isolation` is required for editable installs here: the build needs
`nanobind` and `scikit-build-core` present in the environment you are developing
against, not just in a throwaway isolated one.

After the first install, editing `src/bloom.cpp` is enough — importing
`bloomdemo` re-runs Ninja automatically (`tool.scikit-build.editable.rebuild`),
so there is no reinstall step in the inner loop.

## Usage

```python
from bloomdemo import BloomFilter

filt = BloomFilter(capacity=1_000_000, error_rate=0.01)
filt.add("hello")
filt.add(b"world")          # str and bytes are both accepted
filt.add_all(f"user-{i}" for i in range(1000))

"hello" in filt             # True — never a false negative
"absent" in filt            # almost certainly False
len(filt)                   # 1002, counted exactly
filt.memory_bytes           # ~1.2 MB

merged = filt | other       # union; ValueError if the parameters differ

import pickle
restored = pickle.loads(pickle.dumps(filt))   # bit array round-trips intact
```

## Design notes

**No false negatives, ever.** If `add(x)` succeeded, `x in filt` is `True`. This is
the correctness contract, and it is stated as a Hypothesis property rather than
sampled by example tests.

**False positives are the price.** `error_rate` is a target, not a guarantee for
any single query. The test suite therefore never asserts that a particular item
is absent — it measures the observed rate across many items and allows generous
tolerance. Asserting absence of one item is a flaky test waiting to happen.

**Hashing.** Two 64-bit splitmix64 hashes are combined by the Kirsch–Mitzenmacher
technique to derive *k* probe positions, which is standard practice and avoids
computing *k* independent hashes. `str` keys are hashed as their UTF-8 bytes, so
`filt.add("a")` and `filt.add(b"a")` are the same insertion. Python's built-in
`hash()` is deliberately *not* used: `PYTHONHASHSEED` randomisation would make
pickled filters return garbage in a new process.

**Threading.** The module declares support for free-threaded Python 3.13+
(`FREE_THREADED`): it holds no mutable global state and does not rely on the GIL.
Individual filters are *not* internally synchronised — share one across threads
under a lock, exactly as you would a `dict`.

**No atomics.** A filter is owned by one thread by contract, so the bit array uses
plain writes. Making `add` atomic would tax the one hot path in the library to
support a case the design excludes.

## Benchmarks

Record-only, and skipped by default. Run them deliberately, on a quiet machine:

```bash
.venv/bin/python -m pytest --benchmark-only
```

Nothing asserts on timing and CI never gathers it — benchmark thresholds on a
shared runner produce false alarms, which train you to ignore red builds.

Measured on the development machine (GCC, `-Os`, Python 3.12), short string keys
at a 1% target error rate.

Memory, at 1,000,000 keys — this is the headline:

| | `set` | `BloomFilter` |
| --- | --- | --- |
| memory | 50.3 MB (tracemalloc peak) | **1.20 MB** |

**42x less memory**, in exchange for a 1% false-positive rate and no ability to
enumerate or remove. That trade is the entire reason the data structure exists.

Throughput, at 100,000 keys — this is the *un*flattering half, reported anyway:

| | `set` | `BloomFilter` |
| --- | --- | --- |
| insert | 2.82 ms | 5.38 ms |
| lookup | 3.20 ms | 6.98 ms |

The filter is roughly 2x *slower* than `set`, and pretending otherwise would
undermine the demo. `set` is C-implemented with cached string hashes, while the
filter computes two fresh hashes and touches k=7 scattered words per key. Bloom
filters win on memory, and on the cases memory unlocks — staying resident when a
`set` would not fit, or crossing a network. They do not win a microbenchmark
against `set`, and a template that claimed otherwise would be teaching a lie.

## Deliberate omissions

Left out on purpose, each marked with a comment where it would go:

- **A third-party filter.** Boost 1.89 ships a real one, but provisioning Boost on
  every build machine would be larger than the rest of this project combined.
  `CMakeLists.txt` shows the two lines that would swap it in.
- **`cibuildwheel`.** Config sits commented in `pyproject.toml`. CI here proves the
  build is environment-independent, which is the part worth having.
- **`STABLE_ABI`.** One wheel for all of Python 3.12+, at a small performance cost.
  Noted in `CMakeLists.txt`; not useful until you actually ship wheels.
- **`intersection()`.** Intersecting Bloom filters does not mean what people expect
  — the result over-approximates, and error rates compound unpredictably.
- **`__eq__`.** Equality is meaningless here: identical bit arrays do not imply
  identical contents, and differing ones do not imply differing contents. Filters
  compare by identity, which is the honest answer. Tests that need a structural
  comparison use `__getstate__()`.

## Licence

MIT.
