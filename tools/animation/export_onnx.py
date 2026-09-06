"""Export the unmodified AI4AnimationPy inference graphs and locomotion rig assets.

Requires torch, numpy, onnx, onnxruntime, einops, scikit-learn. Training is never invoked.
Only load trusted upstream .pt files: PyTorch's full-module format is Python pickle.
"""
import io
from asset_format import write_asset, asset_reference
import argparse
import hashlib
import importlib
import json
import re
import struct
import sys
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "tools/cache/animation-export"))
import numpy as np
import torch
import onnx
import onnxruntime as ort

UPSTREAM = ROOT / "external/AI4AnimationPy"


def import_models():
    # Namespace packages bypass the upstream GUI/ECS __init__, not model code.
    # The actual Models and Library .py files below are imported verbatim.
    for name in ["ai4animation", "ai4animation.AI", "ai4animation.AI.Library", "ai4animation.AI.Models"]:
        module = types.ModuleType(name)
        module.__path__ = [str(UPSTREAM.joinpath(*name.split(".")))]
        sys.modules[name] = module
    for name in ["CxM", "MultiLayerPerceptron", "SequentialMLP", "Autoencoder", "CategoricalEncoderDecoder"]:
        importlib.import_module("ai4animation.AI.Models." + name)


class Inference(torch.nn.Module):
    def __init__(self, model, iterations):
        super().__init__()
        self.model, self.iterations = model, iterations

    def forward(self, x):
        if hasattr(self.model, "Sampler"):
            return self.model(x, iterations=self.iterations, sample=False)
        return self.model(x)


def export_model(source, destination, iterations):
    model = torch.load(source, map_location="cpu", weights_only=False).eval()
    wrapper = Inference(model, iterations).eval()
    x = torch.zeros(1, model.input_dim())
    with torch.inference_mode():
        torch.onnx.export(wrapper, x, str(destination), input_names=["features"], output_names=["prediction"],
                          opset_version=17, do_constant_folding=True, dynamo=False)
    graph = onnx.load(destination)
    onnx.checker.check_model(graph)
    options = ort.SessionOptions()
    options.intra_op_num_threads = 1
    session = ort.InferenceSession(str(destination), options, providers=["CPUExecutionProvider"])
    rng = np.random.default_rng(725)
    fixtures, errors = [], []
    with torch.inference_mode():
        for scale in [0, .1, 1, 3]:
            features = rng.normal(size=x.shape).astype(np.float32) * scale
            expected = wrapper(torch.from_numpy(features)).numpy()
            actual = session.run(None, {"features": features})[0]
            np.testing.assert_allclose(actual, expected, atol=2e-4, rtol=2e-4)
            errors.append(float(np.max(np.abs(actual - expected))))
            fixtures.append((features, expected))
    # Native C++ tests consume the same PyTorch golden vectors, including nonzero inputs.
    with io.BytesIO() as f:
        f.write(struct.pack("<III", len(fixtures), x.numel(), fixtures[0][1].size))
        for features, expected in fixtures:
            f.write(features.astype("<f4").tobytes())
            f.write(expected.astype("<f4").tobytes())
        write_asset(destination.with_name(destination.stem + "_golden.asset"), "Binary", f.getvalue(), {"format": "PyTorchGolden"})
    print(destination.name, type(model).__module__, tuple(x.shape), "->", expected.shape, "max error", max(errors), flush=True)
    result = {"source": str(source.relative_to(ROOT) if source.is_relative_to(ROOT) else source), "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "onnx_sha256": hashlib.sha256(destination.read_bytes()).hexdigest(), "input_shape": list(x.shape),
            "output_shape": list(expected.shape), "iterations": iterations, "max_abs_error": max(errors)}

    write_asset(destination.with_suffix(".asset"), "OnnxModel", metadata=result, source=destination.name)
    return result


def quaternion_matrix(q):
    x, y, z, w = q
    return np.array([[1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
                     [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
                     [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]])


def rig_from_glb(path, names):
    from scipy.spatial.transform import Rotation
    data = path.read_bytes()
    length, kind = struct.unpack_from("<II", data, 12)
    assert data[:4] == b"glTF" and kind == 0x4e4f534a
    nodes = json.loads(data[20:20+length])["nodes"]
    parents = {child: i for i, node in enumerate(nodes) for child in node.get("children", [])}
    globals_ = {}

    def world(i):
        if i not in globals_:
            node = nodes[i]
            # Match upstream GLB._nodeGlobalMatrices: rigid TR, scales ignored for the rest rig.
            local = np.eye(4)
            local[:3, :3] = quaternion_matrix(node.get("rotation", [0, 0, 0, 1]))
            local[:3, 3] = node.get("translation", [0, 0, 0])
            globals_[i] = world(parents[i]) @ local if i in parents else local
        return globals_[i]

    by_name = {node.get("name"): i for i, node in enumerate(nodes)}
    # Dog.glb omits HeadSite. Use the complete companion skin's bind hierarchy:
    # motion NPZ bones use different local axes (HeadSite is +X there, -X in the
    # GLB rig). Mixing those frames reverses the head's restoration direction.
    if path.name == "Dog.glb":
        reference_data = (path.parent / "Wolf.glb").read_bytes()
        reference_length = struct.unpack_from("<I", reference_data, 12)[0]
        reference = json.loads(reference_data[20:20+reference_length])["nodes"]
        reference_parents = {child: i for i, node in enumerate(reference) for child in node.get("children", [])}
        reference_names = {node.get("name"): i for i, node in enumerate(reference)}
        for name in names:
            if name in by_name: continue
            k = reference_names[name]
            parent = by_name[reference[reference_parents[k]]["name"]]
            index = len(nodes)
            nodes.append(reference[k].copy())
            parents[index] = parent
            by_name[name] = index
    joints = []
    for name in names:
        i = by_name[name]
        parent = parents.get(i)
        while parent is not None and nodes[parent].get("name") not in names:
            parent = parents.get(parent)
        p = names.index(nodes[parent]["name"]) if parent is not None else -1
        transform = np.linalg.inv(world(parent)) @ world(i) if p >= 0 else world(i)
        joints.append({"name": name, "parent": p, "position": transform[:3, 3].tolist(),
                       "rotation_xyzw": Rotation.from_matrix(transform[:3, :3]).as_quat().tolist()})
    return joints


def export_rig(demo, dest):
    kind = demo.name
    asset_dir = UPSTREAM / "Demos/_ASSETS_" / ("Geno" if kind == "Biped" else "Quadruped")
    spec = importlib.util.spec_from_file_location("rig_definitions", asset_dir / "Definitions.py")
    definitions = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(definitions)
    names = definitions.FULL_BODY_NAMES
    joints = rig_from_glb(asset_dir / ("Model.glb" if kind == "Biped" else "Dog.glb"), names)
    if kind == "Biped":
        contact_names = [definitions.LeftAnkleName, definitions.LeftBallName, definitions.RightAnkleName, definitions.RightBallName]
        starts = [definitions.LeftHipName, definitions.LeftAnkleName, definitions.RightHipName, definitions.RightAnkleName]
        ankles = [-1, 0, -1, 2]
    else:
        contact_names = [definitions.LeftHandSiteName, definitions.RightHandSiteName, definitions.LeftFootSiteName, definitions.RightFootSiteName]
        starts = [definitions.LeftForeArmName, definitions.RightForeArmName, definitions.LeftKneeName, definitions.RightKneeName]
        ankles = [-1] * 4
    contacts = [names.index(n) for n in contact_names]
    feet = []
    for index, (start, end) in enumerate(zip(starts, contacts)):
        chain = [end]
        while chain[-1] != names.index(start):
            chain.append(joints[chain[-1]]["parent"])
            assert chain[-1] >= 0
        feet.append({"chain": list(reversed(chain)), "contact": index, "ankle": ankles[index], "grounded": kind == "Biped"})
    guidances = {}
    for path in sorted((demo / "Guidances").glob("*.npz")):
        with np.load(path, allow_pickle=True) as data:
            source_names = data["Names"].tolist()
            guidances[path.stem] = data["Positions"][[source_names.index(n) for n in names]].tolist()
    attributes = []
    actions = [{"action": name, "attribute": "", "guidance": name, "choices": {}} for name in guidances]
    if kind == "Biped":
        options = [{"value": name, "label": re.sub(r"(?<=[a-z])(?=[A-Z])", " ", name)}
                   for name in guidances if name != "Idle"]
        attributes = [{"key": "locomotion.style", "label": "Style", "default": "BigSteps", "options": options}]
        actions = [{"action": "Idle", "attribute": "", "guidance": "Idle", "choices": {}},
                   {"action": "Locomotion", "attribute": "locomotion.style", "guidance": "",
                    "choices": {option["value"]: option["value"] for option in options}}]
    # Compact non-weight metadata. All network parameters live exclusively in ONNX.
    with io.BytesIO() as f:
        def u(n): f.write(struct.pack("<I", n))
        def floats(v): f.write(np.asarray(v, dtype="<f4").tobytes())
        def string(s):
            data = s.encode("utf-8"); u(len(data)); f.write(data)
        f.write(b"A4C2")
        u(len(joints)); u(16); u(int(kind == "Biped"))
        floats([.5, 10, 3 if kind == "Biped" else 2, .25 if kind == "Biped" else .1, 1, 1.5])
        for joint in joints:
            string(joint["name"]); f.write(struct.pack("<i", joint["parent"]))
            floats(joint["position"]); floats(joint["rotation_xyzw"])
        u(len(contacts))
        for joint in contacts: u(joint)
        u(len(feet))
        for foot in feet:
            u(foot["contact"]); f.write(struct.pack("<i", foot["ankle"])); u(int(foot["grounded"])); u(len(foot["chain"]))
            for joint in foot["chain"]: u(joint)
        u(len(guidances))
        for name, positions in guidances.items(): string(name); floats(positions)
        u(len(attributes))
        for attribute in attributes:
            string(attribute["key"]); string(attribute["label"]); string(attribute["default"])
            u(len(attribute["options"]))
            for option in attribute["options"]: string(option["value"]); string(option["label"])
        u(len(actions))
        for action in actions:
            string(action["action"]); string(action["attribute"]); string(action["guidance"])
            u(len(action["choices"]))
            for value, guidance in action["choices"].items(): string(value); string(guidance)
        write_asset(dest / "controller.asset", "AnimationController", f.getvalue(),
                    {"network": asset_reference(dest / "network.asset"),
                     "postprocessor": asset_reference(dest / "postprocessor.asset"), "license": "CC-BY-NC-4.0"})
    return {"joints": joints, "contacts": contacts, "feet": feet, "guidances": list(guidances),
            "attributes": attributes, "actions": actions,
            "samples": 16, "window": .5, "prediction_hz": 10, "pose_axes": kind == "Biped",
            "differences": ["Rigid quaternion projection of predicted forward/up axes", "Physics feedback rebases cached root predictions",
                            "Gameplay supplies action and movement intent; demo keyboard/gamepad and PID policy remain external"]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--character", choices=["biped", "quadruped", "all"], default="all")
    parser.add_argument("--output", type=Path, default=ROOT / "Projects/Afterlight/Content/animations/ai4animation")
    parser.add_argument("--checkpoint", type=Path, help="Export a standalone upstream model; --output must be an .onnx path")
    parser.add_argument("--iterations", type=int, default=3, help="CxM denoising iterations baked into a standalone ONNX graph")
    args = parser.parse_args()
    torch.set_num_threads(1)
    import_models()
    if args.checkpoint:
        if args.output.suffix != ".onnx": parser.error("--checkpoint requires --output FILE.onnx")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        result = export_model(args.checkpoint.resolve(), args.output, args.iterations)
        write_asset(args.output.with_name(args.output.stem + "_metadata.asset"), "Data", json.dumps(result, indent=2).encode())
        return
    for kind in ["Biped", "Quadruped"]:
        if args.character not in ["all", kind.lower()]: continue
        demo = UPSTREAM / "Demos/Locomotion" / kind
        source = demo / "Models" if kind == "Biped" else demo
        dest = args.output / kind.lower()
        dest.mkdir(parents=True, exist_ok=True)
        network = export_model(source / "Network.pt", dest / "network.onnx", 3 if kind == "Biped" else 1)
        postprocessor = export_model(source / ("PostProcessor.pt" if kind == "Biped" else "Postprocessor.pt"), dest / "postprocessor.onnx", 0)
        metadata = export_rig(demo, dest)
        metadata["network"] = network
        metadata["postprocessor"] = postprocessor
        metadata["upstream_commit"] = "bfb5866681f7ea6dac9984be05181de5955eb48b"
        metadata["license"] = "CC-BY-NC-4.0"
        write_asset(dest / "metadata.asset", "Data", (json.dumps(metadata, indent=2) + "\n").encode())


if __name__ == "__main__": main()
