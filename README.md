# scikit-build-nanobind-example

An Aho–Corasick multi-pattern matcher written in C++20 and bound to Python with
[nanobind](https://nanobind.readthedocs.io), packaged with
**scikit-build-core + CMake + Ninja**.

The matcher is the pretext. The subject is the build and binding stack: a small,
complete, deliberately legible template to copy when starting a real extension
module.

It is also an honest example of *when* a C++ extension is worth the trouble. The
automaton is expensive to build and cheap to reuse, and scanning costs the same
whether you registered ten patterns or ten thousand — so the object living behind
the boundary earns its keep, rather than being a Python data structure in a
costume. At 10,000 patterns it is **81x faster** than the obvious Python
approach; at ten patterns it is a dead heat. Both numbers are below.

## What it demonstrates

| Concern | Where to look |
| --- | --- |
| Backend wiring, dependency pinning, editable rebuilds | `pyproject.toml` |
| Finding Python and nanobind, install rules, per-target flags | `CMakeLists.txt` |
| **Algorithm with no knowledge of Python** | `src/aho_corasick.{hpp,cpp}` |
| **Bindings, and nothing else** | `src/bindings.cpp` |
| Overload resolution (`str` and `bytes`) | `matches` / `count` / `find_all` |
| Converting an arbitrary Python iterable | `collect_patterns` |
| Translating C++ exceptions to Python ones | empty-pattern check in the constructor |
| Pickle support for a C++ type | `__getstate__` / `__setstate__` |
| Type stubs generated at build time | `nanobind_add_stub` in `CMakeLists.txt` |
| Property tests against a brute-force oracle | `tests/test_matcher.py` |

## The layout that matters

The algorithm and the bindings are separate build targets, and the separation is
enforced rather than merely intended:

```
src/aho_corasick.hpp   pure C++20 — no nanobind include anywhere
src/aho_corasick.cpp   builds the `aho_corasick` static library
src/bindings.cpp       the only file containing NB_MODULE
```

`aho_corasick` compiles, links, and could be unit-tested without Python existing.
That buys three things: the algorithm can be reused in a non-Python program, the
binding layer stays thin enough to read in one sitting, and the two get *different
compiler flags* — the library is built `-O3` because it holds the hot loop, while
the binding shim keeps nanobind's size-optimised default because no measurable
time is spent there. Wiring `-O3` across a single merged target would bloat the
binary for no gain.

## Decisions, and why

The choices worth knowing about before you copy this:

| Decision | Choice | Why |
| --- | --- | --- |
| Binding layer | nanobind 2.x | Best scikit-build-core integration, fast compiles, small binaries |
| nanobind source | pip dep + `-m nanobind --cmake_dir` probe | Version pin lives in `pyproject.toml`, where a Python reader looks |
| Algorithm vs bindings | Separate CMake targets | Reusable without Python, and each gets the compiler flags it deserves |
| Dev loop | editable + `editable.rebuild` + persistent `build-dir` | One build path, no drift; Ninja does the incremental work |
| Layout | `src/` layout, package wrapping a private `_core` | Somewhere to put stubs, `py.typed`, and future pure-Python code |
| Keys | `str` + `bytes` overloads | `str` is what people reach for; `bytes` is what is exact |
| Offsets | Code points for `str`, bytes for `bytes` | Byte offsets into a `str` disagree with Python's own indexing |
| Trie children | Sorted vector, binary searched | A dense table scans faster but costs 1 KB per state |
| Tests | pytest + Hypothesis against a brute-force oracle | The oracle is obviously correct; the automaton is not |
| Stubs | `nanobind_add_stub` at build time | Cannot drift from the bindings; the fiddly bit worth recording |
| C++ standard | C++20 | `std::span`, designated initialisers, `<bit>` |
| Free-threading | On | No mutable module state, and a matcher is immutable once built |
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

After the first install, editing any `.cpp` is enough — importing
`ahocorasick_demo` re-runs Ninja automatically
(`tool.scikit-build.editable.rebuild`), so there is no reinstall step in the
inner loop.

## Usage

```python
from ahocorasick_demo import PatternMatcher

matcher = PatternMatcher(["he", "she", "hers"])   # build once...

matcher.matches("ushers")        # True, stops at the first hit
matcher.count("ushers")          # 3
matcher.find_all("ushers")       # [(1, 1), (2, 0), (2, 2)]
                                 # (start, pattern_index), overlaps included

len(matcher)                     # 3 patterns
matcher.num_states               # 8 automaton states
matcher.patterns                 # [b'he', b'she', b'hers']

import pickle
restored = pickle.loads(pickle.dumps(matcher))
```

Patterns and text may each be `str` or `bytes`. `str` is encoded as UTF-8, so a
pattern registered either way matches text given either way.

## Design notes

**Overlapping and nested matches are all reported.** Searching `"he"`, `"she"`,
`"hers"` in `"ushers"` yields all three, which is what Aho–Corasick is for and
what a loop of `str.find` gets wrong. Correctness is pinned by a Hypothesis
property test comparing every result against an obviously-correct brute-force
oracle over 300 generated cases.

**Offsets follow the input type.** For `bytes` input, offsets are byte positions.
For `str`, they are code point positions — the scan counts UTF-8 lead bytes as it
goes, which is nearly free. Reporting byte offsets for `str` would disagree with
every offset Python itself produces: in `"café bar"`, `"bar"` starts at character
5 but byte 6. Getting this wrong is a bug you find in production, not in tests.

**Pickle carries the patterns, not the trie.** Reconstruction is deterministic and
build cost is ~5 ms, so serialising failure links would be effort spent to save
nothing.

**Threading.** The module declares support for free-threaded Python 3.13+
(`FREE_THREADED`): it holds no mutable global state and does not rely on the GIL.
A `PatternMatcher` is immutable once constructed, so sharing one across threads
needs no lock — unusually, this is safe rather than merely permitted.

**Why the automaton is not perfectly flat.** Scan time does creep up with pattern
count (1.8 ms → 22 ms across three orders of magnitude) because more states means
worse cache locality, not more work per byte. The complexity claim is about
algorithmic cost; memory hierarchy still charges rent.

## Benchmarks

Record-only, and skipped by default. Run them deliberately, on a quiet machine:

```bash
.venv/bin/python -m pytest --benchmark-only
```

Nothing asserts on timing and CI never gathers it — benchmark thresholds on a
shared runner produce false alarms, which train you to ignore red builds.

Measured on the development machine (GCC, `-O3`, Python 3.12), scanning 500 KB of
text. `str.count` loop is `sum(text.count(p) for p in patterns)`; regex is a
single compiled alternation of all patterns.

**Scaling in the number of patterns** — the actual argument:

| patterns | automaton | `str.count` loop | speedup |
| --- | --- | --- | --- |
| 10 | 1.80 ms | 1.84 ms | 1.0x |
| 100 | 1.99 ms | 17.5 ms | 8.8x |
| 1,000 | 4.32 ms | 177 ms | 41x |
| 10,000 | 22.1 ms | 1,782 ms | **81x** |

**All three approaches at 10,000 patterns:**

| approach | build | scan |
| --- | --- | --- |
| `PatternMatcher` | 4.83 ms | **21.8 ms** |
| `re` alternation | 4.55 ms | 6,324 ms |
| `str.count` loop | — | 1,753 ms |

Two things worth reading off these numbers honestly:

- **At ten patterns there is no win at all.** Python's `str.count` is C with a
  good substring search, and it ties. If your pattern count is small, do not
  reach for an extension — the crossover is somewhere around a hundred patterns.
- **Building the automaton is as cheap as compiling the equivalent regex**, so
  the setup cost is not a hidden tax; the entire difference is in the scan.

Regex alternation is the *slowest* option despite being the clever-looking one:
the engine backtracks across thousands of alternatives at every position.

## Deliberate omissions

Left out on purpose, each marked with a comment where it would go:

- **A third-party matcher.** The automaton is hand-rolled so the C++ stays
  readable. `CMakeLists.txt` shows where a `find_package` would attach — to the
  algorithm target, not the module, which is another reason to keep them apart.
- **C++-side unit tests.** Now genuinely feasible, since `aho_corasick` builds
  without Python — but the Python property tests cover the algorithm fully, and
  adding CTest means a second test runner in a project whose point is one clean
  build path.
- **`cibuildwheel`.** Config sits commented in `pyproject.toml`. CI here proves
  the build is environment-independent, which is the part worth having.
- **`STABLE_ABI`.** One wheel for all of Python 3.12+, at a small performance
  cost. Noted in `CMakeLists.txt`; not useful until you actually ship wheels.
- **A dense 256-entry goto table per state.** Faster scanning, but 1 KB per state
  runs to hundreds of megabytes at realistic pattern counts. Children are kept in
  a sorted vector and binary-searched instead.

## Licence

MIT.
