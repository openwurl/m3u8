import os
import sys
from os.path import abspath, dirname, exists, join

from setuptools import Extension, setup

long_description = None
if exists("README.md"):
    with open("README.md") as file:
        long_description = file.read()

install_reqs = [
    req for req in open(abspath(join(dirname(__file__), "requirements.txt")))
]

# Optional C extension for faster parsing
# Only build on Mac and Linux, and allow skipping via environment variable
ext_modules = []

# For a single wheel across CPython minor versions, build against the stable ABI
# (abi3). Since python_requires is >=3.10, we can target the 3.10 limited API.
PY_LIMITED_API = "0x030A0000"

# Check if we should build the C extension
build_c_extension = sys.platform in ("darwin", "linux") and os.environ.get(
    "M3U8_NO_C_EXTENSION", ""
).lower() not in ("1", "true", "yes")

if build_c_extension:
    ext_modules.append(
        Extension(
            "m3u8._m3u8_parser",
            sources=["m3u8/_m3u8_parser.c"],
            optional=True,  # Don't fail the build if extension compilation fails
            py_limited_api=True,
            define_macros=[("Py_LIMITED_API", PY_LIMITED_API)],
        )
    )

setup(
    name="m3u8",
    author="Globo.com",
    version="6.3.0",
    license="MIT",
    zip_safe=False,
    include_package_data=True,
    install_requires=install_reqs,
    packages=["m3u8"],
    ext_modules=ext_modules,
    url="https://github.com/openwurl/m3u8",
    description="Python m3u8 parser",
    long_description=long_description,
    long_description_content_type="text/markdown",
    python_requires=">=3.10",
)
