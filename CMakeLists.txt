cmake_minimum_required(VERSION 3.15...3.30)
project(ahocorasick_demo LANGUAGES CXX VERSION ${SKBUILD_PROJECT_VERSION})

# `Development.Module` is the component nanobind needs; it was named `Development`
# before CMake 3.18. Dead code on any modern CMake, kept so the template travels.
if(CMAKE_VERSION VERSION_LESS 3.18)
    set(DEV_MODULE Development)
else()
    set(DEV_MODULE Development.Module)
endif()
find_package(Python 3.9 COMPONENTS Interpreter ${DEV_MODULE} REQUIRED)

# Bindings compiled without optimisation are slow and fat. scikit-build-core does
# not set a build type, so pick one.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# nanobind arrives as a build-time pip dependency (see pyproject.toml). Ask the
# interpreter where its CMake config landed rather than guessing a path.
execute_process(
    COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE nanobind_ROOT)
find_package(nanobind CONFIG REQUIRED)

# --- The algorithm, with no idea Python exists -------------------------------
# A separate target on purpose. It never sees a nanobind header, so it can be
# unit-tested with a C++ framework, linked into a non-Python program, or handed
# to someone who does not care about bindings at all. Keeping this boundary
# honest is easier when the build enforces it.
add_library(aho_corasick STATIC src/aho_corasick.cpp)
target_include_directories(aho_corasick PUBLIC src)
target_compile_features(aho_corasick PUBLIC cxx_std_20)

# Required because this static library is linked into a shared module.
set_property(TARGET aho_corasick PROPERTY POSITION_INDEPENDENT_CODE ON)

# The algorithm is the hot path, so optimise it for speed. This is exactly the
# split nanobind's NOMINSIZE note describes: real work belongs in a library built
# for throughput, while the binding shim below stays size-optimised because no
# measurable time is spent in it.
target_compile_options(aho_corasick PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O3 -Wall -Wextra>
    $<$<CXX_COMPILER_ID:MSVC>:/O2 /W4>)

# --- The binding layer -------------------------------------------------------
# FREE_THREADED: the module keeps no mutable global state, so it does not need
#   the GIL and says so on Python 3.13+ free-threaded builds. Inert on 3.12 and
#   below. A PatternMatcher is immutable after construction, so sharing one
#   across threads is safe; see README.
# NOMINSIZE is deliberately not set. nanobind's default -Os applies only to
#   bindings.cpp, where nothing measurable happens — the work is in the library
#   above, built with -O3.
# STABLE_ABI would yield one wheel for all of Python 3.12+ at some performance
#   cost. Not used: this template does not build multi-version wheels.
nanobind_add_module(_core FREE_THREADED src/bindings.cpp)
target_link_libraries(_core PRIVATE aho_corasick)
target_compile_definitions(_core PRIVATE
                           AHOCORASICK_DEMO_VERSION="${SKBUILD_PROJECT_VERSION}")

if(NOT MSVC)
    target_compile_options(_core PRIVATE -Wall -Wextra)
endif()

# --- Seam: a real third-party dependency -------------------------------------
# The automaton is hand-rolled so the C++ stays readable. Swapping in a library
# would look like:
#
#   find_package(SomeLib REQUIRED)
#   target_link_libraries(aho_corasick PRIVATE SomeLib::SomeLib)
#
# Note it attaches to the algorithm target, not the module — another reason to
# keep the two separate.
# -----------------------------------------------------------------------------

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
