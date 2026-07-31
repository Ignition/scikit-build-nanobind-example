"""A Bloom filter implemented in C++20 and bound with nanobind.

The extension module lives at :mod:`bloomdemo._core` and is an implementation
detail; everything public is re-exported here.
"""

from ._core import BloomFilter, __version__

__all__ = ["BloomFilter", "__version__"]
