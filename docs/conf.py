"""Sphinx configuration for the libtmux-cxx API reference.

Doxygen parses the headers into XML; Breathe turns that XML into Sphinx
nodes; Sphinx renders them. The C++ reference therefore flows through the
same toolchain as the Python port instead of shipping Doxygen's own HTML,
which keeps the site's design uniform and keeps GPL-licensed output off the
site entirely.

Only the pages under docs/api/ and this directory's index are built. The rest
of docs/ is internal engineering material (plans, bakeoffs, decisions) that
is not published.
"""

from __future__ import annotations

import os

project = "libtmux-cxx"
author = "Tony Narlock"

extensions = ["breathe"]

# build-site.sh overrides this with -D breathe_projects.cxx=<scratch>/xml.
# The default points at a bare `doxygen Doxyfile` run in the checkout root.
breathe_projects = {"cxx": os.environ.get("LIBTMUX_CXX_XML", "../xml")}
breathe_default_project = "cxx"
breathe_default_members = ()

# Everything except the reference is internal.
exclude_patterns = [
    "_build",
    "README.md",
    "api.md",
    "api-testing.md",
    "vcpkg-registry.md",
    "bakeoffs/**",
    "decisions/**",
    "design/**",
    "evidence/**",
    "plans/**",
    "superpowers/**",
]

html_theme = "furo"
html_title = "libtmux-cxx"
