cmake_minimum_required(VERSION 3.15...3.30)
project(bloomdemo LANGUAGES CXX VERSION ${SKBUILD_PROJECT_VERSION})

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

# --- Seam: a real third-party dependency -------------------------------------
# The filter is hand-rolled so the C++ stays readable. Boost gained a dedicated
# Bloom filter in 1.89; swapping it in would look like:
#
#   find_package(Boost 1.89 REQUIRED)
#   target_link_libraries(_core PRIVATE Boost::bloom)
#
# The cost is provisioning Boost on every build machine, which is why it is a
# comment and not a dependency.
# -----------------------------------------------------------------------------

# FREE_THREADED: the module keeps no mutable global state, so it does not need the
#   GIL and says so on Python 3.13+ free-threaded builds. Inert on 3.12 and below.
#   Individual filters are not internally synchronised — share them under a lock,
#   exactly as you would a dict.
# NOMINSIZE would build with -O3 instead of nanobind's default -Os. Not used: the
#   work here happens behind the binding layer, so -Os costs nothing measurable.
# STABLE_ABI would yield one wheel for all of Python 3.12+ at some performance
#   cost. Not used: this template does not build multi-version wheels.
nanobind_add_module(
    _core
    FREE_THREADED
    src/bloom.cpp
)

target_compile_features(_core PRIVATE cxx_std_20)
target_compile_definitions(_core PRIVATE BLOOMDEMO_VERSION="${SKBUILD_PROJECT_VERSION}")

# Warnings on our sources only. nanobind's template machinery is noisy, and a
# template that trains you to ignore warnings is worse than one with none.
if(NOT MSVC)
    target_compile_options(_core PRIVATE -Wall -Wextra)
endif()

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

install(TARGETS _core LIBRARY DESTINATION bloomdemo)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/_core.pyi"
              "${CMAKE_CURRENT_BINARY_DIR}/py.typed"
        DESTINATION bloomdemo)
