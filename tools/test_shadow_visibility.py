"""Verify render.castShadow changes ray visibility without hiding raster geometry."""
import json
import uuid
import numpy as np
from test_nrd_shadows import create_project
from test_shader_surfaces import read_asset
from test_physical_lights import capture


def run():
    tag = 'shadow-visibility-' + uuid.uuid4().hex[:8]
    project = create_project(tag)
    _, scene = read_asset(project / 'Content/Test.asset')
    results = {}
    for enabled in (True, False):
        scene['entities'][1]['components']['render']['castShadow'] = enabled
        direct, audit, folder = capture(project, scene, tag, 'on' if enabled else 'off', frames=80)
        albedo = np.fromfile(folder / 'albedo.f32', dtype='<f4').reshape(audit['height'], audit['width'], 5)
        results[enabled] = (direct, albedo)
    before, albedo = results[True]
    after, other = results[False]
    assert np.array_equal(albedo[..., :3], other[..., :3]), 'castShadow changed raster geometry'
    ground = np.max(abs(albedo[..., :3] - .5), axis=2) < .001
    recovered = ground & (before.max(axis=2) < 1e-6) & (after.mean(axis=2) > .05)
    assert recovered.sum() > 100, 'Non-shadow-casting object still occludes light rays'
    metrics = dict(recoveredShadowPixels=int(recovered.sum()), rasterDifference=0)
    (project / 'shadow-visibility-metrics.json').write_text(json.dumps(metrics, indent=2))
    print('PASS: ' + json.dumps(metrics) + ' ' + str(project), flush=True)


if __name__ == '__main__':
    run()
