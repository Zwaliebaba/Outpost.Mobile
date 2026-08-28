#!/usr/bin/env python3
"""Check (or apply) clang-format over the tree.

The file list is derived from the solution rather than spelled here, so a file added by a later
slice is covered the day it lands, and a project that moves is either covered or reported rather
than silently skipped. Only the flat project directories are considered: Shaders/ is HLSL, which
clang-format does not understand, and CompiledShaders/ is generated.

  python Build/CheckFormat.py          report files that are not formatted, exit 1 if any
  python Build/CheckFormat.py --fix    format them in place

The version matters. clang-format's output changes between releases, so CI pins one and this
prints whichever it found -- if your local run disagrees with CI, compare that line first.
"""

import argparse
import os
import subprocess
import sys

from Projects import projects


def sources():
    found = []
    for name, directory in projects():
        if not os.path.isdir(directory):
            raise SystemExit('error: %s is missing, but %s lists it' % (directory, name))
        for source in sorted(os.listdir(directory)):
            if source.endswith(('.h', '.cpp')):
                found.append(os.path.join(directory, source))
    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--fix', action='store_true', help='format in place instead of reporting')
    parser.add_argument('--clang-format', default='clang-format')
    args = parser.parse_args()

    try:
        version = subprocess.run([args.clang_format, '--version'], capture_output=True, text=True,
                                 check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        print('error: %s not found on PATH' % args.clang_format)
        return 1
    print(version)

    files = sources()
    if not files:
        print('error: no sources found -- has the tree moved?')
        return 1

    if args.fix:
        subprocess.run([args.clang_format, '-i'] + files, check=True)
        print('formatted %d files' % len(files))
        return 0

    unformatted = [f for f in files
                   if subprocess.run([args.clang_format, '--dry-run', '--Werror', f],
                                     capture_output=True).returncode != 0]
    if unformatted:
        for f in unformatted:
            print('error: %s is not formatted' % f)
        print('\n%d of %d file(s) need formatting. Run: python Build/CheckFormat.py --fix'
              % (len(unformatted), len(files)))
        return 1

    print('all %d files are formatted.' % len(files))
    return 0


if __name__ == '__main__':
    sys.exit(main())
