// Source-owned helper meshes, using the engine's ordinary STM1 StaticMesh format.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
const folder=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../Content/Models/Gizmo');
fs.mkdirSync(folder,{recursive:true});
function mesh(){return {vertices:[],indices:[]};}
function vertex(m,p,n){m.vertices.push(...p,...n,0,0,1,0,0,1,1,1,1);return m.vertices.length/15-1;}
function quad(m,a,b,c,d){m.indices.push(a,b,c,a,c,d);}
function tube(m,z0,z1,r0,r1){
    const base=m.vertices.length/15,steps=20,slope=(r0-r1)/(z1-z0),length=Math.hypot(1,slope);
    for(let i=0;i<=steps;i++){
        const a=i*Math.PI*2/steps,c=Math.cos(a),s=Math.sin(a),n=[c/length,s/length,slope/length];
        vertex(m,[c*r0,s*r0,z0],n);vertex(m,[c*r1,s*r1,z1],n);
        if(i)quad(m,base+(i-1)*2,base+i*2,base+i*2+1,base+(i-1)*2+1);
    }
}
function ring(full){
    const m=mesh(),steps=full?128:64,sides=10;
    for(let i=0;i<=steps;i++){
        const a=-Math.PI/2+i*(full?2:1)*Math.PI/steps,c=Math.cos(a),s=Math.sin(a);
        for(let j=0;j<=sides;j++){
            const b=j*Math.PI*2/sides,u=Math.cos(b),v=Math.sin(b),r=1+0.018*u;
            vertex(m,[r*c,r*s,0.018*v],[u*c,u*s,v]);
            if(i&&j){const k=i*(sides+1)+j;quad(m,k-sides-2,k-1,k,k-sides-1);}
        }
    }
    return m;
}
function box(m,z,size){
    const points=[[-1,-1,-1],[1,-1,-1],[1,1,-1],[-1,1,-1],[-1,-1,1],[1,-1,1],[1,1,1],[-1,1,1]];
    for(const face of [[0,3,2,1],[4,5,6,7],[0,1,5,4],[2,3,7,6],[0,4,7,3],[1,2,6,5]]){
        const ids=face.map(i=>vertex(m,points[i].map((v,k)=>v*size+(k===2?z:0)),[0,0,1]));
        quad(m,...ids);
    }
}
const move=mesh();tube(move,0,0.78,0.014,0.014);tube(move,0.76,1,0.075,0);
const scale=mesh();tube(scale,0,0.96,0.014,0.014);box(scale,1,0.055);
const plane=mesh();
const corners=(lo,hi)=>[[lo,lo],[hi,lo],[hi,hi],[lo,hi]];
const outer=corners(0.22,0.48).map(([x,y])=>vertex(plane,[x,y,0],[0,0,1]));
const inner=corners(0.24,0.46).map(([x,y])=>vertex(plane,[x,y,0],[0,0,1]));
for(let i=0;i<4;i++){const j=(i+1)%4;quad(plane,outer[i],outer[j],inner[j],inner[i]);}
quad(plane,...corners(0.24,0.46).map(([x,y])=>{
    const i=vertex(plane,[x,y,0],[0,0,1]);plane.vertices.splice(i*15+12,3,0.55,0.55,0.55);return i;
}));
for(const [name,m] of Object.entries({Move:move,Scale:scale,Arc:ring(false),Ring:ring(true),Plane:plane})){
    const bytes=Buffer.alloc(12+m.vertices.length*4+m.indices.length*4);
    bytes.write('STM1');bytes.writeUInt32LE(m.vertices.length/15,4);bytes.writeUInt32LE(m.indices.length,8);
    let offset=12;for(const v of m.vertices){bytes.writeFloatLE(v,offset);offset+=4;}for(const i of m.indices){bytes.writeUInt32LE(i,offset);offset+=4;}
    fs.writeFileSync(path.join(folder,name+'.stm'),bytes);
    const header={id:createHash('md5').update('HumanEditor/Gizmo/'+name).digest('hex'),type:'StaticMesh',name,version:1,storage:'external',metadata:{},source:name+'.stm'};
    fs.writeFileSync(path.join(folder,name+'.asset'),'ALAS1\n'+JSON.stringify(header)+'\n');
}
console.log('Gizmo meshes generated.');
