"""The packaging is half the point of this template, so it gets tested too."""

import pathlib

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


def test_version_comes_from_pyproject_via_cmake():
    # pyproject.toml -> SKBUILD_PROJECT_VERSION -> AHOCORASICK_DEMO_VERSION -> C++.
    assert ahocorasick_demo.__version__ == "0.1.0"


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
