"""Blender scene -> NMO.

The inverse of NmoImport through the shared NmoScene mapping. The exporter never trusts itself:
the operator in __init__ writes the model to bytes and runs the codec's full validation over them
before the file lands on disk, so a mapping bug becomes a refused export naming the broken rule
rather than a broken file.

Scene assumptions, stated rather than discovered (Design/Archive/NmoFormat.md 11):
  - A mesh is a collection tagged by import (PROP_MESH_COLLECTION), or the active collection.
  - Object transforms are applied into the data; nothing here supports a scaled or rotated
    armature object against its submeshes.
  - Pose-bone animation uses quaternion rotation; scale is exported uniform, from its X channel.
  - Weights, clips and marker bindings resolve by bone *name* against the exporter's canonical
    table order (NmoScene.canonical_bone_order), so re-exports reproduce the same indices.
"""

import bpy

from mathutils import Matrix, Quaternion, Vector

from . import NmoFormat as nmo
from . import NmoScene as scene_map


def _mesh_collections(context):
    tagged = [c for c in bpy.data.collections if scene_map.PROP_MESH_COLLECTION in c]
    if tagged:
        return sorted(tagged, key=lambda c: (c[scene_map.PROP_MESH_COLLECTION], c.name))
    active = context.collection
    if active is not None and any(o.type == 'MESH' for o in active.objects):
        return [active]
    raise nmo.NmoError('nothing to export: no collection tagged by an NMO import and the active '
                       'collection holds no mesh object')


def _armatures(collection):
    mesh_rig = None
    local_rigs = []
    for obj in collection.objects:
        if obj.type != 'ARMATURE':
            continue
        scope = obj.get(scene_map.PROP_SCOPE)
        if scope == scene_map.SCOPE_SUBMESH:
            local_rigs.append(obj)
        elif scope == scene_map.SCOPE_MESH or mesh_rig is None:
            mesh_rig = obj
    return mesh_rig, local_rigs


def _local_rest(rig, bone):
    if bone.parent is not None:
        return bone.parent.matrix_local.inverted() @ bone.matrix_local
    return bone.matrix_local.copy()


def _bone_record(rig, bone, parent_index, mesh_bone_index):
    record = nmo.Bone(bone.name)
    record.parent_index = parent_index
    record.mesh_bone_index = mesh_bone_index
    record.local_transform = scene_map.matrix_to_nmo(_local_rest(rig, bone))
    record.inv_bind_pose = scene_map.matrix_to_nmo(
        (rig.matrix_world @ bone.matrix_local).inverted())
    return record


def _mesh_bone_table(rig):
    """The mesh skeleton in canonical order: every parent before its children, stable by name."""
    if rig is None:
        return [], []
    order = scene_map.canonical_bone_order([], list(rig.data.bones))
    index_of = {name: index for index, name in enumerate(order)}
    records = []
    for name in order:
        bone = rig.data.bones[name]
        parent = index_of[bone.parent.name] if bone.parent is not None else nmo.NO_PARENT
        records.append(_bone_record(rig, bone, parent, nmo.NO_BONE))
    return records, order


def _local_bone_table(rig, mesh_records, mesh_order):
    """A submesh rig's table: aliases first in mesh-table order, carrying copies of the mesh
    bone's matrices (their stored transforms are inert -- Design/Archive/NmoFormat.md 5.8), then the local
    bones, parents first."""
    aliases = []
    locals_ = []
    for bone in rig.data.bones:
        alias_of = bone.get(scene_map.PROP_ALIAS)
        if alias_of is not None:
            if alias_of not in mesh_order:
                raise nmo.NmoError('bone %r aliases %r, which the mesh skeleton does not have'
                                   % (bone.name, alias_of))
            aliases.append((mesh_order.index(alias_of), bone))
        else:
            locals_.append(bone)
    aliases.sort(key=lambda pair: pair[0])
    order = scene_map.canonical_bone_order([bone.name for _, bone in aliases], locals_)
    index_of = {name: index for index, name in enumerate(order)}

    records = []
    for name in order:
        bone = rig.data.bones[name]
        parent = index_of.get(bone.parent.name, nmo.NO_PARENT) if bone.parent else nmo.NO_PARENT
        alias_of = bone.get(scene_map.PROP_ALIAS)
        if alias_of is not None:
            mesh_index = mesh_order.index(alias_of)
            record = nmo.Bone(bone.name)
            record.parent_index = parent
            record.mesh_bone_index = mesh_index
            record.local_transform = mesh_records[mesh_index].local_transform
            record.inv_bind_pose = mesh_records[mesh_index].inv_bind_pose
            records.append(record)
        else:
            records.append(_bone_record(rig, bone, parent, nmo.NO_BONE))
    return records, order


# --- clips ---------------------------------------------------------------------------------------

def _clip_sources(rig):
    """(name, action) per clip: one per NLA track (the imported layout), plus the active action
    when it sits on no track -- the state an artist is usually in while authoring."""
    sources = []
    animation = rig.animation_data
    if animation is None:
        return sources
    tracked = set()
    for track in animation.nla_tracks:
        for strip in track.strips:
            if strip.action is not None:
                sources.append((track.name, strip.action))
                tracked.add(strip.action.name)
                break
    if animation.action is not None and animation.action.name not in tracked:
        sources.append((animation.action.name, animation.action))
    return sources


def _bone_curves(action):
    """data_path components per bone: {bone: {channel: {array_index: fcurve}}}."""
    per_bone = {}
    for curve in scene_map.action_fcurves(action):
        path = curve.data_path
        if not path.startswith('pose.bones["'):
            continue
        try:
            bone, channel = path[len('pose.bones["'):].split('"].', 1)
        except ValueError:
            continue
        per_bone.setdefault(bone, {}).setdefault(channel, {})[curve.array_index] = curve
    return per_bone


def _key_times(curves):
    times = set()
    for curve in curves:
        for point in curve.keyframe_points:
            times.add(point.co[0])
    return sorted(times)


def _export_clip(scene, name, action, rig, order):
    clip = nmo.Clip(name)
    per_bone = _bone_curves(action)
    span = []
    for bone_index, bone_name in enumerate(order):
        channels = per_bone.get(bone_name)
        if not channels or bone_name not in rig.data.bones:
            continue
        rest = _local_rest(rig, rig.data.bones[bone_name])
        rest_translation, rest_rotation, _ = rest.decompose()

        track = nmo.Track(bone_index)
        location = channels.get('location', {})
        for frame in _key_times(location.values()):
            basis = Vector([location[a].evaluate(frame) if a in location else 0.0
                            for a in range(3)])
            pose = rest_rotation @ basis + rest_translation
            track.translation_keys.append((scene_map.to_seconds(scene, frame),
                                           scene_map.swap(pose)))
        rotation = channels.get('rotation_quaternion', {})
        for frame in _key_times(rotation.values()):
            basis = Quaternion([rotation[a].evaluate(frame) if a in rotation
                                else (1.0 if a == 0 else 0.0) for a in range(4)])
            basis.normalize()
            track.rotation_keys.append((scene_map.to_seconds(scene, frame),
                                        scene_map.quat_to_nmo(rest_rotation @ basis)))
        scale = channels.get('scale', {})
        for frame in _key_times(scale.values()):
            value = scale[0].evaluate(frame) if 0 in scale else 1.0
            track.scale_keys.append((scene_map.to_seconds(scene, frame), value))

        if track.translation_keys or track.rotation_keys or track.scale_keys:
            clip.tracks.append(track)
            for keys in (track.translation_keys, track.rotation_keys, track.scale_keys):
                span.extend(key[0] for key in keys)
    clip.start_seconds = min(span) if span else 0.0
    clip.end_seconds = max(span) if span else 0.0
    return clip


def _export_clips(scene, rig, order):
    return [_export_clip(scene, name, action, rig, order)
            for name, action in _clip_sources(rig)]


# --- geometry ------------------------------------------------------------------------------------

def _vertex_weights(obj, scope_index_of):
    """Per Blender vertex: up to BONE_INFLUENCES (index, weight) against the scope table, sorted
    descending and normalized to sum 1 -- the writer half of the influence rule (5.6)."""
    group_to_scope = {}
    for group in obj.vertex_groups:
        if group.name in scope_index_of:
            group_to_scope[group.index] = scope_index_of[group.name]
    weights = []
    for vertex in obj.data.vertices:
        influences = [(group_to_scope[g.group], g.weight) for g in vertex.groups
                      if g.group in group_to_scope and g.weight > 0.0]
        influences.sort(key=lambda pair: (-pair[1], pair[0]))
        influences = influences[:nmo.BONE_INFLUENCES]
        total = sum(weight for _, weight in influences)
        if total > 0.0:
            influences = [(index, weight / total) for index, weight in influences]
        weights.append(influences)
    return weights


def _submesh_geometry(obj, weights):
    """Triangulated, welded, axis-crossed geometry: per-corner tuples keyed on the whole
    attribute set (weights included -- two corners may only weld when every consumer agrees),
    triangles facet-major so quads survive (Design/Archive/NmoFormat.md 8)."""
    data = obj.data
    if hasattr(data, 'calc_loop_triangles'):
        data.calc_loop_triangles()
    world = obj.matrix_world
    normal_world = world.inverted_safe().transposed().to_3x3()
    uv_layer = data.uv_layers.active
    colours = None
    point_colours = None
    if data.color_attributes:
        found = (data.color_attributes.get(scene_map.COLOUR_ATTRIBUTE)
                 or data.color_attributes.active_color or data.color_attributes[0])
        if found.domain == 'CORNER':
            colours = found
        else:  # a POINT-domain paint layer indexes by vertex instead of by corner
            point_colours = found
    corner_normals = data.corner_normals

    vertices = []
    remap = {}
    skins = []
    triangles = []
    facets = []
    for loop_triangle in data.loop_triangles:
        corner_indices = []
        for loop_index in loop_triangle.loops:
            loop = data.loops[loop_index]
            position = scene_map.swap(world @ data.vertices[loop.vertex_index].co)
            normal = scene_map.swap((normal_world @ Vector(corner_normals[loop_index].vector))
                                    .normalized())
            if colours is not None:
                colour = scene_map.colour_to_bytes(colours.data[loop_index].color_srgb)
            elif point_colours is not None:
                colour = scene_map.colour_to_bytes(point_colours.data[loop.vertex_index].color_srgb)
            else:
                colour = 0xFFFFFFFF
            if uv_layer is not None:
                u, v = uv_layer.data[loop_index].uv
                uv = (u, 1.0 - v)
            else:
                uv = (0.0, 0.0)
            influence = tuple(weights[loop.vertex_index]) if weights is not None else ()
            key = (tuple(position), tuple(normal), colour, uv, influence)
            found = remap.get(key)
            if found is None:
                found = len(vertices)
                remap[key] = found
                vertices.append(nmo.Vertex(tuple(position), tuple(normal), colour, uv))
                if weights is not None:
                    skins.append(influence)
            corner_indices.append(found)
        triangles.append((corner_indices[0], corner_indices[2], corner_indices[1]))  # re-wind
        facets.append(loop_triangle.polygon_index)
    return vertices, skins, triangles, facets


def _export_material(mat):
    material = nmo.Material(mat.name if mat is not None else 'Default')
    if mat is None:
        return material
    material.base_colour = tuple(mat.diffuse_color)
    if mat.use_nodes:
        principled = next((n for n in mat.node_tree.nodes if n.type == 'BSDF_PRINCIPLED'), None)
        if principled is not None:
            material.base_colour = tuple(principled.inputs['Base Color'].default_value)
            if 'Emission Color' in principled.inputs:
                emission = principled.inputs['Emission Color'].default_value
                strength = principled.inputs['Emission Strength'].default_value
                if strength > 0.0:
                    material.emissive_colour = (emission[0], emission[1], emission[2], strength)
    material.render_flags = int(mat.get(scene_map.PROP_RENDER_FLAGS, 0))
    material.textures = [str(mat.get(scene_map.PROP_TEXTURE % slot, ''))
                         for slot in range(nmo.TEXTURE_SLOTS)]
    return material


def _marker_transform(empty, rig):
    """The marker's mesh-space bind-pose transform. A bone-parented empty is reconstructed from
    stored data only -- the bone's rest matrix and the empty's own parent-relative matrices --
    never from an evaluated pose, so a rig animated by its NLA at export time cannot bake the
    current frame into the marker (the inverse of NmoImport's placement)."""
    if empty.parent_type == 'BONE' and empty.parent is not None and empty.parent_bone:
        bone = rig.data.bones.get(empty.parent_bone) if rig is not None else None
        if bone is not None:
            tail = Matrix.Translation((0.0, bone.length, 0.0))
            local = empty.matrix_parent_inverse @ empty.matrix_basis
            return rig.matrix_world @ bone.matrix_local @ tail @ local
    return empty.matrix_world.copy()


def _export_marker(empty, scope_index_of, rig):
    marker = nmo.Marker(empty.name, str(empty.get(scene_map.PROP_KIND, '')))
    position, orientation, scale = scene_map.transform_to_nmo(_marker_transform(empty, rig))
    marker.position = tuple(position)
    marker.orientation = orientation
    marker.scale = scale
    marker.colour = tuple(empty.color)
    marker.param0 = float(empty.get(scene_map.PROP_PARAM0, 0.0))
    marker.param1 = float(empty.get(scene_map.PROP_PARAM1, 0.0))
    marker.flags = int(empty.get(scene_map.PROP_MARKER_FLAGS, 0))
    if empty.parent_type == 'BONE' and empty.parent_bone:
        index = scope_index_of.get(empty.parent_bone)
        if index is None:
            raise nmo.NmoError('marker %r rides bone %r, which its submesh scope does not hold'
                               % (empty.name, empty.parent_bone))
        marker.parent_bone = index
    return marker


def _gather_markers(collection, sub_objects, local_rig_of, mesh_rig):
    """empty -> submesh object, resolved the way import laid them out: object parent, owning
    local rig, or PROP_OWNER for mesh-skeleton bindings.

    A marker that matches none of those -- an artist's empty dropped at collection level, or one
    that arrived from another format parented under a group node -- falls to the first submesh
    rather than to nothing. The format has no mesh-level marker list to put it in, and a marker
    with no owner used to be skipped by the caller's owner test, which silently deleted authored
    data on the way to a file the codec then validated as fine. Losing where a rigid marker was
    filed costs nothing (its position is mesh space either way); losing the marker costs the
    exhaust that no longer glows."""
    owners = {}
    rig_owner = {rig.name: name for name, rig in local_rig_of.items()}
    by_name = {obj.name: obj for obj in sub_objects}
    fallback = sub_objects[0] if sub_objects else None
    for obj in collection.objects:
        if obj.type != 'EMPTY' or scene_map.PROP_KIND not in obj:
            continue
        if obj.parent is not None and obj.parent.name in by_name:
            owners[obj] = by_name[obj.parent.name]
        elif obj.parent is not None and obj.parent.name in rig_owner:
            owners[obj] = by_name.get(rig_owner[obj.parent.name], fallback)
        elif mesh_rig is not None and obj.parent is mesh_rig:
            owner = obj.get(scene_map.PROP_OWNER)
            owners[obj] = by_name.get(owner, fallback) if owner else fallback
        else:
            owners[obj] = fallback
    return owners


def _export_mesh(context, collection):
    scene = context.scene
    mesh = nmo.Mesh(collection.name)
    mesh_rig, local_rigs = _armatures(collection)
    mesh.bones, mesh_order = _mesh_bone_table(mesh_rig)

    sub_objects = sorted((o for o in collection.objects if o.type == 'MESH'),
                        key=lambda o: (o.get(scene_map.PROP_SUBMESH, 1 << 30), o.name))
    if not sub_objects:
        raise nmo.NmoError('collection %r holds no mesh object' % collection.name)

    # Which local rig belongs to which submesh: the armature modifier's target when it is tagged
    # as a submesh scope, else a rig named "<object>Rig" (the imported layout).
    local_rig_of = {}
    for obj in sub_objects:
        for modifier in obj.modifiers:
            if modifier.type == 'ARMATURE' and modifier.object in local_rigs:
                local_rig_of[obj.name] = modifier.object
        if obj.name not in local_rig_of:
            for rig in local_rigs:
                if rig.name == obj.name + 'Rig':
                    local_rig_of[obj.name] = rig

    if mesh_rig is not None:
        mesh.clips = _export_clips(scene, mesh_rig, mesh_order)

    material_index = {}
    marker_owner = _gather_markers(collection, sub_objects, local_rig_of, mesh_rig)

    built = []  # (sub, vertices, skins, triangles, facets, skinned)
    row2_prefixes = {obj.name for obj in sub_objects}
    for obj in sub_objects:
        sub = nmo.SubMesh(obj.name)

        mat = obj.data.materials[0] if obj.data.materials else None
        mat_name = mat.name if mat is not None else 'Default'
        if mat_name not in material_index:
            material_index[mat_name] = len(mesh.materials)
            mesh.materials.append(_export_material(mat))
        sub.material_index = material_index[mat_name]

        local_rig = local_rig_of.get(obj.name)
        scope_names = mesh_order
        if scene_map.PROP_PALETTE in obj:
            palette = [str(name) for name in obj[scene_map.PROP_PALETTE]]
            for name in palette:
                if name not in mesh_order:
                    raise nmo.NmoError('palette of %r names %r, which the mesh skeleton does not '
                                       'hold' % (obj.name, name))
                record = nmo.Bone(name)
                record.mesh_bone_index = mesh_order.index(name)
                record.local_transform = mesh.bones[record.mesh_bone_index].local_transform
                record.inv_bind_pose = mesh.bones[record.mesh_bone_index].inv_bind_pose
                sub.bones.append(record)
            scope_names = palette
        elif local_rig is not None:
            sub.bones, scope_names = _local_bone_table(local_rig, mesh.bones, mesh_order)
            sub.clips = _export_clips(scene, local_rig, scope_names)

        scope_index_of = {name: index for index, name in enumerate(scope_names)}
        weights = _vertex_weights(obj, scope_index_of) if scope_index_of else None
        vertices, skins, triangles, facets = _submesh_geometry(obj, weights)
        skinned = weights is not None and any(influences for influences in skins)

        for empty, owner in marker_owner.items():
            if owner is not obj:
                continue
            rig = local_rig if local_rig is not None else mesh_rig
            sub.markers.append(_export_marker(empty, scope_index_of, rig))

        scene_map.extents_of([v.position for v in vertices], sub.extents)
        built.append((sub, vertices, skins, triangles, facets, skinned))
        mesh.sub_meshes.append(sub)

    # Mesh-skeleton clips named "<submesh>:<clip>" fold back to their submesh (7, row 2).
    if mesh.clips:
        kept = []
        for clip in mesh.clips:
            prefix, _, rest = clip.name.partition(':')
            target = next((entry[0] for entry in built if entry[0].name == prefix), None)
            if rest and target is not None and prefix in row2_prefixes:
                clip.name = rest
                target.clips.append(clip)
            else:
                kept.append(clip)
        mesh.clips = kept

    _pack_buffers(mesh, built)
    positions = [vertex.position
                 for buffer in mesh.vertex_buffers for vertex in buffer.vertices]
    scene_map.extents_of(positions, mesh.extents)
    return mesh


def _pack_buffers(mesh, built):
    """One vertex and one index buffer per rigidity class -- rigid submeshes must not share a
    buffer with skinned ones, or validation rule 5.12.8 would find zero-weight vertices inside a
    skinned submesh's window. Windows stay contiguous because welding is per submesh."""
    any_skinned = any(entry[5] for entry in built)
    any_rigid = any(not entry[5] for entry in built)
    layout = {}  # buffer index -> (VertexBuffer, IndexBuffer, SkinBuffer or None)
    if any_rigid:
        layout[False] = len(mesh.vertex_buffers)
        mesh.vertex_buffers.append(nmo.VertexBuffer())
        mesh.index_buffers.append(nmo.IndexBuffer())
        if any_skinned:
            mesh.skin_buffers.append(nmo.SkinBuffer())  # rigid companion stays empty
    if any_skinned:
        layout[True] = len(mesh.vertex_buffers)
        mesh.vertex_buffers.append(nmo.VertexBuffer())
        mesh.index_buffers.append(nmo.IndexBuffer())
        mesh.skin_buffers.append(nmo.SkinBuffer())

    for sub, vertices, skins, triangles, facets, skinned in built:
        buffer_index = layout[skinned]
        vb = mesh.vertex_buffers[buffer_index]
        ib = mesh.index_buffers[buffer_index]
        base = len(vb.vertices)
        sub.vertex_buffer_index = buffer_index
        sub.index_buffer_index = buffer_index
        sub.start_index = len(ib.indices)
        sub.primitive_count = len(triangles)
        sub.base_vertex = 0
        sub.min_vertex = base
        sub.vertex_count = len(vertices)
        sub.facets = facets
        vb.vertices.extend(vertices)
        if skinned:
            skin_buffer = mesh.skin_buffers[buffer_index]
            for influences in skins:
                indices = [index for index, _ in influences] + [0] * nmo.BONE_INFLUENCES
                values = [weight for _, weight in influences] + [0.0] * nmo.BONE_INFLUENCES
                skin_buffer.skins.append(nmo.SkinVertex(tuple(indices[:nmo.BONE_INFLUENCES]),
                                                        tuple(values[:nmo.BONE_INFLUENCES])))
        for triangle in triangles:
            ib.indices.extend(corner + base for corner in triangle)

    for ib, vb in zip(mesh.index_buffers, mesh.vertex_buffers):
        ib.index_format = (nmo.INDEX_FORMAT_U16 if len(vb.vertices) <= 0xFFFF
                           else nmo.INDEX_FORMAT_U32)


def export_model(context):
    model = nmo.Model()
    for collection in _mesh_collections(context):
        model.meshes.append(_export_mesh(context, collection))
    return model


def export_file(context, path):
    """Build, self-validate through the codec, then write. A model the reader would reject never
    reaches the disk."""
    data = nmo.write(export_model(context))
    nmo.read(data)
    with open(path, 'wb') as handle:
        handle.write(data)
    return len(data)
