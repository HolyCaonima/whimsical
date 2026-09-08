"""Migrate v9 Map render shapes to explicit engine Mesh assets (v10).

Usage: python tools/migrate_mesh_assets.py <map.asset> [<map.asset> ...]
Existing static and skinned geometry bindings are preserved.
"""
import argparse
import json
from pathlib import Path

ENGINE_MESHES = Path(__file__).resolve().parents[1] / 'engine/Content/Meshes'


def migrate(path):
    magic, header, payload = path.read_text(encoding='utf-8').split('\n', 2)
    if magic != 'ALAS1' or json.loads(header)['type'] != 'Map':
        raise ValueError(f'Expected a Map asset: {path}')
    scene = json.loads(payload)
    if scene['version'] == 10:
        return
    if scene['version'] != 9:
        raise ValueError(f'{path}: migrate to material asset references (v9) first')
    count = 0
    for entity in scene['entities']:
        components = entity['components']
        render = components.get('render')
        if render is None:
            continue
        shape = render.pop('shape', 'box')
        if 'mesh' not in render and 'skin' not in components:
            name = {'box': 'Box', 'capsule': 'Capsule'}[shape]
            mesh_header = json.loads((ENGINE_MESHES / f'{name}.asset').read_text().splitlines()[1])
            render['mesh'] = dict(id=mesh_header['id'], path=f'/Engine/Meshes/{name}')
            count += 1
    scene['version'] = 10
    pretty = '\n' in payload.strip()
    encoded = json.dumps(scene, ensure_ascii=False, indent=2 if pretty else None,
                         separators=None if pretty else (',', ':'))
    path.write_text(magic+'\n'+header+'\n'+encoded+'\n', encoding='utf-8')
    print(f'{path}: v10, bound {count} primitive Mesh assets')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('maps', nargs='+', type=Path)
    for map_path in parser.parse_args().maps:
        migrate(map_path)
