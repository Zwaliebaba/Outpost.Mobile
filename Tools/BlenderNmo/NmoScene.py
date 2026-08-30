"""The Blender <-> NMO mapping, stated once so import and export cannot drift.

Everything that has to be exactly inverse between the two directions lives here: the axis
conversion, the quaternion and matrix conversion, the marker aim compensation, the frame/second
mapping, the canonical bone-table order, and the compatibility shim over Blender's animation API.

Space conversion (Design/Archive/NmoFormat.md 11): NMO is Y-up left-handed with the bow on +Z; Blender is
Z-up right-handed with, under this convention, the bow on +Y. One self-inverse component swap
(x, y, z) <-> (x, z, y) converts positions, normals and translations both ways. The swap is a
reflection, so:

  - Triangle winding reverses on each crossing. NMO wants cross(b - a, c - a) along the outward
    face normal (the clockwise-front rule of Design/Archive/NmoFormat.md 5.2 in that basis); Blender wants
    the same relation in its basis; the swap alone breaks it, so both directions also swap two
    triangle corners, and the double reversal cancels on a round trip.
  - A rotation R maps to S R S (S the swap), which for quaternions is the closed form
    (x, y, z, w) -> (-x, -z, -y, w) -- also self-inverse, and deterministic where a
    matrix-to-quaternion extraction would not be.

Marker aim: an NMO marker's direction is its local +Z (5.10). Blender's arrow and cone empties
draw along their local +Z too -- but the basis swap maps Blender's +Z to NMO's +Y. So the mapping
inserts a fixed local pre-rotation (+Z <-> +Y about X) on each crossing, and "point the arrow at
it" stays true on both sides.
"""

import bpy
import math

from mathutils import Matrix, Quaternion, Vector

# Custom-property names -- the add-on's schema on Blender objects. IDProperties, so they survive
# in the .blend and are visible (and editable) in the UI's Custom Properties panel.
PROP_KIND = 'nmo_kind'
PROP_PARAM0 = 'nmo_param0'
PROP_PARAM1 = 'nmo_param1'
PROP_MARKER_FLAGS = 'nmo_flags'
PROP_PALETTE = 'nmo_palette'
PROP_ALIAS = 'nmo_alias'
PROP_MESH_COLLECTION = 'nmo_mesh'
PROP_RENDER_FLAGS = 'nmo_render_flags'
PROP_TEXTURE = 'nmo_texture_%d'
PROP_SUBMESH = 'nmo_submesh'
PROP_SCOPE = 'nmo_scope'  # on armature objects: SCOPE_MESH or SCOPE_SUBMESH
PROP_OWNER = 'nmo_owner'  # on a marker empty bone-parented to the *mesh* rig: its submesh object

SCOPE_MESH = 'Mesh'
SCOPE_SUBMESH = 'SubMesh'

COLOUR_ATTRIBUTE = 'Colour'
UV_LAYER = 'UVMap'

MARKER_DISPLAY = {
    'Exhaust': 'CONE',
    'NavLight': 'SPHERE',
    'Gun': 'SINGLE_ARROW',
}
MARKER_DISPLAY_FALLBACK = 'PLAIN_AXES'

BONE_LENGTH = 0.5  # metres; display only, the format carries no bone length


# --- vectors, quaternions, matrices --------------------------------------------------------------

def swap(v):
    """(x, y, z) <-> (x, z, y). Its own inverse; works on tuples and mathutils Vectors."""
    return (v[0], v[2], v[1])


def quat_to_nmo(q):
    """Blender Quaternion (w, x, y, z) -> NMO (x, y, z, w) storage, basis-converted."""
    return (-q.x, -q.z, -q.y, q.w)


def quat_from_nmo(q):
    """NMO (x, y, z, w) -> Blender Quaternion. The same map read backwards."""
    return Quaternion((q[3], -q[0], -q[2], -q[1]))


# The marker aim compensation, a fixed local quarter-turn about X. With identity NMO orientation
# the imported empty carries Rx(-90), whose +Z arrow lands on Blender +Y -- the axis the basis
# swap maps to NMO +Z, the marker's direction. Export composes the inverse quarter-turn before
# converting, cancelling it exactly.
AIM_TO_NMO = Quaternion((1.0, 0.0, 0.0), math.radians(90.0))
AIM_FROM_NMO = AIM_TO_NMO.inverted()


def marker_quat_to_nmo(q):
    return quat_to_nmo(q @ AIM_TO_NMO)


def marker_quat_from_nmo(q):
    return quat_from_nmo(q) @ AIM_FROM_NMO


def matrix_to_nmo(m):
    """Blender 4x4 (column-vector) -> the 16 floats of an NMO XMFLOAT4X4 (row-vector, row-major).

    The two conventions transpose against each other, and the basis conjugation S M S swaps rows
    and columns 1 and 2; done together, element [r][c] of the NMO matrix is m[swapped c][swapped r].
    """
    s = (0, 2, 1, 3)
    return tuple(m[s[c]][s[r]] for r in range(4) for c in range(4))


def matrix_from_nmo(values):
    s = (0, 2, 1, 3)
    m = Matrix()
    for r in range(4):
        for c in range(4):
            m[s[c]][s[r]] = values[4 * r + c]
    return m


def transform_to_nmo(m):
    """Blender matrix -> NMO position + orientation quaternion + uniform scale (a marker's TRS)."""
    translation, rotation, scale = m.decompose()
    return swap(translation), marker_quat_to_nmo(rotation), scale.x


def transform_from_nmo(position, orientation, scale):
    m = marker_quat_from_nmo(orientation).to_matrix().to_4x4()
    m.translation = Vector(swap(position))
    return m @ Matrix.Diagonal((scale, scale, scale, 1.0))


# --- time ----------------------------------------------------------------------------------------

def fps(scene):
    return scene.render.fps / scene.render.fps_base


def to_frame(scene, seconds):
    return seconds * fps(scene)


def to_seconds(scene, frame):
    return frame / fps(scene)


# --- the animation API shim ----------------------------------------------------------------------
# Blender 4.4 introduced slotted actions and 5.0 removed legacy Action.fcurves. These two helpers
# are the only places either shape is named.

def action_fcurves_new(action, id_object):
    """Returns fcurves.new(data_path, index) for this action, creating the slot/layer/strip
    machinery on a slotted-actions Blender and using the flat collection on an older one. Also
    assigns the action (and slot, where slots exist) to id_object's animation data."""
    animation = id_object.animation_data or id_object.animation_data_create()
    animation.action = action
    if hasattr(action, 'layers'):  # 4.4+ slotted actions
        slot = action.slots.new(id_type='OBJECT', name=id_object.name)
        layer = action.layers.new('Layer') if not action.layers else action.layers[0]
        strip = layer.strips.new(type='KEYFRAME') if not layer.strips else layer.strips[0]
        channelbag = strip.channelbag(slot, ensure=True)
        animation.action_slot = slot
        return channelbag.fcurves
    return action.fcurves  # pre-slot Blender


def action_fcurves(action):
    """Every fcurve in the action, whatever the Blender. Reads all slots: this add-on writes one
    slot per action, and an artist-made action with several still yields its curves."""
    if hasattr(action, 'layers'):
        curves = []
        for layer in action.layers:
            for strip in layer.strips:
                for channelbag in strip.channelbags:
                    curves.extend(channelbag.fcurves)
        if curves or action.layers:
            return curves
    return list(getattr(action, 'fcurves', []))


# --- canonical bone-table order ------------------------------------------------------------------

def canonical_bone_order(alias_names, local_bones):
    """The exporter's table order: aliases first, in mesh-table order, then local bones with every
    parent before its children (stable, by name inside one depth). Deterministic, so a re-export
    reproduces the same indices -- skin weights, clip tracks and marker bindings all key on them."""
    order = list(alias_names)
    remaining = {bone.name: bone for bone in local_bones}
    placed = set(order)
    while remaining:
        ready = sorted(name for name, bone in remaining.items()
                       if bone.parent is None or bone.parent.name in placed
                       or bone.parent.name not in remaining)
        if not ready:  # a parent cycle cannot come out of Blender; defend anyway
            ready = sorted(remaining)
        for name in ready:
            order.append(name)
            placed.add(name)
            del remaining[name]
    return order


# --- polygons and facet ids ----------------------------------------------------------------------

def rebuild_polygons(triangles, facets):
    """Triangles plus facet ids -> polygon corner lists, walking each facet group's boundary in
    winding order. Triangles that were one polygon are consecutive and share an id (the exporter
    writes them that way); a group that does not reassemble into one simple loop falls back to its
    triangles -- malformed grouping degrades, never fails (Design/Archive/NmoFormat.md 8)."""
    polygons = []
    group = []
    group_id = None

    def flush():
        if not group:
            return
        if len(group) == 1:
            polygons.append(list(group[0]))
        else:
            polygons.extend(_walk_boundary(group))
        group.clear()

    for index, triangle in enumerate(triangles):
        facet = facets[index] if facets is not None else index
        if facet != group_id:
            flush()
            group_id = facet
        group.append(triangle)
    flush()
    return polygons


def _walk_boundary(triangles):
    forward = {}
    interior = set()
    for a, b, c in triangles:
        for edge in ((a, b), (b, c), (c, a)):
            if (edge[1], edge[0]) in forward:
                interior.add((edge[1], edge[0]))
                interior.add(edge)
            forward[edge[0], edge[1]] = edge[1]
    boundary = {}
    for (tail, head), _ in forward.items():
        if (tail, head) not in interior:
            if tail in boundary:  # a vertex leaving twice is not a simple polygon
                return [list(t) for t in triangles]
            boundary[tail] = head
    if not boundary:
        return [list(t) for t in triangles]
    start = next(iter(boundary))
    loop = [start]
    at = boundary[start]
    while at != start:
        loop.append(at)
        if len(loop) > len(boundary):
            return [list(t) for t in triangles]
        at = boundary.get(at)
        if at is None:
            return [list(t) for t in triangles]
    if len(loop) != len(boundary):
        return [list(t) for t in triangles]
    return [loop]


# --- misc ----------------------------------------------------------------------------------------

# Colour byte packing lives in NmoFormat so the OBJ converter and the tests share it without bpy.
from .NmoFormat import colour_from_bytes, colour_to_bytes  # noqa: F401  (re-exported)


def extents_of(positions, extents):
    """Fills an NmoFormat.Extents from NMO-space positions, the same way Tools/NmoFixture.py does,
    so a fixture round-tripped through Blender reproduces its own numbers."""
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


def link_object(collection, obj):
    collection.objects.link(obj)
    return obj
