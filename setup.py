"""
Minimal setup.py for C extension configuration.

All package metadata is in pyproject.toml. This file only provides the
C extension definition, which requires conditional logic that can't be
expressed declaratively.
"""

import os
import sys

from setuptools import Extension, setup

# Optional C extension for faster parsing
# Only build on Mac and Linux, and allow skipping via environment variable
ext_modules = []

# For a single wheel across CPython minor versions, build against the stable ABI
# (abi3). Since python_requires is >=3.10, we can target the 3.10 limited API.
PY_LIMITED_API = 0x030A0000

# Check if we should build the C extension
if (
    sys.platform in ("darwin", "linux")
    and os.environ.get("M3U8_NO_C_EXTENSION", "") != "1"
):
    # When building wheels (cibuildwheel sets this), require the extension
    # For local editable installs, keep it optional so pure-Python fallback works
    is_wheel_build = "CIBUILDWHEEL" in os.environ

    ext_modules.append(
        Extension(
            "openm3u8._m3u8_parser",
            sources=["openm3u8/_m3u8_parser.c"],
            optional=not is_wheel_build,  # Required for wheels, optional otherwise
            py_limited_api=True,
            define_macros=[("Py_LIMITED_API", PY_LIMITED_API)],
        )
    )

setup(ext_modules=ext_modules)
