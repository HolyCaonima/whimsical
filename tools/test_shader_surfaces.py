"""Optional Vulkan regression: python tools/test_shader_surfaces.py (requires RT GPU).

Builds an isolated three-Shader project under build/, then compares a discarded
surface with an absent surface. Albedo, shadow lighting and resolved GI/specular
must agree; an actual foliage surface must change the image. No project assets
are modified. The generated projects, logs and audit captures remain inspectable.
"""
import copy
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]


def read_asset(path):
    _, header, payload = path.read_text(encoding="utf-8").split("\n", 2)
    return json.loads(header), json.loads(payload)


def write_asset(path, kind, payload, identity=None):
    path.parent.mkdir(parents=True, exist_ok=True)
    header = dict(id=identity or uuid.uuid4().hex, name=path.stem, type=kind,
                  version=1, storage="embedded", metadata={})
    data = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
    path.write_bytes(b"ALAS1\n" + json.dumps(header).encode() + b"\n" + data)
    return dict(id=header["id"], path="/Game/" + path.stem)


def set_point_lights(scene, lights):
    """Migrate old diagnostic W/sr fixtures to v7 ECS radiant-power components."""
    scene["version"] = 7
    scene.pop("lights", None)
    scene["entities"] = [e for e in scene["entities"] if "light" not in e["components"]]
    for l in lights:
        p, c = l["positionRadius"], l["colorIntensity"]
        scene["entities"].append(dict(id=uuid.uuid4().hex, name="Light", enabled=True, components=dict(
            transform=dict(position=p[:3]), light=dict(type="point", radius=p[3], color=c[:3],
            intensity=c[3]*4*math.pi*sum(c[:3])))))


def run():
    tag = "shader-surfaces-" + uuid.uuid4().hex[:8]
    project = ROOT / "build" / tag
    content = project / "Content"
    content.mkdir(parents=True)
    shutil.copytree(ROOT / "Projects/Afterlight/Content/shaders", content / "shaders")
    (project / ".project").write_text(json.dumps(dict(version=1, id=uuid.uuid4().hex,
        name="Shader surface regression", startupMap="/Game/Test", scripts=[])), encoding="utf-8")
    refs = {}
    for name in ("Standard", "Paving", "Foliage"):
        shader_file = content / "shaders" / (name + ".asset")
        _, header, _ = shader_file.read_text().split("\n", 2)
        header = json.loads(header)
        refs[name] = dict(id=header["id"], path="/Game/shaders/" + name)
        if name == "Foliage":
            header["metadata"]["properties"].append(dict(name="coverage", type="float", default=1))
            shader_file.write_text("ALAS1\n" + json.dumps(header) + "\n")
            source = content / "shaders/Foliage.glsl"
            source.write_text(source.read_text().replace("return s;", "s.opacity *= properties.coverage;\nreturn s;"))
    # Horizontal quad with real UVs and CCW winding, at y=0 in mesh space.
    vertices = []
    for x, z, u, v in ((-1, 1, 0, 0), (1, 1, 1, 0), (1, -1, 1, 1), (-1, -1, 0, 1)):
        vertices.extend((x, 0, z, 0, 1, 0, u, v, 1, 0, 0, 1, 1, 1, 1))
    mesh = b"STM1" + struct.pack("<II", 4, 6) + struct.pack("<60f", *vertices) + struct.pack("<6I", 0, 1, 2, 0, 2, 3)
    quad = write_asset(content / "Quad.asset", "StaticMesh", mesh)
    _, scene = read_asset(ROOT / "Projects/Afterlight/Content/Maps/RainCourt.asset")
    template = copy.deepcopy(scene["entities"][0])
    scene.update(scripts=[], entities=[], materials=[], materialAssets=[], player="", references={}, data={})
    scene["camera"].update(target=[0, 0, 0], yaw=.6, pitch=.9, distance=13, fov=.62)
    for name, color in (("Paving", [.65, .65, .65]), ("Standard", [.8, .2, .1]), ("Foliage", [.1, .8, .2])):
        scene["materials"].append(dict(shader=refs[name], properties=dict(baseColor=color), textures={}))
    for name, position, scale, material in (("Ground", [0, -.25, 0], [8, .5, 8], 0),
                                           ("Box", [2, .5, 1], [1, 1, 1], 1),
                                           ("Leaf", [0, 1.5, 0], [2, 1, 2], 2)):
        obj = copy.deepcopy(template)
        obj.update(id=uuid.uuid4().hex, name=name)
        obj["components"]["transform"].update(position=position, rotation=[0, 0, 0, 1])
        obj["components"]["render"].update(scale=scale, material=material)
        if name == "Leaf":
            obj["components"]["render"]["mesh"] = quad
        scene["entities"].append(obj)
    captures = {}
    set_point_lights(scene, [dict(positionRadius=[0, 6, 0, .1], colorIntensity=[1, 1, 1, 75])])
    for case, visible, coverage in (("absent", False, 0), ("discarded", True, 0), ("foliage", True, 1)):
        scene["entities"][2]["components"]["render"]["visible"] = visible
        scene["materials"][-1]["properties"]["coverage"] = coverage
        write_asset(content / "Test.asset", "Map", scene)
        audit = tag + "-" + case
        args = [str(ROOT / "build/bin/Release/Afterlight.exe"), "--project", str(project),
                "--frames", "80", "--width", "480", "--height", "320", "--validation",
                "--no-hud", "--capture", "--audit", audit]
        print("Validating " + case, flush=True)
        with (project / (case + ".log")).open("w") as log:
            subprocess.run(args, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
        captures[case] = ROOT / "captures" / audit
        report = json.loads((captures[case] / "render-report.json").read_text())
        assert report["validationActive"] and report["validationErrors"] == 0, report
        assert report["shaderAssets"] == 3 and report["rasterPrograms"] == 3, report
        assert report["shaderCompilations"] == 9, report
        assert json.loads((captures[case] / "audit.json").read_text())["nonFinite"] == 0
    for signal in ("albedo", "direct", "display"):
        values = {}
        for case, folder in captures.items():
            values[case] = [x for pixel in struct.iter_unpack("<5f", (folder / (signal + ".f32")).read_bytes())
                            for x in pixel[:3]]
        error = max(abs(a-b) for a, b in zip(values["absent"], values["discarded"]))
        change = sum(abs(a-b) for a, b in zip(values["absent"], values["foliage"])) / len(values["absent"])
        assert error < 1e-5, (signal, "Discarded surface affected rendering", error)
        assert change > .001, (signal, "Foliage did not affect rendering", change)
        print(f"{signal}: discarded max error={error:.6g}, foliage mean change={change:.6g}")
    print("PASS: raster opacity, ray visibility, GI/specular and Shader compilation counts; " + str(project))


if __name__ == "__main__":
    run()
