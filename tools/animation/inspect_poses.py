"""Render native --dump-poses output with the exported skins, for close-up rig inspection."""
import argparse
import json
import struct
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from mpl_toolkits.mplot3d.art3d import Poly3DCollection
import numpy as np
from export_onnx import ROOT, quaternion_matrix


def load_skin(path):
    with path.open('rb') as f:
        assert f.read(4) == b'SKN1'
        nv, ni, nb = struct.unpack('<III', f.read(12))
        names, binds = [], []
        for _ in range(nb):
            length, = struct.unpack('<I', f.read(4))
            names.append(f.read(length).decode())
            binds.append(np.frombuffer(f.read(64), '<f4').reshape(4, 4).T)
        dtype = np.dtype([('position', '<f4', 3), ('normal', '<f4', 3), ('color', '<f4', 3),
                          ('joints', '<u4', 4), ('weights', '<f4', 4)])
        vertices = np.frombuffer(f.read(nv * 68), dtype)
        triangles = np.frombuffer(f.read(ni * 4), '<u4').reshape(-1, 3)
    return names, np.array(binds), vertices, triangles


def render(directory):
    for kind, mesh in [('biped', 'biped'), ('quadruped', 'dog')]:
        metadata = json.loads((ROOT / 'game/assets/animations/ai4animation' / kind / 'metadata.json').read_text())
        names = [j['name'] for j in metadata['joints']]
        bindings, inverse, vertices, triangles = load_skin(ROOT / 'game/assets/models' / (mesh + '.skin'))
        mapping = [names.index(n) for n in bindings]
        snapshots = json.loads((directory / (kind + '.json')).read_text())
        selected = [s for s in snapshots if s['frame'] in [0, 120, 225, 240]]
        fig = plt.figure(figsize=(16, 9))
        for column, snapshot in enumerate(selected):
            joints = np.array(snapshot['joints'])
            worlds = np.tile(np.eye(4), (len(joints), 1, 1))
            worlds[:, :3, 3] = joints[:, :3]
            worlds[:, :3, :3] = [quaternion_matrix(q) for q in joints[:, 3:]]
            palette = worlds[mapping] @ inverse
            p = np.c_[vertices['position'], np.ones(len(vertices))]
            transformed = np.einsum('vkij,vj->vki', palette[vertices['joints']], p)
            deformed = np.sum(transformed[:, :, :3] * vertices['weights'][:, :, None], axis=1)
            # Display engine X,Z,Y on matplotlib axes, keeping Y vertical.
            xyz = deformed[:, [0, 2, 1]]
            faces = xyz[triangles]
            normals = np.cross(faces[:, 1] - faces[:, 0], faces[:, 2] - faces[:, 0])
            normals /= np.maximum(np.linalg.norm(normals, axis=1, keepdims=True), 1e-8)
            lighting = .35 + .65 * np.abs(normals @ np.array([.3, -.5, .812]))
            color = vertices['color'][triangles].mean(axis=1) ** (1 / 2.2)
            if kind == 'biped': color *= .8
            for row, azimuth in enumerate([-65, 0]):
                ax = fig.add_subplot(2, len(selected), row * len(selected) + column + 1, projection='3d')
                ax.add_collection3d(Poly3DCollection(faces, facecolors=np.clip(color * lighting[:, None], 0, 1), linewidths=0))
                ax.view_init(elev=12, azim=azimuth)
                ax.set(xlim=(-.65, .65), ylim=(-.8, .85), zlim=(0, 1.8 if kind == 'biped' else 1))
                ax.set_box_aspect((1.3, 1.65, 1.8 if kind == 'biped' else 1))
                ax.set_title(('Bind pose' if snapshot['frame'] == 0 else 'Idle' if snapshot['frame'] == 120 else 'Moving') + f" / {snapshot['frame']}")
                ax.set_xlabel('X'); ax.set_ylabel('Z'); ax.set_zlabel('Y')
        fig.tight_layout()
        fig.savefig(directory / (kind + '.png'), dpi=130)
        plt.close(fig)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=Path)
    render(parser.parse_args().directory)
