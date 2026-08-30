#!/usr/bin/env python3
"""Converts the shipped OBJ/MTL hulls to NMO (Design/Archive/NmoFormat.md 13).

A port of the engine's ObjParser conventions into a tool that runs once, instead of a heuristic
that runs in the shipping loader on every boot:

  - Same subset of OBJ read the same way: `v`, `usemtl`, `f` (fan-triangulated), `newmtl`/`Kd`;
    a malformed face is counted and skipped, a missing material falls back to the same grey.
  - Same axis fix: OBJ is right-handed with the bow on -Z; negating Z lands the bow on +Z in the
    left-handed render basis (NeuronClient/ObjParser.h). The negation is a reflection, so the
    converter also reverses each triangle's winding to satisfy the format's clockwise-front rule
    -- the current renderer culls nothing, but the file format is written for the one that will.
  - Same exhaust recovery, run for the last time: union-find over `thruster`-material face
    centroids with the link distance from the median attach-face edge (ObjParser.cpp), emitted as
    named `Exhaust` markers coloured by the thruster material's Kd and pointed aft. NavLight and
    Gun markers cannot come from an OBJ; they are authored in Blender afterwards.

One submesh per material, in first-use order, named for it; the material's Kd is also baked into
vertex colour, so the engine's existing flat-colour path draws the result identically. One facet
id per source OBJ face, so anything quad-shaped survives a Blender round trip (the shipped hulls
are already all triangles). Every written file is read back through the codec's full validation
before it counts as converted.

  python3 Tools/ObjToNmo.py Outpost/Assets/Meshes/*.obj          write .nmo beside each .obj
  python3 Tools/ObjToNmo.py --out Build Outpost/Assets/Meshes/Corvette.obj
"""

import argparse
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'BlenderNmo'))
import NmoFormat as nmo

ATTACH_MATERIAL = 'thruster'  # ObjParser::ATTACH_MATERIAL
DEFAULT_COLOUR = (0.7, 0.7, 0.7)  # ObjParser's DEFAULT_R/G/B
LINK_DISTANCE_FACTOR = 1.5  # multiple of the median attach-face edge, as ObjParser
DEGENERATE_EDGE = 1e-6
AFT = (0.0, 1.0, 0.0, 0.0)  # 180 degrees about +Y: local +Z (the plume) points aft
MIN_EXHAUST_RADIUS = 0.25  # metres; a floor for single-face nozzles


def _read_lines(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as handle:
            return handle.read().splitlines()
    except OSError as error:
        print('cannot open %s (%s)' % (path, error))
        return []


def read_materials(path):
    materials = {}
    current = None
    for line in _read_lines(path):
        parts = line.split()
        if len(parts) >= 2 and parts[0] == 'newmtl':
            current = line.split(None, 1)[1]
            materials[current] = DEFAULT_COLOUR
        elif len(parts) >= 4 and parts[0] == 'Kd' and current is not None:
            try:
                materials[current] = (float(parts[1]), float(parts[2]), float(parts[3]))
            except ValueError:
                pass
    return materials


def _face_index(token, count):
    """'12/34/56', '12//56' and '12' all reference position 12; 0 or out of range is a bad face."""
    head = token.split('/', 1)[0]
    try:
        value = int(head)
    except ValueError:
        return -1
    if value < 0:  # OBJ negative indexing: -1 is the last vertex written so far
        value += count + 1
    return value - 1 if 1 <= value <= count else -1


def read_obj(path, materials):
    """Positions Z-negated into render space; faces fan-triangulated per material, source-face
    ids kept. Returns {material: [(triangle positions, facet id), ...]} in first-use order."""
    positions = []
    per_material = {}
    order = []
    current = None
    bad_faces = 0
    facet = 0
    for line in _read_lines(path):
        parts = line.split()
        if not parts:
            continue
        if parts[0] == 'v' and len(parts) >= 4:
            try:
                positions.append((float(parts[1]), float(parts[2]), -float(parts[3])))
            except ValueError:
                positions.append((0.0, 0.0, 0.0))
        elif parts[0] == 'usemtl':
            current = line.split(None, 1)[1] if len(parts) >= 2 else None
        elif parts[0] == 'f':
            corners = [_face_index(token, len(positions)) for token in parts[1:]]
            if len(corners) < 3 or any(index < 0 for index in corners):
                bad_faces += 1
                continue
            if current not in per_material:
                per_material[current] = []
                order.append(current)
            for fan in range(1, len(corners) - 1):
                triangle = (positions[corners[0]], positions[corners[fan]],
                            positions[corners[fan + 1]])
                per_material[current].append((triangle, facet))
            facet += 1
    return {name: per_material[name] for name in order}, bad_faces


def cluster_exhausts(triangles):
    """ObjParser::ClusterAttachPoints, ported: single-link union-find over the attach faces'
    centroids, link distance from the median edge. Returns (centre, radius) per nozzle, radius
    from the cluster's own spread so the marker seeds a sensibly sized plume."""
    centroids = []
    edges = []
    for (a, b, c), _facet in triangles:
        centroids.append(tuple((a[i] + b[i] + c[i]) / 3.0 for i in range(3)))
        for tail, head in ((a, b), (b, c), (c, a)):
            edges.append(math.dist(tail, head))
    if not centroids:
        return []
    edges = [edge for edge in edges if edge > DEGENERATE_EDGE]
    if not edges:
        return [(centroids[0], MIN_EXHAUST_RADIUS)]
    edges.sort()
    link = edges[len(edges) // 2] * LINK_DISTANCE_FACTOR

    parent = list(range(len(centroids)))

    def find(index):
        while parent[index] != index:
            parent[index] = parent[parent[index]]
            index = parent[index]
        return index

    for i in range(len(centroids)):
        for j in range(i + 1, len(centroids)):
            if math.dist(centroids[i], centroids[j]) <= link and find(i) != find(j):
                parent[find(i)] = find(j)

    clusters = {}
    for index, centroid in enumerate(centroids):  # first-seen order, as the engine
        clusters.setdefault(find(index), []).append(centroid)
    nozzles = []
    for members in clusters.values():
        centre = tuple(sum(member[i] for member in members) / len(members) for i in range(3))
        spread = max((math.dist(centre, member) for member in members), default=0.0)
        nozzles.append((centre, max(spread, edges[len(edges) // 2] * 0.5, MIN_EXHAUST_RADIUS)))
    return nozzles


def _flat_normal(a, b, c):
    u = tuple(b[i] - a[i] for i in range(3))
    v = tuple(c[i] - a[i] for i in range(3))
    normal = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    length = math.sqrt(sum(n * n for n in normal))
    if length <= 0.0:
        return None
    return tuple(n / length for n in normal)


def _extents_of(positions, extents):
    xs = [p[0] for p in positions]
    ys = [p[1] for p in positions]
    zs = [p[2] for p in positions]
    if not xs:
        return extents
    extents.box_min = (min(xs), min(ys), min(zs))
    extents.box_max = (max(xs), max(ys), max(zs))
    centre = tuple((a + b) * 0.5 for a, b in zip(extents.box_min, extents.box_max))
    extents.centre = centre
    extents.radius = max(math.dist(centre, p) for p in positions)
    return extents


def convert(obj_path):
    """One OBJ/MTL pair -> one validated single-mesh Model, or None with a report."""
    name = os.path.splitext(os.path.basename(obj_path))[0]
    materials = read_materials(os.path.splitext(obj_path)[0] + '.mtl')
    per_material, bad_faces = read_obj(obj_path, materials)
    if not per_material:
        print('%s: no faces, skipped' % name)
        return None

    mesh = nmo.Mesh(name)
    vb = nmo.VertexBuffer()
    ib = nmo.IndexBuffer()
    mesh.vertex_buffers = [vb]
    mesh.index_buffers = [ib]

    for material_name, triangles in per_material.items():
        colour = materials.get(material_name, DEFAULT_COLOUR)
        material = nmo.Material(material_name if material_name is not None else 'Default')
        material.base_colour = (*colour, 1.0)
        packed = nmo.colour_to_bytes(material.base_colour)

        sub = nmo.SubMesh(material.name)
        sub.material_index = len(mesh.materials)
        mesh.materials.append(material)
        sub.start_index = len(ib.indices)
        sub.min_vertex = len(vb.vertices)
        sub.facets = []
        remap = {}
        degenerate = 0
        for (a, b, c), facet in triangles:
            normal = _flat_normal(a, c, b)  # winding reversed with the Z negation, normals too
            if normal is None:
                degenerate += 1
                normal = (0.0, 1.0, 0.0)
            for position in (a, c, b):  # reversed: the Z negation flipped the winding
                key = (position, normal)
                index = remap.get(key)
                if index is None:
                    index = len(vb.vertices) - sub.min_vertex
                    remap[key] = index
                    vb.vertices.append(nmo.Vertex(position, normal, packed))
                ib.indices.append(index + sub.min_vertex)
            sub.facets.append(facet)
        sub.primitive_count = len(triangles)
        sub.vertex_count = len(vb.vertices) - sub.min_vertex
        _extents_of([v.position for v in vb.vertices[sub.min_vertex:]], sub.extents)

        if material_name == ATTACH_MATERIAL:
            for nozzle, (centre, radius) in enumerate(cluster_exhausts(triangles)):
                marker = nmo.Marker('Exhaust%d' % nozzle, nmo.MARKER_KIND_EXHAUST)
                marker.position = centre
                marker.orientation = AFT
                marker.scale = radius
                marker.colour = material.base_colour
                sub.markers.append(marker)
        mesh.sub_meshes.append(sub)

    ib.index_format = (nmo.INDEX_FORMAT_U16 if len(vb.vertices) <= 0xFFFF
                       else nmo.INDEX_FORMAT_U32)
    _extents_of([v.position for v in vb.vertices], mesh.extents)

    model = nmo.Model()
    model.meshes = [mesh]
    data = nmo.write(model)
    nmo.read(data)  # never hand over a file the loader would refuse

    exhausts = sum(len(sub.markers) for sub in mesh.sub_meshes)
    print('%s: %d tris, %d verts (was %d), %d submesh(es), %d exhaust marker(s)%s'
          % (name, len(ib.indices) // 3, len(vb.vertices), 3 * (len(ib.indices) // 3),
             len(mesh.sub_meshes), exhausts,
             ', %d bad face(s) skipped' % bad_faces if bad_faces else ''))
    return data


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('inputs', nargs='+', help='.obj files (each with its .mtl beside it)')
    parser.add_argument('--out', help='directory for the .nmo files (default: beside each input)')
    args = parser.parse_args(argv[1:])

    failures = 0
    for obj_path in args.inputs:
        data = convert(obj_path)
        if data is None:
            failures += 1
            continue
        name = os.path.splitext(os.path.basename(obj_path))[0] + '.nmo'
        out_path = os.path.join(args.out, name) if args.out else \
            os.path.join(os.path.dirname(obj_path), name)
        with open(out_path, 'wb') as handle:
            handle.write(data)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
