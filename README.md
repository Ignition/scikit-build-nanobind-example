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
costume. At 10,000 patterns it is **115x faster** than the obvious Python
approach; at ten patterns the margin nearly vanishes. Both numbers are below.

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

The algorithm and the bindings are separate build targets:

```
src/aho_corasick.hpp   pure C++20 — no nanobind include anywhere
src/aho_corasick.cpp   builds the `aho_corasick` static library
src/bindings.cpp       the only file containing NB_MODULE
```

`aho_corasick` compiles, links, and can be unit-tested without Python existing —
and that is enforced by the build, not merely intended:

```bash
cmake -S . -B build-standalone -G Ninja   # no Python, no nanobind, no pip
cmake --build build-standalone            # -> libaho_corasick.a
```

`find_package(Python)` and the nanobind probe live behind
`AHOCORASICK_BUILD_PYTHON_MODULE`, which defaults ON under scikit-build-core and
OFF for a bare `cmake -B build`. A CI job builds this way on every push, so a
stray nanobind include in the algorithm breaks the build rather than the claim.


That buys three things: the algorithm can be reused in a non-Python program, the
binding layer stays thin enough to read in one sitting, and the two get
*different compiler treatment* — nanobind's size-optimised `-Os` applies only to
the binding shim, where no measurable time is spent, while the algorithm keeps
the build type's own optimisation level. Neither target hardcodes an
optimisation flag: `-O3` written into `target_compile_options` is redundant in
Release and silently ruins a Debug build.

The library also sets `CXX_VISIBILITY_PRESET hidden`, matching what
`nanobind_add_module` does for the module. Without it, every `ac::PatternMatcher`
symbol is exported from the `.so` — which bloats the dynamic symbol table and
risks interposition if another extension in the same process exports the same
names at a different version. Turning it on here dropped the exported symbol
count from 47 to 38.

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
| Trie children | Sorted vector, linear scan | Measured: 20% fewer instructions and 40% fewer branch mispredicts than binary search |
| Tests | pytest + Hypothesis against a brute-force oracle | The oracle is obviously correct; the automaton is not |
| Stubs | `nanobind_add_stub` at build time | Cannot drift from the bindings; the fiddly bit worth recording |
| C++ standard | C++20 | `std::span`, designated initialisers, `<bit>` |
| Free-threading | On | No mutable module state, and a matcher is immutable once built |
| Benchmarks | Record-only, skipped in CI | Numbers on demand; CI stays a pure correctness gate |
| Version | `pyproject.toml` → CMake → C++ | One source of truth via `SKBUILD_PROJECT_VERSION` |
| Python range | `>=3.9`, CI on 3.9 / 3.12 / 3.13 | Claim only what is tested |
| CMake floor | 3.18, pinned in a CI job | Same rule: a floor nobody configures against is a guess |
| Optimisation flags | Left to `CMAKE_BUILD_TYPE` | Hardcoded `-O3` is redundant in Release and breaks Debug |
| Symbol visibility | Hidden on the library too | Otherwise the static library's symbols leak out of the module |

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
count (1.2 ms → 15.2 ms across three orders of magnitude) because more states
means worse cache locality, not more work per byte. The complexity claim is about
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
| 10 | 1.22 ms | 1.83 ms | 1.5x |
| 100 | 1.34 ms | 17.4 ms | 13x |
| 1,000 | 2.79 ms | 177 ms | 63x |
| 10,000 | 15.2 ms | 1,753 ms | **115x** |

**All three approaches at 10,000 patterns:**

| approach | build | scan |
| --- | --- | --- |
| `PatternMatcher` | 4.29 ms | **15.1 ms** |
| `re` alternation | 4.53 ms | 6,347 ms |
| `str.count` loop | — | 1,767 ms |

Two things worth reading off these numbers honestly:

- **At ten patterns the win is 1.5x**, which is not worth an extension. Python's
  `str.count` is C with a good substring search. If your pattern count is small,
  do not reach for C++ — the case only becomes compelling in the hundreds.
- **Building the automaton is as cheap as compiling the equivalent regex**, so
  the setup cost is not a hidden tax; the entire difference is in the scan.

Regex alternation is the *slowest* option despite being the clever-looking one:
the engine backtracks across thousands of alternatives at every position.

### What the profiler actually said

The first version used `std::lower_bound` to find a node's child. `perf record`
put **83% of all cycles in that one function**, and `perf stat` explained why:

| | instructions | branches | branch-misses | IPC |
| --- | --- | --- | --- | --- |
| `std::lower_bound` | 4.49B | 1.02B | 98.4M (**9.63%**) | 0.84 |
| linear scan | 3.59B | 1.40B | 58.7M (**4.18%**) | 0.82 |

The linear scan executes *more* branches and is still ~20% faster overall,
because a binary search branches on data the predictor cannot learn. Child lists
are short, so the loop is both fewer instructions and vastly more predictable.

Three things were measured and **rejected**:

- **`[[likely]]` / `[[unlikely]]` on the transition function: ~1%, inside noise.**
  The mispredictions were in a data-dependent search, and no hint can help a
  branch that genuinely goes both ways. Changing the algorithm fixed it; annotating
  it would not have. This is the trap the attributes invite.
- **`__attribute__((always_inline))` on `child`: no measurable gain** over simply
  defining it in the header and letting the compiler decide. The attribute was
  compiler-specific clutter buying 0.4%.
- **A dense 256-entry root transition table: no gain at all** once the linear scan
  was in (14.79 ms vs 14.79 ms). It looked obviously worthwhile beforehand.

The lesson worth taking from this file is the order of operations: profile,
change the algorithm, and reach for hints and attributes last — if ever.

### Where the standard library helps, and where it does not

`std::span` and the ranges algorithms are used throughout — except in the one
function the profiler pointed at:

| Place | Choice |
| --- | --- |
| `patterns()` return, constructor parameter | `std::span` — decouples the API from `std::vector` |
| Keeping child lists sorted | `std::ranges::lower_bound` with a projection, which removes the comparator lambda |
| Counting UTF-8 lead bytes | `std::ranges::count_if` |
| Summing pattern lengths | `std::transform_reduce`, with a lambda — the classic algorithms invoke with `()` rather than `std::invoke`, so a pointer-to-member does not work there as it does in ranges |
| **`child()`, the innermost lookup** | **hand-written loop — `std::ranges::find_if` measured 6% slower** |

`find_if` has to locate the first entry `>= byte` and then re-test for equality;
the hand-written loop returns the instant it matches. At one call per input byte
that shows up. Everywhere else the algorithms are free and read better.

Two places deliberately keep a raw loop for reasons of clarity rather than speed:
`scan` is a stateful traversal with early exit, which no C++20 range adaptor
expresses well, and the output-link walk is a linked list, not a range. Note that
`scan` *is* the factored-out algorithm — `matches`, `count`, and `find_all` are
three folds over it, which is the point of "no raw loops" rather than its literal
reading.


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
- **A dense 256-entry goto table per state.** 1 KB per state runs to hundreds of
  megabytes at realistic pattern counts. Doing it for the *root only* is cheap and
  looked obviously worthwhile — but measured at exactly break-even once children
  were scanned linearly, so it is not here.
- **A flattened CSR node layout.** Each `Node` holds two `std::vector`s, so
  traversal chases pointers. Packing all children into one array with per-node
  offsets would cut the cache misses that dominate the 10,000-pattern case. It
  would also make the code substantially harder to read, which is the wrong trade
  here.
- **A public callback-based scan.** `find_all` allocates a vector; the private
  `scan` it is built on takes a callback and does not. Exposing that would let
  callers avoid the allocation entirely, and is what the C++ library would want if
  it were the product rather than the pretext.

## Licence

MIT.
