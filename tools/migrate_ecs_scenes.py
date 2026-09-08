"""Upgrade ALAS1 Map payloads to optional ECS components, preserving asset/entity IDs.

Usage: python tools/migrate_ecs_scenes.py <Map.asset> [...]
The runtime imports v3-v6; new output uses v7. This tool also removes inert recipe colliders from
authored maps; geometry and collider dimensions remain independent.
"""
import json
import math
import sys
import uuid
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


def upgrade_v4(scene):
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


def upgrade_v6(scene):
    if scene['version'] == 6:
        return scene
    scene = upgrade_v4(scene)
    lights = []
    for index, light in enumerate(scene.pop('lights')):
        entity = dict(id=uuid.uuid4().hex, name=f'Light {index + 1}', enabled=True, components=dict(
            transform=dict(position=light['positionRadius'][:3]),
            light=dict(radius=light['positionRadius'][3], color=light['colorIntensity'][:3], intensity=light['colorIntensity'][3])))
        lights.append(entity)
    if 'cyanLight' in scene.get('data', {}):
        scene['references']['cyanLight'] = lights[scene['data'].pop('cyanLight')]['id']
    scene['entities'].extend(lights)
    scene['version'] = 6
    return scene


def upgrade_v7(scene):
    if scene['version'] >= 7:
        return scene
    scene = upgrade_v6(scene)
    for entity in scene['entities']:
        light = entity['components'].get('light')
        if light is not None:
            light['type'] = 'point'
            light['intensity'] = light.get('intensity', 1)*4*math.pi*sum(light.get('color', [1,1,1]))
    scene['version'] = 7
    return scene


def upgrade(scene):
    scene = upgrade_v7(scene)
    player = scene.pop('player', '')
    if player:
        scene['references'].setdefault('player', player)
    scene['version'] = 8
    return scene


if __name__ == '__main__':
    for argument in sys.argv[1:]:
        path = Path(argument)
        magic, header, payload = path.read_text(encoding='utf-8').split('\n', 2)
        if magic != 'ALAS1' or json.loads(header)['type'] != 'Map':
            raise ValueError(f'Not a Map asset: {path}')
        scene = upgrade(json.loads(payload))
        encoded = json.dumps(scene, ensure_ascii=False, indent=2) if '\n' in payload.strip() else json.dumps(scene, ensure_ascii=False, separators=(',', ':'))
        path.write_bytes((magic + '\n' + header + '\n' + encoded + '\n').encode('utf-8'))
        print(f'{path}: {len(scene["entities"])} entities')
