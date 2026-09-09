"""Call the upstream Blender MCP server over MCP stdio, retaining a local audit."""
import asyncio
import json
import os
import sys
from pathlib import Path
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

async def main():
    env = dict(os.environ, BLENDER_MCP_DISABLE_TELEMETRY='1', BLENDER_HOST='127.0.0.1')
    params = StdioServerParameters(command=sys.executable, args=['-m', 'blender_mcp.server'], env=env)
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            if len(sys.argv) == 1:
                result = await session.call_tool('get_scene_info', {'user_prompt': 'Inspect the isolated authoring scene.'})
            else:
                path = Path(sys.argv[1]).resolve()
                code = "import runpy; runpy.run_path(" + repr(str(path)) + ", run_name='__main__')"
                result = await session.call_tool('execute_blender_code', {'code': code, 'user_prompt': 'Create original Honeybud Court assets for the Afterlight project from the supplied style references.'})
            output = result.model_dump(mode='json')
            print(json.dumps(output, ensure_ascii=False))
            audit = Path(__file__).resolve().parents[2] / 'captures/honeybud'
            audit.mkdir(parents=True, exist_ok=True)
            (audit / 'mcp-last-result.json').write_text(json.dumps(output, indent=2), encoding='utf-8')
            if result.isError or any(getattr(c, 'text', '').startswith('Error') for c in result.content):
                raise RuntimeError('Blender MCP tool failed')

asyncio.run(main())
