#!/usr/bin/env python3
"""Batch-converts the authored GLB hulls in this folder to NMO (Design/NmoFormat.md).

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
CODEC = os.path.join(REPO, 'Tools', 'BlenderNmo')  # NmoFormat, for the read-back summary
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


def _warn_dropped_materials(collection):
    """One submesh carries one material (5.4). A GLB whose mesh splits over several would lose
    all but the first here, silently -- so say so, and name the fix (split the object in Blender).
    """
    for obj in collection.objects:
        if obj.type == 'MESH' and len(obj.data.materials) > 1:
            print('  warning: %r has %d material slots; only %r survives -- split the object per '
                  'material to keep the rest' % (obj.name, len(obj.data.materials),
                                                 obj.data.materials[0].name))


def _short(path):
    """Repo-relative where that is shorter -- an --out pointing outside stays absolute."""
    relative = os.path.relpath(path, REPO)
    return path if relative.startswith('..') else relative


def _summary(path):
    """What actually landed, read back through the codec rather than reported by the writer."""
    sys.path.insert(0, CODEC)
    import NmoFormat as nmo
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

    if 'FINISHED' not in bpy.ops.export_scene.nmo(filepath=target):
        # The operator reports the broken rule itself, above this line, and writes nothing.
        raise RuntimeError('the exporter refused %s' % os.path.basename(source))
    print('  %s (%d bytes): %s' % (_short(target), os.path.getsize(target), _summary(target)))


def run_in_blender(args):
    import bpy

    _require_operators(bpy)
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
