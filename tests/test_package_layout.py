"""The packaging is half the point of this template, so it gets tested too."""

import importlib.metadata
import pathlib
import re

import ahocorasick_demo
from ahocorasick_demo import _core

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent


def test_no_importable_package_at_the_repo_root():
    # The trap the src/ layout exists to prevent: with a top-level ahocorasick_demo/
    # directory, running pytest from the repo root imports the source tree by
    # accident of cwd, with no compiled extension and no generated stub — and
    # packaging bugs stay invisible because the tests never touch the install.
    assert not (REPO_ROOT / "ahocorasick_demo").exists()
    assert (REPO_ROOT / "src" / "ahocorasick_demo" / "__init__.py").is_file()


def test_version_comes_from_cmake_via_pyproject():
    # CMakeLists.txt -> scikit-build-core's regex provider -> wheel metadata, and
    # CMakeLists.txt -> AHOCORASICK_DEMO_VERSION -> C++. The declaration lives in
    # CMake because the library is installable on its own, and a package that
    # builds without Python still needs a version for SOVERSION and find_package.
    #
    # Compared against the parsed value rather than a literal: a hardcoded "0.1.0"
    # here would still pass if the regex provider silently stopped matching and
    # both numbers were bumped independently, which is the drift this arrangement
    # exists to prevent.
    cmake_text = (REPO_ROOT / "CMakeLists.txt").read_text()
    declared = re.search(r"project\([^)]*VERSION\s+([0-9.]+)", cmake_text)

    assert declared, "no project(... VERSION ...) found; the regex provider would fail too"

    # The C++ route: CMakeLists.txt -> AHOCORASICK_DEMO_VERSION -> _core.
    assert ahocorasick_demo.__version__ == declared.group(1)

    # The packaging route: CMakeLists.txt -> scikit-build-core's regex provider ->
    # wheel metadata. Checked separately because it shares no machinery with the
    # line above — the C++ define would still be right if the regex stopped
    # matching, and the wheel would silently take a different version.
    assert importlib.metadata.version("ahocorasick-demo") == declared.group(1)


def test_extension_is_a_compiled_module():
    assert _core.__file__.endswith((".so", ".pyd"))


def test_generated_stub_and_py_typed_ship_beside_the_extension():
    # Anchored to _core, not to ahocorasick_demo.__init__: an editable install redirects
    # pure-Python files to src/ while compiled artifacts stay in the build tree.
    # In a real wheel both land in the same directory.
    extension_dir = pathlib.Path(_core.__file__).resolve().parent

    assert (extension_dir / "py.typed").is_file(), "type checkers ignore stubs without it"

    stub = extension_dir / "_core.pyi"
    assert stub.is_file()
    assert "class PatternMatcher" in stub.read_text()
