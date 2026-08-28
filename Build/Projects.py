#!/usr/bin/env python3
"""Where the projects are, read out of the solution.

Both checks in this directory walk every project in the tree, and both used to spell the list
themselves. Moving the four test suites under Tests/ then broke one of them outright and made the
other quietly skip four projects while still reporting success, which is the worse of the two
failures. The solution already knows where every project is; ask it.
"""

import os
import re

SOLUTION = 'Outpost.slnx'


def read(path):
    with open(path, encoding='utf-8-sig') as handle:
        return handle.read()


def projects():
    """Every project the solution builds, as sorted (name, directory) pairs."""
    found = []
    for path in re.findall(r'<Project Path="([^"]+)"', read(SOLUTION)):
        path = path.replace('\\', '/')
        name = os.path.basename(path)[:-len('.vcxproj')]
        found.append((name, os.path.normpath(os.path.dirname(path))))
    if not found:
        raise SystemExit('error: no projects found in %s -- has the tree moved?' % SOLUTION)
    return sorted(found)
