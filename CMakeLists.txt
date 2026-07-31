# 3.28 is where FILE_SET CXX_MODULES arrived, and the algorithm is a C++20
# module, so that is the floor. Pinned in CI: a floor you never configure against
# is a guess, not a guarantee.
#
# Module scanning also requires a compiler that can report its own import graph.
# That means GCC 14, Clang 16, or MSVC 19.34 and up, and either the Ninja or the
# Visual Studio generator. There is no fallback path; see README.
cmake_minimum_required(VERSION 3.28...3.31)

# SKBUILD_PROJECT_VERSION is only defined when scikit-build-core drives the
# build. The whole point of splitting the algorithm out is that CMake can be run
# directly, so fall back rather than expanding to nothing.
if(NOT DEFINED SKBUILD_PROJECT_VERSION)
    set(SKBUILD_PROJECT_VERSION "0.0.0-standalone")
endif()

project(ahocorasick_demo VERSION 0.1.0 LANGUAGES CXX)

# Fail with something actionable. CMake's own diagnostic says only that the
# compiler cannot report its import graph, which sends people looking for a CMake
# problem. The usual cause is subtler: scikit-build-core takes the compiler from
# the interpreter's own build configuration, so `pip install` can pick a
# different and older compiler than `cmake` does on the same machine. On Debian
# and Ubuntu that is the triplet-prefixed name, which update-alternatives leaves
# alone when you switch the default.
set(_ahocorasick_min_compiler "GCC 14, Clang 16, or MSVC 19.34")
if((CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14) OR
   (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16) OR
   (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.34))
    message(FATAL_ERROR
            "The algorithm is a C++20 module, which needs a compiler that can "
            "report its import graph: ${_ahocorasick_min_compiler} or newer.\n"
            "Configuring with ${CMAKE_CXX_COMPILER_ID} "
            "${CMAKE_CXX_COMPILER_VERSION} at ${CMAKE_CXX_COMPILER}.\n"
            "If that path is not the compiler you expect, set CXX and configure "
            "again, for example CXX=g++-14 pip install -e .")
endif()

# Defaults ON under scikit-build-core and OFF for a bare `cmake -B build`, so the
# claim that the algorithm builds without Python is actually true rather than
# aspirational: nothing below touches find_package(Python) unless this is set.
if(DEFINED SKBUILD)
    set(_ahocorasick_python_default ON)
else()
    set(_ahocorasick_python_default OFF)
endif()
option(AHOCORASICK_BUILD_PYTHON_MODULE
       "Build the nanobind extension module" ${_ahocorasick_python_default})

# Bindings compiled without optimisation are slow and fat. scikit-build-core does
# not set a build type, so pick one.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# So clangd, clang-tidy, and IDEs can see the real flags. Costs nothing and makes
# the standalone library genuinely tool-able.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# gnu++20 rather than c++20 is the default, which lets GNU extensions creep in
# and only fail once someone tries MSVC.
set(CMAKE_CXX_EXTENSIONS OFF)

# Hidden by default, exported deliberately. Set as directory-wide defaults rather
# than per-target properties so a target added later cannot silently start from
# `default` visibility; both seed every target created after this point.
#
# VISIBILITY_INLINES_HIDDEN is not an optional extra. Without it inline member
# functions stay exported and most of the dynamic symbol table survives anyway.
# Both are inert on MSVC, which is opt-in-export to begin with.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# --- The algorithm, with no idea Python exists -------------------------------
# A separate target on purpose. It never sees a nanobind header, so it can be
# unit-tested with a C++ framework, linked into a non-Python program, or handed
# to someone who does not care about bindings at all. Keeping this boundary
# honest is easier when the build enforces it.
#
# The type is BUILD_SHARED_LIBS' to choose, which is the convention consumers
# already expect, except under scikit-build-core. A shared core inside a wheel
# would have to be installed next to the extension module and found again at
# import time through an $ORIGIN RPATH, and buys nothing: the module is the only
# consumer there. So the wheel is always statically linked, and the shared build
# exists for the "linked into a non-Python program" case above.
if(DEFINED SKBUILD)
    set(_ahocorasick_library_type STATIC)
else()
    set(_ahocorasick_library_type "")  # empty: defer to BUILD_SHARED_LIBS
endif()
add_library(aho_corasick ${_ahocorasick_library_type})
add_library(ahocorasick::core ALIAS aho_corasick)

# The module interface unit goes in a CXX_MODULES file set rather than being
# listed as an ordinary source. That is what lets CMake scan it, work out the
# import graph, and order the build so importers compile after it.
# Only the interface unit belongs in the file set. The implementation unit is an
# ordinary source: CMake still scans it and orders it after the interface, but it
# contributes nothing an importer can see.
target_sources(aho_corasick
               PRIVATE src/aho_corasick.cpp
               PUBLIC FILE_SET CXX_MODULES
                      BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src
                      FILES src/aho_corasick.cppm)

# No include directory for consumers: they reach this library by importing it,
# not by finding a header.
target_compile_features(aho_corasick PUBLIC cxx_std_20)

# `export` on a module declaration and `visibility("default")` on a symbol are
# unrelated despite the shared word. The first decides which names an importer is
# allowed to spell; the second decides what survives into the dynamic symbol
# table. Hidden visibility plus a shared library therefore compiles and links the
# library cleanly and then fails in the *consumer*, with undefined references to
# names the module plainly exports. AHOCORASICK_EXPORT, applied in the .cppm, is
# what reconciles the two.
#
# The generated header is included from the global module fragment, because
# macros do not cross a module boundary: defining this on the command line would
# leave the .cppm unable to see it.
include(GenerateExportHeader)
generate_export_header(aho_corasick
    BASE_NAME AHOCORASICK
    EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/generated/ahocorasick_export.h)

# PRIVATE: only the module interface unit includes this. Consumers import.
target_include_directories(aho_corasick
                           PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

# In a static library the macro must expand to nothing; the generated header
# keys that off AHOCORASICK_STATIC_DEFINE. Reading the target's own TYPE rather
# than re-testing BUILD_SHARED_LIBS keeps this correct however the type was
# decided above.
get_target_property(_ahocorasick_type aho_corasick TYPE)
if(_ahocorasick_type STREQUAL "STATIC_LIBRARY")
    target_compile_definitions(aho_corasick PRIVATE AHOCORASICK_STATIC_DEFINE)
else()
    # An unversioned .so is a packaging problem deferred, not avoided.
    set_target_properties(aho_corasick PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})
endif()

set_target_properties(aho_corasick PROPERTIES
    # Required when static: this library is linked into a shared module. Implied
    # when shared, and harmless to state either way.
    POSITION_INDEPENDENT_CODE ON)

# Warnings only — optimisation belongs to CMAKE_BUILD_TYPE. Hardcoding -O3 here
# would be redundant in Release and would silently defeat a Debug build.
target_compile_options(aho_corasick PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>)

# --- Seam: a real third-party dependency -------------------------------------
# The automaton is hand-rolled so the C++ stays readable. Swapping in a library
# would look like:
#
#   find_package(SomeLib REQUIRED)
#   target_link_libraries(aho_corasick PRIVATE SomeLib::SomeLib)
#
# Note it attaches to the algorithm target, not the module — another reason to
# keep the two separate.
#
# The visibility settings above cannot reach such a library: it was compiled
# elsewhere, most likely with default visibility, so linking its archive in
# re-exports its entire symbol table from whichever shared object absorbs it.
# That is the real interposition risk for a Python extension — two wheels in one
# process each exporting a different version of the same third-party symbols.
# --exclude-libs is the link-side backstop, and belongs on both targets because
# which one absorbs the archive depends on how the algorithm was built.
target_link_options(aho_corasick PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:LINKER:--exclude-libs,ALL>)
# -----------------------------------------------------------------------------

if(NOT AHOCORASICK_BUILD_PYTHON_MODULE)
    return()
endif()

# --- Everything below is Python-specific -------------------------------------

find_package(Python 3.9 COMPONENTS Interpreter Development.Module REQUIRED)

# nanobind arrives as a build-time pip dependency (see pyproject.toml). Ask the
# interpreter where its CMake config landed rather than guessing a path.
execute_process(
    COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    OUTPUT_VARIABLE nanobind_ROOT
    RESULT_VARIABLE nanobind_probe_result
    ERROR_VARIABLE nanobind_probe_error)
if(NOT nanobind_probe_result EQUAL 0)
    # Without this the failure surfaces as "could not find nanobind", which sends
    # people hunting for a CMake path problem instead of a missing pip package.
    message(FATAL_ERROR
            "Could not locate nanobind via '${Python_EXECUTABLE} -m nanobind --cmake_dir'.\n"
            "Install it into this interpreter with 'pip install nanobind'.\n"
            "Error was: ${nanobind_probe_error}")
endif()
find_package(nanobind CONFIG REQUIRED)

# FREE_THREADED: the module keeps no mutable global state, so it does not need
#   the GIL and says so on Python 3.13+ free-threaded builds. Inert on 3.12 and
#   below. A PatternMatcher is immutable after construction, so sharing one
#   across threads is safe; see README.
# NOMINSIZE is required, and not for the reason it usually is. nanobind
#   otherwise compiles this target at -Os, and GCC 14 cannot compile a translation
#   unit that imports a module at that optimisation level: it fails to emit the
#   body of an always_inline standard library function reached through the
#   module's global module fragment. Every other level is fine, so this asks for
#   the build type's own optimisation instead. Nothing here is hot enough for the
#   choice to matter on speed.
# STABLE_ABI would yield one wheel for all of Python 3.12+ at some performance
#   cost. Not used: this template does not build multi-version wheels.
nanobind_add_module(_core FREE_THREADED NOMINSIZE src/bindings.cpp)
target_link_libraries(_core PRIVATE ahocorasick::core)
target_compile_definitions(_core PRIVATE
                           AHOCORASICK_DEMO_VERSION="${SKBUILD_PROJECT_VERSION}")

target_compile_options(_core PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>)

# See the seam comment above. This is the target that absorbs the algorithm's
# archive in the static build, which is the configuration wheels are built in.
target_link_options(_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:LINKER:--exclude-libs,ALL>)

# Generated at build time by importing the freshly built module, so it can never
# drift from the bindings. Note: this imports the .so on the build machine, which
# does not work under cross-compilation — guard it if this template is ever used
# for cross-builds.
nanobind_add_stub(
    _core_stub
    MODULE _core
    OUTPUT _core.pyi
    PYTHON_PATH $<TARGET_FILE_DIR:_core>
    DEPENDS _core
    MARKER_FILE py.typed
)

install(TARGETS _core LIBRARY DESTINATION ahocorasick_demo)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/_core.pyi"
              "${CMAKE_CURRENT_BINARY_DIR}/py.typed"
        DESTINATION ahocorasick_demo)
