// Editor helper art: STM1 meshes for the sea-level grid, the light billboards and the wire
// primitives, plus the sprite atlas those billboards sample. One cell table drives both the
// texture and the quad UVs, so the atlas and the meshes cannot drift apart.
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const folder=path.join(root,'Content/Models/Helper'),textureFolder=path.join(root,'Content/Textures');
fs.mkdirSync(folder,{recursive:true});fs.mkdirSync(textureFolder,{recursive:true});
function mesh(){return {vertices:[],indices:[]};}
// STM1 vertex: position, normal, uv, tangent, colour.
function vertex(m,p,normal,color,uv){
    m.vertices.push(p[0],p[1],p[2],normal[0],normal[1],normal[2],uv?uv[0]:0,uv?uv[1]:0,1,0,0,1,color[0],color[1],color[2]);
    return m.vertices.length/15-1;
}
function quad(m,a,b,c,d){m.indices.push(a,b,c,a,c,d);}
function lineXZ(m,x0,z0,x1,z1,half,color){
    const dx=x1-x0,dz=z1-z0,len=Math.hypot(dx,dz),px=-dz/len*half,pz=dx/len*half,up=[0,1,0];
    quad(m,vertex(m,[x0-px,0,z0-pz],up,color),vertex(m,[x1-px,0,z1-pz],up,color),vertex(m,[x1+px,0,z1+pz],up,color),vertex(m,[x0+px,0,z0+pz],up,color));
}
const grid=mesh(),extent=40,step=1,major=10;
for(let i=-extent;i<=extent;i+=step){
    const axis=!i,heavy=!(i%major),half=axis?0.025:heavy?0.014:0.007;
    lineXZ(grid,-extent,i,extent,i,half,axis?[0.62,0.24,0.24]:heavy?[0.38,0.38,0.40]:[0.18,0.18,0.19]);
    lineXZ(grid,i,-extent,i,extent,half,axis?[0.22,0.34,0.62]:heavy?[0.38,0.38,0.40]:[0.18,0.18,0.19]);
}
// Wires are instanced per edge instead of baked per shape: only a rod scaled along its own
// axis keeps a two pixel line while the emitter it measures is stretched. Unit rod spans
// y in [-0.5,0.5] at radius 1; the tip cone stands on y=0 and points at y=1.
function rod(shade){
    const m=mesh(),sides=6,color=[shade,shade,shade];
    for(let i=0;i<sides;i++){
        const a=i*Math.PI*2/sides,b=(i+1)*Math.PI*2/sides;
        const na=[Math.cos(a),0,Math.sin(a)],nb=[Math.cos(b),0,Math.sin(b)];
        quad(m,vertex(m,[na[0],-0.5,na[2]],na,color),vertex(m,[nb[0],-0.5,nb[2]],nb,color),
            vertex(m,[nb[0],0.5,nb[2]],nb,color),vertex(m,[na[0],0.5,na[2]],na,color));
    }
    return m;
}
function cone(shade){
    const m=mesh(),sides=12,color=[shade,shade,shade],up=[0,1,0],down=[0,-1,0];
    const apex=vertex(m,[0,1,0],up,color),hub=vertex(m,[0,0,0],down,color);
    for(let i=0;i<sides;i++){
        const a=i*Math.PI*2/sides,b=(i+1)*Math.PI*2/sides;
        const pa=[Math.cos(a),0,Math.sin(a)],pb=[Math.cos(b),0,Math.sin(b)];
        m.indices.push(apex,vertex(m,pa,[pa[0],1,pa[2]],color),vertex(m,pb,[pb[0],1,pb[2]],color));
        m.indices.push(hub,vertex(m,pb,down,color),vertex(m,pa,down,color));
    }
    return m;
}
// Sprite atlas. Glyphs are signed distance fields so the icons stay smooth at any size,
// and every cell keeps a transparent margin so mip levels never bleed between neighbours.
// The atlas is greyscale: the material tint is what makes an icon amber or, once selected, white.
const CELL=128,COLS=3,ROWS=2;
const cells={IconPoint:[0,0],IconSpot:[1,0],IconDirectional:[2,0],IconRect:[0,1],IconCapsule:[1,1]};
function iconQuad(cell){
    const m=mesh(),n=[0,0,1],white=[1,1,1];
    const u0=cell[0]/COLS,u1=(cell[0]+1)/COLS,v0=cell[1]/ROWS,v1=(cell[1]+1)/ROWS;
    quad(m,vertex(m,[-0.5,-0.5,0],n,white,[u0,v1]),vertex(m,[0.5,-0.5,0],n,white,[u1,v1]),
        vertex(m,[0.5,0.5,0],n,white,[u1,v0]),vertex(m,[-0.5,0.5,0],n,white,[u0,v0]));
    return m;
}
const clamp=(v,lo,hi)=>v<lo?lo:v>hi?hi:v;
const sub=(a,b)=>[a[0]-b[0],a[1]-b[1]];
const dot2=(a,b)=>a[0]*b[0]+a[1]*b[1];
const length2=a=>Math.hypot(a[0],a[1]);
const circle=(p,c,r)=>length2(sub(p,c))-r;
function segment(p,a,b,r){
    const pa=sub(p,a),ba=sub(b,a),h=clamp(dot2(pa,ba)/dot2(ba,ba),0,1);
    return length2([pa[0]-ba[0]*h,pa[1]-ba[1]*h])-r;
}
function roundBox(p,c,half,r){
    const q=[Math.abs(p[0]-c[0])-half[0]+r,Math.abs(p[1]-c[1])-half[1]+r];
    return Math.min(Math.max(q[0],q[1]),0)+Math.hypot(Math.max(q[0],0),Math.max(q[1],0))-r;
}
function polygon(p,v){
    let d=dot2(sub(p,v[0]),sub(p,v[0])),sign=1;
    for(let i=0,j=v.length-1;i<v.length;j=i++){
        const e=sub(v[j],v[i]),w=sub(p,v[i]),h=clamp(dot2(w,e)/dot2(e,e),0,1);
        const b=[w[0]-e[0]*h,w[1]-e[1]*h];
        d=Math.min(d,dot2(b,b));
        const inside=p[1]>=v[i][1],below=p[1]<v[j][1],left=e[0]*w[1]>e[1]*w[0];
        if((inside&&below&&left)||(!inside&&!below&&!left))sign=-sign;
    }
    return sign*Math.sqrt(d);
}
// Arrowhead as two strokes: a chevron stays legible where a filled triangle turns to mush.
function arrow(p,from,to,width,cap){
    const d=sub(to,from),len=length2(d),u=[d[0]/len,d[1]/len],side=[-u[1],u[0]],head=Math.min(cap||0.26,len*0.42);
    const wing=s=>[to[0]-head*(u[0]*0.85+side[0]*s*0.66),to[1]-head*(u[1]*0.85+side[1]*s*0.66)];
    return Math.min(segment(p,from,to,width),segment(p,to,wing(1),width),segment(p,to,wing(-1),width));
}
// Glyphs are drawn out to the cell edge and every stroke is a tenth of it wide. Anything
// daintier survives the atlas but not the forty pixels the billboard actually gets.
const RAY=0.10;
const glyphs={
    // Omni: a source disc with an even burst, the one silhouette that reads as "no direction".
    IconPoint(p){
        let d=circle(p,[0,0],0.32);
        for(let i=0;i<8;i++){
            const a=i*Math.PI/4,c=Math.cos(a),s=Math.sin(a);
            d=Math.min(d,segment(p,[c*0.52,s*0.52],[c*0.92,s*0.92],RAY));
        }
        return d;
    },
    // A shade wider than the beam hanging off it. Detached, the two shapes read as a jar and
    // its lid; joined, they are the one lamp everybody draws when they mean a spot.
    IconSpot(p){
        return Math.min(roundBox(p,[0,0.70],[0.54,0.19],0.09),
            polygon(p,[[-0.34,0.56],[0.34,0.56],[0.80,-0.86],[-0.80,-0.86]]));
    },
    // Parallel rays: no source, no falloff, only a direction shared by the whole scene.
    IconDirectional(p){
        const dir=[0.7071,-0.7071],side=[0.7071,0.7071];
        const at=(o,t)=>[side[0]*o+dir[0]*t,side[1]*o+dir[1]*t];
        let d=1e9;
        for(const o of [-0.72,0,0.72])d=Math.min(d,arrow(p,at(o,-0.44),at(o,0.44),RAY*0.95,0.26));
        return d;
    },
    // A panel seen face on, emitting from one side; the aspect says width and height matter.
    IconRect(p){
        let d=roundBox(p,[0,0.56],[0.84,0.26],0.08);
        for(const x of [-0.56,0,0.56])d=Math.min(d,arrow(p,[x,0.16],[x,x?-0.58:-0.88],RAY*0.9,0.22));
        return d;
    },
    // A tube seen side on. Rays leave its length, not its ends: off the ends they close into
    // the silhouette of a bone, and a bone says nothing about where the light goes.
    IconCapsule(p){
        let d=segment(p,[-0.58,0],[0.58,0],0.17);
        for(const x of [-0.30,0.30])for(const s of [-1,1])
            d=Math.min(d,segment(p,[x,s*0.40],[x,s*0.86],RAY*0.9));
        return d;
    }
};
// Glyphs are authored edge to edge, then inset so the bloom around them still fits the cell.
const GLYPH=0.88;
const FILL_TOP=1,FILL_BOTTOM=0.76,OUTLINE=0.05;
const smooth=t=>t<=0?0:t>=1?1:t*t*(3-2*t);
const atlas=Buffer.alloc(CELL*COLS*CELL*ROWS*4);
function put(cell,x,y,r,g,b,a){
    const px=cell[0]*CELL+x,py=cell[1]*CELL+y,offset=(py*CELL*COLS+px)*4;
    atlas[offset]=Math.round(clamp(r,0,1)*255);atlas[offset+1]=Math.round(clamp(g,0,1)*255);
    atlas[offset+2]=Math.round(clamp(b,0,1)*255);atlas[offset+3]=Math.round(clamp(a,0,1)*255);
}
const aa=1.4/CELL;
for(const [name,glyph] of Object.entries(glyphs)){
    const cell=cells[name];
    for(let y=0;y<CELL;y++)for(let x=0;x<CELL;x++){
        const p=[(x+0.5)/CELL*2-1,1-(y+0.5)/CELL*2],d=GLYPH*glyph([p[0]/GLYPH,p[1]/GLYPH]);
        const fill=1-smooth((d+aa)/(2*aa)),edge=1-smooth((d-0.050+aa)/(2*aa));
        // A dark bloom under the glyph is what keeps a warm icon legible on a bright floor.
        const near=Math.max(0,1-Math.max(d,0)/0.15),shade=0.45*near*near;
        const lift=smooth((p[1]+0.9)/1.8);
        const value=OUTLINE+(FILL_BOTTOM+(FILL_TOP-FILL_BOTTOM)*lift-OUTLINE)*fill;
        put(cell,x,y,value,value,value,Math.max(edge,shade));
    }
}
const width=CELL*COLS,height=CELL*ROWS;
const texture=Buffer.alloc(12+atlas.length);
texture.write('TEX1');texture.writeUInt32LE(width,4);texture.writeUInt32LE(height,8);atlas.copy(texture,12);
fs.writeFileSync(path.join(textureFolder,'LightIcons.tex'),texture);
fs.writeFileSync(path.join(textureFolder,'LightIcons.asset'),'ALAS1\n'+JSON.stringify({
    id:createHash('md5').update('HumanEditor/Textures/LightIcons').digest('hex'),type:'Texture',name:'LightIcons',
    version:1,storage:'external',source:'LightIcons.tex',metadata:{colorSpace:'linear'}})+'\n');
// Preview for eyeballing glyph weight without launching the editor; captures/ is not tracked.
// Colour is not its job: the atlas is grey and the material decides the tint.
const preview=path.resolve(root,'../../captures');
if(fs.existsSync(preview)){
    const raw=Buffer.alloc((width*4+1)*height);
    for(let y=0;y<height;y++)atlas.copy(raw,y*(width*4+1)+1,y*width*4,(y+1)*width*4);
    const table=[...Array(256).keys()].map(n=>{for(let k=0;k<8;k++)n=n&1?0xedb88320^(n>>>1):n>>>1;return n>>>0;});
    const crc=b=>{let c=0xffffffff;for(const v of b)c=table[(c^v)&255]^(c>>>8);return (c^0xffffffff)>>>0;};
    const chunk=(type,data)=>{
        const body=Buffer.concat([Buffer.from(type),data]),out=Buffer.alloc(body.length+8);
        out.writeUInt32BE(data.length,0);body.copy(out,4);out.writeUInt32BE(crc(body),body.length+4);return out;
    };
    const header=Buffer.alloc(13);header.writeUInt32BE(width,0);header.writeUInt32BE(height,4);header[8]=8;header[9]=6;
    fs.writeFileSync(path.join(preview,'light-icons.png'),Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]),
        chunk('IHDR',header),chunk('IDAT',zlib.deflateSync(raw)),chunk('IEND',Buffer.alloc(0))]));
}
const meshes={Grid:grid,Wire:rod(1),WireDim:rod(0.8),WireTip:cone(1),WireTipDim:cone(0.8)};
for(const name of Object.keys(cells))meshes[name]=iconQuad(cells[name]);
for(const name of fs.readdirSync(folder))if(!meshes[name.replace(/\.(stm|asset)$/,'')])fs.rmSync(path.join(folder,name));
for(const [name,m] of Object.entries(meshes)){
    const bytes=Buffer.alloc(12+m.vertices.length*4+m.indices.length*4);
    bytes.write('STM1');bytes.writeUInt32LE(m.vertices.length/15,4);bytes.writeUInt32LE(m.indices.length,8);
    let offset=12;for(const v of m.vertices){bytes.writeFloatLE(v,offset);offset+=4;}for(const i of m.indices){bytes.writeUInt32LE(i,offset);offset+=4;}
    fs.writeFileSync(path.join(folder,name+'.stm'),bytes);
    const header={id:createHash('md5').update('HumanEditor/Helper/'+name).digest('hex'),type:'StaticMesh',name,version:1,storage:'external',metadata:{},source:name+'.stm'};
    fs.writeFileSync(path.join(folder,name+'.asset'),'ALAS1\n'+JSON.stringify(header)+'\n');
}
console.log('Helper meshes and light sprite atlas generated.');
