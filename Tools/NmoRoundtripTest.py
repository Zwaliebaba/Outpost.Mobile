#!/usr/bin/env python3
"""The codec's tests: the byte-exact round trip, and one rejection per validation rule.

Two families, mirroring how Design/NmoFormat.md 5.12 will also be tested in C++ (slice 2):

  - Round trip: fixture -> write -> read -> write must be byte-identical, and the read model must
    still hold every feature the fixture authored (a silent drop round-trips clean, so the bytes
    alone are not enough).
  - Rejection: for each validation clause, one deliberately malformed input the reader must
    refuse, built either as an invalid model (the writer is deliberately permissive) or by
    patching bytes at positions computed from the spec's fixed layout -- never at magic offsets
    copied from a hex dump, so the cases keep testing the right rule when the fixture grows.

Needs nothing but python3. Run from anywhere:

  python3 Tools/NmoRoundtripTest.py
"""

import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'BlenderNmo'))
import NmoFormat as nmo
import NmoFixture

PASSED = 0


def check(name, condition, detail=''):
    global PASSED
    if not condition:
        raise SystemExit('FAIL %s%s' % (name, ': %s' % detail if detail else ''))
    PASSED += 1


def expect_reject(name, data_or_model, rule):
    """The reader must raise NmoError mentioning the rule for this input."""
    try:
        if isinstance(data_or_model, (bytes, bytearray)):
            nmo.read(bytes(data_or_model))
        else:
            nmo.read(nmo.write(data_or_model))
    except nmo.NmoError as error:
        check(name, rule in str(error), 'raised %r, wanted rule %s' % (str(error), rule))
        return
    raise SystemExit('FAIL %s: a file violating %s was accepted' % (name, rule))


def clear_crc(data):
    """Zero the stored CRC (0 = not computed), so a payload patch tests its own rule rather than
    the checksum."""
    return data[:24] + b'\0\0\0\0' + data[28:]


def u32_at(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def patch_u32(data, offset, value):
    return data[:offset] + struct.pack('<I', value) + data[offset + 4:]


# Field positions inside NmoMeshHeader, by pack order (Design/NmoFormat.md 5.4).
MH_NAME_OFFSET = 1
MH_MATERIALS_OFFSET = 3
MH_VERTEX_BUFFERS_OFFSET = 9


def mesh_blob_start(data, mesh_index=0):
    return u32_at(data, 32 + 8 * mesh_index)


def header_field(data, field_index, mesh_index=0):
    return mesh_blob_start(data, mesh_index) + 4 * field_index


def test_round_trip():
    model = NmoFixture.build_model()
    first = nmo.write(model)
    second = nmo.write(nmo.read(first))
    check('round trip is byte-identical', first == second)

    read_back = nmo.read(first)
    gunship = read_back.meshes[0]
    check('mesh name survives', gunship.name == 'Gunship')
    check('materials survive', [m.name for m in gunship.materials] == ['HullPlate', 'GlowStripe'])
    check('emissive survives', gunship.materials[1].emissive_colour[3] == 1.5)
    check('render flags survive', gunship.materials[1].render_flags == nmo.RENDER_FLAG_ADDITIVE)
    check('mesh skeleton survives', [b.name for b in gunship.bones] == ['Root', 'TurretMount'])
    check('mesh clip survives', gunship.clips[0].name == 'Idle'
          and len(gunship.clips[0].tracks[0].translation_keys) == 3)

    hull, turret = gunship.sub_meshes
    check('submesh names survive', (hull.name, turret.name) == ('Hull', 'Turret'))
    check('palette is a pure alias', hull.bones[0].mesh_bone_index == 0)
    check('facet ids survive', hull.facets is not None and hull.facets[0:4] == [0, 0, 1, 1])
    check('turret keeps its local bone', turret.bones[1].mesh_bone_index == nmo.NO_BONE
          and turret.bones[1].parent_index == 0)
    check('turret clip keys survive', len(turret.clips[0].tracks[0].rotation_keys) == 3
          and len(turret.clips[0].tracks[0].scale_keys) == 2)

    kinds = [(m.kind, m.name) for m in hull.markers]
    check('marker kinds survive', kinds == [('Exhaust', 'ExhaustPort'), ('Exhaust', 'ExhaustStarboard'),
                                            ('NavLight', 'NavPort'), ('NavLight', 'NavStarboard'),
                                            ('Gun', 'BowGun')])
    nav = hull.markers[2]
    check('marker colour survives', nav.colour[0] == 1.0 and nav.colour[1] < 0.2)
    check('marker params survive', nav.param0 == 2.0 and hull.markers[3].param1 == 0.5)
    check('marker bone binding survives', turret.markers[0].parent_bone == 1)

    probe = read_back.meshes[1]
    check('unnamed mesh stays unnamed', probe.name is None and probe.sub_meshes[0].name is None)
    check('base vertex window survives', probe.sub_meshes[0].base_vertex == 2
          and probe.index_buffers[0].index_format == nmo.INDEX_FORMAT_U32)
    check('winding convention holds in the fixture',
          all(_cross_matches_normals(mesh) for mesh in read_back.meshes))


def _cross_matches_normals(mesh):
    """Front faces have cross(b-a, c-a) along the stored outward normal (Design/NmoFormat.md 5.2,
    clockwise front in the left-handed basis). The Blender exporter must reproduce this, so the
    fixture itself is checked rather than trusted."""
    for sub in mesh.sub_meshes:
        indices = mesh.index_buffers[sub.index_buffer_index].indices
        vertices = mesh.vertex_buffers[sub.vertex_buffer_index].vertices
        for triangle in range(sub.primitive_count):
            a, b, c = (vertices[indices[sub.start_index + 3 * triangle + corner] + sub.base_vertex]
                       for corner in range(3))
            ux, uy, uz = (b.position[i] - a.position[i] for i in range(3))
            vx, vy, vz = (c.position[i] - a.position[i] for i in range(3))
            cross = (uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx)
            dot = sum(cross[i] * a.normal[i] for i in range(3))
            if dot <= 0.0:
                return False
    return True


def test_preserves_forward_compatible_data():
    model = NmoFixture.build_model()
    marker = nmo.Marker('DockRing', 'DockingPort')  # a kind v2.0 does not define
    marker.position = (0.0, 2.0, 4.0)
    marker.flags = 0x80
    marker.reserved = (7, 9)
    model.meshes[0].sub_meshes[0].markers.append(marker)
    model.meshes[0].reserved = (1, 2, 3, 4, 5, 6, 7)  # nonzero reserved: ignored, not rejected

    read_back = nmo.read(nmo.write(model))
    kept = read_back.meshes[0].sub_meshes[0].markers[-1]
    check('unknown marker kind is kept', kept.kind == 'DockingPort' and kept.flags == 0x80
          and kept.reserved == (7, 9))
    check('nonzero reserved fields are kept', read_back.meshes[0].reserved == (1, 2, 3, 4, 5, 6, 7))

    data = nmo.write(model)
    newer = data[:6] + struct.pack('<H', 9) + data[8:]  # versionMinor 9: load, do not reject
    check('newer minor version loads', nmo.read(newer).version_minor == 9)


def test_rejections():
    pristine = nmo.write(NmoFixture.build_model())

    # 5.12.1 -- identification.
    expect_reject('wrong magic', b'PIE3' + pristine[4:], '5.12.1')
    expect_reject('major version 1', pristine[:4] + struct.pack('<H', 1) + pristine[6:], '5.12.1')
    expect_reject('headerBytes 16', patch_u32(pristine, 8, 16), '5.12.1')
    expect_reject('trailing garbage', pristine + b'\0', '5.12.1')
    expect_reject('truncated file', pristine[:-8], '5.12')  # fileBytes or window, either names 5.12
    flipped = bytearray(pristine)
    flipped[-1] ^= 0xFF
    expect_reject('payload corruption fails the CRC', bytes(flipped), '5.12.1')

    # 5.12.2 -- the mesh directory, and the caps that run before it.
    expect_reject('mesh count over the cap', patch_u32(clear_crc(pristine), 16, 100000), '5.12')
    ref0 = u32_at(pristine, 32)
    expect_reject('unaligned mesh blob', patch_u32(clear_crc(pristine), 32, ref0 + 4), '5.12.2')
    expect_reject('mesh window escapes the file',
                  patch_u32(clear_crc(pristine), 36, len(pristine)), '5.12.2')

    # 5.12.3 -- offsets, strings, sections.
    name_at = u32_at(pristine, header_field(pristine, MH_NAME_OFFSET))
    expect_reject('string over the byte cap',
                  patch_u32(clear_crc(pristine), mesh_blob_start(pristine) + name_at, 2000),
                  '5.12.3')
    start = mesh_blob_start(pristine) + name_at
    bad_utf8 = clear_crc(pristine[:start + 4] + b'\xff\xfe' + pristine[start + 6:])
    expect_reject('invalid UTF-8 in a name', bad_utf8, '5.12.3')
    expect_reject('materials with a zero section offset',
                  patch_u32(clear_crc(pristine), header_field(pristine, MH_MATERIALS_OFFSET), 0),
                  '5.12.3')

    # 5.12.4 -- buffer formats and pairing.
    vb_section = u32_at(pristine, header_field(pristine, MH_VERTEX_BUFFERS_OFFSET))
    expect_reject('unknown vertex format',
                  patch_u32(clear_crc(pristine), mesh_blob_start(pristine) + vb_section, 77),
                  '5.12.4')

    def broken(mutate):
        model = NmoFixture.build_model()
        mutate(model.meshes[0])
        return model

    expect_reject('skin buffer count mismatch',
                  broken(lambda m: m.vertex_buffers.append(nmo.VertexBuffer())), '5.12.4')
    expect_reject('skin element count mismatch',
                  broken(lambda m: m.skin_buffers[0].skins.pop()), '5.12.4')

    # 5.12.5 -- submesh ranges.
    def hull(model_mutator):
        return broken(lambda m: model_mutator(m.sub_meshes[0]))

    expect_reject('material index out of range', hull(lambda s: setattr(s, 'material_index', 9)),
                  '5.12.5')
    expect_reject('index buffer out of range', hull(lambda s: setattr(s, 'index_buffer_index', 3)),
                  '5.12.5')
    expect_reject('index range escapes the buffer',
                  hull(lambda s: (setattr(s, 'facets', None),
                                  setattr(s, 'primitive_count', 1000))), '5.12.5')
    expect_reject('minVertex below baseVertex', hull(lambda s: setattr(s, 'base_vertex', 5)),
                  '5.12.5')
    expect_reject('index outside the vertex window',
                  hull(lambda s: setattr(s, 'vertex_count', 4)), '5.12.5')
    expect_reject('duplicate submesh names',
                  broken(lambda m: setattr(m.sub_meshes[1], 'name', 'Hull')), '5.12.5')

    # 5.12.6 -- bone tables.
    expect_reject('bone parented forward',
                  broken(lambda m: setattr(m.bones[0], 'parent_index', 1)), '5.12.6')
    expect_reject('mesh bone carrying an alias',
                  broken(lambda m: setattr(m.bones[0], 'mesh_bone_index', 0)), '5.12.6')
    expect_reject('alias of a missing mesh bone',
                  hull(lambda s: setattr(s.bones[0], 'mesh_bone_index', 5)), '5.12.6')
    expect_reject('duplicate bone names',
                  broken(lambda m: setattr(m.bones[1], 'name', 'Root')), '5.12.6')

    # 5.12.7 -- clips.
    def turret_clip(mutator):
        return broken(lambda m: mutator(m.sub_meshes[1].clips[0]))

    expect_reject('clip ends before it starts',
                  turret_clip(lambda c: setattr(c, 'end_seconds', -1.0)), '5.12.7')
    expect_reject('clip keys a bone outside its scope',
                  turret_clip(lambda c: setattr(c.tracks[0], 'bone_index', 2)), '5.12.7')
    expect_reject('tracks not strictly increasing by bone',
                  turret_clip(lambda c: c.tracks.extend([nmo.Track(1)])), '5.12.7')
    expect_reject('key times not strictly increasing',
                  turret_clip(lambda c: c.tracks[0].rotation_keys.__setitem__(
                      1, (0.0, (0.0, 0.0, 0.0, 1.0)))), '5.12.7')
    expect_reject('duplicate clip names',
                  broken(lambda m: m.clips.append(nmo.Clip('Idle'))), '5.12.7')

    # 5.12.8 -- skinning.
    expect_reject('skinned submesh with no bone scope',
                  broken(lambda m: (m.sub_meshes[0].bones.clear(), m.bones.clear(),
                                    m.clips.clear())), '5.12.8')
    expect_reject('skin index outside the scope',
                  broken(lambda m: setattr(m.skin_buffers[0].skins[0], 'bone_indices',
                                           (3, 0, 0, 0))), '5.12.8')
    expect_reject('negative weight',
                  broken(lambda m: setattr(m.skin_buffers[0].skins[0], 'bone_weights',
                                           (1.5, -0.5, 0.0, 0.0))), '5.12.8')
    expect_reject('weights not descending',
                  broken(lambda m: setattr(m.skin_buffers[0].skins[0], 'bone_weights',
                                           (0.0, 1.0, 0.0, 0.0))), '5.12.8')
    expect_reject('weights not summing to one',
                  broken(lambda m: setattr(m.skin_buffers[0].skins[0], 'bone_weights',
                                           (0.5, 0.0, 0.0, 0.0))), '5.12.8')
    expect_reject('one vertex skinned by two scopes',
                  broken(lambda m: (setattr(m.sub_meshes[1], 'min_vertex', 0),
                                    setattr(m.sub_meshes[1], 'vertex_count', 36))), '5.12.8')

    # 5.12.9 -- markers.
    expect_reject('marker riding a bone outside the scope',
                  hull(lambda s: setattr(s.markers[0], 'parent_bone', 7)), '5.12.9')
    expect_reject('duplicate marker names',
                  hull(lambda s: setattr(s.markers[1], 'name', 'ExhaustPort')), '5.12.9')


def test_command_line():
    import tempfile
    with tempfile.TemporaryDirectory() as scratch:
        path = os.path.join(scratch, 'fixture.nmo')
        check('fixture writer runs', NmoFixture.main(['NmoFixture.py', path]) == 0)
        check('validate accepts the fixture', nmo.main(['NmoFormat.py', 'validate', path]) == 0)
        check('info runs', nmo.main(['NmoFormat.py', 'info', path]) == 0)
        with open(path, 'r+b') as handle:
            handle.write(b'XXXX')
        check('validate rejects a corrupt file', nmo.main(['NmoFormat.py', 'validate', path]) == 1)


def main():
    test_round_trip()
    test_preserves_forward_compatible_data()
    test_rejections()
    test_command_line()
    print('NmoRoundtripTest: %d checks passed' % PASSED)
    return 0


if __name__ == '__main__':
    sys.exit(main())
