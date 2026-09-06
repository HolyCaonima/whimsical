"""Start the upstream Blender MCP addon in an isolated Blender GUI process."""
import importlib.util
import os
from pathlib import Path

root = Path(__file__).resolve().parents[2]
os.environ['BLENDER_MCP_DISABLE_TELEMETRY'] = '1'
spec = importlib.util.spec_from_file_location('blender_mcp_addon', root / 'third_party/blender-mcp/addon.py')
addon = importlib.util.module_from_spec(spec)
import sys
sys.modules[spec.name] = addon
spec.loader.exec_module(addon)
addon.register()
addon.bpy.types.blendermcp_server.stop()
server = addon.BlenderMCPServer(host='127.0.0.1', port=9876)
addon.bpy.types.blendermcp_server = server
server.start()
