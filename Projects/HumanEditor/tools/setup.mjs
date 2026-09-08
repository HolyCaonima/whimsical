// Rebuild only HumanEditor's small, source-owned asset envelopes and demo map.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const id=s=>createHash('md5').update('HumanEditor/'+s).digest('hex');
function asset(name,type,payload,extra={}){
  const file=path.join(root,'Content',name+'.asset');fs.mkdirSync(path.dirname(file),{recursive:true});
  const existing=fs.existsSync(file)?JSON.parse(fs.readFileSync(file,'utf8').split('\n')[1]):null;
  const header={id:existing?existing.id:id(name),type,name:path.basename(name),version:1,storage:'embedded',metadata:{},...extra};
  fs.writeFileSync(file,'ALAS1\n'+JSON.stringify(header)+'\n'+(header.storage==='external'?'':(typeof payload==='string'?payload:JSON.stringify(payload,null,2))+'\n'));
  return {id:header.id,path:'/Game/'+name};
}
for(const name of ['editor','viewport','gizmo','panels','inspector','browser','main'])asset('scripts/'+name,'Script','',{storage:'external',source:name+'.js'});
asset('Picking','RenderTarget',{width:0,height:0,format:'R32Uint'});
const shader=asset('shaders/Standard','Shader',`SurfaceData s = DefaultSurface(ctx);
s.albedo = properties.baseColor;
s.roughness = properties.roughness;
s.metallic = properties.metallic;
s.emission = properties.emission;
return s;`,{metadata:{materialModel:'metallicRoughness',properties:[{name:'baseColor',type:'vec3',default:[0.5,0.5,0.5]},{name:'roughness',type:'float',default:0.5},{name:'emission',type:'vec3',default:[0,0,0]},{name:'metallic',type:'float',default:0}],textures:[],renderState:{surface:'opaque',cull:'none',alphaCutoff:0.5}}});
const material=(color,roughness=0.5,metallic=0)=>({shader,properties:{baseColor:color,roughness,metallic,emission:[0,0,0]},textures:{}});
asset('Materials/Gizmo','Material',{shader,properties:{},textures:{}});
const surface=(name,color,roughness=0.5,metallic=0)=>{const path='Materials/Workbench/'+name;return asset(path,'Material',material(color,roughness,metallic));};
const ground=surface('Ground',[0.18,0.20,0.23]),cube=surface('Cube',[0.65,0.32,0.12],0.3,0.25),capsule=surface('Capsule',[0.15,0.35,0.55],0.25,0.4);
const entities=[];
function entity(name,components){entities.push({id:id(name),name,enabled:true,components});}
entity('Ground',{transform:{position:[0,-0.2,0]},render:{shape:'box',scale:[12,0.3,12],material:ground}});
entity('Cube',{transform:{position:[0,0.65,0]},render:{shape:'box',scale:[1.3,1.3,1.3],material:cube}});
entity('Capsule',{transform:{position:[2,1,0]},render:{shape:'capsule',scale:[0.7,1.2,0.7],material:capsule}});
entity('Key light',{transform:{position:[1,5,3]},light:{type:'point',intensity:1500,color:[1,0.85,0.65],radius:0.35}});
entity('Fill light',{transform:{position:[-4,3,-2]},light:{type:'point',intensity:700,color:[0.45,0.65,1],radius:0.5}});
asset('Maps/Workbench','Map',{version:9,entities,camera:{target:[0,0.7,0],yaw:0.5,pitch:0.45,distance:13,fov:0.62},navigation:{min:[-20,-5,-20],max:[20,20,20],cellSize:0.5,planeTolerance:0.2},scripts:[],references:{},data:{}});
asset('Settings','Data',{project:'Projects/Afterlight/.project'});
console.log('HumanEditor assets ready.');

