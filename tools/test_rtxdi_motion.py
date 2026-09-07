"""RT GPU regression for RTXDI shadow history. Run after building Release.

A point light and a stationary receiving plane isolate visibility changes from
surface motion. At GPU frame 64 the occluder moves two world units. Measure the
first eight frames after that change with and without NRD confidence feedback.
"""
import json
import subprocess
import uuid

import numpy as np

from test_nrd_shadows import ROOT, create_project
from test_shader_surfaces import read_asset, write_asset


def run():
    tag = "rtxdi-motion-" + uuid.uuid4().hex[:8]
    project = create_project(tag)
    results = {}
    _, scene = read_asset(project / "Content/Test.asset")
    for case, motion, confidence in (("before", False, True), ("after", False, True),
                                     ("off", True, False), ("on", True, True)):
        scene["entities"][1]["components"]["transform"]["position"][0] = 2 if case == "after" else 0
        write_asset(project / "Content/Test.asset", "Map", scene)
        name = tag + "-" + case
        args = [str(ROOT / "build/bin/Release/Afterlight.exe"), "--project", str(project),
                "--frames", "72", "--width", "640", "--height", "400", "--validation",
                "--no-hud", "--present", "immediate", "--capture", "--audit", name,
                "--cvar", "r.DIHistoryConfidence=" + str(confidence).lower()]
        if motion:
            args += ["--audit-occluder", "1"]
        print("Validating " + name, flush=True)
        with (project / (case + ".log")).open("w") as log:
            subprocess.run(args, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
        folder = ROOT / "captures" / name
        report = json.loads((folder / "render-report.json").read_text())
        assert report["validationActive"] and report["validationErrors"] == 0, report
        audit = json.loads((folder / "audit.json").read_text())
        assert audit["nonFinite"] == 0 and audit["samples"] == 8, audit
        h, w = audit["height"], audit["width"]
        results[case] = {n: np.fromfile(folder / (n + ".f32"), dtype="<f4").reshape(h, w, 5)
                         for n in ("albedo", "direct", "raw-diffuse", "denoised-diffuse")}
    before, after, on = results["before"], results["after"], results["on"]
    ground = np.max(np.abs(before["albedo"][:, :, :3] - .5), axis=2) < .001
    ground &= np.max(np.abs(on["albedo"][:, :, :3] - .5), axis=2) < .001
    was_shadow = before["direct"][:, :, :3].max(axis=2) < 1e-6
    is_shadow = on["direct"][:, :, :3].max(axis=2) < 1e-6
    changed = ground & (was_shadow != is_shadow)
    assert changed.sum() > 100, "No moving shadow on a stationary receiver"
    lum = np.array([.2126, .7152, .0722])
    scale = np.mean(on["raw-diffuse"][:, :, :3][ground & ~is_shadow] @ lum)
    metrics = {"changedPixels": int(changed.sum())}
    for case in ("off", "on"):
        data = results[case]
        error = abs((data["denoised-diffuse"][:, :, :3] - after["raw-diffuse"][:, :, :3]) @ lum)
        for name, mask in (("all", changed), ("newShadow", changed & is_shadow), ("newLight", changed & ~is_shadow)):
            metrics[case + "_" + name] = float(error[mask].mean() / scale)
    newly_lit = changed & ~is_shadow
    metrics["directRecoveryRatio"] = float(np.mean(on["direct"][:, :, :3][newly_lit] @ lum) /
                                           np.mean(after["direct"][:, :, :3][newly_lit] @ lum))
    (project / "motion-metrics.json").write_text(json.dumps(metrics, indent=2))
    print(json.dumps(metrics, indent=2), flush=True)
    assert metrics["on_all"] < metrics["off_all"] * .8, "Confidence must reduce moving shadow lag"
    assert metrics["on_newShadow"] < metrics["off_newShadow"], "Darkening shadows still trail"
    assert metrics["on_newLight"] < metrics["off_newLight"], "Uncovered shadows still trail"
    assert .9 < metrics["directRecoveryRatio"] < 1.1, "DI reservoir history delays shadow recovery"
    print("PASS: " + str(project), flush=True)


if __name__ == "__main__":
    run()
