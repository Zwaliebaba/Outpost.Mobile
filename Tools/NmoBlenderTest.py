#!/usr/bin/env python3
"""The Blender add-on's headless test: import -> scene checks -> export -> deep compare.

Runs inside any Blender 4.2+ (`blender --background --python Tools/NmoBlenderTest.py`) or under
the pip `bpy` wheel (`python3 Tools/NmoBlenderTest.py`). Prints SKIPPED and exits 0 where bpy is
not importable, so the codec test can gate a plain-python environment on its own.

Three phases:
  1. Import the golden fixture (Tools/NmoFixture.py) and assert the scene it should have built --
     the collections, rigs, palettes, alias constraints, NLA tracks, marker empties with their
     kinds, colours and parameters, and the axis semantics (the bow on +Y, an exhaust's arrow
     pointing aft).
  2. Export the scene and compare the resulting model against the original, structurally: same
     triangles (canonically rotated, winding preserved), same welded vertex counts, same facet
     partition, bones, clips (times, values, quaternions up to sign), markers, materials, skin
     weights by bone name. Byte-exactness is the codec test's job; through Blender the claim is
     semantic equality, and this is its definition.
  3. Convert a real hull (Corvette) with Tools/ObjToNmo.py and round-trip it the same way.
"""

import math
import os
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
sys.path.insert(0, os.path.join(TOOLS, 'BlenderNmo'))

try:
    import bpy
    from mathutils import Vector
except ImportError:
    print('NmoBlenderTest: SKIPPED (bpy is not importable here; run inside Blender or install '
          'the bpy wheel)')
    sys.exit(0)

import NmoFixture
import NmoFormat as nmo
import ObjToNmo
from NmoRoundtripTest import _cross_matches_normals

import BlenderNmo
from BlenderNmo import NmoExport, NmoImport
from BlenderNmo import NmoScene as scene_map

PASSED = 0


def check(name, condition, detail=''):
    global PASSED
    if not condition:
        raise SystemExit('FAIL %s%s' % (name, ': %s' % detail if detail else ''))
    PASSED += 1


def near(a, b, tolerance=1e-4):
    return all(abs(x - y) <= tolerance for x, y in zip(tuple(a), tuple(b))) \
        and len(tuple(a)) == len(tuple(b))


def quat_equivalent(a, b, tolerance=1e-4):
    dot = abs(sum(x * y for x, y in zip(a, b)))
    return abs(dot - 1.0) <= tolerance


# --- canonical geometry --------------------------------------------------------------------------

def _scope_names(mesh, sub):
    if sub.bones:
        return [bone.name for bone in sub.bones]
    return [bone.name for bone in mesh.bones]


def _canonical_triangles(mesh, sub):
    """Each triangle as an order-canonical tuple of full-attribute corners (weights resolved to
    bone names), plus the facet partition over those triangles."""
    vertices = mesh.vertex_buffers[sub.vertex_buffer_index].vertices
    indices = mesh.index_buffers[sub.index_buffer_index].indices
    skins = mesh.skin_buffers[sub.vertex_buffer_index].skins if mesh.skin_buffers else []
    names = _scope_names(mesh, sub)

    def corner(raw):
        vertex = vertices[raw]
        weights = ()
        if skins:
            skin = skins[raw]
            weights = tuple(sorted((names[index], round(weight, 3))
                                   for index, weight in zip(skin.bone_indices, skin.bone_weights)
                                   if weight > 0.0))
        return (tuple(round(v, 3) for v in vertex.position),
                tuple(round(v, 2) for v in vertex.normal),
                vertex.colour,
                tuple(round(v, 3) for v in vertex.uv),
                weights)

    triangles = []
    for t in range(sub.primitive_count):
        corners = [corner(indices[sub.start_index + 3 * t + c] + sub.base_vertex)
                   for c in range(3)]
        lowest = min(range(3), key=lambda i: corners[i])
        triangles.append(tuple(corners[lowest:] + corners[:lowest]))  # cyclic: winding preserved

    partition = {}
    for t, triangle in enumerate(triangles):
        facet = sub.facets[t] if sub.facets is not None else t
        partition.setdefault(facet, []).append(triangle)
    blocks = frozenset(frozenset(block) for block in partition.values())
    return sorted(triangles), blocks


def compare_meshes(label, before, after):
    check('%s: submesh count' % label, len(before.sub_meshes) == len(after.sub_meshes))
    if before.name is not None:
        check('%s: mesh name' % label, before.name == after.name,
              '%r != %r' % (before.name, after.name))

    for b_mat in before.materials:
        a_mat = next((m for m in after.materials if m.name == b_mat.name), None)
        check('%s: material %r survives' % (label, b_mat.name), a_mat is not None)
        check('%s: material %r base colour' % (label, b_mat.name),
              near(b_mat.base_colour, a_mat.base_colour, 1e-3))
        check('%s: material %r emissive' % (label, b_mat.name),
              near(b_mat.emissive_colour, a_mat.emissive_colour, 1e-3))
        check('%s: material %r flags' % (label, b_mat.name),
              b_mat.render_flags == a_mat.render_flags)
        check('%s: material %r textures' % (label, b_mat.name),
              b_mat.textures == a_mat.textures)

    def compare_bone_tables(what, b_bones, a_bones):
        check('%s: %s bone order' % (label, what),
              [b.name for b in b_bones] == [b.name for b in a_bones],
              '%r != %r' % ([b.name for b in b_bones], [b.name for b in a_bones]))
        for b_bone, a_bone in zip(b_bones, a_bones):
            check('%s: bone %r parent' % (label, b_bone.name),
                  b_bone.parent_index == a_bone.parent_index)
            check('%s: bone %r alias' % (label, b_bone.name),
                  b_bone.mesh_bone_index == a_bone.mesh_bone_index)
            check('%s: bone %r local transform' % (label, b_bone.name),
                  near(b_bone.local_transform, a_bone.local_transform, 1e-4))
            check('%s: bone %r inverse bind' % (label, b_bone.name),
                  near(b_bone.inv_bind_pose, a_bone.inv_bind_pose, 1e-4))

    def compare_clips(what, b_clips, a_clips):
        check('%s: %s clip names' % (label, what),
              sorted(c.name for c in b_clips) == sorted(c.name for c in a_clips),
              '%r != %r' % ([c.name for c in b_clips], [c.name for c in a_clips]))
        for b_clip in b_clips:
            a_clip = next(c for c in a_clips if c.name == b_clip.name)
            check('%s: clip %r span' % (label, b_clip.name),
                  abs(b_clip.start_seconds - a_clip.start_seconds) < 1e-3
                  and abs(b_clip.end_seconds - a_clip.end_seconds) < 1e-3)
            check('%s: clip %r track count' % (label, b_clip.name),
                  len(b_clip.tracks) == len(a_clip.tracks))
            for b_track, a_track in zip(b_clip.tracks, a_clip.tracks):
                check('%s: clip %r track bone' % (label, b_clip.name),
                      b_track.bone_index == a_track.bone_index)
                for series, b_keys, a_keys in (('translation', b_track.translation_keys,
                                                a_track.translation_keys),
                                               ('scale', b_track.scale_keys,
                                                a_track.scale_keys)):
                    check('%s: clip %r %s key count' % (label, b_clip.name, series),
                          len(b_keys) == len(a_keys))
                    for (b_time, b_value), (a_time, a_value) in zip(b_keys, a_keys):
                        check('%s: clip %r %s key' % (label, b_clip.name, series),
                              abs(b_time - a_time) < 1e-3
                              and near([b_value] if series == 'scale' else b_value,
                                       [a_value] if series == 'scale' else a_value, 1e-3))
                check('%s: clip %r rotation key count' % (label, b_clip.name),
                      len(b_track.rotation_keys) == len(a_track.rotation_keys))
                for (b_time, b_value), (a_time, a_value) in zip(b_track.rotation_keys,
                                                                a_track.rotation_keys):
                    check('%s: clip %r rotation key' % (label, b_clip.name),
                          abs(b_time - a_time) < 1e-3 and quat_equivalent(b_value, a_value, 1e-3))

    compare_bone_tables('mesh', before.bones, after.bones)
    compare_clips('mesh', before.clips, after.clips)

    for index, b_sub in enumerate(before.sub_meshes):
        a_sub = after.sub_meshes[index]
        label_sub = '%s %r' % (label, b_sub.name if b_sub.name is not None else index)
        if b_sub.name is not None:
            check('%s: name' % label_sub, b_sub.name == a_sub.name)
        check('%s: material' % label_sub,
              before.materials[b_sub.material_index].name
              == after.materials[a_sub.material_index].name)
        check('%s: triangle count' % label_sub, b_sub.primitive_count == a_sub.primitive_count)
        # Blender averages custom normals across each smooth fan, so corners whose normals
        # differed only by float noise come back identical and weld -- a round trip may hold
        # *fewer* vertices, never more (more would mean the mapping is splitting seams).
        check('%s: welded vertex count' % label_sub, a_sub.vertex_count <= b_sub.vertex_count,
              '%d grew past %d' % (a_sub.vertex_count, b_sub.vertex_count))
        b_tris, b_blocks = _canonical_triangles(before, b_sub)
        a_tris, a_blocks = _canonical_triangles(after, a_sub)
        check('%s: triangles' % label_sub, b_tris == a_tris)
        check('%s: facet partition' % label_sub, b_blocks == a_blocks)
        check('%s: extents' % label_sub,
              near(b_sub.extents.box_min, a_sub.extents.box_min, 1e-3)
              and near(b_sub.extents.box_max, a_sub.extents.box_max, 1e-3)
              and abs(b_sub.extents.radius - a_sub.extents.radius) < 1e-3)

        compare_bone_tables(label_sub, b_sub.bones, a_sub.bones)
        compare_clips(label_sub, b_sub.clips, a_sub.clips)

        b_names = _scope_names(before, b_sub)
        a_names = _scope_names(after, a_sub)
        check('%s: marker names' % label_sub,
              sorted(m.name for m in b_sub.markers) == sorted(m.name for m in a_sub.markers),
              '%r != %r' % ([m.name for m in b_sub.markers], [m.name for m in a_sub.markers]))
        for b_marker in b_sub.markers:
            a_marker = next(m for m in a_sub.markers if m.name == b_marker.name)
            what = '%s: marker %r' % (label_sub, b_marker.name)
            check('%s kind' % what, b_marker.kind == a_marker.kind)
            check('%s position' % what, near(b_marker.position, a_marker.position, 1e-4))
            check('%s orientation' % what,
                  quat_equivalent(b_marker.orientation, a_marker.orientation, 1e-4))
            check('%s scale' % what, abs(b_marker.scale - a_marker.scale) < 1e-4)
            check('%s colour' % what, near(b_marker.colour, a_marker.colour, 1e-3))
            check('%s params' % what, abs(b_marker.param0 - a_marker.param0) < 1e-4
                  and abs(b_marker.param1 - a_marker.param1) < 1e-4)
            check('%s flags' % what, b_marker.flags == a_marker.flags)
            b_bone = b_names[b_marker.parent_bone] if b_marker.parent_bone != nmo.NO_BONE else None
            a_bone = a_names[a_marker.parent_bone] if a_marker.parent_bone != nmo.NO_BONE else None
            check('%s bone binding' % what, b_bone == a_bone, '%r != %r' % (b_bone, a_bone))


def compare_models(label, before, after):
    check('%s: mesh count' % label, len(before.meshes) == len(after.meshes))
    for b_mesh, a_mesh in zip(before.meshes, after.meshes):
        compare_meshes('%s/%s' % (label, b_mesh.name), b_mesh, a_mesh)


# --- phase 1 and 2: the golden fixture through the scene -----------------------------------------

def object_by_name(name):
    obj = bpy.data.objects.get(name)
    check('object %r exists' % name, obj is not None)
    return obj


def test_fixture_import_export(scratch):
    model = NmoFixture.build_model()
    path = os.path.join(scratch, 'fixture.nmo')
    nmo.write_file(model, path)
    NmoImport.import_file(bpy.context, path)

    check('collections created', bpy.data.collections.get('Gunship') is not None
          and bpy.data.collections.get('fixture1') is not None)

    rig = object_by_name('GunshipRig')
    check('mesh rig scope tag', rig.get(scene_map.PROP_SCOPE) == scene_map.SCOPE_MESH)
    check('mesh skeleton bones', set(rig.data.bones.keys()) == {'Root', 'TurretMount'})
    check('TurretMount rest position', near(rig.data.bones['TurretMount'].head_local,
                                            (0.0, -2.0, 1.5), 1e-4))
    check('Idle on an NLA track', any(t.name == 'Idle' for t in rig.animation_data.nla_tracks))

    turret_rig = object_by_name('TurretRig')
    check('local rig scope tag', turret_rig.get(scene_map.PROP_SCOPE) == scene_map.SCOPE_SUBMESH)
    check('alias bone remembers its mesh bone',
          turret_rig.data.bones['TurretMount'].get(scene_map.PROP_ALIAS) == 'TurretMount')
    constraint = turret_rig.pose.bones['TurretMount'].constraints
    check('alias follows the mesh rig', len(constraint) == 1
          and constraint[0].type == 'COPY_TRANSFORMS' and constraint[0].target is rig
          and constraint[0].subtarget == 'TurretMount')
    check('Traverse on an NLA track',
          any(t.name == 'Traverse' for t in turret_rig.animation_data.nla_tracks))

    hull = object_by_name('Hull')
    check('palette is a property, not an armature',
          list(hull[scene_map.PROP_PALETTE]) == ['Root'])
    check('hull weights land in a vertex group', 'Root' in hull.vertex_groups)
    modifier = next((m for m in hull.modifiers if m.type == 'ARMATURE'), None)
    check('hull deforms through the mesh rig', modifier is not None and modifier.object is rig)
    check('hull rebuilt its quads', len(hull.data.polygons) == 6
          and all(len(p.vertices) == 4 for p in hull.data.polygons))

    turret = object_by_name('Turret')
    check('turret vertex groups', {'TurretMount', 'Barrel'} <= set(turret.vertex_groups.keys()))
    modifier = next((m for m in turret.modifiers if m.type == 'ARMATURE'), None)
    check('turret deforms through its local rig',
          modifier is not None and modifier.object is turret_rig)

    exhaust = object_by_name('ExhaustPort')
    check('exhaust display', exhaust.empty_display_type == 'CONE')
    check('exhaust kind', exhaust[scene_map.PROP_KIND] == 'Exhaust')
    check('exhaust colour on the object', near(tuple(exhaust.color), (1.0, 0.6, 0.2, 1.0), 1e-3))
    check('exhaust position (bow is +Y)', near(exhaust.matrix_world.translation,
                                               (-1.0, -4.0, 1.0), 1e-4))
    arrow = exhaust.matrix_world.to_quaternion() @ Vector((0.0, 0.0, 1.0))
    check('exhaust arrow points aft', near(arrow, (0.0, -1.0, 0.0), 1e-4),
          'arrow %r' % (tuple(arrow),))

    nav = object_by_name('NavStarboard')
    check('nav light display and blink', nav.empty_display_type == 'SPHERE'
          and abs(nav[scene_map.PROP_PARAM0] - 2.0) < 1e-6
          and abs(nav[scene_map.PROP_PARAM1] - 0.5) < 1e-6)

    muzzle = object_by_name('Muzzle')
    check('muzzle rides the barrel', muzzle.parent is turret_rig
          and muzzle.parent_type == 'BONE' and muzzle.parent_bone == 'Barrel')
    bind = NmoExport._marker_transform(muzzle, turret_rig)
    check('muzzle bind position', near(bind.translation, (0.0, -2.0, 3.0), 1e-4),
          'bind %r' % (tuple(bind.translation),))

    check('probe survived', bpy.data.objects.get('SubMesh0') is not None
          and len(bpy.data.objects['SubMesh0'].data.polygons) == 1)

    exported = NmoExport.export_model(bpy.context)
    data = nmo.write(exported)
    exported = nmo.read(data)  # the codec's whole validation list over the exporter's output
    for mesh in exported.meshes:
        check('exported winding satisfies the clockwise-front rule',
              _cross_matches_normals(mesh))
    compare_models('roundtrip', model, exported)


# --- phase 3: a real hull through the converter and the scene ------------------------------------

def test_converted_hull(scratch):
    source = os.path.join(os.path.dirname(TOOLS), 'Outpost', 'Assets', 'Meshes', 'Corvette.obj')
    data = ObjToNmo.convert(source)
    check('converter produced a model', data is not None)
    path = os.path.join(scratch, 'Corvette.nmo')
    with open(path, 'wb') as handle:
        handle.write(data)
    before = nmo.read(data)

    NmoImport.import_file(bpy.context, path)
    check('hull collection', bpy.data.collections.get('Corvette') is not None)
    check('hull exhaust markers imported',
          sum(1 for o in bpy.data.objects
              if o.type == 'EMPTY' and o.get(scene_map.PROP_KIND) == 'Exhaust') == 2)

    exported = nmo.read(nmo.write(NmoExport.export_model(bpy.context)))
    compare_models('corvette', before, exported)


PHASES = {
    'fixture': test_fixture_import_export,
    'corvette': test_converted_hull,
}


def main(argv):
    import subprocess
    import tempfile
    if len(argv) == 2 and argv[1] in PHASES:
        # One phase, one process: the scene starts as the wheel's default startup scene and is
        # never reset -- wm.read_factory_settings under the bpy module proved to corrupt the heap
        # intermittently, and a fresh interpreter per phase isolates better than any reset could.
        print('NmoBlenderTest[%s]: Blender %s' % (argv[1], bpy.app.version_string))
        BlenderNmo.register()
        with tempfile.TemporaryDirectory() as scratch:
            PHASES[argv[1]](scratch)
        BlenderNmo.unregister()
        print('NmoBlenderTest[%s]: %d checks passed' % (argv[1], PASSED))
        return 0
    for phase in PHASES:
        result = subprocess.run([sys.executable, os.path.abspath(__file__), phase])
        if result.returncode != 0:
            print('NmoBlenderTest: phase %r failed (%d)' % (phase, result.returncode))
            return 1
    print('NmoBlenderTest: all phases passed')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
