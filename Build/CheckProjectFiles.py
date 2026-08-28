#!/usr/bin/env python3
"""Static checks on the project files, run before anything is compiled.

Each of these replaces a defect that is expensive to find any other way:

  * A malformed .props or .vcxproj reports as MSB4024 once per project that imports it -- nine
    identical errors naming the importer rather than the mistake. This has already cost one CI run
    to an XML comment containing '--', which XML forbids and nothing local flagged.
  * A file missing from its .vcxproj compiles nowhere and links nowhere, and the first symptom is
    an unresolved external from a translation unit that has no obvious relationship to it.
  * A file missing from its .filters is invisible in Solution Explorer, which is how a file gets
    edited by one person and never seen by the next.
  * Two files with the same name in different projects resolve to whichever project root comes
    first on the include path -- silently, and differently per consumer.
  * A pch.cpp that does not include its own pch.h is C2857, reported at line 1 column 1 of a file
    whose entire contents are the thing that is missing. Every project has one and they are all
    identical, so this is exactly the file nobody reads.
  * The compiler settings are copied into every .vcxproj rather than shared through a property
    sheet, so nothing but this check keeps the copies the same. One project quietly drifting off
    /fp:precise, or back onto the debug CRT in Release, is a defect that shows up months later as a
    replay test that fails on one machine, or as a Release measurement of a debug binary.

Run from the repository root. Prints every problem it finds rather than stopping at the first,
because a run that reports one of five is a run you have to do five times.
"""

import os
import re
import sys
import xml.etree.ElementTree as ElementTree

from Projects import projects, read

# Generated or vendored; not hand-written source and not subject to these rules.
IGNORED_DIRS = {'CompiledShaders', 'packages'}


def check_xml(problems):
    """Well-formedness, plus the comment rule that XML enforces and no editor warns about."""
    for root, dirs, files in os.walk('.'):
        dirs[:] = [d for d in dirs if d not in IGNORED_DIRS and not d.startswith('.')]
        for name in sorted(files):
            if not name.endswith(('.props', '.targets', '.vcxproj', '.filters', '.slnx', '.config')):
                continue
            path = os.path.join(root, name)
            text = read(path)
            for match in re.finditer(r'<!--(.*?)-->', text, re.S):
                if '--' in match.group(1) or match.group(1).endswith('-'):
                    line = text[:match.start()].count('\n') + 1
                    problems.append('%s:%d: an XML comment cannot contain "--" or end in "-"' % (path, line))
            try:
                ElementTree.fromstring(text.encode('utf-8'))
            except ElementTree.ParseError as error:
                problems.append('%s: not well-formed XML: %s' % (path, error))


def check_registration(problems):
    """Every source file listed in both the .vcxproj and the .filters, and nothing stale in either."""
    for name, directory in projects():
        if not os.path.isdir(directory):
            problems.append('%s: %s is missing' % (name, directory))
            continue

        sources = sorted(f for f in os.listdir(directory) if f.endswith(('.h', '.cpp')))
        shaders = []
        shader_dir = os.path.join(directory, 'Shaders')
        if os.path.isdir(shader_dir):
            shaders = sorted('Shaders\\' + f for f in os.listdir(shader_dir) if f.endswith(('.hlsl', '.hlsli')))

        project_text = read(os.path.join(directory, '%s.vcxproj' % name))
        filter_text = read(os.path.join(directory, '%s.vcxproj.filters' % name))

        for source in sources + shaders:
            if 'Include="%s"' % source not in project_text:
                problems.append('%s/%s: not listed in %s.vcxproj' % (directory, source, name))
            if 'Include="%s"' % source not in filter_text:
                problems.append('%s/%s: not listed in %s.vcxproj.filters' % (directory, source, name))

        listed = re.findall(r'(?:ClInclude|ClCompile|FxCompile) Include="([^"]+)"', project_text)
        for source in listed:
            if not os.path.exists(os.path.join(directory, source.replace('\\', os.sep))):
                problems.append('%s/%s: listed in the .vcxproj but not on disk' % (directory, source))


def check_shared_blocks(problems):
    """The settings every project repeats verbatim, still repeated verbatim.

    A property sheet made this true by construction. Copies do not, so the copies are delimited by
    the comment they open with and the "end of the shared ... block" marker they close with, and
    this compares them. Change a setting in one project and this reports the other eight.
    """
    blocks = {
        'toolset': (r'  <!--\n    Configuration flavour, decided before .*?'
                    r'  <!-- end of the shared toolset block -->\n'),
        'compiler': (r'  <!--\n    Compiler and linker settings, repeated verbatim .*?'
                     r'  <!-- end of the shared compiler block -->\n'),
    }
    for label, pattern in blocks.items():
        found = {}
        for name, directory in projects():
            path = os.path.join(directory, '%s.vcxproj' % name)
            if not os.path.exists(path):
                continue
            match = re.search(pattern, read(path), re.S)
            if not match:
                problems.append('%s: has no shared %s block, and every project carries one' % (path, label))
                continue
            found.setdefault(match.group(0), []).append(path)
        if len(found) > 1:
            majority = max(found.values(), key=len)
            for owners in found.values():
                if owners is majority:
                    continue
                problems.append('%s: its shared %s block differs from the other %d project(s)'
                                % (', '.join(owners), label, len(majority)))


def check_precompiled_headers(problems):
    """/Yc requires the creating translation unit to include the header it is creating."""
    for _, directory in projects():
        path = os.path.join(directory, 'pch.cpp')
        if not os.path.exists(path):
            problems.append('%s/pch.cpp is missing' % directory)
            continue
        if '#include "pch.h"' not in read(path):
            problems.append('%s/pch.cpp: does not include "pch.h", which /Yc requires (C2857)' % directory)


def check_unique_names(problems):
    """Unique repo-wide and case-insensitively, because several project roots share an include path."""
    seen = {}
    for name, directory in projects():
        if not os.path.isdir(directory):
            continue
        for source in os.listdir(directory):
            if not source.endswith(('.h', '.cpp')) or source.lower().startswith('pch.'):
                continue
            seen.setdefault(source.lower(), []).append(name)
    for source, owners in sorted(seen.items()):
        if len(owners) > 1:
            problems.append('%s: declared in more than one project (%s)' % (source, ', '.join(owners)))


def check_dependency_rules(problems):
    """The engine never names the game. An engine project that does has stopped being an engine.

    Neuron* is the engine and its test suites; everything else in the solution is allowed to know
    about the game.
    """
    for name, directory in projects():
        if not name.startswith('Neuron') or not os.path.isdir(directory):
            continue
        # Comments stripped first: the shared compiler block explains why /fp:precise is set by
        # naming the layer that needs it, and a prose mention is not a reference.
        project_text = read(os.path.join(directory, '%s.vcxproj' % name))
        project_text = re.sub(r'<!--.*?-->', '', project_text, flags=re.S)
        if 'GameLogic' in project_text:
            problems.append('%s/%s.vcxproj: an engine project references GameLogic' % (directory, name))
        for source in os.listdir(directory):
            if not source.endswith(('.h', '.cpp')):
                continue
            body = read(os.path.join(directory, source))
            for game_header in ('GameLogic.h', 'World.h', 'ShipState.h', 'Movement.h', 'Formation.h', 'SimTuning.h'):
                if '#include "%s"' % game_header in body:
                    problems.append('%s/%s: an engine file includes the game header %s'
                                    % (directory, source, game_header))


def main():
    problems = []
    check_xml(problems)
    check_registration(problems)
    check_shared_blocks(problems)
    check_precompiled_headers(problems)
    check_unique_names(problems)
    check_dependency_rules(problems)

    if problems:
        for problem in problems:
            print('error: %s' % problem)
        print('\n%d problem(s). See AGENTS.md sections 2 and 3.' % len(problems))
        return 1

    print('project files: XML well-formed, every source registered, shared blocks identical, '
          'names unique, layers intact.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
