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

# docs/_templates/search.html overrides Furo's built-in search page (Sphinx
# always renders it from a template literally named "search.html") to
# redirect to the shell's Pagefind search instead of a UI with no index at
# this path (notes/status.md, "Sphinx's own search page is a dead end", in
# the docs-site repo). This port has no other templates_path, unlike the
# Python port's gp_sphinx default, so it must be set explicitly here.
templates_path = ["_templates"]

# Closes notes/status.md's "Python and C++ are unskinned islands" glitch:
# without these two lines this port renders as stock Furo, with none of the
# site's palette and no port/version switcher. libtmux-org.css maps the
# shared --lt-* tokens (docs/_static/libtmux-org.css) onto Furo's own
# --color-* contract; shell.js injects the header/footer chrome and the
# version switcher. Both load from a stable URL rather than being vendored,
# so a chrome fix reaches this already-published build without a rebuild
# — see notes/research/03-design-token-bridge.md.
html_static_path = ["_static"]
html_css_files = ["libtmux-org.css"]
html_js_files = [("/_shell/shell.js", {"defer": "defer"})]
