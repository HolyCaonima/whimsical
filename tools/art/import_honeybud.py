"""Assemble the Blender kit manifest into a native, portable Afterlight Map."""
import copy
import json
import math
import sys
import uuid
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
CONTENT=ROOT/'Projects/Afterlight/Content'
sys.path.insert(0,str(ROOT/'tools/animation'))
from asset_format import read_asset,write_asset,asset_reference

manifest=json.loads((ROOT/'Projects/Afterlight/SourceArt/Honeybud/manifest.json').read_text())
from honeybud_budget import audit
audit(manifest)
original=json.loads(read_asset(CONTENT/'Maps/RainCourt.asset')[1])
materials=[];material_assets=[];mat_index={}
for name,ref in manifest['materials'].items():
    mat_index[name]=len(materials)
    data=json.loads(read_asset(CONTENT/(ref['path'][6:]+'.asset'))[1])
    materials.append(data)
    material_assets.append({'index':mat_index[name],'asset':ref})

def uid(name):return uuid.uuid5(uuid.NAMESPACE_URL,'afterlight/honeybud/'+name).hex
objects=[]
def object_at(name,p,scale=(1,1,1),material=0,visible=True):
    return {'id':uid(name),'name':name,'enabled':True,'components':{'transform':{'position':list(p),'rotation':[0,0,0,1]},
            'render':{'shape':'box','scale':list(scale),'offset':[0,0,0],'animationScale':[1,1,1],'material':material,'visible':visible},
            }}

for i,instance in enumerate(manifest['placements']):
    x,y,z=instance['position'];scale=instance['scale']
    for part in manifest['kits'][instance['kit']]:
        o=object_at(f"{instance['kit']}_{i:03d}_{part['material']}",(x,z,-y),(scale,)*3,mat_index[part['material']])
        o['components']['transform']['rotation']=[0,math.sin(instance['angle']/2),0,math.cos(instance['angle']/2)];o['components']['render']['mesh']=part['mesh'];objects.append(o)

def collider(name,p,half,yaw=0,walkable=False):
    o=object_at(name,p,visible=False);o['components']['transform']['rotation']=[0,math.sin(yaw/2),0,math.cos(yaw/2)]
    o['components'].pop('render')
    o['components']['collider']={'shape':{'type':'box','halfExtents':list(half)},'blocking':True,'walkable':walkable}
    objects.append(o)

collider('Navigation lawn',(0,-.15,0),(10.5,.15,8.6),walkable=True)
collider('Cottage walls',(1,1.75,-5.1),(2.43,1.75,1.80))
collider('Fountain basin',(-.6,.4,.1),(1.77,.4,1.77))
collider('Pergola bench',(-6.2,.9,-3.35),(1.5,.9,.63))
collider('Tea bench',(6,.9,-1.6),(1.5,.9,.63),-.28)
collider('Tea table',(5.4,.48,.25),(.74,.48,.74))
for x in [-8.6,-6]:collider('Planter '+str(x),(x,.27,3.55),(1.2,.27,.76))
for x in [-10,10]:collider('Boundary '+str(x),(x,.6,0),(.16,.6,8.1))
collider('Back boundary',(0,.6,-8.1),(10,.6,.16))
for x in [-7,7]:collider('Front boundary '+str(x),(x,.6,8),(3.2,.6,.16))
for x in [-7.8,-4.6]:
    for y in [1.65,4.35]:collider('Pergola column '+str((x,y)),(x,1.6,-y),(.13,1.6,.13))

references={}
for role,p in [('player',(2,1,4.7)),('companion',(3.4,.55,5.4))]:
    source=next(o for o in original['entities'] if o['id']==original['references'][role])
    o=copy.deepcopy(source);o['id']=uid(role);o['components']['transform']['position']=[p[0],source['components']['transform']['position'][1],p[2]]
    o['components']['render']['material']=len(materials)
    character_mat=copy.deepcopy(original['materials'][source['components']['render']['material']]);character_mat['shader']=asset_reference(CONTENT/'shaders/Standard.asset')
    materials.append(character_mat);objects.append(o);references[role]=o['id']

script='''function initialize() {
    var player = Engine.sceneObject('player');
    Engine.setPlayer(player);
    Locomotion.init(player);
    Companion.init(Engine.sceneObject('companion'), player);
    CameraRig.init(Engine.cameraState());
    CameraRig.follow = false;
    Controller.message = 'Honeybud Court';
    Engine.log('Honeybud Court: Blender-authored static meshes, shared PBR materials and garden navigation.');
}
'''
script_path=CONTENT/'scripts/levels/honeybud_court.asset'
write_asset(script_path,'Script',script.encode())
lighting=json.loads((ROOT/'Projects/Afterlight/SourceArt/lighting.json').read_text())['HoneybudCourt']
objects.extend(copy.deepcopy(lighting['lights']))
scene={'version':7,'entities':objects,'materials':materials,'materialAssets':material_assets,
       'camera':{'target':[0,.5,-.8],'yaw':.32,'pitch':.57,'distance':27.5,'fov':.72},
       'navigation':{'min':[-10.6,0,-8.7],'max':[10.6,8,8.7],'cellSize':.25,'planeTolerance':.03},
       'player':references['player'],'references':references,'data':{'theme':'Honeybud Court'},
       'scripts':[asset_reference(script_path)]}
path=CONTENT/'Maps/HoneybudCourt.asset'
write_asset(path,'Map',json.dumps(scene,indent=2).encode(),metadata={'authoring':'Blender MCP','description':'Original warm miniature garden kit; see SourceArt/Honeybud.'})
print(json.dumps({'map':str(path),'objects':len(objects),'uniqueMeshParts':sum(len(v) for v in manifest['kits'].values()),'trianglesUnique':sum(p['triangles'] for v in manifest['kits'].values() for p in v),'materials':len(materials)},indent=2))
