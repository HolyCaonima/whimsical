"""Generate project-owned static meshes; no engine mesh extensions needed."""
import math, struct, json, hashlib
from pathlib import Path
root=Path(__file__).resolve().parents[1]/'Content'
out=root/'Models'; out.mkdir(exist_ok=True)
def write(name, vertices, indices):
    (out/(name+'.stm')).write_bytes(b'STM1'+struct.pack('<II',len(vertices)//15,len(indices))+struct.pack('<%sf'%len(vertices),*vertices)+struct.pack('<%sI'%len(indices),*indices))
    header=dict(id=hashlib.md5(('ConstraintLab/'+name).encode()).hexdigest(),type='StaticMesh',name=name,version=1,storage='external',metadata={},source=name+'.stm')
    (out/(name+'.asset')).write_text('ALAS1\n'+json.dumps(header)+'\n',encoding='utf-8')
def vertex(v,p,n,u,w):
    v.extend([*p,*n,u,w,1,0,0,1,1,1,1])
v=[]; ids=[]; sides=16
for i in range(sides+1):
    a=i*2*math.pi/sides; c,s=math.cos(a),math.sin(a)
    for y in [-0.5,0.5]:vertex(v,[c,y,s],[c,0,s],i/sides,y+0.5)
    if i:
        k=i*2; ids.extend([k-2,k-1,k+1,k-2,k+1,k])
write('Rod',v,ids)
v=[]; ids=[]; rows=16; cols=24
for j in range(rows+1):
    p=j*math.pi/rows
    for i in range(cols+1):
        a=i*2*math.pi/cols; n=[math.sin(p)*math.cos(a),math.cos(p),math.sin(p)*math.sin(a)]
        vertex(v,n,n,i/cols,j/rows)
        if i and j:
            k=j*(cols+1)+i; ids.extend([k-cols-2,k,k-1,k-cols-2,k-cols-1,k])
write('Sphere',v,ids)
# One immutable mesh for each exhibition stand, instead of separate box instances.
v=[]; ids=[]
def box(center, scale):
    points=[[-1,-1,-1],[1,-1,-1],[1,1,-1],[-1,1,-1],[-1,-1,1],[1,-1,1],[1,1,1],[-1,1,1]]
    faces=[([0,3,2,1],[0,0,-1]),([4,5,6,7],[0,0,1]),([0,1,5,4],[0,-1,0]),([2,3,7,6],[0,1,0]),([0,4,7,3],[-1,0,0]),([1,2,6,5],[1,0,0])]
    for corners,normal in faces:
        base=len(v)//15
        for index in corners:vertex(v,[center[k]+points[index][k]*scale[k]/2 for k in range(3)],normal,0,0)
        ids.extend([base,base+1,base+2,base,base+2,base+3])
box([0,-0.18,0],[9,0.35,6.5]);box([0,0.35,0],[0.8,0.7,0.8])
for direction in [-1,1]:
    box([direction*3.5,2.5,0.22],[0.055,5,0.055])
    box([direction*3.5,0.1,0],[0.7,0.2,0.7])
box([-3.5,5,0],[0.22,0.22,0.22])
write('Stand',v,ids)
# Keep material ids stable when regenerating geometry.
for name,color,emission in [('Node',[0.72,0.40,0.13],[0,0,0]),('Edge',[0.78,0.45,0.17],[0,0,0]),('Obstacle',[0.07,0.36,0.38],[0.005,0.02,0.02])]:
    path=root/'Materials'/(name+'.asset')
    if path.exists():
        lines=path.read_text(encoding='utf-8-sig').splitlines();header=json.loads(lines[1]);body=json.loads('\n'.join(lines[2:]))
    else:
        header=dict(id=hashlib.md5(('ConstraintLab/material/'+name).encode()).hexdigest(),type='Material',name=name,version=1,storage='embedded',metadata={})
        body=json.loads('\n'.join((root/'Materials/Node.asset').read_text().splitlines()[2:]))
    body['properties'].update(baseColor=color,emission=emission,roughness=0.75)
    path.write_text('ALAS1\n'+json.dumps(header)+'\n'+json.dumps(body,indent=2)+'\n',encoding='utf-8')
print('Rope meshes generated.')
