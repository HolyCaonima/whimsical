"""RT GPU regression: compare RTXDI against an analytic all-lights sum on a plane.

Exercises unequal light power, RGB lights, the 256-light limit, zero lights and
non-block-aligned image dimensions. No NRD output enters the reference comparison.
"""
import json
import subprocess
import uuid

import numpy as np

from test_nrd_shadows import ROOT, create_project
from test_shader_surfaces import read_asset, write_asset


def reference(camera, lights, w, h):
    yaw, pitch, distance = camera["yaw"], camera["pitch"], camera["distance"]
    eye = distance * np.array([np.sin(yaw) * np.cos(pitch), np.sin(pitch), np.cos(yaw) * np.cos(pitch)])
    forward = -eye / np.linalg.norm(eye)
    right = np.cross(forward, [0, 1, 0])
    right /= np.linalg.norm(right)
    up = np.cross(right, forward)
    y, x = np.mgrid[:h, :w]
    half_fov = np.tan(camera["fov"] / 2)
    direction = (forward + ((x + .5) / w * 2 - 1)[..., None] * right * half_fov * w / h
                 + (1 - (y + .5) / h * 2)[..., None] * up * half_fov)
    position = eye + direction * (-eye[1] / direction[:, :, 1])[..., None]
    view = eye - position
    view /= np.linalg.norm(view, axis=2)[..., None]
    nv = view[:, :, 1]
    alpha = .8 ** 2
    smith = lambda c: 2 * c / (c + np.sqrt(alpha ** 2 + (1 - alpha ** 2) * c ** 2))
    rgb = np.zeros((h, w, 3))
    for light in lights:
        delta = np.array(light["positionRadius"][:3]) - position
        distance2 = np.sum(delta ** 2, axis=2)
        l = delta / np.sqrt(distance2)[..., None]
        halfway = l + view
        halfway /= np.linalg.norm(halfway, axis=2)[..., None]
        nl, nh = l[:, :, 1], halfway[:, :, 1]
        vh = np.sum(view * halfway, axis=2)
        distribution = alpha ** 2 / (np.pi * (nh ** 2 * (alpha ** 2 - 1) + 1) ** 2)
        fresnel = .04 + .96 * (1 - vh) ** 5
        brdf = .5 / np.pi + distribution * smith(nv) * smith(nl) * fresnel / np.maximum(4 * nv * nl, 1e-5)
        color = np.array(light["colorIntensity"][:3]) * light["colorIntensity"][3]
        rgb += (nl * brdf / distance2)[..., None] * color
    return position, rgb


def run():
    tag = "rtxdi-lights-" + uuid.uuid4().hex[:8]
    project = create_project(tag)
    _, scene = read_asset(project / "Content/Test.asset")
    scene["objects"] = scene["objects"][:1]
    lights = [dict(positionRadius=[(i % 16 - 7.5) * .4, 5 + i % 3, (i // 16 - 7.5) * .4, 0],
                   colorIntensity=[.2 + (i % 3 == 0), .2 + (i % 3 == 1), .2 + (i % 3 == 2),
                                   .05 if i < 248 else 8 + (i - 248) * 2]) for i in range(256)]
    for case, active in (("many", lights), ("zero", [])):
        scene["lights"] = active
        write_asset(project / "Content/Test.asset", "Map", scene)
        name = tag + "-" + case
        args = [str(ROOT / "build/bin/Release/Afterlight.exe"), "--project", str(project),
                "--frames", "96", "--width", "643", "--height", "403", "--validation",
                "--no-hud", "--present", "immediate", "--capture", "--audit", name]
        print("Validating " + name, flush=True)
        with (project / (case + ".log")).open("w") as log:
            subprocess.run(args, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
        folder = ROOT / "captures" / name
        report = json.loads((folder / "render-report.json").read_text())
        audit = json.loads((folder / "audit.json").read_text())
        assert report["validationActive"] and report["validationErrors"] == 0, report
        assert audit["nonFinite"] == 0, audit
        h, w = audit["height"], audit["width"]
        actual = np.fromfile(folder / "direct.f32", dtype="<f4").reshape(h, w, 5)[:, :, :3]
        if not active:
            assert np.max(abs(actual)) == 0, "Zero lights must produce zero direct radiance"
            continue
        position, expected = reference(scene["camera"], active, w, h)
        # Exclude silhouettes and the box's side faces from the plane reference.
        mask = (abs(position[:, :, 0]) < 3.5) & (abs(position[:, :, 2]) < 3.5)
        relative_error = np.mean(abs(actual[mask] - expected[mask])) / np.mean(expected[mask])
        energy_error = np.max(abs(actual[mask].mean(0) / expected[mask].mean(0) - 1))
        # ReSTIR samples are correlated in space and time. Compare regional energy
        # with the exact sum; retain raw per-pixel noise as a separate reported metric.
        tiles = []
        for y in range(0, h, 32):
            for x in range(0, w, 32):
                inside = mask[y:y+32, x:x+32]
                if inside.sum() < 512:
                    continue
                tiles.append(abs(actual[y:y+32, x:x+32][inside].mean(0) -
                                 expected[y:y+32, x:x+32][inside].mean(0)))
        tile_error = np.mean(tiles) / np.mean(expected[mask])
        metrics = dict(relativePixelError=float(relative_error), relativeEnergyError=float(energy_error),
                       relativeTileError=float(tile_error))
        (folder / "light-metrics.json").write_text(json.dumps(metrics, indent=2))
        print(json.dumps(metrics, indent=2), flush=True)
        assert tile_error < .05 and energy_error < .02, "RTXDI disagrees with the analytic sum"
    print("PASS: " + str(project), flush=True)


if __name__ == "__main__":
    run()
