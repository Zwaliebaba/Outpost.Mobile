#!/usr/bin/env python3
"""Static checks on the tree, run before anything is compiled.

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
  * The compiler settings are spelled per project rather than shared through a property sheet,
    so nothing but this check keeps them the same. One project quietly drifting off /fp:precise, or
    back onto the debug CRT in Release, is a defect that shows up months later as a replay test
    that fails on one machine, or as a Release measurement of a debug binary.

The last four checks are about source rather than project files, and they are here rather than in a
file of their own because they answer the same question: what can be decided before a compiler runs.
AGENTS.md 1 says several of its rules are checked "by eye", and the review found the drift sitting
exactly where nothing looked (MmoScalabilityReview.md C2). What a grep can decide without types is
checked here; what needs types stays with the reader, and each check says which it is.

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


def check_shared_settings(problems):
    """The compiler settings every project spells for itself, still spelling the same thing.

    A property sheet made this true by construction, and there is no sheet: each .vcxproj carries
    these per Configuration|Platform, the way Visual Studio writes them, so that its property pages
    can read and edit them. Nothing but this check keeps the copies in step. One project quietly
    off /fp:precise, or back on the debug CRT in Release, is a defect that surfaces months later as
    a replay test that fails on one machine, or as a Release measurement of a debug binary.

    Read out of the XML rather than out of marked-off regions of text, so a project file that
    Visual Studio has rewritten is still checked.
    """
    shared = {
        'UseDebugLibraries', 'PlatformToolset', 'CharacterSet', 'WholeProgramOptimization',
        'LinkIncremental', 'LanguageStandard', 'ConformanceMode', 'WarningLevel', 'SDLCheck',
        'MultiProcessorCompilation', 'FloatingPointModel', 'PrecompiledHeader',
        'PrecompiledHeaderFile', 'PrecompiledHeaderOutputFile', 'AdditionalOptions',
        'PreprocessorDefinitions', 'Optimization', 'FunctionLevelLinking', 'IntrinsicFunctions',
        'GenerateDebugInformation', 'EnableCOMDATFolding', 'OptimizeReferences',
    }
    # NeuronClient is the only project with shaders, so its FxCompile settings share with nothing.
    ignored_items = {'FxCompile'}
    per_configuration = re.compile(r"'\$\(Configuration\)\|\$\(Platform\)'=='([^']+)'")

    def name_of(node):
        return node.tag.split('}')[-1]

    # (configuration, setting) -> value -> [project names]
    seen = {}
    for project, directory in projects():
        path = os.path.join(directory, '%s.vcxproj' % project)
        if not os.path.exists(path):
            continue
        for group in ElementTree.fromstring(read(path).encode('utf-8')):
            match = per_configuration.fullmatch(group.get('Condition', ''))
            if not match:
                continue
            settings = []
            if name_of(group) == 'PropertyGroup':
                settings = [(name_of(child), name_of(child), child.text) for child in group]
            elif name_of(group) == 'ItemDefinitionGroup':
                for item in group:
                    if name_of(item) in ignored_items:
                        continue
                    settings += [('%s/%s' % (name_of(item), name_of(child)), name_of(child),
                                  child.text) for child in item]
            for label, setting, value in settings:
                if setting in shared:
                    seen.setdefault((match.group(1), label), {}).setdefault(value or '', []).append(project)

    for (configuration, label), values in sorted(seen.items()):
        if len(values) < 2:
            continue
        agreed = max(values, key=lambda value: len(values[value]))
        for value, owners in sorted(values.items()):
            if value == agreed:
                continue
            problems.append('%s: %s is "%s" in %s, and "%s" in the other %d project(s)'
                            % (configuration, label, value, ', '.join(sorted(owners)), agreed,
                               len(values[agreed])))


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


def game_headers():
    """Every header GameLogic publishes, read off disk rather than listed here.

    This was a hard-coded six while GameLogic had grown to fourteen, so an engine file including
    UniverseSnapshot.h or SpatialIndex.h passed CI (MmoScalabilityReview.md C2). A list that has to be
    edited when a header is added is a list that is wrong from the next commit onwards, and wrong in
    the direction that lets the defect through rather than the one that reports it.
    """
    for name, directory in projects():
        if name == 'GameLogic':
            return sorted(f for f in os.listdir(directory) if f.endswith('.h') and f != 'pch.h')
    raise SystemExit('error: the solution no longer has a GameLogic project -- has the tree moved?')


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
            for game_header in game_headers():
                if '#include "%s"' % game_header in body:
                    problems.append('%s/%s: an engine file includes the game header %s'
                                    % (directory, source, game_header))


# The headers that mean "there is a screen". NeuronClient may include them; the libraries the
# server is built from may not, because the server is meant to run in a container without one.
GRAPHICS_INCLUDE = re.compile(r'#\s*include\s*<(d3d\w*|dxgi\w*|d2d\w*|dwrite\w*|dcomp\w*)\.h>', re.I)
HEADLESS_PROJECTS = ('NeuronCore', 'NeuronServer')


def check_headless(problems):
    """NeuronCore and NeuronServer never include a graphics header (AGENTS.md 2)."""
    for name, directory in projects():
        if name not in HEADLESS_PROJECTS or not os.path.isdir(directory):
            continue
        for source in sorted(os.listdir(directory)):
            if not source.endswith(('.h', '.cpp')):
                continue
            body = read(os.path.join(directory, source))
            for match in GRAPHICS_INCLUDE.finditer(body):
                line = body[:match.start()].count('\n') + 1
                problems.append('%s/%s:%d: a headless library includes the graphics header <%s.h>'
                                % (directory, source, line, match.group(1)))


# ---------------------------------------------------------------------------------------------
# Source conformance. AGENTS.md 1 says several of its rules are checked "by eye", and the review
# found drift sitting exactly where no machine looks (MmoScalabilityReview.md C2). These are the
# rules a grep can decide without a compiler; the ones needing types stay with the reader, and this
# file does not pretend otherwise.


def source_files():
    """Every hand-written .h and .cpp the solution builds, as paths."""
    for name, directory in projects():
        if not os.path.isdir(directory):
            continue
        for source in sorted(os.listdir(directory)):
            if source.endswith(('.h', '.cpp')):
                yield os.path.join(directory, source)


def code_only(text):
    """Comments and string bodies blanked, so prose about a rule is not read as a breach of it.

    Transport.h explains that it is not called ITransport; a checker that read comments would fail
    the tree for the sentence that documents the rule. Lengths are not preserved, so line numbers
    come from counting newlines in the returned text, not the original.
    """
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'//[^\n]*', '', text)
    return re.sub(r'"(?:\\.|[^"\\\n])*"', '""', text)


def line_of(text, offset):
    return text[:offset].count('\n') + 1


TYPE_DECLARATION = re.compile(r'\b(?:class|struct|enum\s+class|enum)\s+([A-Za-z_]\w*)')
ALIAS_DECLARATION = re.compile(r'\busing\s+([A-Za-z_]\w*)\s*=')


def check_type_names(problems):
    """R2: a type name carries no prefix, no Base/Impl suffix, and no _t.

    The prefix test is the four letters R2 names -- I, C, S or E followed by a capital -- and not
    "two capitals to start", which is what this said before anyone compared it with the line below.
    The wider test would trip on QUIC_API_TABLE, which is MsQuic's type and not ours to rename, so
    R4's GpuDevice-not-GPUDevice stays a rule the reader keeps rather than one this holds.
    """
    banned_prefix = re.compile(r'^(?:I|C|S|E)[A-Z]')
    for path in source_files():
        body = code_only(read(path))
        for rule in (TYPE_DECLARATION, ALIAS_DECLARATION):
            for match in rule.finditer(body):
                name = match.group(1)
                reason = None
                if banned_prefix.match(name):
                    reason = 'a type name carries no I/C/S/E prefix'
                elif name.endswith(('Base', 'Impl')):
                    reason = 'a type name carries no Base or Impl suffix'
                elif name.endswith('_t'):
                    reason = 'a type name carries no _t suffix'
                elif name.startswith('Abstract'):
                    reason = 'a type name carries no Abstract prefix'
                if reason:
                    problems.append('%s:%d: %s (AGENTS.md 1 R2): %s'
                                    % (path, line_of(body, match.start()), reason, name))


# The UK-spelled families AGENTS.md R11 lists as standing, and the US spelling that would split
# each of them. A split family is a grep that silently misses half its answers, which is the actual
# cost being guarded against -- not the spelling itself.
SPELLING_FAMILIES = (
    ('Metres', re.compile(r'[Mm]eters|METERS')),
    ('Colour', re.compile(r'[Cc]olors?\b|COLORS?\b|[Cc]olor[A-Z_]')),
    ('Neighbour', re.compile(r'[Nn]eighbors?\b|NEIGHBORS?\b|[Nn]eighbor[A-Z_]')),
    ('Centre', re.compile(r'[Cc]enters?\b|CENTERS?\b|[Cc]enter[A-Z_]')),
    ('GREY', re.compile(r'[Gg]rays?\b|GRAYS?\b|[Gg]ray[A-Z_]')),
)

# Only names the tree itself declares are checked, told apart by the tree's own markers: R1's
# leading underscore on a parameter, R8's m_/sm_ on class state, R3's UPPER_CASE on a constant, and
# a type name. A local spelled camelCase is not reachable this way and neither is a call into a
# platform API, which is the point -- D3D12 and Win32 spell Color and Center and are not ours to
# rename.
DECLARED_NAME = re.compile(r'\b(m_[A-Za-z_]\w*|sm_[A-Za-z_]\w*|_[a-z]\w*)\b')
DECLARED_CONSTANT = re.compile(r'\bconstexpr\s+[\w:<>,\s*&]+?\b([A-Z][A-Z0-9_]{2,})\s*(?:=|\{|\[)')


def check_spelling_families(problems):
    """R11: a new identifier does not split one of the standing UK-spelled families."""
    for path in source_files():
        body = code_only(read(path))
        for rule in (DECLARED_NAME, DECLARED_CONSTANT, TYPE_DECLARATION, ALIAS_DECLARATION):
            for match in rule.finditer(body):
                name = match.group(1)
                for family, us_spelling in SPELLING_FAMILIES:
                    if us_spelling.search(name):
                        problems.append('%s:%d: %s splits the standing %s family (AGENTS.md 1 R11)'
                                        % (path, line_of(body, match.start()), name, family))


# Identifiers <windows.h> defines as object-like macros. A declarator with one of these names does
# not fail to compile at its declaration -- it expands, and the error lands somewhere else entirely.
# This cost a CI run to `Link near;`, where `near` expands to nothing and the eight errors that
# followed all named the line after it. min and max are not here: NeuronCore.h defines NOMINMAX.
WINDOWS_MACROS = ('near', 'far', 'small', 'interface', 'IN', 'OUT', 'ERROR', 'DELETE', 'TRUE', 'FALSE')
SHADOWED_MACRO = re.compile(r'\b[A-Za-z_]\w*(?:\s*::\s*\w+)*\s*[*&]?\s+(' + '|'.join(WINDOWS_MACROS) + r')\s*[;={,)\[]')


def check_shadowed_macros(problems):
    """No declarator is named after a macro the Windows headers define."""
    for path in source_files():
        body = code_only(read(path))
        for match in SHADOWED_MACRO.finditer(body):
            problems.append('%s:%d: `%s` is a macro <windows.h> defines, so this declaration expands '
                            'to something else (AGENTS.md 3)'
                            % (path, line_of(body, match.start()), match.group(1)))


BRACED_ARGUMENT = re.compile(r'[(,]\s*\{\s*([^{}]*?)\s*\}\s*[,)]')
LITERAL_ELEMENT = re.compile(r"^(?:[-+]?\d[\w.]*|true|false|nullptr|'\\?.')$")


def paren_depths(body):
    """Parenthesis depth at each character, so a table row is told from a call argument."""
    depths = []
    depth = 0
    for character in body:
        depths.append(depth)
        if character == '(':
            depth += 1
        elif character == ')':
            depth = max(0, depth - 1)
    return depths


def check_literal_aggregates(problems):
    """A braced call argument of nothing but literals names its fields.

    `Connect(client, server, {0, 256, 3})` is a Desc whose fields are bound by position and named
    nowhere. Adding a field to that Desc rebinds every element silently: this cost a CI run when a
    capacity landed between two others and turned a loss test's `dropOneInN` into a ring depth, so
    the test passed while measuring nothing. Designated initializers make that a compile error.

    Deliberately narrow. Only all-literal lists are flagged, because `{penX, penY, cell.u0}` at
    least names its elements through its variables, and only inside a call, because a row of a
    constant table is a table row and reads as one. A braced list of variables passed positionally
    into an aggregate is the same hazard and is not caught here -- that needs types, and this file
    has none.
    """
    for path in source_files():
        body = code_only(read(path))
        depths = paren_depths(body)
        for match in BRACED_ARGUMENT.finditer(body):
            inner = match.group(1)
            if not inner or inner.startswith('.') or ',' not in inner:
                continue
            if depths[match.start()] < 1:
                continue
            if not all(LITERAL_ELEMENT.match(part.strip()) for part in inner.split(',')):
                continue
            problems.append('%s:%d: {%s} binds fields by position and names none of them; use '
                            'designated initializers (AGENTS.md 1 R8)'
                            % (path, line_of(body, match.start()), inner))


def main():
    problems = []
    check_xml(problems)
    check_registration(problems)
    check_shared_settings(problems)
    check_precompiled_headers(problems)
    check_unique_names(problems)
    check_dependency_rules(problems)
    check_headless(problems)
    check_type_names(problems)
    check_spelling_families(problems)
    check_shadowed_macros(problems)
    check_literal_aggregates(problems)

    if problems:
        for problem in problems:
            print('error: %s' % problem)
        print('\n%d problem(s). See AGENTS.md sections 1, 2 and 3.' % len(problems))
        return 1

    print('project files: XML well-formed, every source registered, shared settings agree, '
          'names unique, layers intact, headless libraries headless.')
    print('source names: no banned type affixes, no split spelling families, no shadowed Windows '
          'macros, no positional literal aggregates.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
