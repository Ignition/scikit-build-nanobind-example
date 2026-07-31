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
costume. At 10,000 patterns it is **117x faster** than the obvious Python
approach; at ten patterns the margin nearly vanishes. Both numbers are below.

## What it demonstrates

| Concern | Where to look |
| --- | --- |
| Backend wiring, dependency pinning, editable rebuilds | `pyproject.toml` |
| Finding Python and nanobind, install rules, per-target flags | `CMakeLists.txt` |
| **A C++20 module in a `FILE_SET CXX_MODULES`** | `CMakeLists.txt` |
| **Algorithm with no knowledge of Python** | `src/aho_corasick.cppm` + `.cpp` |
| **Interface / implementation split across module units** | `src/aho_corasick.cpp` |
| **Bindings, and nothing else** | `src/bindings.cpp` |
| Overload resolution (`str` and `bytes`) | `matches` / `count` / `find_all` |
| Converting an arbitrary Python iterable | `collect_patterns` |
| Translating C++ exceptions to Python ones | empty-pattern check in the constructor |
| Pickle support for a C++ type | `__getstate__` / `__setstate__` |
| Type stubs generated at build time | `nanobind_add_stub` in `CMakeLists.txt` |
| Property tests against a brute-force oracle | `tests/test_matcher.py` |
| **C++ unit tests, fetched framework and all** | `tests/test_matcher.cpp` + `FetchContent` in `CMakeLists.txt` |

## The layout that matters

The algorithm and the bindings are separate build targets:

```
src/aho_corasick.cppm  module interface: the exported declarations
src/aho_corasick.cpp   module implementation unit: the definitions
src/bindings.cpp       the only file containing NB_MODULE
```

The algorithm is a C++20 module rather than a header and source pair, so the
boundary is enforced by the language and not only by convention: an importer
sees the exported class and nothing else. The trie node type, the transition
function, and the traversal are genuinely private, which a header cannot
express. `bindings.cpp` reaches it with `import aho_corasick;` while still
including nanobind's headers in the same file, which mixes cleanly.

`aho_corasick` compiles, links, and is unit-tested without Python existing — and
that is enforced by the build, not merely intended:

```bash
cmake -S . -B build-cpp -G Ninja   # no Python, no nanobind, no pip
cmake --build build-cpp
ctest --test-dir build-cpp         # 11 cases, no interpreter involved
```

The tests are C++ because that is where the units are. `tests/test_matcher.cpp`
owns the fixed-case behaviour and, more usefully, the four things the binding
layer cannot reach at all: byte offsets over non-ASCII text, character offsets
over invalid UTF-8, `Pattern::chars`, and the constructor's exception as a C++
type rather than a translated `ValueError`. `tests/test_matcher.py` keeps what
genuinely exercises the whole stack — the `str`/`bytes` overloads, pickle,
packaging, and the Hypothesis property tests against a brute-force oracle, which
have no C++ equivalent worth having.

That suite is also the only thing that links a consumer against the shared
library. Drop an `AHOCORASICK_EXPORT` and `BUILD_SHARED_LIBS=ON` now fails to
link here, rather than in somebody else's project.

Building the tests needs the network the first time, to fetch Catch2. To build
the library with genuinely nothing installed, turn them off:

```bash
cmake -S . -B build-standalone -G Ninja -DAHOCORASICK_BUILD_TESTS=OFF
cmake --build build-standalone            # -> libaho_corasick.a
```

### Using it from another project

It installs, and exports a CMake package, so reuse does not mean vendoring this
repository:

```bash
cmake --install build-standalone --prefix /some/prefix
```

```cmake
find_package(ahocorasick 0.1 REQUIRED)
target_link_libraries(your_target PRIVATE ahocorasick::core)
```

```cpp
import aho_corasick;   // and nothing else from this project
```

Two things about installing a C++20 module are worth knowing before copying this
part. **The interface unit ships as source, not as a BMI** — a BMI is tied to one
compiler, one version and one set of flags, so a package containing one would
work only on the machine that built it. The consumer compiles the interface, so
it needs a module-capable toolchain too; there is no way to hand a module to an
older compiler.

That has a second consequence, and it is the one that quietly breaks builds:
because the consumer compiles the interface unit, the interface unit's `#include
<ahocorasick_export.h>` has to resolve *in the consumer's build*. So the
generated export header is installed and the include directory is `PUBLIC`, even
though no consumer ever writes that include. The same applies to
`AHOCORASICK_STATIC_DEFINE`, which is why it is a `PUBLIC` compile definition
rather than a private one — inert on ELF, but a `dllimport` against a static
archive on MSVC if it is missing.

`install(EXPORT ... CXX_MODULES_DIRECTORY ...)` is likewise not optional. Without
it the exported target carries no module information and a consumer's `import`
finds nothing to import.

Static by default, and `BUILD_SHARED_LIBS=ON` gets you `libaho_corasick.so`
instead. The wheel ignores the setting and always links statically: a shared core
inside a wheel would have to be installed beside the extension module and found
again at import time through an `$ORIGIN` RPATH, and the module is the only
consumer there anyway.

The shared build needs one thing the static build does not. `export` on a module
declaration and ELF symbol visibility are unrelated despite the shared word — the
first says what an importer may *name*, the second says what survives into the
dynamic symbol table, which this project hides by default. Left alone, the shared
library compiles and links cleanly and then fails in the *consumer*, with
undefined references to names the module plainly exports. So the public members
carry `AHOCORASICK_EXPORT`, generated by CMake's `GenerateExportHeader` and
included from the global module fragment because macros do not cross a module
boundary. It expands to nothing in a static build.

The annotation is per member rather than on the class, so that the ABI surface
matches the language surface: annotating the class would export the private trie
internals the module exists to hide. `nm -D --defined-only libaho_corasick.so`
lists the seven public functions and one weak libstdc++ template instantiation —
no `step`, no `child`, no `build_failure_links`, no `scan`.

### Why two files, not one

Merging the algorithm into a single module interface unit is tempting and wrong.
Definitions written there form part of the compiled interface, so editing a
function body invalidates it and every importer is rebuilt. Measured on this
project, a comment-only change to a function body recompiled the binding layer.
Splitting the definitions into a module implementation unit, which names the
module without exporting from it, restores what a header and source pair gives
for free: editing the implementation recompiles only that file, while editing
the interface correctly rebuilds importers.

The exception is the handful of accessors still defined in-class in the
interface, `child` and `node` among them, which are there because they must stay
inlinable in the hot loop. Editing one of those does rebuild importers. That is
the trade, and it is the same one a header forces.

The tidier spelling for this is a private module fragment, `module :private;`,
which keeps both halves in one file. GCC has not implemented it, in 14 or in 16,
and reports `sorry, unimplemented: private module fragment`. Clang accepts it.
An implementation unit is portable today and achieves the same separation.

## Toolchain requirements

Modules are not free. This project needs:

| | Required | Why |
| --- | --- | --- |
| CMake | **3.28+** | `FILE_SET CXX_MODULES` |
| Compiler | **GCC 14+, Clang 16+, MSVC 19.34+** | must be able to report its own import graph |
| Generator | **Ninja or Visual Studio** | the only ones that scan for modules |

There is no header-based fallback. Both requirements are checked at configure
time by building a throwaway module rather than by comparing version numbers
against a table — so the check covers the generator as well as the compiler, and
does not need editing when a new compiler grows module support. On an
unsupported toolchain the build stops with a message naming the compiler *and*
the generator it found, and two CI jobs assert that it does.

The probe is a nested `cmake` run rather than `try_compile`, which is the
obvious spelling and does not work: when the toolchain cannot scan for modules
the failure happens while *generating* the test project, and `try_compile`
reports that as a hard error, so configuration stops before the useful message
can be printed.

**The gotcha worth knowing.** scikit-build-core takes the compiler from the
interpreter's own build configuration, not from your shell. On Debian and Ubuntu
that is the triplet-prefixed name, `x86_64-linux-gnu-g++`, which is a *different
symlink* from `g++` and stays on the distribution's older GCC even after
`update-alternatives` switches the default. So `cmake` can succeed while
`pip install` fails on the same machine. Set the compiler explicitly:

```bash
CXX=g++-14 pip install -e ".[test]" --no-build-isolation
```

`find_package(Python)` and the nanobind probe live behind
`AHOCORASICK_BUILD_PYTHON_MODULE`, which defaults ON under scikit-build-core and
OFF for a bare `cmake -B build`. A CI job builds this way on every push, so a
stray nanobind include in the algorithm breaks the build rather than the claim.


That buys two things: the algorithm can be reused in a non-Python program, and
the binding layer stays thin enough to read in one sitting. Separate targets also
allow separate compiler flags, which is usually the third benefit — nanobind
compiles the binding shim at `-Os` by default, and that shim is where no
measurable time is spent.

This project gives that up, deliberately. `nanobind_add_module` is passed
`NOMINSIZE`, because GCC 14 cannot compile a translation unit that imports a
module at `-Os`: it fails to emit the body of an `always_inline` standard library
function reached through the module's global module fragment. Both targets
therefore build at the build type's own level. Nothing in the shim is hot enough
for it to matter, but the flags are no longer the illustration they were before
the module conversion. Neither target hardcodes an optimisation flag either way:
`-O3` written into `target_compile_options` is redundant in Release and silently
ruins a Debug build.

Hidden visibility is a project-wide default rather than a per-target property, so
a target added later cannot silently start from `default`. Without it every
`ac::PatternMatcher` symbol is exported from the `.so`, which bloats the dynamic
symbol table and risks interposition if another extension in the same process
exports the same names at a different version.

`--exclude-libs,ALL` on the link line finishes the job. Visibility settings only
reach code this project compiles; a third-party static library arrives already
compiled, most likely with default visibility, and re-exports its whole symbol
table from whichever shared object absorbs it. There is no such dependency yet,
but it also strips the weak libstdc++ template instantiations that survive
otherwise: the built extension module exports **1** symbol, `PyInit__core`, down
from 31.

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
| Tests | Catch2 for units, pytest + Hypothesis for integration | Shift left: unit-test the C++ in C++, and let Python test the whole stack |
| Test framework | Catch2 v3, pinned tag, via `FetchContent` | Version lives in the build; the one dependency this template otherwise never demonstrates |
| Stubs | `nanobind_add_stub` at build time | Cannot drift from the bindings; the fiddly bit worth recording |
| Algorithm form | A C++20 module | The privacy boundary is enforced by the language, not by convention |
| C++ standard | C++20 | `std::span`, the ranges algorithms and their projections |
| Free-threading | On | No mutable module state, and a matcher is immutable once built |
| Benchmarks | Record-only, skipped in CI | Numbers on demand; CI stays a pure correctness gate |
| Version | `CMakeLists.txt` → `pyproject.toml` → C++ | The C++ package needs a version when Python is absent |
| Python range | `>=3.9`, CI on 3.9 / 3.12 / 3.13 | Claim only what is tested |
| CMake floor | 3.28, pinned in a CI job | `FILE_SET CXX_MODULES` needs it; a floor nobody configures against is a guess |
| Optimisation flags | Left to `CMAKE_BUILD_TYPE` | Hardcoded `-O3` is redundant in Release and breaks Debug |
| Symbol visibility | Hidden project-wide, exported per member | Otherwise the algorithm's symbols leak out of the extension module |
| Library type | `BUILD_SHARED_LIBS`, forced static for the wheel | A shared core in a wheel needs an `$ORIGIN` RPATH and buys nothing |
| Reuse | Installed + exported CMake package | Reuse without vendoring; the seam's second consumer, tested in CI |
| Installed module | Interface unit as source, never a BMI | A BMI is specific to one compiler, version and flag set |

## Quick start

```bash
python3 -m venv .venv
CXX=g++-14 .venv/bin/python -m pip install -e ".[test]" --no-build-isolation
.venv/bin/python -m pytest
```

`--no-build-isolation` is required for editable installs here: the build needs
`nanobind` and `scikit-build-core` present in the environment you are developing
against, not just in a throwaway isolated one.

After the first install, editing any `.cpp` or `.cppm` is enough — importing
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
count (1.1 ms → 15.0 ms across three orders of magnitude) because more states
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
| 10 | 1.09 ms | 1.81 ms | 1.7x |
| 100 | 1.20 ms | 17.2 ms | 14x |
| 1,000 | 2.59 ms | 174 ms | 67x |
| 10,000 | 15.0 ms | 1,757 ms | **117x** |

**All three approaches at 10,000 patterns:**

| approach | build | scan |
| --- | --- | --- |
| `PatternMatcher` | 4.33 ms | **15.2 ms** |
| `re` alternation | 4.59 ms | 6,241 ms |
| `str.count` loop | — | 1,754 ms |

Two things worth reading off these numbers honestly:

- **At ten patterns the win is under 2x**, which is not worth an extension. Python's
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
  defining it in-class in the module interface and letting the compiler decide.
  The attribute was compiler-specific clutter buying 0.4%.
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
- **`cibuildwheel`.** Config sits commented in `pyproject.toml`, and as written it
  would not work: the manylinux images ship compilers too old to scan for
  modules. Shipping wheels from this template means either building a newer
  toolchain into the image or giving up modules. CI here proves the build is
  environment-independent, which is the part worth having.
- **`STABLE_ABI`.** One wheel for all of Python 3.12+, at a small performance
  cost. Noted in `CMakeLists.txt`; not useful until you actually ship wheels.
- **`import std`.** Standard library headers go in the global module fragment
  instead. `import std` needs a newer toolchain than this project already asks
  for, and is still behind an experimental gate in CMake.
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
