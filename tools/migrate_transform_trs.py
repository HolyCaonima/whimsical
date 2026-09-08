"""Migrate Map v10 render dimensions into Transform TRS (v11).

Usage: python tools/migrate_transform_trs.py <map.asset> [<map.asset> ...]
Visual-only dimensions on entities with other spatial consumers become children.
"""
import argparse
import json
from pathlib import Path
import uuid


def rotate(q, v):
    x, y, z, w = q
    tx, ty, tz = 2*(y*v[2]-z*v[1]), 2*(z*v[0]-x*v[2]), 2*(x*v[1]-y*v[0])
    return [v[0]+w*tx+y*tz-z*ty, v[1]+w*ty+z*tx-x*tz, v[2]+w*tz+x*ty-y*tx]


def migrate_scene(scene):
    if scene['version'] == 11:
        return 0
    if scene['version'] != 10:
        raise ValueError('Migrate to explicit Mesh references (v10) first')
    parents = {e['components'].get('transform', {}).get('parent') for e in scene['entities']}
    children = []
    for entity in scene['entities']:
        components = entity['components']
        transform = components.get('transform')
        if transform is not None:
            transform['scale'] = [1, 1, 1]
        render = components.get('render')
        if render is None:
            continue
        scale = render.pop('scale', [1, 1, 1])
        animated = render.pop('animationScale', [1, 1, 1])
        scale = [a*b for a, b in zip(scale, animated)]
        offset = render.pop('offset', [0, 0, 0])
        if scale == [1, 1, 1] and offset == [0, 0, 0]:
            continue
        # Preserve physics, lights, animation and existing children's coordinate frames.
        spatial = {'collider', 'light', 'animator', 'joints', 'jointColliders', 'skin'}
        if spatial.intersection(components) or entity['id'] in parents:
            if 'skin' in components:
                raise ValueError(f"{entity['name']}: move skinned visual and animation to a child explicitly")
            children.append(dict(
                id=uuid.uuid5(uuid.NAMESPACE_URL, 'afterlight/visual/'+entity['id']).hex,
                name=entity['name']+' Visual', enabled=True,
                components=dict(transform=dict(parent=entity['id'], position=offset,
                                               rotation=[0, 0, 0, 1], scale=scale), render=render)))
            del components['render']
        else:
            delta = rotate(transform.get('rotation', [0, 0, 0, 1]), offset)
            transform['position'] = [a+b for a, b in zip(transform.get('position', [0, 0, 0]), delta)]
            transform['scale'] = scale
    scene['entities'].extend(children)
    scene['version'] = 11
    return len(children)


def migrate(path):
    magic, header, payload = path.read_text(encoding='utf-8').split('\n', 2)
    if magic != 'ALAS1' or json.loads(header)['type'] != 'Map':
        raise ValueError(f'Expected a Map asset: {path}')
    scene = json.loads(payload)
    if scene['version'] == 11:
        return
    count = migrate_scene(scene)
    pretty = '\n' in payload.strip()
    encoded = json.dumps(scene, ensure_ascii=False, indent=2 if pretty else None,
                         separators=None if pretty else (',', ':'))
    path.write_text(magic+'\n'+header+'\n'+encoded+'\n', encoding='utf-8')
    print(f'{path}: v11, {count} visual children')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('maps', nargs='+', type=Path)
    for map_path in parser.parse_args().maps:
        migrate(map_path)
