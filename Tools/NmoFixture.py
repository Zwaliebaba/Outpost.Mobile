#!/usr/bin/env python3
"""Builds the golden NMO fixture every test reads.

One model, two meshes, exercising every feature of Design/Archive/NmoFormat.md 5 at the smallest size
that still means something:

  "Gunship"  - two submeshes over one shared vertex buffer; two materials (one emissive,
               additive); a two-bone mesh skeleton with a mesh clip; a hull submesh skinned
               through a one-entry alias palette, with facet ids and Exhaust / NavLight / Gun
               markers; a turret submesh with a local skeleton (an alias root and a local barrel
               bone), its own SRT clip, and a bone-parented Gun marker.
  <unnamed>  - the degenerate shapes: no name at either level, U32 indices, a nonzero
               baseVertex/minVertex window with unreferenced vertices outside it, no bones, no
               markers, no facets, no skin.

The Python tests build the fixture; the C++ suite reads it as committed bytes at
Tests/NeuronClientTests/Assets/NmoFixture.nmo. That copy is the narrow, deliberate exception to
"nothing generated is committed" (AGENTS.md section 1) that Design/Archive/NmoFormat.md 15 D3 records:
this file is its generator and its diff is its review, so regenerating and comparing is part of
every slice that touches it. Coordinates are chosen exactly representable in float32 where flat,
so comparisons in the Blender round-trip test can be exact rather than tolerant where exactness is
cheap.

  python3 Tools/NmoFixture.py out.nmo    write the fixture to out.nmo
"""

import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'BlenderNmo'))
import NmoFormat as nmo


def _extents_of(vertices):
    extents = nmo.Extents()
    xs = [v.position[0] for v in vertices]
    ys = [v.position[1] for v in vertices]
    zs = [v.position[2] for v in vertices]
    extents.box_min = (min(xs), min(ys), min(zs))
    extents.box_max = (max(xs), max(ys), max(zs))
    centre = tuple((a + b) * 0.5 for a, b in zip(extents.box_min, extents.box_max))
    extents.centre = centre
    extents.radius = max(math.dist(centre, v.position) for v in vertices)
    return extents


def _box(centre, half, colour):
    """24 vertices (4 per face, flat normals) and 12 triangles, facet-major so two consecutive
    triangles share each facet id -- the shape the Blender importer rebuilds quads from."""
    cx, cy, cz = centre
    hx, hy, hz = half
    faces = [
        ((0.0, 0.0, 1.0), [(-hx, -hy, hz), (hx, -hy, hz), (hx, hy, hz), (-hx, hy, hz)]),
        ((0.0, 0.0, -1.0), [(hx, -hy, -hz), (-hx, -hy, -hz), (-hx, hy, -hz), (hx, hy, -hz)]),
        ((1.0, 0.0, 0.0), [(hx, -hy, hz), (hx, -hy, -hz), (hx, hy, -hz), (hx, hy, hz)]),
        ((-1.0, 0.0, 0.0), [(-hx, -hy, -hz), (-hx, -hy, hz), (-hx, hy, hz), (-hx, hy, -hz)]),
        ((0.0, 1.0, 0.0), [(-hx, hy, hz), (hx, hy, hz), (hx, hy, -hz), (-hx, hy, -hz)]),
        ((0.0, -1.0, 0.0), [(-hx, -hy, -hz), (hx, -hy, -hz), (hx, -hy, hz), (-hx, -hy, hz)]),
    ]
    uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
    vertices = []
    indices = []
    facets = []
    for facet, (normal, corners) in enumerate(faces):
        base = len(vertices)
        for corner, position in enumerate(corners):
            vertices.append(nmo.Vertex((position[0] + cx, position[1] + cy, position[2] + cz),
                                       normal, colour, uvs[corner]))
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
        facets += [facet, facet]
    return vertices, indices, facets


def _tetra(centre, size, colour):
    """12 vertices (3 per face, flat normals) and 4 triangles."""
    cx, cy, cz = centre
    apex = (cx, cy + size, cz)
    ring = [(cx + size, cy, cz), (cx - size * 0.5, cy, cz + size * 0.75),
            (cx - size * 0.5, cy, cz - size * 0.75)]
    corners = [(ring[0], ring[1], apex), (ring[1], ring[2], apex), (ring[2], ring[0], apex),
               (ring[0], ring[2], ring[1])]
    vertices = []
    indices = []
    for triangle in corners:
        a, b, c = triangle
        ux, uy, uz = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        vx, vy, vz = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
        normal = (uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx)
        length = math.sqrt(sum(n * n for n in normal)) or 1.0
        normal = tuple(n / length for n in normal)
        base = len(vertices)
        for position in triangle:
            vertices.append(nmo.Vertex(position, normal, colour, (0.0, 0.0)))
        indices += [base, base + 1, base + 2]
    return vertices, indices


def _translation(x, y, z):
    return (1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            x, y, z, 1.0)


def _gunship():
    mesh = nmo.Mesh('Gunship')

    plate = nmo.Material('HullPlate')
    plate.base_colour = (0.55, 0.6, 0.66, 1.0)
    glow = nmo.Material('GlowStripe')
    glow.base_colour = (0.25, 0.85, 0.4, 1.0)
    glow.emissive_colour = (0.25, 0.85, 0.4, 1.5)
    # Trim, so the faction paints it: base_colour is a shade under the multiply
    # (Design/Archive/NmoFormat.md 5.5). HullPlate stays unflagged, so the golden bytes carry both
    # values of the bit and a reader can be tested on each.
    glow.render_flags = nmo.RENDER_FLAG_ADDITIVE | nmo.RENDER_FLAG_RACE_TINTED
    mesh.materials = [plate, glow]

    root = nmo.Bone('Root')
    mount = nmo.Bone('TurretMount')
    mount.parent_index = 0
    mount.local_transform = _translation(0.0, 1.5, -2.0)
    mount.inv_bind_pose = _translation(0.0, -1.5, 2.0)
    mesh.bones = [root, mount]

    idle = nmo.Clip('Idle')
    idle.end_seconds = 2.0
    sway = nmo.Track(1)  # TurretMount
    sway.translation_keys = [(0.0, (0.0, 1.5, -2.0)), (1.0, (0.0, 1.75, -2.0)),
                             (2.0, (0.0, 1.5, -2.0))]
    idle.tracks = [sway]
    mesh.clips = [idle]

    hull_verts, hull_indices, hull_facets = _box((0.0, 1.0, 0.0), (2.0, 1.0, 4.0), 0xFF8C9AA8)
    turret_verts, turret_indices = _tetra((0.0, 2.0, -2.0), 1.0, 0xFF66D98F)

    vb = nmo.VertexBuffer(hull_verts + turret_verts)
    ib = nmo.IndexBuffer(nmo.INDEX_FORMAT_U16,
                         hull_indices + [i + len(hull_verts) for i in turret_indices])
    mesh.vertex_buffers = [vb]
    mesh.index_buffers = [ib]

    skins = nmo.SkinBuffer()
    for _ in hull_verts:  # the hull rides its palette's only entry (Root, aliased)
        skins.skins.append(nmo.SkinVertex((0, 0, 0, 0), (1.0, 0.0, 0.0, 0.0)))
    for index in range(len(turret_verts)):  # base ring on the alias, barrel tip on the local bone
        bone = 1 if index >= 9 else 0
        skins.skins.append(nmo.SkinVertex((bone, 0, 0, 0), (1.0, 0.0, 0.0, 0.0)))
    mesh.skin_buffers = [skins]

    hull = nmo.SubMesh('Hull')
    hull.material_index = 0
    hull.primitive_count = len(hull_indices) // 3
    hull.min_vertex = 0
    hull.vertex_count = len(hull_verts)
    hull.extents = _extents_of(hull_verts)
    hull.facets = hull_facets
    palette = nmo.Bone('Root')  # a pure-alias table: the palette configuration
    palette.mesh_bone_index = 0
    hull.bones = [palette]

    port = nmo.Marker('ExhaustPort', nmo.MARKER_KIND_EXHAUST)
    port.position = (-1.0, 1.0, -4.0)
    port.orientation = (0.0, 1.0, 0.0, 0.0)  # 180 degrees about +Y: the plume points -Z, aft
    port.scale = 0.5
    port.colour = (1.0, 0.6, 0.2, 1.0)
    port.flags = nmo.MARKER_FLAG_RACE_TINTED  # the flagged half of the pair; the other is not
    starboard = nmo.Marker('ExhaustStarboard', nmo.MARKER_KIND_EXHAUST)
    starboard.position = (1.0, 1.0, -4.0)
    starboard.orientation = (0.0, 1.0, 0.0, 0.0)
    starboard.scale = 0.5
    starboard.colour = (1.0, 0.6, 0.2, 1.0)
    nav_port = nmo.Marker('NavPort', nmo.MARKER_KIND_NAV_LIGHT)
    nav_port.position = (-2.0, 1.5, 0.0)
    nav_port.scale = 0.25
    nav_port.colour = (1.0, 0.1, 0.1, 1.0)
    nav_port.param0 = 2.0  # blink period, seconds
    nav_port.param1 = 0.0  # phase
    nav_starboard = nmo.Marker('NavStarboard', nmo.MARKER_KIND_NAV_LIGHT)
    nav_starboard.position = (2.0, 1.5, 0.0)
    nav_starboard.scale = 0.25
    nav_starboard.colour = (0.1, 1.0, 0.1, 1.0)
    nav_starboard.param0 = 2.0
    nav_starboard.param1 = 0.5
    bow_gun = nmo.Marker('BowGun', nmo.MARKER_KIND_GUN)
    bow_gun.position = (0.0, 1.0, 4.0)
    bow_gun.scale = 0.3
    hull.markers = [port, starboard, nav_port, nav_starboard, bow_gun]

    turret = nmo.SubMesh('Turret')
    turret.material_index = 1
    turret.start_index = len(hull_indices)
    turret.primitive_count = len(turret_indices) // 3
    turret.min_vertex = len(hull_verts)
    turret.vertex_count = len(turret_verts)
    turret.extents = _extents_of(turret_verts)
    mount_alias = nmo.Bone('TurretMount')  # local table row 0: an alias of mesh bone 1
    mount_alias.mesh_bone_index = 1
    mount_alias.local_transform = mount.local_transform
    mount_alias.inv_bind_pose = mount.inv_bind_pose
    barrel = nmo.Bone('Barrel')  # row 1: a genuinely local bone hung off the alias
    barrel.parent_index = 0
    barrel.local_transform = _translation(0.0, 0.5, 0.0)
    barrel.inv_bind_pose = _translation(0.0, -2.0, 2.0)
    turret.bones = [mount_alias, barrel]

    traverse = nmo.Clip('Traverse')
    traverse.end_seconds = 2.0
    swing = nmo.Track(1)  # Barrel
    half = math.sqrt(0.5)
    swing.rotation_keys = [(0.0, (0.0, 0.0, 0.0, 1.0)), (1.0, (0.0, half, 0.0, half)),
                           (2.0, (0.0, 1.0, 0.0, 0.0))]
    swing.scale_keys = [(0.0, 1.0), (2.0, 1.25)]
    traverse.tracks = [swing]
    turret.clips = [traverse]

    muzzle = nmo.Marker('Muzzle', nmo.MARKER_KIND_GUN)
    muzzle.position = (0.0, 3.0, -2.0)
    muzzle.scale = 0.2
    muzzle.parent_bone = 1  # rides the Barrel
    turret.markers = [muzzle]

    mesh.sub_meshes = [hull, turret]
    mesh.extents = _extents_of(vb.vertices)
    return mesh


def _probe():
    """Unnamed mesh, unnamed submesh, U32 indices, a nonzero baseVertex/minVertex window with two
    unreferenced vertices below it. The degenerate end of every optional feature."""
    mesh = nmo.Mesh(None)
    mesh.materials = [nmo.Material('ProbeSkin')]
    mesh.materials[0].base_colour = (0.8, 0.7, 0.2, 1.0)

    vertices = [nmo.Vertex((9.0, 9.0, 9.0)), nmo.Vertex((9.0, 9.0, 8.0))]  # padding, unreferenced
    triangle = [(0.0, 0.0, 0.0), (0.0, 1.0, 0.0), (1.0, 0.0, 0.0)]  # wound for its -Z normal (5.2)
    for position in triangle:
        vertices.append(nmo.Vertex(position, (0.0, 0.0, -1.0), 0xFFCCB233))
    mesh.vertex_buffers = [nmo.VertexBuffer(vertices)]
    mesh.index_buffers = [nmo.IndexBuffer(nmo.INDEX_FORMAT_U32, [0, 1, 2])]

    sub = nmo.SubMesh(None)
    sub.primitive_count = 1
    sub.base_vertex = 2
    sub.min_vertex = 2
    sub.vertex_count = 3
    sub.extents = _extents_of(vertices[2:])
    mesh.sub_meshes = [sub]
    mesh.extents = _extents_of(vertices)
    return mesh


def build_model():
    model = nmo.Model()
    model.meshes = [_gunship(), _probe()]
    return model


def main(argv):
    if len(argv) != 2:
        print('usage: NmoFixture.py out.nmo')
        return 2
    data = nmo.write_file(build_model(), argv[1])
    print('wrote %s (%d bytes)' % (argv[1], len(data)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
