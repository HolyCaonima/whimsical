"""Headless diagnostic: execute upstream Biped Predict/Animate, Actor and IK verbatim.

Only scene/UI initialization is replaced. No files under external are modified.
The fixed input schedule matches animation_tests --dump-poses.
"""
from asset_format import read_asset
import ast
import importlib
import json
import sys
import types
from pathlib import Path
import numpy as np
import torch
from scipy.spatial.transform import Rotation as ScipyRotation
from export_onnx import ROOT, UPSTREAM, import_models, quaternion_matrix


def run(directory):
    import_models()
    top = sys.modules['ai4animation']
    for package in ['Components', 'Animation', 'IK']:
        name = 'ai4animation.' + package
        module = types.ModuleType(name)
        module.__path__ = [str(UPSTREAM / 'ai4animation' / package)]
        sys.modules[name] = module
    host = types.ModuleType('ai4animation.AI4Animation')
    host.AI4Animation = types.SimpleNamespace(Standalone=None)
    sys.modules[host.__name__] = host
    top.AI4Animation = host.AI4Animation
    from ai4animation.Math import Transform, Rotation, Vector3, Tensor
    from ai4animation import Utility
    from ai4animation.Components.Actor import Actor
    from ai4animation.IK.FABRIK import FABRIK
    from ai4animation.Animation.RootModule import RootModule
    from ai4animation.Animation.MotionModule import MotionModule
    from ai4animation.Animation.TimeSeries import TimeSeries
    from ai4animation.AI.FeedTensor import FeedTensor
    from ai4animation.AI.ReadTensor import ReadTensor
    for name, value in [('Rotation', Rotation), ('Vector3', Vector3), ('FABRIK', FABRIK)]:
        setattr(top, name, value)
    demo = UPSTREAM / 'Demos/Locomotion/Biped'
    sys.path.insert(0, str(demo))
    sys.path.insert(0, str(UPSTREAM / 'Demos/_ASSETS_/Geno'))
    import Definitions
    from LegIK import LegIK
    from Sequence import Sequence
    Time = types.SimpleNamespace(DeltaTime=1/60, TotalTime=0)
    scope = dict(Actor=Actor, Transform=Transform, Rotation=Rotation, Vector3=Vector3,
                 Tensor=Tensor, RootModule=RootModule, MotionModule=MotionModule,
                 FeedTensor=FeedTensor, ReadTensor=ReadTensor, Sequence=Sequence,
                 Definitions=Definitions, Time=Time)
    tree = ast.parse((demo / 'Program.py').read_text())
    statements = [node for node in tree.body if isinstance(node, ast.Assign)]
    cls = next(node for node in tree.body if isinstance(node, ast.ClassDef) and node.name == 'Program')
    cls.body = [node for node in cls.body if isinstance(node, ast.FunctionDef) and node.name in ['Predict', 'Animate']]
    # SCRIPT_DIR / ASSETS_PATH are setup assignments, unrelated to the runtime.
    statements = [node for node in statements if all(isinstance(t, ast.Name) and t.id.isupper() and t.id not in ['SCRIPT_DIR', 'ASSETS_PATH'] for t in node.targets)]
    exec(compile(ast.Module(body=statements + [cls], type_ignores=[]), str(demo / 'Program.py'), 'exec'), scope)
    metadata = json.loads(read_asset(ROOT / 'Projects/Afterlight/Content/animations/ai4animation/biped/metadata.asset')[1])
    names = [j['name'] for j in metadata['joints']]
    worlds = []
    for joint in metadata['joints']:
        local = np.eye(4); local[:3, 3] = joint['position']; local[:3, :3] = quaternion_matrix(joint['rotation_xyzw'])
        worlds.append(worlds[joint['parent']] @ local if joint['parent'] >= 0 else local)
    actor = Actor.__new__(Actor)
    actor.BoneNames = names
    actor.Transforms = np.array(worlds, dtype=np.float32)
    actor.Root = np.eye(4, dtype=np.float32)
    actor.Velocities = np.zeros((len(names), 3), dtype=np.float32)
    actor.Bones = [Actor.Bone(actor, i, types.SimpleNamespace(Name=n)) for i, n in enumerate(names)]
    actor.NameToBoneMap = dict(zip(names, actor.Bones))
    for i, bone in enumerate(actor.Bones):
        parent = metadata['joints'][i]['parent']
        if parent >= 0: bone.SetParent(actor.Bones[parent])
        bone.ComputeZeroTransform()
    actor.SyncToScene = lambda: None
    program = scope['Program']()
    program.Actor = actor
    program.Model = torch.load(demo / 'Models/Network.pt', map_location='cpu', weights_only=False).eval()
    program.PostProcessor = torch.load(demo / 'Models/PostProcessor.pt', map_location='cpu', weights_only=False).eval()
    program.NetworkIterations = 3
    program.SolverIterations = 1; program.SolverAccuracy = 1e-3
    program.Timescale = 1.; program.Synchronization = 0.; program.TrajectoryCorrection = .25
    program.ControlSeries = TimeSeries(0., .5, 16)
    program.SimulationObject = RootModule.Series(program.ControlSeries)
    program.RootControl = RootModule.Series(program.ControlSeries)
    program.GuidanceControl = types.SimpleNamespace(Positions=None)
    program.ContactBones = ['LeftFoot', 'LeftToeBase', 'RightFoot', 'RightToeBase']
    program.ContactIndices = actor.GetBoneIndices(program.ContactBones)
    program.LeftLegIK = LegIK(FABRIK(actor.GetBone('LeftUpLeg'), actor.GetBone('LeftFoot')), FABRIK(actor.GetBone('LeftFoot'), actor.GetBone('LeftToeBase')))
    program.RightLegIK = LegIK(FABRIK(actor.GetBone('RightUpLeg'), actor.GetBone('RightFoot')), FABRIK(actor.GetBone('RightFoot'), actor.GetBone('RightToeBase')))
    program.Previous = None; program.Sequence = None
    guidances = {}
    for name in ['Idle', 'Neutral']:
        with np.load(demo / 'Guidances' / (name + '.npz'), allow_pickle=True) as g:
            order = g['Names'].tolist()
            guidances[name] = g['Positions'][[order.index(n) for n in names]]
    snapshots = []
    torch.set_num_threads(1)
    with torch.inference_mode():
        for frame in range(361):
            if frame % 15 == 0:
                pose = Transform.TransformationTo(actor.Transforms, actor.Root)
                snapshots.append({'frame': frame, 'joints': np.c_[pose[:, :3, 3], ScipyRotation.from_matrix(pose[:, :3, :3]).as_quat()].tolist()})
            Time.TotalTime = frame / 60
            velocity = Vector3.Create(0, 0, 0 if frame < 120 else 2)
            position = Vector3.Lerp(program.SimulationObject.GetPosition(0), actor.GetRootPosition(), program.Synchronization)
            program.SimulationObject.Control(position, Vector3.Create(0, 0, 1), velocity, Time.DeltaTime)
            program.GuidanceControl.Positions = guidances['Idle' if frame < 120 else 'Neutral']
            if program.Sequence is not None:
                program.RootControl.Transforms = Transform.Interpolate(program.SimulationObject.Transforms, program.Sequence.Trajectory.Transforms, .25)
                for i in range(16):
                    target = Transform.GetPosition(program.RootControl.Transforms)[i:]
                    current = actor.GetRootPosition().reshape(-1, 3)
                    time = program.RootControl.Timestamps[i:].reshape(-1, 1)
                    program.RootControl.Velocities[i] = Tensor.Sum(target - current, axis=0, keepDim=False) / Tensor.Sum(time, axis=0, keepDim=False)
                program.RootControl.Velocities = Vector3.Lerp(program.RootControl.Velocities, program.Sequence.Trajectory.Velocities, .25)
            if frame % 6 == 0:
                program.Timestamp = Time.TotalTime
                program.Predict()
            program.Animate()
    directory.mkdir(parents=True, exist_ok=True)
    (directory / 'biped.json').write_text(json.dumps(snapshots))
    print('Upstream Biped runtime:', directory / 'biped.json')


if __name__ == '__main__':
    run(Path(sys.argv[1]))
