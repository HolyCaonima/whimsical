"""Build the engine's ordinary STM1 Box and Capsule assets. No runtime primitive path."""
import json
import math
import struct
import uuid
from pathlib import Path

DESTINATION = Path(__file__).resolve().parents[1] / 'engine/Content/Meshes'


def write_mesh(name, vertices, indices):
    DESTINATION.mkdir(parents=True, exist_ok=True)
    payload = b'STM1' + struct.pack('<II', len(vertices), len(indices))
    payload += b''.join(struct.pack('<15f', *v) for v in vertices)
    payload += struct.pack(f'<{len(indices)}I', *indices)
    (DESTINATION / f'{name}.stm').write_bytes(payload)
    header = dict(id=uuid.uuid5(uuid.NAMESPACE_URL, f'afterlight/engine/meshes/{name}').hex,
                  type='StaticMesh', name=name, version=1, storage='external',
                  source=f'{name}.stm', metadata={})
    (DESTINATION / f'{name}.asset').write_text('ALAS1\n' + json.dumps(header) + '\n', encoding='utf-8')


def vertex(position, normal):
    # Preserve the former primitives' position, normal, UV, tangent and colour data.
    return (*position, *normal, 0, 0, 1, 0, 0, 1, 1, 1, 1)


def cross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])


def generate():
    vertices, indices = [], []
    for normal in ((1,0,0), (-1,0,0), (0,1,0), (0,-1,0), (0,0,1), (0,0,-1)):
        tangent = (1,0,0) if normal[1] else cross((0,1,0), normal)
        bitangent = cross(normal, tangent)
        base = len(vertices)
        for u, v in ((-1,-1), (1,-1), (1,1), (-1,1)):
            vertices.append(vertex(tuple((normal[i]+tangent[i]*u+bitangent[i]*v)*.5 for i in range(3)), normal))
        indices.extend(base+i for i in (0,1,2,0,2,3))
    write_mesh('Box', vertices, indices)
    vertices, indices = [], []
    segments, rings = 24, 8
    for hemisphere in range(2):
        for ring in range(rings+1):
            theta = (0 if hemisphere else -math.pi*.5) + ring/rings*math.pi*.5
            for segment in range(segments+1):
                phi = segment/segments*2*math.pi
                normal = (math.cos(theta)*math.cos(phi), math.sin(theta), math.cos(theta)*math.sin(phi))
                position = (normal[0]*.4, normal[1]*.4+(.6 if hemisphere else -.6), normal[2]*.4)
                vertices.append(vertex(position, normal))
    for ring in range(2*(rings+1)-1):
        for segment in range(segments):
            a = ring*(segments+1)+segment
            b = a+segments+1
            indices.extend((a,b,a+1,a+1,b,b+1))
    write_mesh('Capsule', vertices, indices)


if __name__ == '__main__':
    generate()
