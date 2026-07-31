"""The packaging is half the point of this template, so it gets tested too."""

import importlib.metadata
import pathlib
import re

import ahocorasick_demo
from ahocorasick_demo import _core

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent


def test_no_importable_package_at_the_repo_root():
    # The trap src/ layout prevents: a top-level ahocorasick_demo/ would be
    # imported by accident of cwd, with no compiled extension and no stub, so
    # packaging bugs would stay invisible.
    assert not (REPO_ROOT / "ahocorasick_demo").exists()
    assert (REPO_ROOT / "src" / "ahocorasick_demo" / "__init__.py").is_file()


def test_version_comes_from_cmake_via_pyproject():
    # The declaration lives in CMake because the library is installable on its
    # own, and needs a version for SOVERSION and find_package without Python.
    #
    # Compared against the parsed value rather than a literal: a hardcoded "0.1.0"
    # would still pass if both numbers were bumped independently, which is exactly
    # the drift this arrangement prevents.
    cmake_text = (REPO_ROOT / "CMakeLists.txt").read_text()
    declared = re.search(r"project\([^)]*VERSION\s+([0-9.]+)", cmake_text)

    assert declared, "no project(... VERSION ...) found; the regex provider would fail too"

    # The C++ route: AHOCORASICK_DEMO_VERSION -> _core.
    assert ahocorasick_demo.__version__ == declared.group(1)

    # The packaging route, through scikit-build-core's regex provider. Checked
    # separately because it shares no machinery with the line above.
    assert importlib.metadata.version("ahocorasick-demo") == declared.group(1)


def test_extension_is_a_compiled_module():
    assert _core.__file__.endswith((".so", ".pyd"))


def test_generated_stub_and_py_typed_ship_beside_the_extension():
    # Anchored to _core: an editable install redirects pure-Python files to src/
    # while compiled artifacts stay in the build tree. A wheel puts both together.
    extension_dir = pathlib.Path(_core.__file__).resolve().parent

    assert (extension_dir / "py.typed").is_file(), "type checkers ignore stubs without it"

    stub = extension_dir / "_core.pyi"
    assert stub.is_file()
    assert "class PatternMatcher" in stub.read_text()
