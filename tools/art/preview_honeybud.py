"""Save a glTF interchange copy and render the Blender authoring preview."""
import bpy
from pathlib import Path
root=Path(__file__).resolve().parents[2]/'Projects/Afterlight/SourceArt/Honeybud'
bpy.ops.object.select_all(action='DESELECT')
for obj in bpy.data.collections['Honeybud Court'].objects:
    if obj.type=='MESH':obj.select_set(True)
bpy.ops.export_scene.gltf(filepath=str(root/'HoneybudCourt.glb'),export_format='GLB',use_selection=True)
# Rendering runs from a timer so the MCP request can acknowledge before a long render.
def render():
    bpy.ops.render.render(write_still=True)
    return None
bpy.app.timers.register(render,first_interval=1)
print('GLB exported; Cycles preview queued.')
