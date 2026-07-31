# 3.28 is where FILE_SET CXX_MODULES arrived, and the algorithm is a C++20
# module, so that is the floor. Pinned in CI: a floor you never configure against
# is a guess, not a guarantee.
#
# Module scanning also requires a compiler that can report its own import graph.
# That means GCC 14, Clang 16, or MSVC 19.34 and up, and either the Ninja or the
# Visual Studio generator. There is no fallback path; see README.
cmake_minimum_required(VERSION 3.28...3.31)

# The single source of truth for the version, in both directions: it reaches
# Python as wheel metadata (scikit-build-core reads this line; see the
# tool.scikit-build.metadata table in pyproject.toml) and C++ as
# AHOCORASICK_DEMO_VERSION below.
#
# It lives here rather than in pyproject.toml because the C++ library must have a
# version in a build where Python is absent by design. Reading it from packaging
# metadata would mean SOVERSION and the installed CMake package version were
# correct only when a Python file happened to be present and parseable.
project(ahocorasick_demo VERSION 0.1.0 LANGUAGES CXX)

# Can this toolchain actually build a C++20 module? Asked by building one, rather
# than by testing the compiler against a hardcoded table of versions. A table has
# to be maintained, says nothing about the generator, and is wrong the moment a
# compiler starts or stops supporting modules.
#
# The probe is a nested `cmake` run rather than try_compile(). try_compile() is
# the obvious spelling and does not work here: when the toolchain cannot scan for
# modules the failure happens while *generating* the test project, which
# try_compile() reports as a hard CMake error. Configuration then stops inside the
# probe, and the message below — the entire reason the check exists — never runs.
# execute_process() captures the exit status instead of propagating it.
#
# Both the compiler and the generator are covered, because the probe is
# configured with this build's own ${CMAKE_GENERATOR}.
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

# Fail with something actionable. CMake's own diagnostic says only that the
# compiler cannot report its import graph, which sends people looking for a CMake
# problem. Two causes are more likely, and neither is obvious from that text.
#
# The generator: only Ninja and Visual Studio scan for modules, and CMake's
# default on Unix is Makefiles, so `cmake -B build` with no -G fails on a
# perfectly capable compiler.
#
# The compiler: scikit-build-core takes it from the interpreter's own build
# configuration, so `pip install` can pick a different and older one than `cmake`
# does on the same machine. On Debian and Ubuntu that is the triplet-prefixed
# name, which update-alternatives leaves alone when you switch the default.
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
            # Referenced rather than inlined: the probe's own output is thirty
            # lines of nested-configure chatter around one useful sentence, and
            # pasting it here buries the two causes named above.
            "Full probe log: ${_ahocorasick_probe_log}")
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

# Every target this project compiles wants the same warnings, so they are stated
# once and linked in rather than repeated per target. Named for what it holds:
# the other per-target flags below are deliberately *not* uniform, and hiding
# them behind a general-sounding "options" target would make a per-target
# decision look like a shared one.
add_library(ahocorasick_warnings INTERFACE)
target_compile_options(ahocorasick_warnings INTERFACE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>)

# Included here rather than beside the install rules below, because
# CMAKE_INSTALL_INCLUDEDIR is referenced by the target's own usage requirements.
include(GNUInstallDirs)

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

# Consumers reach this library by importing it, never by including a header.
# There is one include directory below all the same, and it is not a consumer
# header: it holds the generated export macro, which the interface unit itself
# includes. See the comment there for why an installed consumer needs it anyway.
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

# PUBLIC, not PRIVATE, and the reason is worth spelling out because the obvious
# reading is wrong. Consumers still never include this header themselves - they
# reach the library by importing it. But an installed C++20 module ships its
# interface unit as *source*, and the consumer compiles it, because a BMI is
# specific to one compiler, version and flag set and cannot be distributed. That
# compilation runs the .cppm's global module fragment, which includes this
# header. So it has to be on the consumer's include path even though no consumer
# ever writes the #include.
target_include_directories(aho_corasick
                           PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
                                  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# In a static library the macro must expand to nothing; the generated header
# keys that off AHOCORASICK_STATIC_DEFINE. Reading the target's own TYPE rather
# than re-testing BUILD_SHARED_LIBS keeps this correct however the type was
# decided above.
get_target_property(_ahocorasick_type aho_corasick TYPE)
if(_ahocorasick_type STREQUAL "STATIC_LIBRARY")
    # PUBLIC for the same reason the include directory above is: a consumer
    # compiles the interface unit itself, so it needs the macro in the same state
    # this build used. Getting it wrong is inert on ELF but produces a dllimport
    # against a static archive on MSVC.
    target_compile_definitions(aho_corasick PUBLIC AHOCORASICK_STATIC_DEFINE)
else()
    # An unversioned .so is a packaging problem deferred, not avoided.
    set_target_properties(aho_corasick PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})
endif()

set_target_properties(aho_corasick PROPERTIES
    # Required when static: this library is linked into a shared module. Implied
    # when shared, and harmless to state either way.
    POSITION_INDEPENDENT_CODE ON

    # So an installed consumer spells the target exactly as an in-tree one does.
    # ALIAS targets are not exported, so without this the package would offer
    # ahocorasick::aho_corasick while this file uses ahocorasick::core, and the
    # two would drift in every example and error message.
    EXPORT_NAME core)

# Warnings only — optimisation belongs to CMAKE_BUILD_TYPE. Hardcoding -O3 here
# would be redundant in Release and would silently defeat a Debug build.
#
# BUILD_LOCAL_INTERFACE, not a plain PRIVATE link. A static library records even
# its PRIVATE link libraries in its interface, as $<LINK_ONLY:...>, so exporting
# this target would demand that the warnings target be exported too — putting an
# internal build detail into the installed package, where a consumer would see
# ahocorasick::ahocorasick_warnings and rightly wonder what it was for.
target_link_libraries(aho_corasick
                      PRIVATE $<BUILD_LOCAL_INTERFACE:ahocorasick_warnings>)

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

# --- Installing the library --------------------------------------------------
# Without this the library is reusable only by vendoring the repository and
# calling add_subdirectory. Installing gives the seam above its second consumer:
# a project that finds this one with find_package and imports the module.
#
# OFF under scikit-build-core, where the extension module is the only consumer
# and a copy of the algorithm inside the wheel would be dead weight.
if(DEFINED SKBUILD)
    set(_ahocorasick_install_default OFF)
else()
    set(_ahocorasick_install_default ON)
endif()
option(AHOCORASICK_INSTALL
       "Generate install rules for the C++ library" ${_ahocorasick_install_default})

if(AHOCORASICK_INSTALL)
    include(CMakePackageConfigHelpers)

    # The module interface unit is installed as source. Deliberately no
    # CXX_MODULES_BMI: a BMI is tied to one compiler, one version and one set of
    # flags, so shipping one would produce a package that works only for the
    # machine that built it. The consumer compiles the interface instead, which
    # is why it needs the include directory and the static define above.
    install(TARGETS aho_corasick
            EXPORT ahocorasickTargets
            FILE_SET CXX_MODULES DESTINATION ${CMAKE_INSTALL_LIBDIR}/cxx/miu
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/generated/ahocorasick_export.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    # CXX_MODULES_DIRECTORY is not optional here. Without it the exported target
    # carries no module information at all, and a consumer's `import
    # aho_corasick;` fails to find anything to import.
    install(EXPORT ahocorasickTargets
            NAMESPACE ahocorasick::
            CXX_MODULES_DIRECTORY cxx-modules
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ahocorasick)

    # SameMajorVersion rather than exact: the SOVERSION set above already says
    # that a major bump is the breaking one, so the package version check should
    # agree with the ABI promise rather than contradict it.
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
# Placed above the Python section on purpose. Both sections are optional, and the
# early `return()` below only works for whichever comes last, so the section that
# can be guarded by a plain if() goes first and the return() stays the final word
# in the file.
#
# Defaults OFF under scikit-build-core - a wheel build has no business compiling
# a test framework - and ON otherwise, mirroring AHOCORASICK_BUILD_PYTHON_MODULE
# above. Note the consequence: with tests ON the configure step needs network the
# first time, which is why CI's standalone-library job passes OFF explicitly to
# keep proving that the algorithm builds with nothing installed.
if(DEFINED SKBUILD)
    set(_ahocorasick_tests_default OFF)
else()
    set(_ahocorasick_tests_default ON)
endif()
option(AHOCORASICK_BUILD_TESTS
       "Build the C++ unit tests" ${_ahocorasick_tests_default})

if(AHOCORASICK_BUILD_TESTS)
    # The project's first real third-party C++ dependency, and the one piece of
    # dependency acquisition this template otherwise never demonstrates. Pinned to
    # a tag rather than a branch: a test framework that changes under you turns a
    # green build red for reasons that have nothing to do with this code.
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(Catch2)

    enable_testing()

    # Reaches the algorithm the same way bindings.cpp does - `import
    # aho_corasick;` - so this target is a genuine second consumer of the module,
    # not a re-compilation of its sources. In the shared configuration it is also
    # the only thing that links the library, which is what makes a missing
    # AHOCORASICK_EXPORT a build failure rather than a downstream surprise.
    add_executable(ahocorasick_tests tests/test_matcher.cpp)
    target_link_libraries(ahocorasick_tests
                          PRIVATE ahocorasick::core
                                  Catch2::Catch2WithMain
                                  ahocorasick_warnings)

    # catch_discover_tests registers each TEST_CASE with CTest individually, so a
    # failure names the case rather than just the executable.
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    catch_discover_tests(ahocorasick_tests)
endif()

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
                           AHOCORASICK_DEMO_VERSION="${PROJECT_VERSION}")

target_link_libraries(_core PRIVATE ahocorasick_warnings)

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
