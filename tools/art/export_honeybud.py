"""Export the optimized kit without starting an expensive preview render."""
import bpy
from pathlib import Path

root = Path(__file__).resolve().parents[2] / 'Projects/Afterlight/SourceArt/Honeybud'
bpy.ops.object.select_all(action='DESELECT')
for obj in bpy.data.collections['Honeybud Court'].objects:
    if obj.type == 'MESH':
        obj.select_set(True)
bpy.ops.export_scene.gltf(filepath=str(root / 'HoneybudCourt.glb'),
                          export_format='GLB', use_selection=True, export_vertex_color='ACTIVE')
bpy.data.orphans_purge(do_recursive=True)
bpy.ops.wm.save_as_mainfile(filepath=str(root / 'HoneybudCourt.blend'))
print('Optimized GLB and Blender source saved; no offline render started.')
