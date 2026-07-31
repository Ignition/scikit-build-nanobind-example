# Build architecture: implementation plan

Four stages, from the CMake architecture review. Each stage configures, builds,
and passes the full test suite on its own.

## Decisions settled up front

| # | Decision | Chosen | Rejected, and why |
| --- | --- | --- | --- |
| 1 | Test placement | Unit tests in C++, pytest as integration | Link-witness only — misses the four behaviours the bindings cannot reach |
| 2 | Division of labour | C++ owns the unreachable surface plus core algorithm units; pytest keeps the Hypothesis oracle, overloads, pickle, packaging | Maximal move — the oracle has no C++ equivalent worth having; porting it yields fixed cases that catch strictly less |
| 3 | Framework | Catch2 v3 via `FetchContent` with `FIND_PACKAGE_ARGS` | Vendored doctest — dodges demonstrating dependency acquisition, which is the template's one conspicuous gap |
| 4 | Test default | ON for bare `cmake`, OFF under scikit-build-core; CI jobs split | OFF everywhere — tests invisible unless you read `CMakeLists.txt` |
| 5 | Internals access | Public surface only | `friend` in `aho_corasick.cppm` — puts test scaffolding in the file the design argument rests on |
| 6 | Version root | Invert: `project(VERSION)` owns it, `pyproject.toml` reads it back | Strip the fallback — leaves the standalone installed package at `0.0.0` |
| 7 | Export header | Install it; include dir becomes `PUBLIC` via `BUILD_INTERFACE`/`INSTALL_INTERFACE` | Hand-rolled macro — an untested MSVC `dllimport` path to save a README sentence |
| 8 | Toolchain guard | Nested `cmake` probe via `execute_process`, cached — see the amendment in stage 1 | `CMAKE_CXX_SCANDEP_SOURCE` — undocumented private variable |
| 9 | Shared flags | `ahocorasick_warnings` INTERFACE target; `--exclude-libs,ALL` stays inline | Bundling both — makes a per-target link decision look uniform |
| 10 | File layout | One `CMakeLists.txt`; tests section ordered before the Python section | `add_subdirectory(tests)` — costs the read-top-to-bottom property that makes the template worth copying |

Stage order is de-risking, not importance: stages 1 and 2 are small and
independent, and stage 4 wants both the version root (stage 2) and a working C++
consumer (stage 3) already in place.

---

## Stage 1: Toolchain guard and warnings

**Goal**: The guard tests the capability instead of enumerating compilers, and
covers the generator requirement the README already claims. Warning flags live
in one place.

**Amendment during implementation.** Decision 8 specified `try_compile(...
SOURCES_TYPE CXX_MODULE)`. It does not work for this purpose: when the toolchain
cannot scan for modules the failure happens while *generating* the test project,
which `try_compile` raises as a hard CMake error. Configuration then aborts
inside the probe and the actionable message never runs — reproducing exactly the
opaque diagnostic the guard exists to replace. Verified against g++-13/Ninja and
g++-14/Unix Makefiles.

The intent of decision 8 is preserved with a nested `cmake` run via
`execute_process`, which captures the exit status instead of propagating it. No
hardcoded compiler or generator lists; both dimensions covered by one predicate.

Replace `CMakeLists.txt:26-37` with a cached probe:

- `file(WRITE ...)` a throwaway project into `${CMAKE_CURRENT_BINARY_DIR}/cmake-probe`
  — generated, not committed.
- Configure it with this build's own `${CMAKE_GENERATOR}` and `${CMAKE_CXX_COMPILER}`,
  then build it; guarded by `if(NOT DEFINED AHOCORASICK_HAVE_CXX_MODULES)` and
  cached, so it costs one configure rather than every configure.
- On failure the probe's output goes to `probe.log` and the message references
  the path. Inlining it buries the two named causes under thirty lines of
  nested-configure chatter.
- The `FATAL_ERROR` text carries over verbatim, including the scikit-build-core
  compiler-mismatch paragraph at `CMakeLists.txt:20-25` — that prose is the most
  valuable thing in the guard. Extend it to name the generator as a second
  possible cause.

Add an `ahocorasick_warnings` INTERFACE library holding `-Wall -Wextra` / `/W4`,
linked `PRIVATE` by `aho_corasick` and `_core`. Delete the two duplicated blocks
at `CMakeLists.txt:153-155` and `222-224`. Leave both `--exclude-libs,ALL`
blocks exactly where they are.

**Success Criteria**
- `try_compile` covers compiler and generator in one predicate; no hardcoded
  compiler IDs or version numbers remain in `CMakeLists.txt`.
- Configuring with the Unix Makefiles generator fails at configure time with the
  project's own message, not CMake's.
- `nm -D --defined-only` on the built `_core` still reports exactly 1 symbol.

**Tests**
- `rejects-old-compiler` CI job passes **unmodified** — it greps for
  `"C++20 module"` and `"g++-13"`, and both still appear (the compiler path in
  the message contains the latter).
- New CI step: configure with `-G "Unix Makefiles"`, assert it fails and the
  message names the generator.
- `minimum-cmake` job passes — the probe uses nothing newer than the pinned floor.

**Status**: Complete

Verified: guard accepts g++-14/Ninja and rejects g++-13/Ninja, g++-14/Makefiles,
and g++-13/Makefiles; both `rejects-old-compiler` greps pass unmodified; new
`rejects-bad-generator` job's three greps pass; 23 passed / 13 skipped; `_core`
exports 1 dynamic symbol; shared build leaks 0 private internals; `-Wall` present
on all three translation units.

---

## Stage 2: One version root

**Goal**: `project(VERSION)` is the single source of truth, so the standalone C++
package has a real version in a build where Python is absent by design.

- `project(ahocorasick_demo VERSION 0.1.0 ...)` becomes the root. Delete the
  `SKBUILD_PROJECT_VERSION` fallback at `CMakeLists.txt:13-15`.
- `AHOCORASICK_DEMO_VERSION` is defined from `PROJECT_VERSION`.
- `pyproject.toml` switches to `dynamic = ["version"]` with
  `[tool.scikit-build.metadata.version]` using the
  `scikit_build_core.metadata.regex` provider, `input = "CMakeLists.txt"`.
- README: the *Version* row in **Decisions, and why** reverses direction to
  `CMakeLists.txt → pyproject.toml`, and gains the reason (the C++ package must
  have a version without Python).

**Success Criteria**
- `SOVERSION` on `libaho_corasick.so` and `ahocorasick_demo.__version__` cannot
  disagree — one edit site.
- Bumping only `project(VERSION)` changes both.

**Tests**
- `tests/test_package_layout.py:22` — rename
  `test_version_comes_from_pyproject_via_cmake` to reflect the reversed
  direction; assertion value is unchanged at `0.1.0`.
- New: assert `ahocorasick_demo.__version__` equals the version parsed from
  `CMakeLists.txt`, so the regex provider is pinned by a test rather than
  trusted.

**Risk**: the exact `scikit-build-core` metadata-provider key names need
confirming against the installed version's documentation before writing them —
verify first, don't copy from memory.

**Status**: Not Started

---

## Stage 3: C++ unit tests

**Goal**: The algorithm module's interface becomes the test surface, and the
shared-library export path gets a consumer that links it.

- `option(AHOCORASICK_BUILD_TESTS ...)`, default ON, computed OFF under `SKBUILD`
  — mirroring `AHOCORASICK_BUILD_PYTHON_MODULE` at `CMakeLists.txt:42-48`.
- Catch2 v3 via `FetchContent_Declare(... FIND_PACKAGE_ARGS)` so a system copy is
  used when present and the network is a fallback.
- `include(CTest)`; one test executable linking `ahocorasick::core` and
  `ahocorasick_warnings`, registered with `catch_discover_tests`.
- **Placement**: this section goes *before* the Python section, so the `return()`
  seam at `CMakeLists.txt:178` survives as the last guard in the file.
- C++ sources live in `tests/` beside the pytest files — `tests/test_matcher.cpp`
  next to `tests/test_matcher.py`. pytest collects only `test_*.py`, so no
  configuration changes.

Tests to write, the four unreachable behaviours first:

1. `Offsets::Characters` over invalid UTF-8 — positions are meaningless but the
   call is well-defined, as claimed at `aho_corasick.cppm:83`.
2. `Offsets::Bytes` over multibyte text — the cross combination the bindings
   hardwire away.
3. `Pattern::chars` versus `Pattern::text.size()` for multibyte patterns — the
   bindings drop this field entirely.
4. The constructor throws `std::invalid_argument` as a type, not a translated
   `ValueError`.
5. `PatternMatcher` copy and move semantics — nanobind placement-news it and
   holds it by pointer, so these are never exercised today.

Then move from `tests/test_matcher.py`: `test_finds_overlapping_and_nested_matches`,
`test_counts_overlapping_occurrences`, `test_matches_stops_at_the_first_hit`,
`test_len_and_state_count`, `test_no_patterns_matches_nothing`,
`test_repeated_patterns_are_reported_separately`.

`tests/test_matcher.py` keeps the Hypothesis oracle block, the str/bytes overload
tests, offsets-follow-input-type, pickle, and the non-UTF-8 pattern test — all
genuinely integration-level.

**CI split** (both claims currently share one job):

- `standalone-library` — passes `-DAHOCORASICK_BUILD_TESTS=OFF` explicitly, and
  asserts no network is needed to configure. Proves the dependency-free claim in
  a form that can actually regress.
- `cpp-tests` (new) — tests ON, runs `ctest` twice: default, then
  `-DBUILD_SHARED_LIBS=ON`.

**Success Criteria**
- A missing `AHOCORASICK_EXPORT` on any exported member fails `cpp-tests` at link
  time in the shared configuration.
- The C++ suite runs with no Python and no nanobind present.
- Python test count drops by exactly the six moved tests; no coverage is lost.

**Tests**
- `ctest` green in both link modes.
- Deliberate-break check during development: strip `AHOCORASICK_EXPORT` from one
  member, confirm the shared build fails to link and the static build still
  passes. Revert.

**Status**: Not Started

---

## Stage 4: Install and export

**Goal**: The algorithm/bindings seam gets its second adapter — the library is
consumable via `find_package`, not only by vendoring the tree.

- `AHOCORASICK_INSTALL` option, default OFF under `SKBUILD` — the wheel must not
  ship the core library.
- The generated export header moves from `PRIVATE` (`CMakeLists.txt:129`) to
  `PUBLIC` with `BUILD_INTERFACE`/`INSTALL_INTERFACE`, and is installed.
  **This is the price of the stage**: `CMakeLists.txt:109` and the matching
  README claim ("No include directory for consumers") are no longer true and must
  be rewritten. A consumer recompiles the interface unit, so it needs the header.
- `install(TARGETS aho_corasick EXPORT ahocorasickTargets FILE_SET CXX_MODULES ...)`
  — install module **sources**, not BMIs. BMIs are compiler-, version- and
  flag-specific; `CXX_MODULES_BMI` is for controlled in-tree use, not distribution.
- `install(EXPORT ahocorasickTargets NAMESPACE ahocorasick:: CXX_MODULES_DIRECTORY cxx-modules ...)`
  — without `CXX_MODULES_DIRECTORY` the exported target cannot be imported at all.
- `ahocorasickConfig.cmake.in` plus `write_basic_package_version_file`, using
  `GNUInstallDirs` for destinations.

**Success Criteria**
- A separate project consumes the install tree with
  `find_package(ahocorasick 0.1 REQUIRED)` and `import aho_corasick;`, in both
  link modes.
- Version comparison in `find_package` works — which is what stage 2 was for.
- Building a wheel does not install the core library.

**Tests**
- New CI job: install to a prefix, then configure and build a minimal consumer
  against it, static and shared. This is the test that proves the second adapter
  exists rather than being asserted.
- Existing wheel tests unchanged; add an assertion that the wheel contains no
  `libaho_corasick` artefact.

**Risk**: consumers need a module-capable compiler too, since they recompile the
interface unit. Document it in the README's toolchain table — it is a real
constraint on who can use the installed package.

**Status**: Not Started

---

## Not doing

**Splitting `CMakeLists.txt` into subdirectories.** Reconsidered once the file
was projected to ~340 lines and rejected again. It moves complexity rather than
concentrating it, and costs the read-top-to-bottom property that is the
template's actual deliverable. The file is long because roughly 60% of it is
prose, and that prose is the point.

One objection raised against the split during review was wrong and should not be
reused: directory-scope variables *are* inherited by subdirectories, so
`add_subdirectory` would not have broken the visibility defaults at
`CMakeLists.txt:73-74`.

**A `friend` seam into the automaton's internals.** `step`, `child`,
`build_failure_links` and `scan` stay private. Reaching them requires a `friend`
declaration in `aho_corasick.cppm` — the file whose selling point is that it
exposes nothing, and the file that rebuilds every importer when edited. Their
correctness is pinned through the public surface by the oracle property tests.
