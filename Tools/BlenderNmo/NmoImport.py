"""NMO -> Blender scene.

One collection per mesh, one mesh object per submesh, armatures for the bone scopes, actions on
NLA tracks for the clips, empties for the markers. The inverse of NmoExport, through the shared
mapping in NmoScene -- anything imported here and exported unchanged must produce the same model
back (the headless test in Tools/NmoBlenderTest.py holds the two to that).
"""

import bpy

from mathutils import Matrix, Vector

from . import NmoFormat as nmo
from . import NmoScene as scene_map


def _set_mode(context, obj, mode):
    context.view_layer.objects.active = obj
    bpy.ops.object.mode_set(mode=mode)


def _rest_world_matrices(bones, mesh_rest_worlds):
    """Blender-space rest matrix per table entry. Locals chain onto their parent's rest; aliases
    take the mesh bone's rest -- their stored matrices are inert copies (Design/Archive/NmoFormat.md 5.8),
    so the mesh skeleton is the truth here exactly as it is at runtime."""
    rests = []
    for bone in bones:
        if bone.mesh_bone_index != nmo.NO_BONE and mesh_rest_worlds is not None:
            rests.append(mesh_rest_worlds[bone.mesh_bone_index].copy())
            continue
        local = scene_map.matrix_from_nmo(bone.local_transform)
        if bone.parent_index == nmo.NO_PARENT:
            rests.append(local)
        else:
            rests.append(rests[bone.parent_index] @ local)
    return rests


def _build_armature(context, collection, name, bones, mesh_rest_worlds, mesh_bone_names=None):
    """An armature object holding one bone table, bones oriented by their rest matrices. Alias
    entries remember the mesh bone they stand for by *name* -- names survive an artist reordering
    a skeleton where indices would not."""
    armature = bpy.data.armatures.new(name)
    rig = scene_map.link_object(collection, bpy.data.objects.new(name, armature))
    rests = _rest_world_matrices(bones, mesh_rest_worlds)

    _set_mode(context, rig, 'EDIT')
    editable = []
    for index, bone in enumerate(bones):
        edit_bone = armature.edit_bones.new(bone.name)
        edit_bone.head = (0.0, 0.0, 0.0)
        edit_bone.tail = (0.0, scene_map.BONE_LENGTH, 0.0)
        edit_bone.matrix = rests[index]
        editable.append(edit_bone)
    for index, bone in enumerate(bones):
        if bone.parent_index != nmo.NO_PARENT:
            editable[index].parent = editable[bone.parent_index]
    _set_mode(context, rig, 'OBJECT')

    for bone in bones:
        rig.pose.bones[bone.name].rotation_mode = 'QUATERNION'
        if bone.mesh_bone_index != nmo.NO_BONE and mesh_bone_names is not None:
            armature.bones[bone.name][scene_map.PROP_ALIAS] = mesh_bone_names[bone.mesh_bone_index]
    return rig, rests


def _alias_constraints(rig, bones, mesh_rig, mesh_bone_names):
    """A local table's alias bones follow the mesh skeleton in Blender the same way they do at
    runtime -- through the mesh bone, not through their stored copies."""
    for index, bone in enumerate(bones):
        if bone.mesh_bone_index == nmo.NO_BONE or mesh_rig is None:
            continue
        constraint = rig.pose.bones[bone.name].constraints.new('COPY_TRANSFORMS')
        constraint.target = mesh_rig
        constraint.subtarget = mesh_bone_names[bone.mesh_bone_index]


def _local_rest_trs(bones, rests):
    """Parent-relative rest translation and rotation per table entry, for the pose-basis maths.
    Rest matrices out of edit bones are orthonormal, which is what keeps the location, rotation
    and scale channels independent of each other in both directions."""
    out = []
    for index, bone in enumerate(bones):
        if bone.parent_index == nmo.NO_PARENT:
            local = rests[index]
        else:
            local = rests[bone.parent_index].inverted() @ rests[index]
        translation, rotation, _scale = local.decompose()
        out.append((translation, rotation))
    return out


def _import_clips(context, rig, clips, bones, rests):
    """Each clip becomes an action on its own NLA track named for the clip, keys converted from
    the bone's absolute local pose to Blender's rest-relative basis, linear interpolation to match
    the format's sampling rule (Design/Archive/NmoFormat.md 5.9)."""
    if not clips:
        return
    scene = context.scene
    local_trs = _local_rest_trs(bones, rests)
    animation = rig.animation_data or rig.animation_data_create()
    for clip in clips:
        action = bpy.data.actions.new(clip.name)
        fcurves = scene_map.action_fcurves_new(action, rig)

        def add_keys(data_path, index, keys):
            curve = fcurves.new(data_path, index=index)
            curve.keyframe_points.add(len(keys))
            for point, (frame, value) in zip(curve.keyframe_points, keys):
                point.co = (frame, value)
                point.interpolation = 'LINEAR'

        for track in clip.tracks:
            bone = bones[track.bone_index]
            rest_translation, rest_rotation = local_trs[track.bone_index]
            rest_rotation_inv = rest_rotation.inverted()
            path = 'pose.bones["%s"]' % bone.name

            if track.translation_keys:
                samples = []
                for seconds, value in track.translation_keys:
                    pose = Vector(scene_map.swap(value))
                    basis = rest_rotation_inv @ (pose - rest_translation)
                    samples.append((scene_map.to_frame(scene, seconds), basis))
                for axis in range(3):
                    add_keys(path + '.location', axis,
                             [(frame, basis[axis]) for frame, basis in samples])
            if track.rotation_keys:
                samples = []
                for seconds, value in track.rotation_keys:
                    basis = rest_rotation_inv @ scene_map.quat_from_nmo(value)
                    samples.append((scene_map.to_frame(scene, seconds), basis))
                for axis in range(4):  # rotation_quaternion is stored (w, x, y, z)
                    add_keys(path + '.rotation_quaternion', axis,
                             [(frame, basis[axis]) for frame, basis in samples])
            if track.scale_keys:
                samples = [(scene_map.to_frame(scene, seconds), value)
                           for seconds, value in track.scale_keys]
                for axis in range(3):
                    add_keys(path + '.scale', axis, samples)

        nla_track = animation.nla_tracks.new()
        nla_track.name = clip.name
        start = int(round(scene_map.to_frame(scene, clip.start_seconds)))
        strip = nla_track.strips.new(clip.name, start, action)
        strip.name = clip.name
        if hasattr(strip, 'action_slot') and getattr(animation, 'action_slot', None):
            strip.action_slot = animation.action_slot
    animation.action = None


def _import_material(cache, material):
    if material.name in cache:
        return cache[material.name]
    mat = bpy.data.materials.new(material.name)
    mat.use_nodes = True
    mat.diffuse_color = material.base_colour
    principled = next((n for n in mat.node_tree.nodes if n.type == 'BSDF_PRINCIPLED'), None)
    if principled is not None:
        principled.inputs['Base Color'].default_value = material.base_colour
        emissive = material.emissive_colour
        if 'Emission Color' in principled.inputs:
            principled.inputs['Emission Color'].default_value = (*emissive[:3], 1.0)
            principled.inputs['Emission Strength'].default_value = emissive[3]
    mat[scene_map.PROP_RENDER_FLAGS] = material.render_flags
    for slot, texture in enumerate(material.textures):
        mat[scene_map.PROP_TEXTURE % slot] = texture
    cache[material.name] = mat
    return mat


def _import_geometry(name, mesh, sub, material):
    """One submesh's window of the shared buffers -> a Blender mesh, quads rebuilt from facet ids,
    winding and axes crossed per NmoScene's rules."""
    vertices = mesh.vertex_buffers[sub.vertex_buffer_index].vertices
    indices = mesh.index_buffers[sub.index_buffer_index].indices
    window = range(sub.min_vertex, sub.min_vertex + sub.vertex_count)

    positions = [scene_map.swap(vertices[v].position) for v in window]
    triangles = []
    for triangle in range(sub.primitive_count):
        corner = [indices[sub.start_index + 3 * triangle + c] + sub.base_vertex - sub.min_vertex
                  for c in range(3)]
        triangles.append((corner[0], corner[2], corner[1]))  # the swap reverses winding; undo it
    polygons = scene_map.rebuild_polygons(triangles, sub.facets)

    data = bpy.data.meshes.new(name)
    data.from_pydata(positions, [], polygons, shade_flat=False)
    data.materials.append(material)

    # Create every layer before touching any layer's data: adding an attribute reallocates the
    # mesh's custom data, so a reference fetched before the second new() dangles -- writes through
    # it corrupt the heap, nondeterministically. Fetch both fresh once the layout is final.
    data.uv_layers.new(name=scene_map.UV_LAYER)
    data.color_attributes.new(scene_map.COLOUR_ATTRIBUTE, 'BYTE_COLOR', 'CORNER')
    uv_layer = data.uv_layers[scene_map.UV_LAYER]
    colours = data.color_attributes[scene_map.COLOUR_ATTRIBUTE]
    loop_normals = []
    for loop in data.loops:
        vertex = vertices[loop.vertex_index + sub.min_vertex]
        u, v = vertex.uv
        uv_layer.data[loop.index].uv = (u, 1.0 - v)  # NMO V runs down, Blender's runs up
        colours.data[loop.index].color_srgb = scene_map.colour_from_bytes(vertex.colour)
        loop_normals.append(scene_map.swap(vertex.normal))
    data.normals_split_custom_set(loop_normals)
    data.update()
    return data


def _import_markers(context, collection, sub, owner, rig, scope_names):
    """Markers become empties. parent_bone indexes the submesh's bone scope: the local rig when
    the submesh has a table, else the mesh rig -- in the latter case the empty remembers its
    submesh through PROP_OWNER, because the mesh rig serves every submesh.

    A bone-parented empty is placed through matrix_basis against the bone's *rest* pose, never
    through matrix_world: assignment through matrix_world bakes in whatever the depsgraph last
    evaluated -- and the freshly imported NLA already poses the rig at the current frame -- while
    stored rest matrices are pure data. Export inverts the same equation, so the marker's bind
    transform survives any pose (NmoExport._marker_transform)."""
    for marker in sub.markers:
        empty = bpy.data.objects.new(marker.name, None)
        empty.empty_display_type = scene_map.MARKER_DISPLAY.get(marker.kind,
                                                                scene_map.MARKER_DISPLAY_FALLBACK)
        empty.empty_display_size = 1.0
        empty[scene_map.PROP_KIND] = marker.kind
        empty[scene_map.PROP_PARAM0] = marker.param0
        empty[scene_map.PROP_PARAM1] = marker.param1
        empty[scene_map.PROP_MARKER_FLAGS] = marker.flags
        empty.color = marker.colour
        scene_map.link_object(collection, empty)
        bind_world = scene_map.transform_from_nmo(marker.position, marker.orientation,
                                                  marker.scale)
        if marker.parent_bone != nmo.NO_BONE and rig is not None:
            empty.parent = rig
            empty.parent_type = 'BONE'
            empty.parent_bone = scope_names[marker.parent_bone]
            if rig.get(scene_map.PROP_SCOPE) == scene_map.SCOPE_MESH:
                empty[scene_map.PROP_OWNER] = owner.name
            bone = rig.data.bones[empty.parent_bone]
            rest_parent = rig.matrix_world @ bone.matrix_local \
                @ Matrix.Translation((0.0, bone.length, 0.0))
            empty.matrix_parent_inverse = Matrix.Identity(4)
            empty.matrix_basis = rest_parent.inverted() @ bind_world
        else:
            empty.parent = owner
            empty.matrix_world = bind_world


def _import_skin(obj, mesh, sub, scope_names):
    if not mesh.skin_buffers:
        return False
    skins = mesh.skin_buffers[sub.vertex_buffer_index].skins
    if not skins:
        return False
    groups = {}
    for local_index, vertex in enumerate(range(sub.min_vertex, sub.min_vertex + sub.vertex_count)):
        skin = skins[vertex]
        for bone_index, weight in zip(skin.bone_indices, skin.bone_weights):
            if weight <= 0.0:
                continue
            name = scope_names[bone_index]
            if name not in groups:
                groups[name] = obj.vertex_groups.new(name=name)
            groups[name].add([local_index], weight, 'REPLACE')
    return True


def import_model(context, model, name_hint):
    """Builds the whole model into the scene; returns the created collections."""
    collections = []
    materials = {}
    for mesh_index, mesh in enumerate(model.meshes):
        mesh_name = mesh.name if mesh.name is not None else '%s%d' % (name_hint, mesh_index)
        collection = bpy.data.collections.new(mesh_name)
        context.scene.collection.children.link(collection)
        collection[scene_map.PROP_MESH_COLLECTION] = mesh_index
        collections.append(collection)

        mesh_rig, mesh_rests = (None, None)
        mesh_bone_names = [bone.name for bone in mesh.bones]
        if mesh.bones:
            mesh_rig, mesh_rests = _build_armature(context, collection, mesh_name + 'Rig',
                                                   mesh.bones, None)
            mesh_rig[scene_map.PROP_SCOPE] = scene_map.SCOPE_MESH
            _import_clips(context, mesh_rig, mesh.clips, mesh.bones, mesh_rests)

        for sub_index, sub in enumerate(mesh.sub_meshes):
            sub_name = sub.name if sub.name is not None else 'SubMesh%d' % sub_index
            material = _import_material(materials, mesh.materials[sub.material_index])
            data = _import_geometry(sub_name, mesh, sub, material)
            obj = scene_map.link_object(collection, bpy.data.objects.new(sub_name, data))
            obj[scene_map.PROP_SUBMESH] = sub_index
            if mesh_rig is not None:
                obj.parent = mesh_rig

            local_rig = None
            scope_names = [bone.name for bone in mesh.bones]
            deform_rig = mesh_rig
            if sub.bones:
                scope_names = [bone.name for bone in sub.bones]
                if all(bone.mesh_bone_index != nmo.NO_BONE for bone in sub.bones):
                    # A pure-alias palette: a property, deliberately not a second armature.
                    obj[scene_map.PROP_PALETTE] = [mesh.bones[b.mesh_bone_index].name
                                                   for b in sub.bones]
                    scope_names = [mesh.bones[b.mesh_bone_index].name for b in sub.bones]
                else:
                    local_rig, local_rests = _build_armature(context, collection, sub_name + 'Rig',
                                                             sub.bones, mesh_rests, mesh_bone_names)
                    local_rig[scene_map.PROP_SCOPE] = scene_map.SCOPE_SUBMESH
                    _alias_constraints(local_rig, sub.bones, mesh_rig, mesh_bone_names)
                    _import_clips(context, local_rig, sub.clips, sub.bones, local_rests)
                    if mesh_rig is not None:
                        local_rig.parent = mesh_rig
                    deform_rig = local_rig
            elif sub.clips and mesh_rig is not None:
                # Submesh clips over the mesh skeleton (Design/Archive/NmoFormat.md 7, row 2): actions on
                # the mesh rig, named back to their submesh by prefix at export.
                prefixed = []
                for clip in sub.clips:
                    renamed = nmo.Clip('%s:%s' % (sub_name, clip.name))
                    renamed.start_seconds = clip.start_seconds
                    renamed.end_seconds = clip.end_seconds
                    renamed.flags = clip.flags
                    renamed.tracks = clip.tracks
                    prefixed.append(renamed)
                _import_clips(context, mesh_rig, prefixed, mesh.bones, mesh_rests)

            if _import_skin(obj, mesh, sub, scope_names) and deform_rig is not None:
                modifier = obj.modifiers.new('Armature', 'ARMATURE')
                modifier.object = deform_rig

            if sub.bones and local_rig is not None:
                _import_markers(context, collection, sub, obj, local_rig,
                                [bone.name for bone in sub.bones])
            else:
                # No local table: the marker scope is the mesh skeleton (palettes carry no bones
                # of their own to ride -- parent_bone in a pure-alias table still names it).
                palette_names = [mesh.bones[b.mesh_bone_index].name for b in sub.bones] \
                    if sub.bones else mesh_bone_names
                _import_markers(context, collection, sub, obj, mesh_rig, palette_names)
    return collections


def import_file(context, path):
    model = nmo.read_file(path)
    import os
    hint = os.path.splitext(os.path.basename(path))[0]
    return import_model(context, model, hint)
