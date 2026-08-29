#!/usr/bin/env python3
"""NMO version 2.0 -- the reference codec.

This module is the executable statement of Design/NmoFormat.md section 5: an in-memory model, a
writer, a reader, and the validation list, in that document's order and under its rule numbers.
Where this file and the document disagree, one of them has a bug and the round-trip test is how
it is found.

Deliberately dependency-free (stdlib only) and free of any Blender import, so the same module
serves the add-on inside Blender, the fixture builder, and a bare `python3` running the tests.

  python3 Tools/BlenderNmo/NmoFormat.py info file.nmo        print a structural summary
  python3 Tools/BlenderNmo/NmoFormat.py validate file.nmo    exit 0 if the file conforms

Reading raises NmoError naming the violated rule; it never repairs (Design/NmoFormat.md 5.12).
All floats on disk are float32: values coming back from `read` are the exact float32 values, so
write(read(data)) reproduces the input byte for byte.
"""

import struct
import sys
import zlib

FILE_MAGIC = 0x324F4D4E  # "NMO2" on disk, little-endian
VERSION_MAJOR = 2  # 1.x is the Interstellar Outpost dialect
VERSION_MINOR = 0
MAX_STRING_BYTES = 1024
TEXTURE_SLOTS = 4  # 0 base, 1 emissive, 2 normal, 3 reserved
BONE_INFLUENCES = 4
NO_PARENT = -1
NO_BONE = -1

INDEX_FORMAT_U16 = 0
INDEX_FORMAT_U32 = 1
VERTEX_FORMAT_STANDARD = 0
SKIN_FORMAT_STANDARD = 0
VERTEX_STRIDE = 36
SKIN_STRIDE = 32

RENDER_FLAG_DOUBLE_SIDED = 0x1
RENDER_FLAG_ALPHA_BLEND = 0x2
RENDER_FLAG_ADDITIVE = 0x4

# Marker kinds defined by v2.0 (Design/NmoFormat.md 5.10). The format carries any kind string;
# this list only names the ones tools give a bespoke display to.
MARKER_KIND_EXHAUST = 'Exhaust'
MARKER_KIND_NAV_LIGHT = 'NavLight'
MARKER_KIND_GUN = 'Gun'

# Sanity caps (5.12): bounds on what a hostile count can make a reader allocate before the deeper
# checks run. Recommended in the design, enforced by this codec.
MAX_MESHES = 4096
MAX_BONES = 1024
MAX_MATERIALS = 256
MAX_MARKERS = 1024
MAX_CLIPS = 256

IDENTITY_4X4 = (1.0, 0.0, 0.0, 0.0,
                0.0, 1.0, 0.0, 0.0,
                0.0, 0.0, 1.0, 0.0,
                0.0, 0.0, 0.0, 1.0)
IDENTITY_QUAT = (0.0, 0.0, 0.0, 1.0)

FILE_HEADER = struct.Struct('<IHHIIIIII')  # 32
MESH_REF = struct.Struct('<II')  # 8
MESH_HEADER = struct.Struct('<24I')  # 96
MATERIAL = struct.Struct('<8fI3I')  # 48
BUFFER_HEADER = struct.Struct('<4I')  # 16
VERTEX = struct.Struct('<6fI2f')  # 36
SKIN_VERTEX = struct.Struct('<4I4f')  # 32
EXTENTS = struct.Struct('<10f')  # 40
SUB_MESH = struct.Struct('<17I10f5I')  # 128
BONE = struct.Struct('<2i32f')  # 136
CLIP = struct.Struct('<2f2I')  # 16
SRT_TRACK = struct.Struct('<4I')  # 16
TRANSLATION_KEY = struct.Struct('<4f')  # 16
ROTATION_KEY = struct.Struct('<5f')  # 20
SCALE_KEY = struct.Struct('<2f')  # 8

assert FILE_HEADER.size == 32 and MESH_HEADER.size == 96 and MATERIAL.size == 48
assert VERTEX.size == VERTEX_STRIDE and SKIN_VERTEX.size == SKIN_STRIDE
assert SUB_MESH.size == 128 and BONE.size == 136 and EXTENTS.size == 40


class NmoError(Exception):
    """A file (or a model about to be written) violates the specification."""


def colour_to_bytes(colour):
    """Float RGBA -> the uint32 with bytes R,G,B,A at ascending addresses (5.2): R is the lowest
    byte on this little-endian format."""
    r, g, b, a = (max(0, min(255, round(channel * 255.0))) for channel in colour)
    return r | (g << 8) | (b << 16) | (a << 24)


def colour_from_bytes(value):
    return ((value & 0xFF) / 255.0, ((value >> 8) & 0xFF) / 255.0,
            ((value >> 16) & 0xFF) / 255.0, ((value >> 24) & 0xFF) / 255.0)


# --- the in-memory model -------------------------------------------------------------------------
# Plain attribute bags rather than dataclasses, so the module imports on any Python Blender embeds
# and construction sites read as assignments. Tuples hold fixed-arity values (positions, colours,
# matrices flattened row-major); lists hold sequences.

class Material:
    def __init__(self, name=''):
        self.name = name
        self.base_colour = (1.0, 1.0, 1.0, 1.0)
        self.emissive_colour = (0.0, 0.0, 0.0, 0.0)
        self.render_flags = 0
        self.reserved = (0, 0, 0)
        self.textures = [''] * TEXTURE_SLOTS


class Vertex:
    __slots__ = ('position', 'normal', 'colour', 'uv')

    def __init__(self, position, normal=(0.0, 1.0, 0.0), colour=0xFFFFFFFF, uv=(0.0, 0.0)):
        self.position = position
        self.normal = normal
        self.colour = colour  # uint32, bytes R,G,B,A ascending
        self.uv = uv

    def key(self):
        return (self.position, self.normal, self.colour, self.uv)


class SkinVertex:
    __slots__ = ('bone_indices', 'bone_weights')

    def __init__(self, bone_indices=(0, 0, 0, 0), bone_weights=(0.0, 0.0, 0.0, 0.0)):
        self.bone_indices = bone_indices
        self.bone_weights = bone_weights


class IndexBuffer:
    def __init__(self, index_format=INDEX_FORMAT_U16, indices=None):
        self.index_format = index_format
        self.indices = indices if indices is not None else []


class VertexBuffer:
    def __init__(self, vertices=None):
        self.vertices = vertices if vertices is not None else []


class SkinBuffer:
    def __init__(self, skins=None):
        self.skins = skins if skins is not None else []  # empty = the paired VB is rigid


class Extents:
    def __init__(self):
        self.centre = (0.0, 0.0, 0.0)
        self.radius = 0.0
        self.box_min = (0.0, 0.0, 0.0)
        self.box_max = (0.0, 0.0, 0.0)


class Bone:
    def __init__(self, name=''):
        self.name = name
        self.parent_index = NO_PARENT
        self.mesh_bone_index = NO_BONE  # >= 0 only in a submesh table: an alias of that mesh bone
        self.local_transform = IDENTITY_4X4
        self.inv_bind_pose = IDENTITY_4X4


class Track:
    def __init__(self, bone_index=0):
        self.bone_index = bone_index
        self.translation_keys = []  # [(timeSeconds, (x, y, z))]
        self.rotation_keys = []  # [(timeSeconds, (x, y, z, w))]
        self.scale_keys = []  # [(timeSeconds, scale)]


class Clip:
    def __init__(self, name=''):
        self.name = name
        self.start_seconds = 0.0
        self.end_seconds = 0.0
        self.flags = 0
        self.tracks = []


class Marker:
    def __init__(self, name='', kind=''):
        self.name = name
        self.kind = kind  # '' = a plain point; unknown kinds are legal and preserved
        self.position = (0.0, 0.0, 0.0)
        self.orientation = IDENTITY_QUAT  # the marker's direction is its local +Z
        self.scale = 1.0
        self.parent_bone = NO_BONE
        self.colour = (1.0, 1.0, 1.0, 1.0)
        self.param0 = 0.0
        self.param1 = 0.0
        self.flags = 0
        self.reserved = (0, 0)


class SubMesh:
    def __init__(self, name=None):
        self.name = name  # None = unnamed (offset 0 on disk); '' round-trips distinctly
        self.material_index = 0
        self.index_buffer_index = 0
        self.vertex_buffer_index = 0
        self.start_index = 0
        self.primitive_count = 0
        self.base_vertex = 0
        self.min_vertex = 0
        self.vertex_count = 0
        self.flags = 0
        self.extents = Extents()
        self.bones = []
        self.clips = []
        self.markers = []
        self.facets = None  # None = absent; else one uint32 per triangle
        self.reserved = (0, 0, 0, 0, 0)


class Mesh:
    def __init__(self, name=None):
        self.name = name
        self.flags = 0
        self.materials = []
        self.sub_meshes = []
        self.index_buffers = []
        self.vertex_buffers = []
        self.skin_buffers = []  # [] or one per vertex buffer
        self.extents = Extents()
        self.bones = []
        self.clips = []
        self.reserved = (0,) * 7


class Model:
    def __init__(self):
        self.version_minor = VERSION_MINOR
        self.flags = 0
        self.reserved = 0
        self.meshes = []


# --- writing -------------------------------------------------------------------------------------

def _pad4(chunk):
    return chunk + b'\0' * (-len(chunk) % 4)


def _pad16(chunk):
    return chunk + b'\0' * (-len(chunk) % 16)


def _write_string(text):
    encoded = text.encode('utf-8')
    if len(encoded) > MAX_STRING_BYTES:
        raise NmoError('string longer than %d bytes: %r...' % (MAX_STRING_BYTES, text[:40]))
    return _pad4(struct.pack('<I', len(encoded)) + encoded)


def _write_bone_records(bones):
    out = bytearray()
    for bone in bones:
        out += _write_string(bone.name)
        out += BONE.pack(bone.parent_index, bone.mesh_bone_index,
                         *bone.local_transform, *bone.inv_bind_pose)
    return bytes(out)


def _write_clip_records(clips):
    out = bytearray()
    for clip in clips:
        out += _write_string(clip.name)
        out += CLIP.pack(clip.start_seconds, clip.end_seconds, len(clip.tracks), clip.flags)
        for track in clip.tracks:
            out += SRT_TRACK.pack(track.bone_index, len(track.translation_keys),
                                  len(track.rotation_keys), len(track.scale_keys))
        for track in clip.tracks:
            for time, value in track.translation_keys:
                out += TRANSLATION_KEY.pack(time, *value)
            for time, value in track.rotation_keys:
                out += ROTATION_KEY.pack(time, *value)
            for time, value in track.scale_keys:
                out += SCALE_KEY.pack(time, value)
    return bytes(out)


def _write_marker_records(markers):
    out = bytearray()
    for marker in markers:
        out += _write_string(marker.name)
        out += _write_string(marker.kind)
        out += struct.pack('<3f4ffi4f2fI2I', *marker.position, *marker.orientation,
                           marker.scale, marker.parent_bone, *marker.colour,
                           marker.param0, marker.param1, marker.flags, *marker.reserved)
    return bytes(out)


def _index_stride(index_format):
    if index_format == INDEX_FORMAT_U16:
        return 2
    if index_format == INDEX_FORMAT_U32:
        return 4
    raise NmoError('unknown index format %d' % index_format)


def _write_mesh_blob(mesh):
    """One mesh blob in the recommended physical order (Design/NmoFormat.md 5.11).

    Offsets in the headers are authoritative and blob-relative; this writer lays sections out in
    document order and records where each lands.
    """
    blob = bytearray(b'\0' * MESH_HEADER.size)  # header written last, offsets known by then

    def align4():
        blob.extend(b'\0' * (-len(blob) % 4))

    def align16():
        blob.extend(b'\0' * (-len(blob) % 16))

    name_offset = 0
    if mesh.name is not None:
        name_offset = len(blob)
        blob.extend(_write_string(mesh.name))

    materials_offset = len(blob) if mesh.materials else 0
    for material in mesh.materials:
        blob.extend(_write_string(material.name))
        blob.extend(MATERIAL.pack(*material.base_colour, *material.emissive_colour,
                                  material.render_flags, *material.reserved))
        if len(material.textures) != TEXTURE_SLOTS:
            raise NmoError('material %r must carry exactly %d texture slots'
                           % (material.name, TEXTURE_SLOTS))
        for texture in material.textures:
            blob.extend(_write_string(texture))

    # The submesh table sits before the records it points at, so its own offset is known now and
    # each record offset is patched into the packed rows as the records land.
    sub_meshes_offset = len(blob) if mesh.sub_meshes else 0
    table_at = len(blob)
    blob.extend(b'\0' * SUB_MESH.size * len(mesh.sub_meshes))

    sub_rows = []
    for sub in mesh.sub_meshes:
        sub_name_offset = 0
        if sub.name is not None:
            sub_name_offset = len(blob)
            blob.extend(_write_string(sub.name))
        bones_offset = len(blob) if sub.bones else 0
        blob.extend(_write_bone_records(sub.bones))
        clips_offset = len(blob) if sub.clips else 0
        blob.extend(_write_clip_records(sub.clips))
        markers_offset = len(blob) if sub.markers else 0
        blob.extend(_write_marker_records(sub.markers))
        facets_offset = 0
        if sub.facets is not None:
            if len(sub.facets) != sub.primitive_count:
                raise NmoError('submesh %r: %d facet ids for %d triangles'
                               % (sub.name, len(sub.facets), sub.primitive_count))
            facets_offset = len(blob)
            blob.extend(struct.pack('<%dI' % len(sub.facets), *sub.facets))
        extents = sub.extents
        sub_rows.append(SUB_MESH.pack(
            sub.material_index, sub.index_buffer_index, sub.vertex_buffer_index,
            sub.start_index, sub.primitive_count, sub.base_vertex, sub.min_vertex,
            sub.vertex_count, sub.flags, sub_name_offset, len(sub.bones), bones_offset,
            len(sub.clips), clips_offset, len(sub.markers), markers_offset, facets_offset,
            *extents.centre, extents.radius, *extents.box_min, *extents.box_max, *sub.reserved))
    blob[table_at:table_at + SUB_MESH.size * len(sub_rows)] = b''.join(sub_rows)

    align16()
    index_buffers_offset = len(blob) if mesh.index_buffers else 0
    for buffer in mesh.index_buffers:
        stride = _index_stride(buffer.index_format)
        payload = struct.pack('<%d%s' % (len(buffer.indices), 'H' if stride == 2 else 'I'),
                              *buffer.indices)
        blob.extend(BUFFER_HEADER.pack(buffer.index_format, stride, len(buffer.indices), 0))
        blob.extend(_pad16(payload))

    vertex_buffers_offset = len(blob) if mesh.vertex_buffers else 0
    for buffer in mesh.vertex_buffers:
        blob.extend(BUFFER_HEADER.pack(VERTEX_FORMAT_STANDARD, VERTEX_STRIDE,
                                       len(buffer.vertices), 0))
        payload = bytearray()
        for vertex in buffer.vertices:
            payload += VERTEX.pack(*vertex.position, *vertex.normal, vertex.colour, *vertex.uv)
        blob.extend(_pad16(bytes(payload)))

    skin_buffers_offset = len(blob) if mesh.skin_buffers else 0
    for buffer in mesh.skin_buffers:
        blob.extend(BUFFER_HEADER.pack(SKIN_FORMAT_STANDARD, SKIN_STRIDE, len(buffer.skins), 0))
        payload = bytearray()
        for skin in buffer.skins:
            payload += SKIN_VERTEX.pack(*skin.bone_indices, *skin.bone_weights)
        blob.extend(_pad16(bytes(payload)))

    align4()
    extents_offset = len(blob)
    blob.extend(EXTENTS.pack(*mesh.extents.centre, mesh.extents.radius,
                             *mesh.extents.box_min, *mesh.extents.box_max))

    bones_offset = len(blob) if mesh.bones else 0
    blob.extend(_write_bone_records(mesh.bones))
    clips_offset = len(blob) if mesh.clips else 0
    blob.extend(_write_clip_records(mesh.clips))

    blob[:MESH_HEADER.size] = MESH_HEADER.pack(
        mesh.flags, name_offset, len(mesh.materials), materials_offset,
        len(mesh.sub_meshes), sub_meshes_offset, len(mesh.index_buffers), index_buffers_offset,
        len(mesh.vertex_buffers), vertex_buffers_offset, len(mesh.skin_buffers),
        skin_buffers_offset, extents_offset, len(mesh.bones), bones_offset,
        len(mesh.clips), clips_offset, *mesh.reserved)
    return bytes(blob)


def write(model):
    """Model -> bytes. Deterministic: the same model always yields the same bytes."""
    blobs = [_write_mesh_blob(mesh) for mesh in model.meshes]
    refs = []
    cursor = FILE_HEADER.size + MESH_REF.size * len(blobs)
    body = bytearray()
    for blob in blobs:
        pad = -cursor % 16
        body.extend(b'\0' * pad)
        cursor += pad
        refs.append((cursor, len(blob)))
        body.extend(blob)
        cursor += len(blob)

    payload = b''.join(MESH_REF.pack(*ref) for ref in refs) + bytes(body)
    file_bytes = FILE_HEADER.size + len(payload)
    header = FILE_HEADER.pack(FILE_MAGIC, VERSION_MAJOR, model.version_minor, FILE_HEADER.size,
                              file_bytes, len(blobs), model.flags,
                              zlib.crc32(payload), model.reserved)
    return header + payload


def write_file(model, path):
    data = write(model)
    with open(path, 'wb') as handle:
        handle.write(data)
    return data


# --- reading -------------------------------------------------------------------------------------

class _Cursor:
    """Bounds-checked reads over one mesh blob. Every take is validated before it happens, so a
    hostile offset or count fails with the rule name rather than an IndexError somewhere later
    (rule 5.12.3)."""

    def __init__(self, blob, rule):
        self.blob = blob
        self.rule = rule

    def take(self, offset, size, what):
        if offset < 0 or size < 0 or offset + size > len(self.blob):
            raise NmoError('%s: %s at %d..%d escapes the mesh blob (%d bytes)'
                           % (self.rule, what, offset, offset + size, len(self.blob)))
        return self.blob[offset:offset + size]

    def section(self, offset, count, what):
        """A section that holds records must have an offset; 0 means absent (5.12.3)."""
        if count and not offset:
            raise NmoError('%s: %d %s records but the section offset is 0' % (self.rule, count, what))

    def string(self, offset, what):
        if offset % 4 != 0:
            raise NmoError('%s: %s string at %d is not 4-aligned' % (self.rule, what, offset))
        (length,) = struct.unpack_from('<I', self.take(offset, 4, what))
        if length > MAX_STRING_BYTES:
            raise NmoError('%s: %s string of %d bytes exceeds the %d cap'
                           % (self.rule, what, length, MAX_STRING_BYTES))
        raw = self.take(offset + 4, length, what)
        try:
            text = raw.decode('utf-8', errors='strict')
        except UnicodeDecodeError as error:
            raise NmoError('%s: %s string is not valid UTF-8 (%s)' % (self.rule, what, error))
        padded = 4 + length + (-length % 4)
        return text, offset + padded


def _read_bone_records(cursor, offset, count, scope):
    if count > MAX_BONES:
        raise NmoError('5.12: %s bone count %d exceeds the sanity cap %d'
                       % (scope, count, MAX_BONES))
    bones = []
    at = offset
    for index in range(count):
        name, at = cursor.string(at, '%s bone %d name' % (scope, index))
        fields = BONE.unpack(cursor.take(at, BONE.size, '%s bone %d' % (scope, index)))
        at += BONE.size
        bone = Bone(name)
        bone.parent_index = fields[0]
        bone.mesh_bone_index = fields[1]
        bone.local_transform = fields[2:18]
        bone.inv_bind_pose = fields[18:34]
        bones.append(bone)
    return bones


def _read_clip_records(cursor, offset, count, scope):
    if count > MAX_CLIPS:
        raise NmoError('5.12: %s clip count %d exceeds the sanity cap %d'
                       % (scope, count, MAX_CLIPS))
    clips = []
    at = offset
    for index in range(count):
        name, at = cursor.string(at, '%s clip %d name' % (scope, index))
        start, end, track_count, flags = CLIP.unpack(
            cursor.take(at, CLIP.size, '%s clip %r' % (scope, name)))
        at += CLIP.size
        clip = Clip(name)
        clip.start_seconds = start
        clip.end_seconds = end
        clip.flags = flags
        counts = []
        for track_index in range(track_count):
            bone_index, t_count, r_count, s_count = SRT_TRACK.unpack(
                cursor.take(at, SRT_TRACK.size, 'clip %r track %d' % (name, track_index)))
            at += SRT_TRACK.size
            clip.tracks.append(Track(bone_index))
            counts.append((t_count, r_count, s_count))
        for track, (t_count, r_count, s_count) in zip(clip.tracks, counts):
            for _ in range(t_count):
                key = TRANSLATION_KEY.unpack(cursor.take(at, 16, 'clip %r keys' % name))
                track.translation_keys.append((key[0], key[1:4]))
                at += 16
            for _ in range(r_count):
                key = ROTATION_KEY.unpack(cursor.take(at, 20, 'clip %r keys' % name))
                track.rotation_keys.append((key[0], key[1:5]))
                at += 20
            for _ in range(s_count):
                key = SCALE_KEY.unpack(cursor.take(at, 8, 'clip %r keys' % name))
                track.scale_keys.append((key[0], key[1]))
                at += 8
        clips.append(clip)
    return clips


def _read_marker_records(cursor, offset, count, sub_name):
    if count > MAX_MARKERS:
        raise NmoError('5.12: submesh %r marker count %d exceeds the sanity cap %d'
                       % (sub_name, count, MAX_MARKERS))
    markers = []
    at = offset
    for index in range(count):
        name, at = cursor.string(at, 'marker %d name' % index)
        kind, at = cursor.string(at, 'marker %r kind' % name)
        fields = struct.unpack('<3f4ffi4f2fI2I', cursor.take(at, 72, 'marker %r' % name))
        at += 72
        marker = Marker(name, kind)
        marker.position = fields[0:3]
        marker.orientation = fields[3:7]
        marker.scale = fields[7]
        marker.parent_bone = fields[8]
        marker.colour = fields[9:13]
        marker.param0 = fields[13]
        marker.param1 = fields[14]
        marker.flags = fields[15]
        marker.reserved = fields[16:18]
        markers.append(marker)
    return markers


def _read_mesh_blob(blob, mesh_index):
    cursor = _Cursor(blob, '5.12.3')
    if len(blob) < MESH_HEADER.size:
        raise NmoError('5.12.3: mesh %d blob of %d bytes cannot hold a mesh header'
                       % (mesh_index, len(blob)))
    fields = MESH_HEADER.unpack_from(blob)
    (flags, name_offset, material_count, materials_offset, sub_mesh_count, sub_meshes_offset,
     index_buffer_count, index_buffers_offset, vertex_buffer_count, vertex_buffers_offset,
     skin_buffer_count, skin_buffers_offset, extents_offset, bone_count, bones_offset,
     clip_count, clips_offset) = fields[:17]

    mesh = Mesh()
    mesh.flags = flags
    mesh.reserved = fields[17:24]
    if name_offset:
        mesh.name, _ = cursor.string(name_offset, 'mesh name')

    if material_count > MAX_MATERIALS:
        raise NmoError('5.12: material count %d exceeds the sanity cap %d'
                       % (material_count, MAX_MATERIALS))
    cursor.section(materials_offset, material_count, 'material')
    cursor.section(sub_meshes_offset, sub_mesh_count, 'submesh')
    cursor.section(index_buffers_offset, index_buffer_count, 'index buffer')
    cursor.section(vertex_buffers_offset, vertex_buffer_count, 'vertex buffer')
    cursor.section(skin_buffers_offset, skin_buffer_count, 'skin buffer')
    cursor.section(bones_offset, bone_count, 'mesh bone')
    cursor.section(clips_offset, clip_count, 'mesh clip')
    at = materials_offset
    for index in range(material_count):
        name, at = cursor.string(at, 'material %d name' % index)
        material = Material(name)
        packed = MATERIAL.unpack(cursor.take(at, MATERIAL.size, 'material %r' % name))
        at += MATERIAL.size
        material.base_colour = packed[0:4]
        material.emissive_colour = packed[4:8]
        material.render_flags = packed[8]
        material.reserved = packed[9:12]
        material.textures = []
        for slot in range(TEXTURE_SLOTS):
            texture, at = cursor.string(at, 'material %r texture %d' % (name, slot))
            material.textures.append(texture)
        mesh.materials.append(material)

    def read_buffers(count, offset, kind):
        headers = []
        at = offset
        if offset % 16 != 0 and count:
            raise NmoError('5.12.3: %s buffer section at %d is not 16-aligned' % (kind, offset))
        for index in range(count):
            fmt, stride, elements, _reserved = BUFFER_HEADER.unpack(
                cursor.take(at, BUFFER_HEADER.size, '%s buffer %d header' % (kind, index)))
            at += BUFFER_HEADER.size
            payload_bytes = stride * elements
            payload = cursor.take(at, payload_bytes, '%s buffer %d payload' % (kind, index))
            at += payload_bytes + (-payload_bytes % 16)
            headers.append((fmt, stride, elements, payload))
        return headers

    for index, (fmt, stride, elements, payload) in enumerate(
            read_buffers(index_buffer_count, index_buffers_offset, 'index')):
        if stride != _index_stride(fmt):  # raises on an unknown format (5.12.4)
            raise NmoError('5.12.4: index buffer %d stride %d does not match format %d'
                           % (index, stride, fmt))
        buffer = IndexBuffer(fmt)
        buffer.indices = list(struct.unpack('<%d%s' % (elements, 'H' if stride == 2 else 'I'),
                                            payload))
        mesh.index_buffers.append(buffer)

    for index, (fmt, stride, elements, payload) in enumerate(
            read_buffers(vertex_buffer_count, vertex_buffers_offset, 'vertex')):
        if fmt != VERTEX_FORMAT_STANDARD or stride != VERTEX_STRIDE:
            raise NmoError('5.12.4: vertex buffer %d format %d stride %d is not Standard/36'
                           % (index, fmt, stride))
        buffer = VertexBuffer()
        for element in range(elements):
            fields = VERTEX.unpack_from(payload, element * VERTEX_STRIDE)
            buffer.vertices.append(Vertex(fields[0:3], fields[3:6], fields[6], fields[7:9]))
        mesh.vertex_buffers.append(buffer)

    for index, (fmt, stride, elements, payload) in enumerate(
            read_buffers(skin_buffer_count, skin_buffers_offset, 'skin')):
        if fmt != SKIN_FORMAT_STANDARD or stride != SKIN_STRIDE:
            raise NmoError('5.12.4: skin buffer %d format %d stride %d is not Standard/32'
                           % (index, fmt, stride))
        buffer = SkinBuffer()
        for element in range(elements):
            fields = SKIN_VERTEX.unpack_from(payload, element * SKIN_STRIDE)
            buffer.skins.append(SkinVertex(fields[0:4], fields[4:8]))
        mesh.skin_buffers.append(buffer)

    if extents_offset:
        packed = EXTENTS.unpack(cursor.take(extents_offset, EXTENTS.size, 'mesh extents'))
        mesh.extents.centre, mesh.extents.radius = packed[0:3], packed[3]
        mesh.extents.box_min, mesh.extents.box_max = packed[4:7], packed[7:10]

    mesh.bones = _read_bone_records(cursor, bones_offset, bone_count, 'mesh')
    mesh.clips = _read_clip_records(cursor, clips_offset, clip_count, 'mesh')

    at = sub_meshes_offset
    for index in range(sub_mesh_count):
        row = SUB_MESH.unpack(cursor.take(at, SUB_MESH.size, 'submesh %d' % index))
        at += SUB_MESH.size
        sub = SubMesh()
        (sub.material_index, sub.index_buffer_index, sub.vertex_buffer_index, sub.start_index,
         sub.primitive_count, sub.base_vertex, sub.min_vertex, sub.vertex_count, sub.flags,
         sub_name_offset, sub_bone_count, sub_bones_offset, sub_clip_count, sub_clips_offset,
         marker_count, markers_offset, facets_offset) = row[:17]
        sub.extents.centre, sub.extents.radius = row[17:20], row[20]
        sub.extents.box_min, sub.extents.box_max = row[21:24], row[24:27]
        sub.reserved = row[27:32]
        if sub_name_offset:
            sub.name, _ = cursor.string(sub_name_offset, 'submesh %d name' % index)
        cursor.section(sub_bones_offset, sub_bone_count, 'submesh bone')
        cursor.section(sub_clips_offset, sub_clip_count, 'submesh clip')
        cursor.section(markers_offset, marker_count, 'marker')
        sub.bones = _read_bone_records(cursor, sub_bones_offset, sub_bone_count,
                                       'submesh %r' % sub.name)
        sub.clips = _read_clip_records(cursor, sub_clips_offset, sub_clip_count,
                                       'submesh %r' % sub.name)
        sub.markers = _read_marker_records(cursor, markers_offset, marker_count, sub.name)
        if facets_offset:
            raw = cursor.take(facets_offset, 4 * sub.primitive_count,
                              'submesh %r facet ids' % sub.name)
            sub.facets = list(struct.unpack('<%dI' % sub.primitive_count, raw))
        mesh.sub_meshes.append(sub)

    return mesh


def read(data, check_crc=True):
    """Bytes -> Model, validated per Design/NmoFormat.md 5.12. Raises NmoError, never repairs."""
    if len(data) < FILE_HEADER.size:
        raise NmoError('5.12.1: %d bytes cannot hold a file header' % len(data))
    (magic, version_major, version_minor, header_bytes, file_bytes, mesh_count, flags,
     payload_crc32, reserved) = FILE_HEADER.unpack_from(data)
    if magic != FILE_MAGIC:
        raise NmoError('5.12.1: magic 0x%08X is not "NMO2"' % magic)
    if version_major != VERSION_MAJOR:
        raise NmoError('5.12.1: major version %d is not %d' % (version_major, VERSION_MAJOR))
    if header_bytes != FILE_HEADER.size:
        raise NmoError('5.12.1: headerBytes %d is not %d' % (header_bytes, FILE_HEADER.size))
    if file_bytes != len(data):
        raise NmoError('5.12.1: fileBytes %d but %d bytes were read' % (file_bytes, len(data)))
    if mesh_count > MAX_MESHES:
        raise NmoError('5.12: mesh count %d exceeds the sanity cap %d' % (mesh_count, MAX_MESHES))
    if check_crc and payload_crc32 != 0:
        actual = zlib.crc32(data[header_bytes:])
        if actual != payload_crc32:
            raise NmoError('5.12.1: payload CRC 0x%08X does not match stored 0x%08X'
                           % (actual, payload_crc32))

    model = Model()
    model.version_minor = version_minor
    model.flags = flags
    model.reserved = reserved

    directory_end = header_bytes + MESH_REF.size * mesh_count
    if directory_end > len(data):
        raise NmoError('5.12.2: mesh directory of %d entries escapes the file' % mesh_count)
    for index in range(mesh_count):
        offset, length = MESH_REF.unpack_from(data, header_bytes + MESH_REF.size * index)
        if offset % 16 != 0:
            raise NmoError('5.12.2: mesh %d blob at %d is not 16-aligned' % (index, offset))
        if offset < directory_end or offset + length > len(data):
            raise NmoError('5.12.2: mesh %d window %d..%d escapes [%d, %d)'
                           % (index, offset, offset + length, directory_end, len(data)))
        model.meshes.append(_read_mesh_blob(data[offset:offset + length], index))

    _validate_model(model)
    return model


def read_file(path, check_crc=True):
    with open(path, 'rb') as handle:
        return read(handle.read(), check_crc)


# --- validation beyond framing (rules 5.12.5 - 5.12.9) -------------------------------------------

def _validate_bone_table(bones, mesh_bone_count, scope, is_mesh_scope):
    names = set()
    for index, bone in enumerate(bones):
        if bone.name in names:
            raise NmoError('5.12.6: %s bone name %r is not unique' % (scope, bone.name))
        names.add(bone.name)
        if not (NO_PARENT <= bone.parent_index < index):
            raise NmoError('5.12.6: %s bone %r parent %d is not before it in the table'
                           % (scope, bone.name, bone.parent_index))
        if is_mesh_scope:
            if bone.mesh_bone_index != NO_BONE:
                raise NmoError('5.12.6: mesh bone %r carries meshBoneIndex %d; mesh-scope '
                               'records carry -1' % (bone.name, bone.mesh_bone_index))
        elif not (bone.mesh_bone_index == NO_BONE or 0 <= bone.mesh_bone_index < mesh_bone_count):
            raise NmoError('5.12.6: %s bone %r aliases mesh bone %d of %d'
                           % (scope, bone.name, bone.mesh_bone_index, mesh_bone_count))


def _validate_clips(clips, scope_size, scope):
    names = set()
    for clip in clips:
        if clip.name in names:
            raise NmoError('5.12.7: %s clip name %r is not unique' % (scope, clip.name))
        names.add(clip.name)
        if not clip.end_seconds >= clip.start_seconds:
            raise NmoError('5.12.7: clip %r ends (%g) before it starts (%g)'
                           % (clip.name, clip.end_seconds, clip.start_seconds))
        previous_bone = -1
        for track in clip.tracks:
            if track.bone_index <= previous_bone:
                raise NmoError('5.12.7: clip %r tracks are not strictly increasing by bone'
                               % clip.name)
            previous_bone = track.bone_index
            if track.bone_index >= scope_size:
                raise NmoError('5.12.7: clip %r keys bone %d of a %d-bone scope'
                               % (clip.name, track.bone_index, scope_size))
            for series_name, series in (('translation', track.translation_keys),
                                        ('rotation', track.rotation_keys),
                                        ('scale', track.scale_keys)):
                times = [key[0] for key in series]
                if any(b <= a for a, b in zip(times, times[1:])):
                    raise NmoError('5.12.7: clip %r bone %d %s keys are not strictly '
                                   'increasing in time' % (clip.name, track.bone_index,
                                                           series_name))


def _validate_model(model):
    for mesh in model.meshes:
        mesh_label = mesh.name if mesh.name is not None else '<unnamed>'

        if mesh.skin_buffers and len(mesh.skin_buffers) != len(mesh.vertex_buffers):
            raise NmoError('5.12.4: mesh %r has %d skin buffers for %d vertex buffers; the count '
                           'is 0 or equal' % (mesh_label, len(mesh.skin_buffers),
                                              len(mesh.vertex_buffers)))
        for index, skin in enumerate(mesh.skin_buffers):
            paired = len(mesh.vertex_buffers[index].vertices)
            if skin.skins and len(skin.skins) != paired:
                raise NmoError('5.12.4: skin buffer %d holds %d elements for a %d-vertex buffer'
                               % (index, len(skin.skins), paired))

        _validate_bone_table(mesh.bones, 0, 'mesh %r' % mesh_label, is_mesh_scope=True)
        _validate_clips(mesh.clips, len(mesh.bones), 'mesh %r' % mesh_label)

        # Which vertices each differently-scoped skinned submesh touches (5.12.8).
        claimed = {}  # (vb, vertex) -> id(scope owner submesh) for overlap checking

        sub_names = set()
        for sub in mesh.sub_meshes:
            label = 'submesh %r' % (sub.name if sub.name is not None else '<unnamed>')
            if sub.material_index >= len(mesh.materials):
                raise NmoError('5.12.5: %s material %d of %d'
                               % (label, sub.material_index, len(mesh.materials)))
            if sub.index_buffer_index >= len(mesh.index_buffers):
                raise NmoError('5.12.5: %s index buffer %d of %d'
                               % (label, sub.index_buffer_index, len(mesh.index_buffers)))
            if sub.vertex_buffer_index >= len(mesh.vertex_buffers):
                raise NmoError('5.12.5: %s vertex buffer %d of %d'
                               % (label, sub.vertex_buffer_index, len(mesh.vertex_buffers)))
            indices = mesh.index_buffers[sub.index_buffer_index].indices
            if sub.start_index + 3 * sub.primitive_count > len(indices):
                raise NmoError('5.12.5: %s index range %d..%d escapes its %d-index buffer'
                               % (label, sub.start_index,
                                  sub.start_index + 3 * sub.primitive_count, len(indices)))
            if sub.min_vertex < sub.base_vertex:
                raise NmoError('5.12.5: %s minVertex %d is below baseVertex %d'
                               % (label, sub.min_vertex, sub.base_vertex))
            vertex_total = len(mesh.vertex_buffers[sub.vertex_buffer_index].vertices)
            if sub.min_vertex + sub.vertex_count > vertex_total:
                raise NmoError('5.12.5: %s vertex window %d..%d escapes its %d-vertex buffer'
                               % (label, sub.min_vertex, sub.min_vertex + sub.vertex_count,
                                  vertex_total))
            for raw in indices[sub.start_index:sub.start_index + 3 * sub.primitive_count]:
                biased = raw + sub.base_vertex
                if not (sub.min_vertex <= biased < sub.min_vertex + sub.vertex_count):
                    raise NmoError('5.12.5: %s uses vertex %d outside [%d, %d)'
                                   % (label, biased, sub.min_vertex,
                                      sub.min_vertex + sub.vertex_count))
            if sub.facets is not None and len(sub.facets) != sub.primitive_count:
                raise NmoError('5.12.5: %s carries %d facet ids for %d triangles'
                               % (label, len(sub.facets), sub.primitive_count))

            if sub.name is not None:
                if sub.name in sub_names:
                    raise NmoError('5.12.5: submesh name %r is not unique' % sub.name)
                sub_names.add(sub.name)

            _validate_bone_table(sub.bones, len(mesh.bones), label, is_mesh_scope=False)
            scope_size = len(sub.bones) if sub.bones else len(mesh.bones)
            _validate_clips(sub.clips, scope_size, label)

            marker_names = set()
            for marker in sub.markers:
                if marker.name in marker_names:
                    raise NmoError('5.12.9: %s marker name %r is not unique' % (label, marker.name))
                marker_names.add(marker.name)
                if not (marker.parent_bone == NO_BONE or 0 <= marker.parent_bone < scope_size):
                    raise NmoError('5.12.9: %s marker %r rides bone %d of a %d-bone scope'
                                   % (label, marker.name, marker.parent_bone, scope_size))

            # 5.12.8: skinned submeshes.
            skins = []
            if mesh.skin_buffers:
                skins = mesh.skin_buffers[sub.vertex_buffer_index].skins
            if skins:
                if scope_size == 0:
                    raise NmoError('5.12.8: %s is skinned but has no bone scope' % label)
                scope_key = id(sub.bones) if sub.bones else id(mesh.bones)
                for vertex in range(sub.min_vertex, sub.min_vertex + sub.vertex_count):
                    for influence in range(BONE_INFLUENCES):
                        if skins[vertex].bone_indices[influence] >= scope_size:
                            raise NmoError('5.12.8: %s vertex %d skins bone %d of a %d-bone scope'
                                           % (label, vertex, skins[vertex].bone_indices[influence],
                                              scope_size))
                    weights = skins[vertex].bone_weights
                    if any(w < 0.0 for w in weights):
                        raise NmoError('5.12.8: %s vertex %d has a negative weight' % (label, vertex))
                    if any(b > a for a, b in zip(weights, weights[1:])):
                        raise NmoError('5.12.8: %s vertex %d weights are not descending'
                                       % (label, vertex))
                    if abs(sum(weights) - 1.0) > 1e-3:
                        raise NmoError('5.12.8: %s vertex %d weights sum to %g'
                                       % (label, vertex, sum(weights)))
                    key = (sub.vertex_buffer_index, vertex)
                    owner = claimed.setdefault(key, scope_key)
                    if owner != scope_key:
                        raise NmoError('5.12.8: vertex %d of buffer %d is skinned by two '
                                       'different bone scopes' % (vertex, sub.vertex_buffer_index))


# --- command line --------------------------------------------------------------------------------

def _info(model):
    lines = ['NMO 2.%d, %d mesh(es)' % (model.version_minor, len(model.meshes))]
    for mesh in model.meshes:
        lines.append('mesh %r: %d material(s), %d submesh(es), %d bone(s), %d clip(s)'
                     % (mesh.name, len(mesh.materials), len(mesh.sub_meshes), len(mesh.bones),
                        len(mesh.clips)))
        for sub in mesh.sub_meshes:
            markers = ', '.join('%s/%s' % (m.kind or 'point', m.name) for m in sub.markers)
            lines.append('  submesh %r: %d tris, %d bone(s), %d clip(s), markers [%s]'
                         % (sub.name, sub.primitive_count, len(sub.bones), len(sub.clips),
                            markers))
    return '\n'.join(lines)


def main(argv):
    if len(argv) != 3 or argv[1] not in ('info', 'validate'):
        print(__doc__.strip().splitlines()[0])
        print('usage: NmoFormat.py info|validate file.nmo')
        return 2
    try:
        model = read_file(argv[2])
    except (OSError, NmoError) as error:
        print('error: %s' % error)
        return 1
    if argv[1] == 'info':
        print(_info(model))
    else:
        print('%s conforms to NMO 2.%d' % (argv[2], model.version_minor))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
