"""Optional real Vulkan ECS/resource regression. Requires an RT GPU and Release build."""
import json
from pathlib import Path
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]


def run():
    output = ROOT / 'build' / 'ecs-gpu-test'
    output.mkdir(parents=True, exist_ok=True)
    with (output / 'run.log').open('w') as log:
        subprocess.run([str(ROOT / 'build/bin/Release/Afterlight.exe'), '--ecs-smoke',
                        '--width', '800', '--height', '500'], cwd=ROOT,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    report = json.loads((ROOT / 'captures/render-report.json').read_text())
    (output / 'render-report.json').write_text(json.dumps(report, indent=2) + '\n')
    assert report['validationActive'] and report['validationErrors'] == 0, report
    # Initial primitives + two skins; one skin reattach; shared static A; replacement B.
    assert report['geometryBuilds'] == 7, report
    # Detached skin, last reference to static A, static B.
    assert report['geometryReleases'] == 3, report
    assert report['geometryBufferGrowths'] > 4, report
    assert report['sceneResyncs'] == 1, report
    assert report['sceneSlotWrites'] < report['sceneSlotWritesIfRebuilt'] // 4, report
    assert '[ECS smoke] completed stage 9' in (output / 'run.log').read_text(), output
    print('PASS: ECS GPU lifecycle; 7 mesh builds, 3 releases, no unrelated scene rebuilds or validation errors.')
    # A data-only world must render without inventing a hidden instance or material.
    project = output / 'empty-project'
    content = project / 'Content'
    content.mkdir(parents=True, exist_ok=True)
    _, header, payload = (ROOT / 'Projects/Afterlight/Content/Maps/RainCourt.asset').read_text().split('\n', 2)
    scene = json.loads(payload)
    scene.update(entities=[dict(id=uuid.uuid4().hex, name='Data only', enabled=True, components=dict(data={}))],
                 scripts=[], references={}, player='', materials=[], materialAssets=[], lights=[], data={})
    (project / '.project').write_text(json.dumps(dict(version=1, id=uuid.uuid4().hex, name='Empty ECS',
                                                   startupMap='/Game/Empty', scripts=[])))
    (content / 'Empty.asset').write_text('ALAS1\n' + header + '\n' + json.dumps(scene))
    with (output / 'empty.log').open('w') as log:
        subprocess.run([str(ROOT / 'build/bin/Release/Afterlight.exe'), '--project', str(project),
                        '--frames', '60', '--capture', '--validation', '--width', '800', '--height', '500'],
                       cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
    empty = json.loads((ROOT / 'captures/render-report.json').read_text())
    (output / 'empty-report.json').write_text(json.dumps(empty, indent=2) + '\n')
    assert empty['validationErrors'] == 0 and empty['sceneSlots'] == 0 and empty['shaderAssets'] == 0, empty
    print('PASS: data-only ECS scene renders with no dummy entities or materials.')


if __name__ == '__main__':
    run()
