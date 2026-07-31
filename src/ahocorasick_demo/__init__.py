"""Aho-Corasick multi-pattern matching, implemented in C++20 and bound with nanobind.

The extension module lives at :mod:`ahocorasick_demo._core` and is an
implementation detail; everything public is re-exported here.
"""

from ._core import PatternMatcher, __version__

__all__ = ["PatternMatcher", "__version__"]
