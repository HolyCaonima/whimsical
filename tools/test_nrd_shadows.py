"""Optional RT GPU regression: python tools/test_nrd_shadows.py.

A frozen, uniform ground plane and one point light provide a noise-free DI
shadow mask. Compare NRD's linear diffuse output with the temporal mean of its
actual input at that shadow boundary, before materials, fog and tone mapping.
The generated project, captures and measurements stay under build/ and captures/.
Requires numpy; uses the existing asset writers from test_shader_surfaces.
"""
import copy
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import uuid

import numpy as np

from test_shader_surfaces import ROOT, read_asset, write_asset


def create_project(tag):
    project = ROOT / "build" / tag
    content = project / "Content"
    content.mkdir(parents=True)
    shutil.copytree(ROOT / "Projects/Afterlight/Content/shaders", content / "shaders")
    (project / ".project").write_text(json.dumps(dict(version=1, id=uuid.uuid4().hex,
        name="NRD shadow regression", startupMap="/Game/Test", scripts=[])), encoding="utf-8")
    header = json.loads((content / "shaders/Standard.asset").read_text().split("\n", 2)[1])
    shader = dict(id=header["id"], path="/Game/shaders/Standard")
    _, scene = read_asset(ROOT / "Projects/Afterlight/Content/Maps/RainCourt.asset")
    template = copy.deepcopy(scene["entities"][0])
    scene.update(scripts=[], entities=[], materials=[], materialAssets=[], player="", references={}, data={})
    scene["camera"].update(target=[0, 0, 0], yaw=.6, pitch=.9, distance=13, fov=.62)
    scene["lights"] = [dict(positionRadius=[-3, 6, -1, 0], colorIntensity=[1, 1, 1, 75])]
    for color in ([.5, .5, .5], [.2, .1, .05]):
        scene["materials"].append(dict(shader=shader,
            properties=dict(baseColor=color, roughness=.8, metallic=0), textures={}))
    for name, position, scale, material in (("Ground", [0, -.25, 0], [8, .5, 8], 0),
                                           ("Occluder", [0, 1, 0], [1, 2, 1], 1)):
        obj = copy.deepcopy(template)
        obj.update(id=uuid.uuid4().hex, name=name)
        obj["components"]["transform"].update(position=position, rotation=[0, 0, 0, 1])
        obj["components"]["render"].update(scale=scale, material=material)
        scene["entities"].append(obj)
    write_asset(content / "Test.asset", "Map", scene)
    return project


def run():
    tag = "nrd-shadow-" + uuid.uuid4().hex[:8]
    project = create_project(tag)
    args = [str(ROOT / "build/bin/Release/Afterlight.exe"), "--project", str(project),
            "--frames", "192", "--width", "640", "--height", "400", "--validation",
            "--present", "immediate", "--capture", "--audit", tag]
    print("Validating single-NRD shadow edges: " + tag, flush=True)
    with (project / "run.log").open("w") as log:
        subprocess.run(args, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
    analyze(ROOT / "captures" / tag)


def analyze(folder):
    report = json.loads((folder / "render-report.json").read_text())
    assert report["validationActive"] and report["validationErrors"] == 0, report
    audit = json.loads((folder / "audit.json").read_text())
    assert audit["nonFinite"] == 0, audit
    h, w = audit["height"], audit["width"]

    def signal(name):
        return np.fromfile(folder / (name + ".f32"), dtype="<f4").reshape(h, w, 5)

    albedo, direct = signal("albedo"), signal("direct")
    raw, denoised = signal("raw-diffuse"), signal("denoised-diffuse")
    ground = np.max(np.abs(albedo[:, :, :3] - .5), axis=2) < .001
    shadow = ground & (direct[:, :, :3].max(axis=2) < 1e-6)
    lit = ground & (direct[:, :, :3].mean(axis=2) > .05)
    # Select both sides within two pixels of a real lighting edge on the plane.
    near_lit, near_shadow = np.zeros((h, w), bool), np.zeros((h, w), bool)
    for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0), (0, 2), (0, -2), (2, 0), (-2, 0)):
        near_lit |= np.roll(lit, (dy, dx), axis=(0, 1))
        near_shadow |= np.roll(shadow, (dy, dx), axis=(0, 1))
    edge = (shadow & near_lit) | (lit & near_shadow)
    edge[:2] = edge[-2:] = False
    edge[:, :2] = edge[:, -2:] = False
    assert edge.sum() > 50, "Scene does not contain enough shadow edge pixels"
    luminance = np.array([.2126, .7152, .0722])
    reference = raw[:, :, :3] @ luminance
    filtered = denoised[:, :, :3] @ luminance
    scale = np.mean(reference[lit])
    error = np.mean(np.abs(filtered[edge] - reference[edge])) / scale
    raw_rms = np.sqrt(raw[ground, 3].mean())
    filtered_rms = np.sqrt(denoised[ground, 3].mean())
    metrics = dict(edgePixels=int(edge.sum()), relativeEdgeError=float(error),
                   rawTemporalRms=float(raw_rms), denoisedTemporalRms=float(filtered_rms))
    (folder / "shadow-metrics.json").write_text(json.dumps(metrics, indent=2))
    print(json.dumps(metrics, indent=2), flush=True)
    # Allow 3% of the lit-plane signal at the edge; the old settings lose ~8%.
    assert error < .03, "NRD substantially blurs the point-light shadow boundary"
    assert filtered_rms < raw_rms, "NRD does not reduce temporal noise on the ground"
    print("PASS: " + str(folder))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--analyze", type=Path, help="Measure an existing audit without rerunning the GPU")
    args = parser.parse_args()
    if args.analyze:
        analyze(args.analyze)
    else:
        run()
