"""Extract v7/v8 Map material tables into Material assets and write v9 references.

Usage: python tools/migrate_material_assets.py <map.asset> [<map.asset> ...]
Run once on authored maps; loading a map never creates or rewrites assets.
"""
import argparse
import json
import re
import uuid
from pathlib import Path


def read_asset(path):
    magic, header, payload = path.read_text(encoding='utf-8').split('\n', 2)
    if magic != 'ALAS1':
        raise ValueError(f'Not an ALAS1 asset: {path}')
    return json.loads(header), json.loads(payload)


def encode(header, payload):
    return 'ALAS1\n' + json.dumps(header, ensure_ascii=False) + '\n' + json.dumps(payload, ensure_ascii=False, indent=2) + '\n'


def migrate(path):
    original = path.read_text(encoding='utf-8').split('\n', 2)
    header, scene = read_asset(path)
    if header['type'] != 'Map' or scene['version'] == 9:
        return
    if scene['version'] not in (7, 8):
        raise ValueError(f'{path}: migrate older ECS/light formats before material extraction')
    content = next(p for p in path.parents if p.name == 'Content')
    material_dir = next((p for p in content.iterdir() if p.is_dir() and p.name.lower() == 'materials'), content / 'Materials')
    bindings = {item['index']: item['asset'] for item in scene.get('materialAssets', [])}
    outputs = {}
    used_names = set()
    for index, definition in enumerate(scene['materials']):
        if index in bindings:
            continue
        names = [e['name'] for e in scene['entities'] if e['components'].get('render', {}).get('material') == index]
        label = names[0] if names else f'Material {index + 1}'
        name = re.sub(r'[^\w]+', '_', label, flags=re.UNICODE).strip('_') or 'Material'
        if name in used_names:
            name += f'_{index}'
        used_names.add(name)
        relative = Path(material_dir.name) / path.stem / name
        material_header = dict(id=uuid.uuid5(uuid.NAMESPACE_URL, f'afterlight/material/{header["id"]}/{index}').hex,
                               type='Material', name=label, version=1, storage='embedded', metadata={})
        target = content / relative.with_suffix('.asset')
        text = encode(material_header, definition)
        if target.exists() and read_asset(target) != (material_header, definition):
            raise ValueError(f'Refusing to overwrite a different asset: {target}')
        outputs[target] = text
        bindings[index] = dict(id=material_header['id'], path='/Game/' + relative.as_posix())
    for entity in scene['entities']:
        render = entity['components'].get('render')
        if render is not None:
            render['material'] = bindings[render.get('material', 0)]
    scene.pop('materials')
    scene.pop('materialAssets', None)
    player = scene.pop('player', '')
    if player:
        scene['references']['player'] = player
    scene['version'] = 9
    for target, text in outputs.items():
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding='utf-8')
    pretty = '\n' in original[2].strip()
    payload = json.dumps(scene, ensure_ascii=False, indent=2 if pretty else None,
                         separators=None if pretty else (',', ':'))
    path.write_text(original[0] + '\n' + original[1] + '\n' + payload + '\n', encoding='utf-8')
    print(f'{path}: v9, extracted {len(outputs)} Material assets')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('maps', nargs='+', type=Path)
    for map_path in parser.parse_args().maps:
        migrate(map_path.resolve())
