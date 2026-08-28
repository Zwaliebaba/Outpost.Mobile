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

Run from the repository root. Prints every problem it finds rather than stopping at the first,
because a run that reports one of five is a run you have to do five times.
"""

import os
import re
import sys
import xml.etree.ElementTree as ElementTree

PROJECTS = ['NeuronCore', 'NeuronClient', 'NeuronServer', 'GameLogic', 'Outpost',
            'NeuronCoreTests', 'NeuronClientTests', 'NeuronServerTests', 'GameLogicTests']

# Generated or vendored; not hand-written source and not subject to these rules.
IGNORED_DIRS = {'CompiledShaders', 'packages'}


def read(path):
    with open(path, encoding='utf-8-sig') as handle:
        return handle.read()


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
    for project in PROJECTS:
        if not os.path.isdir(project):
            problems.append('%s: project directory is missing' % project)
            continue

        sources = sorted(f for f in os.listdir(project) if f.endswith(('.h', '.cpp')))
        shaders = []
        shader_dir = os.path.join(project, 'Shaders')
        if os.path.isdir(shader_dir):
            shaders = sorted('Shaders\\' + f for f in os.listdir(shader_dir) if f.endswith(('.hlsl', '.hlsli')))

        project_text = read(os.path.join(project, '%s.vcxproj' % project))
        filter_text = read(os.path.join(project, '%s.vcxproj.filters' % project))

        for name in sources + shaders:
            if 'Include="%s"' % name not in project_text:
                problems.append('%s/%s: not listed in %s.vcxproj' % (project, name, project))
            if 'Include="%s"' % name not in filter_text:
                problems.append('%s/%s: not listed in %s.vcxproj.filters' % (project, name, project))

        listed = re.findall(r'(?:ClInclude|ClCompile|FxCompile) Include="([^"]+)"', project_text)
        for name in listed:
            if not os.path.exists(os.path.join(project, name.replace('\\', os.sep))):
                problems.append('%s/%s: listed in the .vcxproj but not on disk' % (project, name))


def check_unique_names(problems):
    """Unique repo-wide and case-insensitively, because several project roots share an include path."""
    seen = {}
    for project in PROJECTS:
        if not os.path.isdir(project):
            continue
        for name in os.listdir(project):
            if not name.endswith(('.h', '.cpp')) or name.lower().startswith('pch.'):
                continue
            seen.setdefault(name.lower(), []).append(project)
    for name, owners in sorted(seen.items()):
        if len(owners) > 1:
            problems.append('%s: declared in more than one project (%s)' % (name, ', '.join(owners)))


def check_dependency_rules(problems):
    """The engine never names the game. An engine project that does has stopped being an engine."""
    for project in ['NeuronCore', 'NeuronClient', 'NeuronServer',
                    'NeuronCoreTests', 'NeuronClientTests', 'NeuronServerTests']:
        if not os.path.isdir(project):
            continue
        project_text = read(os.path.join(project, '%s.vcxproj' % project))
        if 'GameLogic' in project_text:
            problems.append('%s/%s.vcxproj: an engine project references GameLogic' % (project, project))
        for name in os.listdir(project):
            if not name.endswith(('.h', '.cpp')):
                continue
            body = read(os.path.join(project, name))
            for game_header in ('GameLogic.h', 'World.h', 'ShipState.h', 'Movement.h', 'Formation.h', 'SimTuning.h'):
                if '#include "%s"' % game_header in body:
                    problems.append('%s/%s: an engine file includes the game header %s' % (project, name, game_header))


def main():
    problems = []
    check_xml(problems)
    check_registration(problems)
    check_unique_names(problems)
    check_dependency_rules(problems)

    if problems:
        for problem in problems:
            print('error: %s' % problem)
        print('\n%d problem(s). See AGENTS.md sections 2 and 3.' % len(problems))
        return 1

    print('project files: XML well-formed, every source registered, names unique, layers intact.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
