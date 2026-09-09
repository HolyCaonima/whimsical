"""Native GPU readback views; restore the authored Map after camera-only captures."""
import json
import subprocess
import sys
from pathlib import Path
from PIL import Image

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/animation'))
from asset_format import read_asset,write_asset
map_path=ROOT/'Projects/Afterlight/Content/Maps/HoneybudCourt.asset'
output=ROOT/'Projects/Afterlight/SourceArt/Honeybud'
audit=ROOT/'captures/honeybud/refined'
audit.mkdir(parents=True,exist_ok=True)
original=map_path.read_bytes()
header,payload=read_asset(map_path)
scene=json.loads(payload)
views={'courtyard':scene['camera'],
       'textiles':{'target':[5.8,.5,-1.0],'yaw':.17,'pitch':.42,'distance':7.7,'fov':.67},
       'ceramic':{'target':[-.6,.5,-.1],'yaw':-.25,'pitch':.30,'distance':9.3,'fov':.66}}
requested=sys.argv[1:] or list(views)
try:
    for name in requested:
        scene['camera']=views[name]
        write_asset(map_path,'Map',json.dumps(scene,indent=2).encode())
        with (audit/f'{name}.log').open('w') as log:
            subprocess.run([str(ROOT/'build/bin/Release/Whimsical.exe'),'--map','/Game/Maps/HoneybudCourt',
                            '--no-hud','--width','1440','--height','1000','--frames','140','--capture','--validation'],
                           cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
        Image.open(ROOT/'captures/frame.bmp').save(output/f'project_{name}.png')
        report=json.loads((ROOT/'captures/render-report.json').read_text())
        (audit/f'{name}.json').write_text(json.dumps(report,indent=2))
        if report['validationErrors']:raise RuntimeError('Vulkan validation failed')
        print(name,report['fps'],report['gpuAverageMs'],flush=True)
finally:
    map_path.write_bytes(original)
