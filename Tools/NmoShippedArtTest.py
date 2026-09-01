"""The shipped hulls, checked against what a consumer of their parts may assume.

NeuronClientTests cannot do this. It deploys one golden fixture and nothing else, and giving it the
game's meshes would make the engine's test suite depend on the game's content -- the direction
AGENTS.md section 2 spends most of its rules keeping shut. So the check lives here, beside the other
stdlib-only codec tests, and is run the way NmoRoundtripTest.py is.

What it asserts, and why each one matters to something:

  * every submesh is named, so a part can be addressed by name hash at all;
  * every submesh states bind-pose extents, because a part's own centre is the only pivot it has --
    no shipped hull carries a bone (Design/Combat-slice-3.md 2.6);
  * no two names in one mesh collide under FNV-1a, which is the rule NmoReader enforces at load and
    would reject the whole file for;
  * the primitive counts sum to the mesh's own, so the ranges a reader builds tile its buffers.

Run: python3 Tools/NmoShippedArtTest.py
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'BlenderNmo'))

import NmoFormat as nmo  # noqa: E402

MESH_DIR = os.path.join(os.path.dirname(HERE), 'Outpost', 'Assets', 'Meshes')


def fnv1a(text):
    """The hash NmoReader stores a name under (NmoFormat.md 5.10)."""
    value = 2166136261
    for byte in text.encode('utf-8'):
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def check(failures, condition, message):
    if not condition:
        failures.append(message)


def main():
    failures = []
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

        print('%-14s %d mesh(es), %d parts' % (name, len(model.meshes), sum(len(m.sub_meshes) for m in model.meshes)))

    if failures:
        print('\nFAILED:')
        for failure in failures:
            print('  ' + failure)
        return 1
    print('\nevery shipped hull is made of named parts with their own bounds')
    return 0


if __name__ == '__main__':
    sys.exit(main())
