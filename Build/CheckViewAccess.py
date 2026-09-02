"""What the composition root calls on the classes it owns, and whether it is allowed to.

**This is not a compiler and does not pretend to be one.** It exists because `Outpost` cannot be
compiled anywhere but Windows -- it reaches D3D12 and WinRT through `NeuronClient` -- so an agent or
a contributor working on another platform can syntax-check every library in the tree and still push a
composition root that does not build. That has cost two CI runs on one branch: a call to a private
`UniverseView` member, and a `->` through a member held by value.

Both are the same shape: a name that EXISTS but is not reachable from where it was written. A
compiler catches it instantly and nothing else here does, so this reads the headers and checks the
two things a grep can actually be sure about:

  * a member called on one of the root's owned objects is declared in that class's PUBLIC section;
  * a member reached with `->` is declared as a pointer, and one reached with `.` is not.

It is deliberately conservative. An unknown name is reported, because the alternative -- staying quiet
about anything it cannot parse -- makes a clean run mean nothing. If it is wrong about something, the
fix is to teach it, not to skip it.

Not in CI: MSVC gates the same mistakes there, five minutes later. This is what you run in the two
seconds before you push. AGENTS.md section 12 says so.

Run: python Build/CheckViewAccess.py
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# Which member of the composition root is which class, and where that class is declared. Every entry
# is a thing OutpostApp owns and calls through; the list grows when the root grows a screen.
OWNED = [
    ('m_view', 'UniverseView', 'Outpost/UniverseView.h'),
    ('m_hud', 'Hud', 'Outpost/Hud.h'),
    ('m_assembly', 'AssemblyScreen', 'Outpost/AssemblyScreen.h'),
    ('m_sheet', 'FleetSheet', 'Outpost/FleetSheet.h'),
    ('m_map', 'GalaxyScreen', 'Outpost/GalaxyScreen.h'),
    ('m_log', 'EventLog', 'Outpost/EventLog.h'),
]


def public_members(path, class_name):
    """Method names declared in the public section of _class_name, at class scope.

    Brace depth is what keeps a nested struct's members out of it: only declarations one level inside
    the class body are the class's own, so ShipView::Pivot is not read as UniverseView::Pivot.
    """
    text = open(os.path.join(ROOT, path), encoding='utf-8').read()
    start = text.find('class %s' % class_name)
    if start < 0:
        start = text.find('struct %s' % class_name)
    if start < 0:
        raise RuntimeError('%s: no declaration of %s' % (path, class_name))

    names = set()
    depth = 0
    # A struct is public by default; a class is not.
    section = 'public' if text[start:].startswith('struct') else 'private'
    for line in text[start:].split('\n'):
        stripped = line.strip()
        if depth <= 1 and re.match(r'^(public|private|protected)\s*:', stripped):
            section = stripped.split(':')[0]
        if depth == 1 and section == 'public' and not stripped.startswith(('//', '*', '/*')):
            found = re.search(r'\b(\w+)\s*\(', stripped)
            if found:
                names.add(found.group(1))
        depth += line.count('{') - line.count('}')
        if depth <= 0 and names:
            break
    return names


def declared_as_pointer(member):
    """Whether OutpostApp holds _member by pointer. A '.' through a pointer, or a '->' through a
    value, is the second of the two mistakes this file exists for."""
    text = open(os.path.join(ROOT, 'Outpost/OutpostApp.h'), encoding='utf-8').read()
    found = re.search(r'^\s*[\w:]+(\s*\*)?\s+%s\s*(=[^;]*)?;' % re.escape(member), text, re.M)
    return bool(found and found.group(1))


def main():
    problems = []
    surface = {}
    for member, class_name, header in OWNED:
        surface[member] = (class_name, public_members(header, class_name), declared_as_pointer(member))

    # OutpostApp.cpp ALONE, and that is the whole scope on purpose. OWNED describes the composition
    # root's members, and the same name means something else one class over -- UniverseView has its
    # own m_log, and holds it by pointer where the root holds one by value. Checking every file would
    # report that as twenty failures and teach a reader to ignore the output.
    #
    # It is also the only file in the tree that no compiler in a non-Windows environment ever sees.
    # Every other Outpost source can be built here behind a stub; this one is the composition root and
    # reaches the whole graphics stack, so it is exactly where an unreachable call survives to CI.
    for path in [os.path.join(ROOT, 'Outpost', 'OutpostApp.cpp')]:
        if True:
            shown = os.path.relpath(path, ROOT).replace(os.sep, '/')
            for number, line in enumerate(open(path, encoding='utf-8'), 1):
                if line.lstrip().startswith('//'):
                    continue
                for member, (class_name, names, is_pointer) in surface.items():
                    for call in re.findall(r'\b%s\.(\w+)\s*\(' % re.escape(member), line):
                        if call not in names:
                            problems.append('%s:%d: %s.%s() is not public on %s' % (shown, number, member, call, class_name))
                    for call in re.findall(r'\b%s->(\w+)' % re.escape(member), line):
                        if not is_pointer:
                            problems.append('%s:%d: %s->%s -- OutpostApp holds %s by value, use "."'
                                            % (shown, number, member, call, member))
                        elif call not in names:
                            problems.append('%s:%d: %s->%s() is not public on %s' % (shown, number, member, call, class_name))
                    if is_pointer and re.search(r'\b%s\.\w+\s*\(' % re.escape(member), line):
                        problems.append('%s:%d: %s is a pointer, use "->"' % (shown, number, member))

    for member, (class_name, names, is_pointer) in sorted(surface.items()):
        print('%-12s %-16s %2d public member(s)%s' % (member, class_name, len(names), ' (pointer)' if is_pointer else ''))

    if problems:
        print('\nFAILED:')
        for problem in problems:
            print('  ' + problem)
        return 1
    print('\nevery call the composition root makes on what it owns is one it is allowed to make')
    return 0


if __name__ == '__main__':
    sys.exit(main())
