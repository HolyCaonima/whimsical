"""Build portable Maple Circuit assets using the engine's public ALAS1 / STM1 formats.

Run: python Projects/MapleCircuit/SourceArt/build.py
All geometry is original, generated here; no engine files or other projects are edited.
"""
import json
import math
import random
import struct
import uuid
from pathlib import Path
from PIL import Image, ImageDraw

PROJECT = Path(__file__).resolve().parents[1]
CONTENT = PROJECT / 'Content'
random.seed(17)

def uid(name):
    return uuid.uuid5(uuid.NAMESPACE_URL, 'maple-circuit/' + name).hex

def asset(path, kind, payload=b'', metadata=None, source=None):
    dest = CONTENT / (path + '.asset')
    dest.parent.mkdir(parents=True, exist_ok=True)
    header = dict(id=uid(path), name=path.rsplit('/', 1)[-1], version=1, type=kind,
                  storage='external' if source else 'embedded', metadata=metadata or {})
    if source:
        header['source'] = source
    if not isinstance(payload, bytes):
        payload = json.dumps(payload, ensure_ascii=False, separators=(',', ':')).encode()
    dest.write_bytes(b'ALAS1\n' + json.dumps(header).encode() + b'\n' + payload)
    return dict(id=header['id'], path='/Game/' + path)

def rgb(hexcolor):
    values = [int(hexcolor[i:i+2], 16) / 255 for i in (0, 2, 4)]
    return tuple(v / 12.92 if v <= .04045 else ((v + .055) / 1.055)**2.4 for v in values)

def sub(a, b): return tuple(x-y for x, y in zip(a, b))
def cross(a, b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def unit(a):
    length = math.sqrt(sum(x*x for x in a))
    return tuple(x/length for x in a)

class Mesh:
    def __init__(self): self.vertices, self.indices = [], []

    def face(self, points, color, normals=None):
        normal = unit(cross(sub(points[1], points[0]), sub(points[2], points[0])))
        tangent = unit(sub(points[1], points[0]))
        start = len(self.vertices)
        for i, p in enumerate(points):
            self.vertices.append((*p, *(normals[i] if normals else normal),
                                  float(i in (1, 2)), float(i >= 2), *tangent, 1., *rgb(color)))
        for i in range(1, len(points)-1): self.indices.extend((start, start+i, start+i+1))

    def box(self, p, size, color):
        x,y,z=p; a,b,c=[v/2 for v in size]
        v=[(x+i*a,y+j*b,z+k*c) for i,j,k in [(-1,-1,-1),(1,-1,-1),(1,1,-1),(-1,1,-1),
                                            (-1,-1,1),(1,-1,1),(1,1,1),(-1,1,1)]]
        for f in [(0,3,2,1),(4,5,6,7),(0,4,7,3),(1,2,6,5),(3,7,6,2),(0,1,5,4)]:
            self.face([v[i] for i in f], color)

    def ring(self, p, levels, segments, color, axis='y', smooth=False):
        # levels = (axis offset, radius). End disks are included when radii are nonzero.
        def point(h, r, a):
            q=(r*math.cos(a),h,r*math.sin(a))
            if axis=='x': q=(q[1],q[0],q[2])
            return tuple(p[i]+q[i] for i in range(3))
        for (h0,r0),(h1,r1) in zip(levels, levels[1:]):
            for i in range(segments):
                a=math.tau*i/segments; b=math.tau*(i+1)/segments
                pts=[point(h0,r0,a),point(h1,r1,a),point(h1,r1,b),point(h0,r0,b)]
                # Tiny tip radii keep all triangles nondegenerate.
                normals=None
                if smooth:
                    normals=[]
                    slope=(r0-r1)/(h1-h0)
                    for angle in (a,a,b,b):
                        n=unit((math.cos(angle),slope,math.sin(angle)))
                        normals.append((n[1],n[0],n[2]) if axis=='x' else n)
                self.face(pts,color,normals)
        for h,r in (levels[0],levels[-1]):
            self.face([point(h,r,math.tau*i/segments) for i in range(segments)],color)

    def save(self, name):
        data=b'STM1'+struct.pack('<II',len(self.vertices),len(self.indices))
        data+=b''.join(struct.pack('<15f',*v) for v in self.vertices)
        data+=struct.pack('<%dI'%len(self.indices),*self.indices)
        return asset('models/'+name,'StaticMesh',data)

def catmull(a,b,c,d,t):
    return tuple(.5*((2*b[k])+(-a[k]+c[k])*t+(2*a[k]-5*b[k]+4*c[k]-d[k])*t*t+
                     (-a[k]+3*b[k]-3*c[k]+d[k])*t*t*t) for k in range(2))

controls=[(-48,-25),(-48,12),(-45,40),(-30,52),(0,48),(10,35),(10,23),(23,20),
          (35,28),(45,17),(46,-28),(37,-49),(22,-50),(13,-34),(-5,-31),(-26,-38),(-43,-39)]
controls=[(x*1.25,z*1.25) for x,z in controls]
raw=[]
for i in range(len(controls)):
    for j in range(32):
        raw.append(catmull(controls[i-1],controls[i],controls[(i+1)%len(controls)],controls[(i+2)%len(controls)],j/32))
lengths=[0.]
for i in range(len(raw)): lengths.append(lengths[-1]+math.dist(raw[i],raw[(i+1)%len(raw)]))
length=lengths[-1]
# Equal arc-length samples make AI lookahead and lap progress independent of control-point density.
track=[]; edge=0
for i in range(384):
    distance=length*i/384
    while lengths[edge+1]<distance: edge+=1
    t=(distance-lengths[edge])/(lengths[edge+1]-lengths[edge])
    a,b=raw[edge],raw[(edge+1)%len(raw)]
    track.append([a[0]+(b[0]-a[0])*t,a[1]+(b[1]-a[1])*t])

def sample(index, offset=0):
    a=track[index%len(track)]; b=track[(index+1)%len(track)]
    dx,dz=unit((b[0]-a[0],b[1]-a[1]))
    return (a[0]+dz*offset,a[1]-dx*offset,math.atan2(dx,dz))

def strip(mesh,left,right,y,color,start=0,end=384):
    for i in range(start,end):
        a=sample(i,left); b=sample(i,right); c=sample(i+1,right); d=sample(i+1,left)
        mesh.face([(a[0],y,a[1]),(d[0],y,d[1]),(c[0],y,c[1]),(b[0],y,b[1])],color)

objects=[]; references={}; meshes={}; material=[]
def obj(name,p=(0,0,0),scale=(1,1,1),mesh=None,mat=0,yaw=0,half=None,layer=1,enabled=True,ref=False):
    components=dict(transform=dict(position=list(p),rotation=[0,math.sin(yaw/2),0,math.cos(yaw/2)]))
    if mesh:
        components['render']=dict(scale=list(scale),offset=[0,0,0],animationScale=[1,1,1],material=material[mat],visible=True,mesh=mesh)
    if half:
        components['collider']=dict(shape=dict(type='box',halfExtents=list(half)),motion='kinematic' if ref else 'static',
                                   layer=layer,blocking=True,pickable=False,walkable=False)
    o=dict(id=uid('object/'+name),name=name,enabled=enabled,components=components)
    objects.append(o)
    if ref: references[name]=o['id']
    return o

schema=dict(materialModel='metallicRoughness',properties=[
    dict(name='baseColor',type='vec3',default=[1,1,1]),dict(name='roughness',type='float',default=.75),
    dict(name='emission',type='vec3',default=[0,0,0]),dict(name='metallic',type='float',default=0)],
    textures=[],renderState=dict(surface='opaque',cull='none',alphaCutoff=.5))
standard=asset('shaders/Standard','Shader',metadata=schema,source='Standard.glsl')
skyshader=asset('shaders/Sky','Shader',metadata=schema,source='Sky.glsl')
for name, rough in [('Surface',.8),('Gloss',.38),('Matte',.95)]:
    material.append(asset('Materials/'+name,'Material',dict(shader=standard,properties=dict(roughness=rough),textures={})))
material.append(asset('Materials/Sky','Material',dict(shader=skyshader,properties={},textures={})))

# Road, sidewalk, curbs and lane paint are a single shared authored mesh.
road=Mesh();strip(road,-6.6,6.6,.025,'303B42')
for side in (-1,1):
    strip(road,side*6.6,side*9.0,.05,'C9C9B5')
    for i in range(384):
        strip(road,side*6.6,side*7.1,.075,'F0E9D6' if (i//2)%2 else 'D95E52',i,i+1)
for i in range(0,384,7): strip(road,-.065,.065,.043,'DFE5DB',i,i+3)
for i in range(12):
    strip(road,-6.6+i*1.1,-5.5+i*1.1,.05,'EDC963' if i%2 else '26323B',0,1)
    strip(road,-6.6+i*1.1,-5.5+i*1.1,.05,'26323B' if i%2 else 'EDC963',1,2)
obj('Circuit',mesh=road.save('circuit'))
lawn=Mesh();lawn.box((0,-.15,0),(250,.3,250),'79A356')
o=obj('Lawn',mesh=lawn.save('lawn'),half=(125,.15,125),p=(0,0,0));o['components']['collider']['walkable']=True
# Physics ground top sits below car bottoms; flat racing is governed by the project vehicle controller.
o['components']['collider']['shape']['halfExtents']=[125,.1,125];o['position'][1]=-.1

tree=Mesh();tree.ring((0,0,0),[(0,.52),(.35,.36),(3.8,.22)],7,'B98263')
tree.ring((0,0,0),[(2.8,1.0),(3.6,1.65),(5.5,1.48),(7.7,.7),(8.4,.05)],9,'279D73',smooth=True)
treeref=tree.save('maple_tree')
shrub=Mesh();shrub.ring((0,0,0),[(.05,.9),(.5,1.1),(1.4,.7),(1.6,.05)],8,'37805E');shrubref=shrub.save('hedge')

def house(name,wall,two):
    m=Mesh();height=6.5 if two else 3.5
    m.box((0,.25,0),(6.4,.5,5.4),'9B6250')
    m.box((0,height/2+.4,0),(6,height,5),wall)
    for y in [i*.38+.65 for i in range(int(height/.38))]: m.box((0,y,0),(6.035,.025,5.035),'AEBDBD')
    m.box((0,height+.42,0),(6.35,.19,5.35),'ECEEDF')
    # Gabled roof, ridge along X.
    a,b,c,d,e,f=(-3.5,height+.5,-3),(3.5,height+.5,-3),(-3.5,height+2.1,0),(3.5,height+2.1,0),(-3.5,height+.5,3),(3.5,height+.5,3)
    m.face([a,b,d,c],'454341');m.face([c,d,f,e],'514A41');m.face([a,c,e],'ECE2CD');m.face([b,f,d],'ECE2CD')
    for sign in (-1,1):
        for j in range(1,8):
            z=sign*j*.375;y=height+2.1-j*.2
            m.box((0,y,z),(7.04,.065,.08),'373B37')
    m.box((1.6,height+1.8,-.5),(.64,2,.65),'9B6551')
    m.box((1.6,height+2.82,-.5),(.83,.12,.82),'6E5350')
    for y in ([2,5.25] if two else [2]):
        for x in (-1.95,1.95):
            m.box((x,y,2.54),(1.45,1.65,.1),'ECEFE2');m.box((x,y,2.61),(1.23,1.43,.06),'82AFBF')
            m.box((x,y,2.66),(.065,1.43,.035),'ECEFE2');m.box((x,y,2.66),(1.23,.07,.035),'ECEFE2')
            for dx in (-.87,.87): m.box((x+dx,y,2.56),(.21,1.65,.1),'31564F')
        for z in (-1.35,1.35):
            for x in (-3.04,3.04):
                m.box((x,y,z),(.1,1.65,1.3),'ECEFE2');m.box((x*1.025,y,z),(.06,1.42,1.08),'82AFBF')
    m.box((0,1.38,2.56),(1.1,2.3,.14),'E9E5D5');m.box((0,1.34,2.66),(.88,2.13,.08),'A04440')
    m.box((.28,1.25,2.74),(.08,.08,.08),'E6C264')
    m.box((0,.17,3),(1.8,.34,.9),'D6D3C2')
    return m.save(name)

houses=[house('house_blue','99B4BF',True),house('house_cream','DCDCCB',False),house('house_sage','A9B8A2',True)]
lamp=Mesh();lamp.ring((0,0,0),[(0,.26),(.35,.13),(6.5,.10)],8,'283D3F')
lamp.box((.48,6.4,0),(1.12,.16,.16),'283D3F');lamp.box((.95,6.26,0),(.56,.18,.42),'E2DCAE');lampref=lamp.save('street_lamp')
barrier=Mesh();barrier.box((0,.5,0),(2.8,1,.55),'E6BB59');barrierref=barrier.save('barrier')
tire=Mesh();tire.ring((0,0,0),[(0,.6),(.22,.66),(.45,.6)],12,'252C2D');tireref=tire.save('tire_stack')

for i in range(0,384,12):
    for side in (-1,1):
        x,z,yaw=sample(i,side*(11.8+random.random()*1.5));s=random.uniform(.85,1.2)
        obj('Tree_%s_%s'%(i,side),(x,0,z),(s,s,s),treeref,half=(.34,3.5,.34))
    if i%24==0:
        x,z,yaw=sample(i,9.8);obj('Lamp_'+str(i),(x,0,z),mesh=lampref,half=(.15,3,.15))

for i in range(0,384,23):
    for side in (-1,1):
        x,z,yaw=sample(i,side*18)
        # Avoid placing a house across the nearby inside segment of a tight turn.
        if min(math.hypot(x-p[0],z-p[1]) for p in track)<14: continue
        yaw+=-side*math.pi/2
        obj('House_%s_%s'%(i,side),(x,0,z),mesh=houses[(i//23+(side==1))%3],yaw=yaw,half=(3.2,3.6,2.7))
        for j in (-3.8,3.8):
            hx=x+math.cos(yaw)*j;hz=z-math.sin(yaw)*j
            obj('Hedge_%s_%s_%s'%(i,side,j),(hx,0,hz),(1.4,.9,1.4),shrubref)

for i in list(range(74,108,3))+list(range(183,210,3))+list(range(252,285,3)):
    x,z,yaw=sample(i,-7.85)
    if i%2:
        obj('Barrier_'+str(i),(x,0,z),mesh=barrierref,yaw=yaw,half=(1.4,.6,.28))
    else:
        obj('Tires_'+str(i),(x,0,z),mesh=tireref,half=(.62,.5,.62))
        obj('Tires_top_'+str(i),(x,.45,z),mesh=tireref)

# White, open-cockpit sports car. Local forward is +Z; root is 0.72 m above the road.
car=Mesh()
car.box((0,-.18,0),(1.98,.45,4.15),'EEEFE2')
car.box((0,.12,1.15),(1.86,.3,1.63),'F5F3E8')
car.box((0,.16,-1.34),(1.88,.35,1.23),'F5F3E8')
car.box((0,-.38,-2.07),(1.92,.18,.2),'B6BCC0')
car.box((0,-.3,2.08),(1.95,.18,.16),'253335')
car.box((0,.0,-2.12),(1.81,.25,.07),'283537')
car.box((0,.0,-2.166),(1.64,.17,.02),'BA3544')
for x in (-.52,.52):
    car.box((x,0,-2.18),(.22,.14,.025),'F0E9D8')
    car.box((x,-.02,2.14),(.51,.19,.07),'EFE8CA')
    car.ring((x,-.43,-2.05),[(-.15,.115),(.17,.115)],10,'28363A',axis='y')
for x in (-.93,.93):
    car.box((x,.2,-.02),(.16,.52,1.66),'ECECDD')
    for y in (-.06,.07,.2): car.box((x*1.085,y,.02),(.025,.055,.72),'23353A')
    car.box((x*1.13,.39,.73),(.25,.12,.2),'D8DFDA')
car.box((0,.17,-.2),(1.68,.08,1.35),'263B3C')
for x in (-.43,.43):
    car.box((x,.4,-.45),(.55,.5,.22),'626D64');car.box((x,.2,-.17),(.55,.15,.62),'4D625E')
car.box((0,.46,.62),(1.74,.63,.065),'476674')
car.box((0,.81,.62),(1.87,.06,.12),'DCE5DE')
for x in (-.87,.87): car.box((x,.48,.62),(.065,.64,.11),'DBE1D9')
car.box((0,.7,-1.78),(2.22,.105,.48),'394C50')
for x in (-.69,.69): car.box((x,.44,-1.78),(.08,.5,.16),'263B3F')
for z in (-1.65,-1.48,-1.31,-1.14): car.box((0,.345,z),(.86,.035,.075),'293B3E')
carref=car.save('roadster')
wheel=Mesh();wheel.ring((0,0,0),[(-.2,.38),(-.14,.43),(.14,.43),(.2,.38)],16,'222E32',axis='x')
for x in (-.205,.205):
    wheel.ring((x,0,0),[(-.012,.29),(.012,.29)],12,'BFC9C6',axis='x')
    wheel.ring((x*1.06,0,0),[(-.012,.105),(.012,.105)],8,'687C82',axis='x')
    for i in range(5):
        a=i*math.tau/5
        wheel.face([(x*1.08,.08*math.cos(a-.4),.08*math.sin(a-.4)),
                    (x*1.08,.27*math.cos(a-.12),.27*math.sin(a-.12)),
                    (x*1.08,.27*math.cos(a+.12),.27*math.sin(a+.12)),
                    (x*1.08,.08*math.cos(a+.4),.08*math.sin(a+.4))],'EAF0E7')
wheelref=wheel.save('wheel')
for i in range(4):
    x,z,yaw=sample(382-(i//2)*4,(-2.2 if i%2==0 else 2.2))
    obj('car'+str(i),(x,.72,z),mesh=carref,mat=1,yaw=yaw,half=(.98,.4,2.06),layer=2,ref=True)
    for w in range(4):
        wheel_entity=obj('car%d_wheel%d'%(i,w),(1.03 if w%2 else -1.03,-.26,1.27 if w<2 else -1.3),mesh=wheelref,mat=1,ref=True)
        wheel_entity['components']['transform']['parent']=references['car'+str(i)]

smoke=Mesh();smoke.ring((0,0,0),[(-.4,.12),(-.2,.42),(.2,.43),(.46,.1)],7,'899697');smokeref=smoke.save('smoke_opaque')
for i in range(48): obj('smoke'+str(i),mesh=smokeref,mat=2,enabled=False,ref=True)
skid=Mesh();skid.box((0,0,0),(.19,.006,.85),'263235');skidref=skid.save('skid')
for i in range(128): obj('skid'+str(i),mesh=skidref,enabled=False,ref=True)
sky=Mesh()
for j in range(18):
    lo=-math.pi/2+.001+j*(math.pi-.002)/18;hi=-math.pi/2+.001+(j+1)*(math.pi-.002)/18
    for i in range(36):
        a=i*math.tau/36;b=(i+1)*math.tau/36
        normals=[(math.cos(h)*math.cos(t),math.sin(h),math.cos(h)*math.sin(t)) for h,t in [(lo,a),(hi,a),(hi,b),(lo,b)]]
        sky.face([tuple(v*148 for v in n) for n in normals],'FFFFFF',normals)
obj('sky',mesh=sky.save('sky_dome'),mat=3,ref=True)

data=dict(track=track,trackLength=length,roadHalfWidth=6.6,laps=3,smokeCount=48,skidCount=128)
scripts=[]
for name in ('track','vehicle','race','hud','main'):
    asset('scripts/'+name,'Script',source=name+'.js')
    scripts.append('/Game/scripts/'+name)
PROJECT.joinpath('.project').write_text(json.dumps(dict(version=1,id=uid('project'),name='Maple Circuit',
    startupMap='/Game/Maps/MapleCircuit',scripts=scripts),indent=2)+'\n',encoding='utf-8')
lighting=json.loads((PROJECT/'SourceArt/lighting.json').read_text())
objects.extend(lighting['lights'])
for entity in objects:
    if entity['name'] in lighting['renderOverrides']:
        entity['components']['render'].update(lighting['renderOverrides'][entity['name']])
scene=dict(version=10,entities=objects,
    camera=dict(target=[-60,1,-30],yaw=math.pi,pitch=.27,distance=10,fov=.95),
    navigation=dict(min=[-120,0,-120],max=[120,12,120],cellSize=1,planeTolerance=.03),
    references=references,data=data,scripts=[])
asset('Maps/MapleCircuit','Map',scene)
# Native-engine QA scene uses exactly the same assets and gameplay with player AI enabled.
scene['data']=dict(data,verification=True)
asset('Maps/Verification','Map',scene)
controls_script=asset('tests/controls','Script',source='controls.js')
scene['data']=dict(data)
scene['scripts']=[controls_script]
asset('Maps/ControlsVerification','Map',scene)
scene['data']=dict(data,showcase=True)
asset('Maps/DriftPreview','Map',scene)

ui=CONTENT/'UI';ui.mkdir(parents=True,exist_ok=True)
im=Image.new('RGBA',(360,400),(0,0,0,0));draw=ImageDraw.Draw(im)
minimum=[min(p[k] for p in track) for k in range(2)];maximum=[max(p[k] for p in track) for k in range(2)]
def map_point(p): return (35+(p[0]-minimum[0])/(maximum[0]-minimum[0])*290,
                          35+(maximum[1]-p[1])/(maximum[1]-minimum[1])*330)
line=[map_point(p) for p in track];line.append(line[0])
draw.line(line,fill=(61,87,90,255),width=15,joint='curve');draw.line(line,fill=(190,211,207,255),width=7,joint='curve')
x,y=map_point(track[0]);draw.line([(x-12,y),(x+12,y)],fill=(242,204,101,255),width=5)
im.save(ui/'circuit.png')
stats=dict(objects=len(objects),meshAssets=len(list((CONTENT/'models').glob('*.asset'))),trackLength=round(length,2),
           triangles=sum((struct.unpack('<II',f.read_bytes().split(b'\n',2)[2][4:12])[1]//3) for f in (CONTENT/'models').glob('*.asset')))
PROJECT.joinpath('SourceArt/manifest.json').write_text(json.dumps(stats,indent=2)+'\n')
print(json.dumps(stats))
