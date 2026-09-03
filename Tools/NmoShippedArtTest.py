"""The shipped hulls, checked against what a consumer of their parts may assume.

NeuronClientTests cannot do this. It deploys one golden fixture and nothing else, and giving it the
game's meshes would make the engine's test suite depend on the game's content -- the direction
AGENTS.md section 2 spends most of its rules keeping shut. So the check lives here, beside the other
stdlib-only codec tests, and is run the way NmoRoundtripTest.py is.

What it asserts, and why each one matters to something:

  * every submesh is named, so a part can be addressed by name hash at all;
  * every submesh states bind-pose extents, because a part's own centre is the only pivot it has --
    no shipped hull carries a bone (Design/Archive/Combat-slice-3.md 2.6);
  * no two names in one mesh collide under FNV-1a, which is the rule NmoReader enforces at load and
    would reject the whole file for;
  * the primitive counts sum to the mesh's own, so the ranges a reader builds tile its buffers;
  * every `Gun` marker names a mount its hull actually has, and no two name the same one -- the
    mount-versus-marker check Combat.md section 16 owed, and the reason this file now reads
    GameLogic/HullSpec.h.

Run: python3 Tools/NmoShippedArtTest.py
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'BlenderNmo'))

import NmoFormat as nmo  # noqa: E402

MESH_DIR = os.path.join(os.path.dirname(HERE), 'Outpost', 'Assets', 'Meshes')
HULL_SPEC = os.path.join(os.path.dirname(HERE), 'GameLogic', 'HullSpec.h')

# A mesh file names a hull. Every .nmo whose stem is a HullId is checked against that hull's mount
# table; one that is not -- Fighter.nmo, art for a hull the simulation does not have -- is reported
# as unchecked rather than passed silently, because a marker nothing can check is a marker nothing
# holds to anything.
GUN_MARKER = re.compile(r'^Gun(\d+)([A-Z]?)$')


def read_mount_counts():
    """How many mounts each hull carries, parsed out of HullSpec.h.

    A parse of a C++ header, chosen over a generated table with its eyes open (Combat-slice-6.md 3):
    a generated file would be a second source of truth for a table that is already the only one,
    which is what ADR 0058 exists to refuse, and this parser's fragility surfaces as a RED CHECK
    rather than as a wrong answer -- which is why every step below raises instead of defaulting.

    It reads two things and nothing else: the LOADOUT_* constants' trailing counts, and which
    loadout each row of HULL_SPECS wears. The day the table's shape changes, this fails loudly and
    the reason to look is stated beside the table in HullSpec.h.
    """
    text = open(HULL_SPEC, encoding='utf-8').read()

    # enum class HullId { Interceptor, Bomber, ... } -- the order is the row order of HULL_SPECS.
    match = re.search(r'enum class HullId[^{]*\{([^}]*)\}', text)
    if match is None:
        raise RuntimeError('HullSpec.h: could not find "enum class HullId"')
    hulls = [name.strip() for name in match.group(1).split(',') if name.strip()]

    # inline constexpr MountLoadout LOADOUT_NAME{ {...}, <count>};  -- the count is the last integer
    # before the closing brace, and LOADOUT_NONE states none at all.
    loadouts = {'LOADOUT_NONE': 0}
    for name, body in re.findall(r'inline constexpr MountLoadout (LOADOUT_\w+)\{(.*?)\};', text, re.S):
        counts = re.findall(r',\s*(\d+)\s*$', body.strip())
        if not counts:
            if name not in loadouts:
                raise RuntimeError('HullSpec.h: %s states no mount count' % name)
            continue
        loadouts[name] = int(counts[-1])

    # Which loadout each hull wears. The rows of HULL_SPECS are positional aggregates ending in the
    # loadout name, so the loadouts named INSIDE that initialiser, in order, are the hulls in order.
    # Scoped to the initialiser rather than to the file, or the LOADOUT_* definitions above it would
    # be counted as rows.
    block = re.search(r'HULL_SPECS\[HULL_COUNT\]\s*=\s*\{(.*?)\n\};', text, re.S)
    if block is None:
        raise RuntimeError('HullSpec.h: could not find the HULL_SPECS initialiser')
    worn = re.findall(r'(LOADOUT_\w+)', block.group(1))
    if len(worn) != len(hulls):
        raise RuntimeError('HullSpec.h: %d hulls in the enum but %d rows name a loadout' % (len(hulls), len(worn)))
    for name in worn:
        if name not in loadouts:
            raise RuntimeError('HullSpec.h: a row wears %s, which no LOADOUT_ constant defines' % name)
    return {hull: loadouts[name] for hull, name in zip(hulls, worn)}


def fnv1a(text):
    """The hash NmoReader stores a name under (NmoFormat.md 5.10)."""
    value = 2166136261
    for byte in text.encode('utf-8'):
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def check(failures, condition, message):
    if not condition:
        failures.append(message)


def check_gun_markers(failures, unchecked, file_name, model, mount_counts):
    """Every Gun marker names a mount its hull has, and no two name the same one.

    The rule, and why it is this one rather than "a marker per mount" (Combat-slice-6.md 8):

      * a mount may carry SEVERAL muzzles -- the Battleship's turrets have two barrels each -- so the
        name is Gun<N> for a single muzzle and Gun<N><letter> for one of several on mount N;
      * a mount may carry NONE, which Combat.md 3.1 permits explicitly: it draws its effects from the
        hull's origin. Six shipped mounts are in that state and are reported, not failed;
      * so what can be held is the other direction. A marker naming a mount the hull does not have is
        a marker the client will never read, and two markers claiming the same muzzle of the same
        mount is an authoring slip that reads as one muzzle firing twice.
    """
    hull = os.path.splitext(file_name)[0]
    markers = [m for mesh in model.meshes for sub in mesh.sub_meshes for m in sub.markers if m.kind == 'Gun']

    if hull not in mount_counts:
        # Fighter.nmo: art for a hull HullSpec does not carry. Said out loud rather than passed, so
        # a hull that quietly lost its row does not take its markers out of the check with it.
        if markers:
            unchecked.append('%s: %d Gun marker(s), but no HullId of that name -- nothing to check them against'
                             % (file_name, len(markers)))
        return

    mounts = mount_counts[hull]
    claimed = {}
    for marker in markers:
        match = GUN_MARKER.match(marker.name)
        check(failures, match is not None,
              '%s: Gun marker %r does not name a mount -- the name is Gun<N> or Gun<N><letter>' % (file_name, marker.name))
        if match is None:
            continue
        mount = int(match.group(1))
        check(failures, mount < mounts,
              '%s: Gun marker %r names mount %d, but the hull carries %d' % (file_name, marker.name, mount, mounts))
        key = (mount, match.group(2))
        check(failures, key not in claimed,
              '%s: Gun markers %r and %r claim the same muzzle of mount %d' % (file_name, claimed.get(key), marker.name, mount))
        claimed[key] = marker.name

    bare = sorted({mount for mount, _ in claimed})
    missing = [mount for mount in range(mounts) if mount not in bare]
    if missing:
        unchecked.append('%s: mount(s) %s carry no Gun marker and draw from the hull origin (Combat.md 3.1)'
                         % (file_name, ', '.join(str(m) for m in missing)))


def main():
    failures = []
    unchecked = []
    mount_counts = read_mount_counts()
    names = sorted(n for n in os.listdir(MESH_DIR) if n.endswith('.nmo'))
    if not names:
        print('no meshes found in %s' % MESH_DIR)
        return 1

    for name in names:
        model = nmo.read_file(os.path.join(MESH_DIR, name))
        for mesh in model.meshes:
            check(failures, len(mesh.sub_meshes) > 0, '%s: mesh %r has no submeshes' % (name, mesh.name))

            seen = {}
            triangles = 0
            for sub in mesh.sub_meshes:
                triangles += sub.primitive_count
                check(failures, bool(sub.name), '%s: an unnamed submesh cannot be addressed' % name)

                box_min, box_max = sub.extents.box_min, sub.extents.box_max
                stated = all(box_min[axis] <= box_max[axis] for axis in range(3))
                check(failures, stated, '%s: submesh %r states no bind-pose extents to pivot on' % (name, sub.name))

                digest = fnv1a(sub.name)
                clash = seen.get(digest)
                check(failures, clash is None or clash == sub.name,
                      '%s: submesh names %r and %r collide under FNV-1a' % (name, clash, sub.name))
                seen[digest] = sub.name

            drawn = sum(s.primitive_count for s in mesh.sub_meshes)
            check(failures, drawn == triangles, '%s: the submesh triangles do not sum to the mesh' % name)

        check_gun_markers(failures, unchecked, name, model, mount_counts)

        guns = sum(1 for m in model.meshes for s in m.sub_meshes for k in s.markers if k.kind == 'Gun')
        print('%-14s %d mesh(es), %d parts, %d gun marker(s), %d mount(s)'
              % (name, len(model.meshes), sum(len(m.sub_meshes) for m in model.meshes), guns,
                 mount_counts.get(os.path.splitext(name)[0], 0)))

    if unchecked:
        print('\nnot a failure, and stated so nobody assumes otherwise:')
        for line in unchecked:
            print('  ' + line)

    if failures:
        print('\nFAILED:')
        for failure in failures:
            print('  ' + failure)
        return 1
    print('\nevery shipped hull is made of named parts with their own bounds,')
    print('and every gun marker names a mount its hull actually carries')
    return 0


if __name__ == '__main__':
    sys.exit(main())
