"""Upgrade ALAS1 Map payloads to optional ECS components, preserving asset/entity IDs.

Usage: python tools/migrate_ecs_scenes.py <Map.asset> [...]
The runtime imports v3/v4; new output uses v5. This tool also removes inert recipe colliders from
authored maps; geometry and collider dimensions remain independent.
"""
import json
import sys
from pathlib import Path


def upgrade_v3(scene):
    if scene['version'] == 4:
        return scene
    if scene['version'] != 3:
        raise ValueError('Expected Map version 3 or 4')
    entities = []
    for o in scene.pop('objects'):
        components = dict(transform=dict(position=o['position'], rotation=o['rotation']), render=o['render'])
        collider = o['physics']
        if any(collider[k] for k in ('blocking', 'walkable', 'pickable')) or (o['animation'] and o['animation']['rootMotion']):
            components['collider'] = collider
        if o['interactable']:
            components['interactable'] = {}
        if o['animation']:
            animator = dict(o['animation'])
            mesh = animator.pop('mesh')
            components['animator'] = animator
            if mesh:
                components['skin'] = mesh
        if o['joints']:
            components['joints'] = o['joints']
        if o['jointColliders']:
            components['jointColliders'] = o['jointColliders']
        entities.append(dict(id=o['id'], name=o['name'], enabled=o['enabled'], components=components))
    scene['version'] = 4
    scene['entities'] = entities
    return scene


def upgrade(scene):
    if scene['version'] == 5:
        return scene
    scene = upgrade_v3(scene)
    for entity in scene['entities']:
        components = entity['components']
        if components.get('jointColliders') == []:
            del components['jointColliders']
        if 'animator' in components:
            root = components['animator'].pop('rootMotion', True)
            if root:
                components['rootMotion'] = dict(mode='grounded', preserveAnchor=True)
    scene['version'] = 5
    return scene


if __name__ == '__main__':
    for argument in sys.argv[1:]:
        path = Path(argument)
        magic, header, payload = path.read_text(encoding='utf-8').split('\n', 2)
        if magic != 'ALAS1' or json.loads(header)['type'] != 'Map':
            raise ValueError(f'Not a Map asset: {path}')
        scene = upgrade(json.loads(payload))
        encoded = json.dumps(scene, ensure_ascii=False, indent=2) if '\n' in payload.strip() else json.dumps(scene, ensure_ascii=False, separators=(',', ':'))
        path.write_text(magic + '\n' + header + '\n' + encoded + '\n', encoding='utf-8')
        print(f'{path}: {len(scene["entities"])} entities')
