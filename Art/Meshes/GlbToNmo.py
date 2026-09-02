#!/usr/bin/env python3
"""Batch-converts the authored GLB hulls in this folder to NMO (Design/Archive/NmoFormat.md).

The conversion itself is the Blender add-on's (Tools/BlenderNmo): the axis swap, the winding
reversal, materials, skinning, clips and markers all live there, stated once so import and export
cannot drift. This script is only the driver -- it starts Blender headless, and per file empties
the scene, imports the GLB through Blender's own glTF importer, groups what arrived into one
collection named for the file (the add-on names the NMO mesh after it), and runs the add-on's
exporter over it. Nothing here knows the format.

Run it with a plain interpreter; it finds Blender and re-invokes itself inside it:

    python Art/Meshes/GlbToNmo.py                      convert every .glb here, .nmo beside it
    python Art/Meshes/GlbToNmo.py Corvette.glb         just this one
    python Art/Meshes/GlbToNmo.py --out Outpost/Assets/Meshes
    python Art/Meshes/GlbToNmo.py --blender "D:/Blender/blender.exe"

The add-on validates every model through the codec before it writes, so a file that lands on disk
is one the reader accepts; this script reads each result back anyway and prints what it holds, so
a conversion that succeeds and produces the wrong thing is visible rather than silent. Two things
a GLB cannot carry are worth reading in that summary: **markers** (exhausts, navigation lights,
gun mounts) and **clips** come out as zero unless the GLB was authored with them -- the OBJ path's
recovered exhausts do not survive a trip through a mesh exported from a modelling package, and are
authored in Blender afterwards.
"""

import argparse
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
TOOLS = os.path.join(REPO, 'Tools')
CODEC = os.path.join(TOOLS, 'BlenderNmo')  # the add-on, whose schema and codec this reuses
# The GLB author's spelling of the two marker fields the add-on reads off the object rather
# than off a custom property (_adopt_markers translates them). The other four extras keys the
# hulls carry -- nmo_kind, nmo_param0, nmo_param1, nmo_flags -- are already the add-on's own.
GLTF_SCALE = 'nmo_scale'
GLTF_COLOUR = 'nmo_colour'
GLTF_COLOUR_HEX = 'nmo_colour_hex'

# The glTF importer narrates every node it builds at INFO, which buries this script's own
# output. Its `loglevel` property is not the knob -- it overwrites it from bpy.app.debug_value
# on every run (io_scene_gltf2's set_debug_log), and 2 is that function's ERROR.
GLTF_QUIET = 2

# Where Blender installs itself, most recent first once the version sort has run. An explicit
# --blender or $BLENDER wins over all of it.
BLENDER_GLOBS = (
    r'C:\Program Files\Blender Foundation\Blender*\blender.exe',
    r'C:\Program Files (x86)\Blender Foundation\Blender*\blender.exe',
    '/usr/bin/blender',
    '/usr/local/bin/blender',
    '/Applications/Blender.app/Contents/MacOS/Blender',
)


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description='Convert GLB hulls to NMO through the Blender add-on.')
    parser.add_argument('meshes', nargs='*',
                        help='GLB files to convert (default: every .glb beside this script)')
    parser.add_argument('--out', metavar='DIR',
                        help='write the .nmo files here (default: beside each .glb)')
    parser.add_argument('--blender', metavar='EXE',
                        help='the Blender to run (default: $BLENDER, PATH, then the installed one)')
    return parser.parse_args(argv)


def resolve_inputs(args):
    """The GLB paths to convert, absolute, in a stable order."""
    if args.meshes:
        paths = [os.path.abspath(path) for path in args.meshes]
    else:
        paths = sorted(glob.glob(os.path.join(HERE, '*.glb')))
    return paths


# --- outside Blender: find it and re-invoke ------------------------------------------------------

def _version_key(path):
    """Sorts installs newest first: the trailing digits of the directory name, numerically."""
    numbers = re.findall(r'\d+', os.path.basename(os.path.dirname(path)))
    return tuple(-int(number) for number in numbers) or (0,)


def find_blender(explicit):
    for candidate in (explicit, os.environ.get('BLENDER')):
        if candidate:
            if os.path.isfile(candidate):
                return candidate
            print('error: %s is not a file' % candidate)
            return None
    from shutil import which
    found = which('blender')
    if found:
        return found
    for pattern in BLENDER_GLOBS:
        matches = sorted((path for path in glob.glob(pattern) if os.path.isfile(path)),
                         key=_version_key)
        if matches:
            return matches[0]
    return None


def relaunch(argv, args):
    blender = find_blender(args.blender)
    if blender is None:
        print('error: no Blender found. Pass --blender <path to blender.exe> or set $BLENDER.')
        return 1
    print('%s' % blender, flush=True)  # flushed: the child's output interleaves after it
    # --python-exit-code before --python, so an exception in here fails the run rather than
    # exiting 0 with a traceback in the log. No --factory-startup: that would disable the
    # user's add-ons, and the NMO exporter is one of them.
    command = [blender, '--background', '--python-exit-code', '1',
               '--python', os.path.abspath(__file__), '--'] + argv
    return subprocess.call(command)


# --- inside Blender: the conversion ---------------------------------------------------------------

def _load_schema():
    """The add-on's own modules, from the repository copy: NmoScene for the property names and
    the marker display table, NmoFormat for reading a written file back. Named rather than
    re-spelled, so this script cannot drift from the schema it is writing into."""
    global scene_map, nmo
    # Both paths, as Tools/NmoBlenderTest.py sets them up: NmoScene ends on a relative import, so
    # it loads as part of the BlenderNmo package and not on its own, while NmoFormat is stdlib-only
    # and loads flat.
    for entry in (CODEC, TOOLS):
        if entry not in sys.path:
            sys.path.insert(0, entry)
    import NmoFormat
    from BlenderNmo import NmoScene
    scene_map = NmoScene
    nmo = NmoFormat


def _require_operators(bpy):
    """Both halves of the pipeline have to be registered before the first file is touched."""
    if 'nmo' not in dir(bpy.ops.export_scene):
        raise SystemExit('error: the Neuron NMO add-on is not enabled in this Blender. Install '
                         'Tools/BlenderNmo.zip through Edit > Preferences > Add-ons and enable it.')
    if 'gltf' not in dir(bpy.ops.import_scene):
        try:
            bpy.ops.preferences.addon_enable(module='io_scene_gltf2')
        except RuntimeError as error:
            raise SystemExit('error: Blender\'s glTF importer is not available (%s)' % error)


def _regroup(bpy, name):
    """Everything the import made, in one collection tagged the way an NMO import tags its own.

    The exporter prefers tagged collections over the active one, and names the mesh after the
    collection -- so the tag is what makes the NMO mesh come out called "Corvette" rather than
    "Scene Collection", without depending on which collection is active in a headless session.
    """
    scene = bpy.context.scene
    collection = bpy.data.collections.new(name)
    scene.collection.children.link(collection)
    collection['nmo_mesh'] = 0
    for obj in list(scene.collection.all_objects):
        for owner in list(obj.users_collection):
            owner.objects.unlink(obj)
        collection.objects.link(obj)
    return collection


# Design/Archive/NmoFormat.md 13.1: which half of the corpus's material vocabulary is the model's own paint
# and which half is the faction's, and what shade each liveried one is. Applied here, by name, once,
# at conversion -- never in the loader, because "the material called plate is the liveried one" is a
# convention a rename breaks silently, where a flag is authored, round-tripped and visible in the
# file. A material whose GLB extras already state nmo_render_flags or nmo_base_colour keeps those
# instead, so an author can override the row at source.
#
# The three liveried rows are greyscale on purpose: they are a brightness ladder, and that ladder is
# the whole content of them -- plate reads as body, accent as trim, thruster as hot, at every livery
# and in the same order. A hue painted into one would be discarded by the renderer's multiply and
# would mislead whoever opened the file next.
MATERIAL_SHADES = {
    'hull': (False, (0.27, 0.27, 0.27)),      # structural plating; the ship's own
    'glass': (False, (0.12, 0.12, 0.12)),     # canopy, dark enough to read as a window
    'plate': (True, (0.45, 0.45, 0.45)),      # the painted panels -- the biggest liveried area
    'accent': (True, (0.80, 0.80, 0.80)),     # trim: the stripe that names the faction at distance
    'thruster': (True, (1.00, 1.00, 1.00)),   # nozzles, the brightest thing on an unlit hull
    'aperture': (True, (0.80, 0.80, 0.80)),   # Stargate's sixth name; follows accent
}
GLTF_RENDER_FLAGS = 'nmo_render_flags'  # an extras key that wins over the table above
GLTF_BASE_COLOUR = 'nmo_base_colour'

# A quarter turn about Blender's +X, which is what points a marker's arrow aft.
#
# A marker's direction is its local +Z (Design/Archive/NmoFormat.md 5.10), and an empty's arrow is its
# Blender +Z, which is *up* -- so an Exhaust the GLB left unrotated aims its plume at the sky. Aft
# in Blender is -Y (the bow is +Y, section 11), and +90 degrees about +X is the rotation that takes
# +Z there, and the swap lands it on a half turn about NMO's +X -- which aims the plume at -Z, the
# same direction Tools/ObjToNmo.py has always written, differing from the fixture's half turn about
# +Y only by a roll about the plume's own axis, which a cone has no opinion about. An authored
# rotation passes through untouched.
EXHAUST_AIM_AFT = (0.70710678, 0.70710678, 0.0, 0.0)  # (w, x, y, z), the order mathutils takes


def _adopt_materials(collection):
    """Apply Design/Archive/NmoFormat.md 13.1's table to the collection's materials, by name.

    Returns how many came out liveried, so a hull that lost its paint in conversion is a number in
    the summary rather than something noticed in a screenshot three days later. A material the table
    does not know is left exactly as authored and traced once -- a new name should be noticed, not
    silently drawn as the model's own paint.
    """
    flagged = 0
    seen = set()
    for obj in collection.objects:
        for mat in (obj.data.materials if obj.type == 'MESH' else []):
            if mat is None or mat.name in seen:
                continue
            seen.add(mat.name)
            row = MATERIAL_SHADES.get(mat.name)
            if row is None:
                print('  warning: material %r is not in the livery table; left as authored'
                      % mat.name)
                continue
            liveried, shade = row
            # Whether the author stated flags at source, read BEFORE anything below writes the same
            # key: GLTF_RENDER_FLAGS and PROP_RENDER_FLAGS are one property, so a check made after
            # the liveried row set it would mistake this table's own work for the author's word.
            authored_flags = GLTF_RENDER_FLAGS in mat
            if GLTF_BASE_COLOUR not in mat:
                _set_base_colour(mat, shade, _authored_opacity(mat) if _is_blended(mat) else 1.0)
            if authored_flags:
                mat[scene_map.PROP_RENDER_FLAGS] = int(mat[GLTF_RENDER_FLAGS])
            elif liveried:
                mat[scene_map.PROP_RENDER_FLAGS] = (int(mat.get(scene_map.PROP_RENDER_FLAGS, 0))
                                                    | nmo.RENDER_FLAG_RACE_TINTED)
            # A material the GLB authored blended stays blended: the flag routes it into the
            # renderer's blended overlay pass, and the opacity rides base_colour.w through
            # _set_base_colour above. Additive to the table's row, never instead of it -- the
            # Stargate's aperture is both liveried and translucent.
            if _is_blended(mat) and not authored_flags:
                mat[scene_map.PROP_RENDER_FLAGS] = (int(mat.get(scene_map.PROP_RENDER_FLAGS, 0))
                                                    | nmo.RENDER_FLAG_ALPHA_BLEND)
            if int(mat.get(scene_map.PROP_RENDER_FLAGS, 0)) & nmo.RENDER_FLAG_RACE_TINTED:
                flagged += 1
    return flagged


def _set_base_colour(mat, shade, alpha=1.0):
    """Both places the exporter reads a base colour from, so the two cannot disagree."""
    mat.diffuse_color = (shade[0], shade[1], shade[2], alpha)
    if mat.use_nodes:
        principled = next((n for n in mat.node_tree.nodes if n.type == 'BSDF_PRINCIPLED'), None)
        if principled is not None:
            principled.inputs['Base Color'].default_value = (shade[0], shade[1], shade[2], alpha)


def _authored_opacity(mat):
    """The GLB's opacity: the Principled Alpha input the glTF importer filled from
    baseColorFactor, with the viewport colour as the node-free fallback."""
    if mat.use_nodes:
        principled = next((n for n in mat.node_tree.nodes if n.type == 'BSDF_PRINCIPLED'), None)
        if principled is not None and 'Alpha' in principled.inputs:
            return float(principled.inputs['Alpha'].default_value)
    return float(mat.diffuse_color[3])


def _is_blended(mat):
    """Whether the GLB authored this material translucent: its opacity says so and nothing else is
    asked. Blender's material blend properties were both tried and both lied under 5.1's importer
    -- surface_render_method reads BLENDED on every material and blend_method no longer means what
    it did -- where the Alpha input holds exactly the baseColorFactor alpha the artist wrote. The
    one shape this cannot see, alphaMode BLEND at opacity 1.0, draws identically opaque anyway."""
    return _authored_opacity(mat) < 0.999


def _luminance(colour):
    return 0.2126 * colour[0] + 0.7152 * colour[1] + 0.0722 * colour[2]


def _warn_dropped_materials(collection):
    """One submesh carries one material (5.4). A GLB whose mesh splits over several would lose
    all but the first here, silently -- so say so, and name the fix (split the object in Blender).
    """
    for obj in collection.objects:
        if obj.type == 'MESH' and len(obj.data.materials) > 1:
            print('  warning: %r has %d material slots; only %r survives -- split the object per '
                  'material to keep the rest' % (obj.name, len(obj.data.materials),
                                                 obj.data.materials[0].name))


def _adopt_markers(bpy, collection):
    """Translate the GLB's marker nodes into the add-on's scene schema.

    The hulls carry their exhausts, navigation lights and gun mounts as node `extras`, and the
    glTF importer copies every key onto the empty as a custom property. Four of them are already
    the add-on's own schema and need nothing: `nmo_kind`, `nmo_param0`, `nmo_param1`, `nmo_flags`.
    The other two are the GLB author's encoding of fields the add-on reads off the object itself,
    and this is where the two spellings meet:

      - `nmo_colour` -> `object.color`, the exporter's marker colour (linear RGBA, as the file
        wants it, so it copies straight through).
      - `nmo_scale` -> the empty's world scale, the exporter's marker scale. It is assigned
        through matrix_world rather than through `.scale`, because these empties hang under a
        group node the GLB scales by ~3.5, and the exporter decomposes the world matrix: a local
        scale would arrive multiplied by the parent's, and the authored nozzle radius would be
        wrong by whatever that node happens to hold.

    Both are then deleted, so the scene holds one statement of each value rather than a live one
    and a stale copy an artist could edit expecting it to matter. The display type is set from the
    kind for the same reason a marker imported from a .nmo gets one: so the cones and spheres look
    like themselves when this is opened in Blender to author on top of it.
    """
    from mathutils import Matrix, Quaternion

    adopted = 0
    flagged = 0
    for obj in collection.objects:
        if obj.type != 'EMPTY' or scene_map.PROP_KIND not in obj:
            continue
        kind = str(obj[scene_map.PROP_KIND])
        obj.empty_display_type = scene_map.MARKER_DISPLAY.get(kind,
                                                              scene_map.MARKER_DISPLAY_FALLBACK)
        obj.empty_display_size = 1.0
        colour = obj.get(GLTF_COLOUR)
        if colour is not None:
            obj.color = tuple(colour)[:4]

        # Exhaust is livery and NavLight and Gun are not (Design/Archive/NmoFormat.md 5.10): port red and
        # starboard green are a convention older than any faction here, and liveried they would turn
        # red-on-red for the Vandals. An extras nmo_flags wins over that default -- and "stated" has
        # to mean a *nonzero* word rather than a present key, because the authoring tool writes
        # nmo_flags: 0 onto every marker it exports, so a presence test would never let the default
        # fire at all. No other marker bit is defined, so nothing is lost by reading 0 as unstated;
        # the day a second bit exists, an author who wants an unliveried exhaust sets that one.
        if kind == nmo.MARKER_KIND_EXHAUST and int(obj.get(scene_map.PROP_MARKER_FLAGS, 0)) == 0:
            obj[scene_map.PROP_MARKER_FLAGS] = nmo.MARKER_FLAG_RACE_TINTED
        if int(obj.get(scene_map.PROP_MARKER_FLAGS, 0)) & nmo.MARKER_FLAG_RACE_TINTED:
            # A flagged colour is a shade, not a colour (5.5): the hue is discarded by the multiply,
            # so writing the luminance is the only honest thing to leave in the file. A green plume
            # left green would read as authored intent that the renderer then throws away.
            shade = _luminance(tuple(obj.color)[:3])
            obj.color = (shade, shade, shade, tuple(obj.color)[3])
            flagged += 1

        translation, rotation, _ = obj.matrix_world.decompose()
        scale = float(obj.get(GLTF_SCALE, 1.0))
        # An identity-rotated exhaust points its +Z at the bow, which is backwards for a plume.
        # Nothing reads a marker's orientation yet; this closes the content defect rather than
        # leaving it for the slice that finally aims one.
        if kind == nmo.MARKER_KIND_EXHAUST and _is_identity_rotation(rotation):
            rotation = Quaternion(EXHAUST_AIM_AFT)
        obj.matrix_world = (Matrix.Translation(translation) @ rotation.to_matrix().to_4x4()
                            @ Matrix.Diagonal((scale, scale, scale, 1.0)))
        for consumed in (GLTF_COLOUR, GLTF_COLOUR_HEX, GLTF_SCALE):
            if consumed in obj:
                del obj[consumed]
        adopted += 1
    return adopted, flagged


def _is_identity_rotation(rotation, tolerance=1e-4):
    """A quaternion within a whisker of (1, 0, 0, 0), either sign -- q and -q are one rotation."""
    return abs(abs(rotation.w) - 1.0) <= tolerance


def _short(path):
    """Repo-relative where that is shorter -- an --out pointing outside stays absolute."""
    relative = os.path.relpath(path, REPO)
    return path if relative.startswith('..') else relative


def _summary(path):
    """What actually landed, read back through the codec rather than reported by the writer."""
    model = nmo.read_file(path)
    mesh = model.meshes[0]
    triangles = sum(sub.primitive_count for sub in mesh.sub_meshes)
    vertices = sum(len(buffer.vertices) for buffer in mesh.vertex_buffers)
    markers = sum(len(sub.markers) for sub in mesh.sub_meshes)
    clips = len(mesh.clips) + sum(len(sub.clips) for sub in mesh.sub_meshes)
    return ('%d submesh(es), %d triangles, %d vertices, %d material(s), %d bone(s), %d clip(s), '
            '%d marker(s)' % (len(mesh.sub_meshes), triangles, vertices, len(mesh.materials),
                              len(mesh.bones), clips, markers))


def convert(bpy, source, out_dir):
    name = os.path.splitext(os.path.basename(source))[0]
    target = os.path.join(out_dir, name + '.nmo')

    bpy.ops.wm.read_homefile(use_empty=True)  # not read_factory_settings: it would drop the add-on
    bpy.ops.import_scene.gltf(filepath=source)
    collection = _regroup(bpy, name)
    if not any(obj.type == 'MESH' for obj in collection.objects):
        raise RuntimeError('%s holds no mesh' % os.path.basename(source))
    _warn_dropped_materials(collection)
    liveried_materials = _adopt_materials(collection)
    _, liveried_markers = _adopt_markers(bpy, collection)

    if 'FINISHED' not in bpy.ops.export_scene.nmo(filepath=target):
        # The operator reports the broken rule itself, above this line, and writes nothing.
        raise RuntimeError('the exporter refused %s' % os.path.basename(source))
    print('  %s (%d bytes): %s, %d liveried material(s), %d liveried marker(s)'
          % (_short(target), os.path.getsize(target), _summary(target), liveried_materials,
             liveried_markers))


def run_in_blender(args):
    import bpy

    _require_operators(bpy)
    _load_schema()
    bpy.app.debug_value = GLTF_QUIET
    sources = resolve_inputs(args)
    if not sources:
        print('nothing to convert: no .glb in %s' % HERE)
        return 1

    failed = []
    for source in sources:
        print('%s' % _short(source))
        out_dir = os.path.abspath(args.out) if args.out else os.path.dirname(source)
        os.makedirs(out_dir, exist_ok=True)
        try:
            convert(bpy, source, out_dir)
        except (OSError, RuntimeError) as error:
            print('  error: %s' % error)
            failed.append(os.path.basename(source))

    print('%d of %d converted' % (len(sources) - len(failed), len(sources)))
    if failed:
        print('failed: %s' % ', '.join(failed))
    return 1 if failed else 0


def main():
    inside_blender = '--' in sys.argv and os.path.basename(sys.argv[0]).startswith('blender')
    if not inside_blender:
        try:
            import bpy  # noqa: F401  (the bpy wheel, or a Blender that invoked us another way)
            inside_blender = True
        except ImportError:
            inside_blender = False

    argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else sys.argv[1:]
    args = parse_args(argv)
    return run_in_blender(args) if inside_blender else relaunch(argv, args)


if __name__ == '__main__':
    sys.exit(main())
