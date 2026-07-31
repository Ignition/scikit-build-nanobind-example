# 3.28 is where FILE_SET CXX_MODULES arrived. There is no header-based fallback.
cmake_minimum_required(VERSION 3.28...3.31)

# The single source of truth for the version; pyproject.toml parses this line
# back out. It lives here because SOVERSION and the exported package version must
# be right in a build where Python is absent by design.
project(ahocorasick_demo VERSION 0.1.0 LANGUAGES CXX)

# Build a module to find out whether this toolchain can, rather than comparing
# the compiler against a table of versions that says nothing about the generator.
# Using this build's own generator covers both.
#
# try_compile() is the obvious spelling and does not work: an unsupported
# toolchain fails while *generating* the test project, which try_compile() raises
# as a hard error, so configuration stops before the message below can run.
# execute_process() captures the exit status instead of propagating it.
if(NOT DEFINED AHOCORASICK_HAVE_CXX_MODULES)
    set(_ahocorasick_probe ${CMAKE_CURRENT_BINARY_DIR}/cmake-probe)
    file(WRITE ${_ahocorasick_probe}/CMakeLists.txt
"cmake_minimum_required(VERSION 3.28)\n\
project(ahocorasick_probe LANGUAGES CXX)\n\
add_executable(probe main.cpp)\n\
target_sources(probe PRIVATE FILE_SET CXX_MODULES\n\
               BASE_DIRS \${CMAKE_CURRENT_SOURCE_DIR} FILES probe.cppm)\n\
target_compile_features(probe PRIVATE cxx_std_20)\n")
    file(WRITE ${_ahocorasick_probe}/probe.cppm
         "export module ahocorasick_probe;\nexport int probe() { return 0; }\n")
    file(WRITE ${_ahocorasick_probe}/main.cpp
         "import ahocorasick_probe;\nint main() { return probe(); }\n")

    execute_process(
        COMMAND ${CMAKE_COMMAND} -S ${_ahocorasick_probe}
                                 -B ${_ahocorasick_probe}/build
                                 -G "${CMAKE_GENERATOR}"
                                 -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                                 -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
        RESULT_VARIABLE _ahocorasick_probe_configured
        OUTPUT_VARIABLE _ahocorasick_probe_output
        ERROR_VARIABLE _ahocorasick_probe_output)
    if(_ahocorasick_probe_configured EQUAL 0)
        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${_ahocorasick_probe}/build
            RESULT_VARIABLE _ahocorasick_probe_built
            OUTPUT_VARIABLE _ahocorasick_probe_output
            ERROR_VARIABLE _ahocorasick_probe_output)
    else()
        set(_ahocorasick_probe_built 1)
    endif()

    if(_ahocorasick_probe_configured EQUAL 0 AND _ahocorasick_probe_built EQUAL 0)
        set(AHOCORASICK_HAVE_CXX_MODULES ON CACHE INTERNAL
            "Toolchain can build a C++20 module")
    else()
        set(AHOCORASICK_HAVE_CXX_MODULES OFF CACHE INTERNAL
            "Toolchain can build a C++20 module")
        file(WRITE ${_ahocorasick_probe}/probe.log "${_ahocorasick_probe_output}")
        set(_ahocorasick_probe_log ${_ahocorasick_probe}/probe.log
            CACHE INTERNAL "Where the module probe recorded its output")
    endif()
endif()

# CMake's own diagnostic names neither likely cause. The generator, because
# Makefiles is the default on Unix and does not scan. Or the compiler, because
# scikit-build-core takes it from the interpreter's build configuration, which on
# Debian and Ubuntu is a triplet-prefixed name that update-alternatives leaves
# alone - so `pip install` and `cmake` can disagree on the same machine.
if(NOT AHOCORASICK_HAVE_CXX_MODULES)
    message(FATAL_ERROR
            "The algorithm is a C++20 module, and this toolchain cannot build "
            "one.\n"
            "Configuring with ${CMAKE_CXX_COMPILER_ID} "
            "${CMAKE_CXX_COMPILER_VERSION} at ${CMAKE_CXX_COMPILER}, "
            "generator ${CMAKE_GENERATOR}.\n"
            "Modules need a compiler that can report its import graph (GCC 14, "
            "Clang 16, or MSVC 19.34 and up) and either the Ninja or the Visual "
            "Studio generator.\n"
            "If the generator is wrong, configure again with -G Ninja. If that "
            "compiler path is not the one you expect, set CXX and configure "
            "again, for example CXX=g++-14 pip install -e .\n"
            # Referenced, not inlined: the probe's output is mostly
            # nested-configure chatter and would bury the causes named above.
            "Full probe log: ${_ahocorasick_probe_log}")
endif()

# Nothing below touches find_package(Python) unless this is set, so "builds
# without Python" is enforced rather than merely intended.
if(DEFINED SKBUILD)
    set(_ahocorasick_python_default ON)
else()
    set(_ahocorasick_python_default OFF)
endif()
option(AHOCORASICK_BUILD_PYTHON_MODULE
       "Build the nanobind extension module" ${_ahocorasick_python_default})

# scikit-build-core does not set one, and an unoptimised binding layer is slow.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# So clangd and clang-tidy see the real flags.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The default is gnu++20, which lets extensions creep in and fail only on MSVC.
set(CMAKE_CXX_EXTENSIONS OFF)

# Directory-wide rather than per-target, so a target added later cannot start
# from `default`. Without VISIBILITY_INLINES_HIDDEN, inline members stay exported
# and most of the symbol table survives anyway. Both inert on MSVC.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Named for what it holds, not "options": the other per-target flags below are
# deliberately not uniform, and a general name would hide that.
add_library(ahocorasick_warnings INTERFACE)
target_compile_options(ahocorasick_warnings INTERFACE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>)

# Early, because CMAKE_INSTALL_INCLUDEDIR is used in the target's own usage
# requirements below.
include(GNUInstallDirs)

# --- The algorithm, with no idea Python exists -------------------------------
# A separate target so the boundary is enforced by the build rather than by
# convention: it never sees a nanobind header.
#
# The type is BUILD_SHARED_LIBS' to choose, except under scikit-build-core: a
# shared core in a wheel would need an $ORIGIN RPATH to find at import time and
# buys nothing, since the extension module is the only consumer there.
if(DEFINED SKBUILD)
    set(_ahocorasick_library_type STATIC)
else()
    set(_ahocorasick_library_type "")  # empty: defer to BUILD_SHARED_LIBS
endif()
add_library(aho_corasick ${_ahocorasick_library_type})
add_library(ahocorasick::core ALIAS aho_corasick)

# Only the interface unit belongs in the file set - that is what makes CMake scan
# it and order importers after it. The implementation unit is an ordinary source:
# still scanned and ordered, but it contributes nothing an importer can see.
target_sources(aho_corasick
               PRIVATE src/aho_corasick.cpp
               PUBLIC FILE_SET CXX_MODULES
                      BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src
                      FILES src/aho_corasick.cppm)

target_compile_features(aho_corasick PUBLIC cxx_std_20)

# `export` on a module declaration and `visibility("default")` are unrelated
# despite the shared word: the first says what an importer may name, the second
# what reaches the dynamic symbol table. Hidden visibility plus a shared library
# therefore links cleanly here and fails in the *consumer*. AHOCORASICK_EXPORT,
# applied in the .cppm, reconciles them.
#
# The .cppm includes the generated header rather than taking a command-line
# define, because macros do not cross a module boundary.
include(GenerateExportHeader)
generate_export_header(aho_corasick
    BASE_NAME AHOCORASICK
    EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/generated/ahocorasick_export.h)

# PUBLIC despite no consumer ever writing this #include. An installed module
# ships its interface unit as source and the consumer compiles it, which runs the
# global module fragment - so the header must be on the consumer's include path.
target_include_directories(aho_corasick
                           PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
                                  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# The macro must expand to nothing in a static library. Reading the target's own
# TYPE stays correct however the type was decided above.
get_target_property(_ahocorasick_type aho_corasick TYPE)
if(_ahocorasick_type STREQUAL "STATIC_LIBRARY")
    # PUBLIC for the same reason as the include directory: the consumer compiles
    # the interface unit and needs the macro in the state this build used. Inert
    # on ELF, but a dllimport against a static archive on MSVC.
    target_compile_definitions(aho_corasick PUBLIC AHOCORASICK_STATIC_DEFINE)
else()
    # An unversioned .so is a packaging problem deferred, not avoided.
    set_target_properties(aho_corasick PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})
endif()

set_target_properties(aho_corasick PROPERTIES
    # Required when static, since this is linked into a shared module.
    POSITION_INDEPENDENT_CODE ON

    # ALIAS targets are not exported, so without this the installed package would
    # offer ahocorasick::aho_corasick while this file uses ahocorasick::core.
    EXPORT_NAME core)

# Warnings only: hardcoding -O3 would be redundant in Release and would defeat a
# Debug build.
#
# BUILD_LOCAL_INTERFACE, because a static library records even PRIVATE link
# libraries as $<LINK_ONLY:...>, and exporting this target would then demand the
# warnings target be exported into the installed package too.
target_link_libraries(aho_corasick
                      PRIVATE $<BUILD_LOCAL_INTERFACE:ahocorasick_warnings>)

# --- Seam: a real third-party dependency -------------------------------------
# The automaton is hand-rolled, so there is no such dependency yet. One would
# attach here, to the algorithm rather than to the bindings:
#
#   find_package(SomeLib REQUIRED)
#   target_link_libraries(aho_corasick PRIVATE SomeLib::SomeLib)
#
# The visibility settings above cannot reach it: compiled elsewhere with default
# visibility, its archive re-exports its whole symbol table from whichever shared
# object absorbs it - two wheels in one process then export rival versions of the
# same symbols. --exclude-libs is the link-side backstop, and belongs on both
# targets because which one absorbs the archive depends on the library type.
target_link_options(aho_corasick PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:LINKER:--exclude-libs,ALL>)
# -----------------------------------------------------------------------------

# --- Installing the library --------------------------------------------------
# Without this, reuse means vendoring the repository. OFF under scikit-build-core,
# where a copy of the algorithm inside the wheel would be dead weight.
if(DEFINED SKBUILD)
    set(_ahocorasick_install_default OFF)
else()
    set(_ahocorasick_install_default ON)
endif()
option(AHOCORASICK_INSTALL
       "Generate install rules for the C++ library" ${_ahocorasick_install_default})

if(AHOCORASICK_INSTALL)
    include(CMakePackageConfigHelpers)

    # Source, and deliberately no CXX_MODULES_BMI: a BMI is tied to one compiler,
    # version and flag set, so shipping one yields a package that works only on
    # the machine that built it.
    install(TARGETS aho_corasick
            EXPORT ahocorasickTargets
            FILE_SET CXX_MODULES DESTINATION ${CMAKE_INSTALL_LIBDIR}/cxx/miu
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/generated/ahocorasick_export.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    # Without CXX_MODULES_DIRECTORY the exported target carries no module
    # information and a consumer's `import` finds nothing.
    install(EXPORT ahocorasickTargets
            NAMESPACE ahocorasick::
            CXX_MODULES_DIRECTORY cxx-modules
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ahocorasick)

    # SameMajorVersion, to agree with the SOVERSION promise above rather than
    # contradict it.
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/ahocorasickConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ahocorasickConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/ahocorasickConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ahocorasick)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ahocorasickConfig.cmake
                  ${CMAKE_CURRENT_BINARY_DIR}/ahocorasickConfigVersion.cmake
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ahocorasick)
endif()

# --- The C++ tests -----------------------------------------------------------
# Above the Python section deliberately: both are optional and the early return()
# below only works for whichever comes last.
#
# With tests ON the first configure needs network, to fetch Catch2. That is why
# turning them off is the documented way to build with nothing installed.
if(DEFINED SKBUILD)
    set(_ahocorasick_tests_default OFF)
else()
    set(_ahocorasick_tests_default ON)
endif()
option(AHOCORASICK_BUILD_TESTS
       "Build the C++ unit tests" ${_ahocorasick_tests_default})

if(AHOCORASICK_BUILD_TESTS)
    # Pinned to a tag: a framework that moves under you turns the build red for
    # reasons unrelated to this code.
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(Catch2)

    enable_testing()

    # Reaches the algorithm by importing it, as bindings.cpp does, so this is a
    # real second consumer rather than a recompilation of its sources. In the
    # shared build it is the only thing that links the library, which is what
    # turns a missing AHOCORASICK_EXPORT into a failure here.
    add_executable(ahocorasick_tests tests/test_matcher.cpp)
    target_link_libraries(ahocorasick_tests
                          PRIVATE ahocorasick::core
                                  Catch2::Catch2WithMain
                                  ahocorasick_warnings)

    # Registers each TEST_CASE with CTest separately, so a failure names the case.
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(ahocorasick_tests)
endif()

if(NOT AHOCORASICK_BUILD_PYTHON_MODULE)
    return()
endif()

# --- Everything below is Python-specific -------------------------------------

find_package(Python 3.9 COMPONENTS Interpreter Development.Module REQUIRED)

# nanobind is a build-time pip dependency; ask the interpreter where its CMake
# config landed rather than guessing.
execute_process(
    COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    OUTPUT_VARIABLE nanobind_ROOT
    RESULT_VARIABLE nanobind_probe_result
    ERROR_VARIABLE nanobind_probe_error)
if(NOT nanobind_probe_result EQUAL 0)
    # Otherwise this surfaces as "could not find nanobind", which reads as a
    # CMake path problem rather than a missing pip package.
    message(FATAL_ERROR
            "Could not locate nanobind via '${Python_EXECUTABLE} -m nanobind --cmake_dir'.\n"
            "Install it into this interpreter with 'pip install nanobind'.\n"
            "Error was: ${nanobind_probe_error}")
endif()
find_package(nanobind CONFIG REQUIRED)

# FREE_THREADED: no mutable global state here, and a PatternMatcher is immutable
#   once built, so the module does not need the GIL. Inert before 3.13.
# NOMINSIZE: required, and not for the usual reason. nanobind otherwise compiles
#   this at -Os, where GCC 14 fails to emit the body of an always_inline standard
#   library function reached through the module's global module fragment. Every
#   other level works, so this defers to the build type.
# STABLE_ABI is unused: this template does not build multi-version wheels.
nanobind_add_module(_core FREE_THREADED NOMINSIZE src/bindings.cpp)
target_link_libraries(_core PRIVATE ahocorasick::core)
target_compile_definitions(_core PRIVATE
                           AHOCORASICK_DEMO_VERSION="${PROJECT_VERSION}")

target_link_libraries(_core PRIVATE ahocorasick_warnings)

# The target that absorbs the algorithm's archive in the static build, which is
# how wheels are built. See the seam comment above.
target_link_options(_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:LINKER:--exclude-libs,ALL>)

# Generated by importing the freshly built module, so it cannot drift from the
# bindings. It imports the .so on the build machine, so guard this if you ever
# cross-compile.
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
