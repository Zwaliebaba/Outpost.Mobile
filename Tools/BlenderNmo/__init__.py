"""Blender add-on: import and export Neuron NMO ship meshes (Design/Archive/NmoFormat.md).

Ships both registration shapes so either installer works: blender_manifest.toml for the 4.2+
extensions platform, and this bl_info for the legacy add-on installer. The operators are thin --
the format lives in NmoFormat (pure Python, no bpy), the scene mapping in NmoScene, and the two
directions in NmoImport / NmoExport.
"""

bl_info = {
    'name': 'Neuron NMO format',
    'author': 'Outpost',
    'description': 'Import and export Neuron NMO ship meshes (.nmo): submeshes, materials, '
                   'bone animation and typed markers (exhausts, navigation lights, guns)',
    'version': (2, 0, 0),
    'blender': (4, 2, 0),
    'location': 'File > Import/Export > Neuron Mesh (.nmo)',
    'category': 'Import-Export',
}

import bpy

from bpy.props import StringProperty
from bpy_extras.io_utils import ExportHelper, ImportHelper

from . import NmoFormat
from . import NmoImport
from . import NmoExport


class NMO_OT_import(bpy.types.Operator, ImportHelper):
    """Import a Neuron NMO mesh, its skeletons, clips and markers"""

    bl_idname = 'import_scene.nmo'
    bl_label = 'Import Neuron Mesh (.nmo)'
    bl_options = {'REGISTER', 'UNDO'}

    filename_ext = '.nmo'
    filter_glob: StringProperty(default='*.nmo', options={'HIDDEN'})

    def execute(self, context):
        try:
            collections = NmoImport.import_file(context, self.filepath)
        except (OSError, NmoFormat.NmoError) as error:
            self.report({'ERROR'}, str(error))
            return {'CANCELLED'}
        self.report({'INFO'}, 'Imported %d mesh(es) from %s' % (len(collections), self.filepath))
        return {'FINISHED'}


class NMO_OT_export(bpy.types.Operator, ExportHelper):
    """Export tagged NMO collections (or the active collection) as a Neuron NMO mesh"""

    bl_idname = 'export_scene.nmo'
    bl_label = 'Export Neuron Mesh (.nmo)'

    filename_ext = '.nmo'
    filter_glob: StringProperty(default='*.nmo', options={'HIDDEN'})

    def execute(self, context):
        try:
            written = NmoExport.export_file(context, self.filepath)
        except (OSError, NmoFormat.NmoError) as error:
            # The exporter validates its own output through the codec before anything lands on
            # disk, so a mapping defect surfaces here as a refused export naming the broken rule.
            self.report({'ERROR'}, str(error))
            return {'CANCELLED'}
        self.report({'INFO'}, 'Wrote %s (%d bytes)' % (self.filepath, written))
        return {'FINISHED'}


def _menu_import(self, _context):
    self.layout.operator(NMO_OT_import.bl_idname, text='Neuron Mesh (.nmo)')


def _menu_export(self, _context):
    self.layout.operator(NMO_OT_export.bl_idname, text='Neuron Mesh (.nmo)')


CLASSES = (NMO_OT_import, NMO_OT_export)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.TOPBAR_MT_file_import.append(_menu_import)
    bpy.types.TOPBAR_MT_file_export.append(_menu_export)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(_menu_export)
    bpy.types.TOPBAR_MT_file_import.remove(_menu_import)
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
