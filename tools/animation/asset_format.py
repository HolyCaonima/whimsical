"""Offline .asset authoring; payload codecs remain independent of the envelope."""
import json
import uuid
from pathlib import Path


def read_asset(path):
    with Path(path).open('rb') as file:
        if file.readline().rstrip(b'\r\n') != b'ALAS1':
            raise ValueError(f'Invalid asset envelope: {path}')
        header = json.loads(file.readline())
        if header['version'] != 1:
            raise ValueError('Unsupported asset version')
        return header, file.read()


def write_asset(path, kind, payload=b'', metadata=None, source=None):
    path = Path(path)
    previous = read_asset(path)[0] if path.exists() else None
    if previous and previous['type'] != kind:
        raise ValueError(f'Cannot change the type of {path}')
    if source is not None and payload:
        raise ValueError('Header-only assets cannot contain payloads')
    header = dict(previous or {})
    header.update(id=previous['id'] if previous else uuid.uuid4().hex,
                  name=header.get('name', path.stem), type=kind, version=1,
                  storage='external' if source is not None else 'embedded',
                  metadata=metadata if metadata is not None else header.get('metadata', {}))
    if source is not None:
        header['source'] = source
    else:
        header.pop('source', None)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_bytes(b'ALAS1\n' + json.dumps(header, ensure_ascii=False).encode('utf-8') + b'\n' + payload)
    temporary.replace(path)


def asset_reference(path):
    path = Path(path).resolve()
    if path.suffix != '.asset':
        raise ValueError('Asset references must target .asset files, never payload files')
    content = next(parent for parent in path.parents if parent.name == 'Content')
    return {'id': read_asset(path)[0]['id'],
            'path': '/Game/' + path.relative_to(content).with_suffix('').as_posix()}
