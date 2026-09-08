// Rebuild only HumanEditor's small, source-owned asset envelopes and demo map.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const id=s=>createHash('md5').update('HumanEditor/'+s).digest('hex');
function asset(name,type,payload,extra={}){
  const file=path.join(root,'Content',name+'.asset');fs.mkdirSync(path.dirname(file),{recursive:true});
  const header={id:id(name),type,name:path.basename(name),version:1,storage:'embedded',metadata:{},...extra};
  fs.writeFileSync(file,'ALAS1\n'+JSON.stringify(header)+'\n'+(header.storage==='external'?'':(typeof payload==='string'?payload:JSON.stringify(payload,null,2))+'\n'));
}
for(const name of ['editor','viewport','gizmo','panels','browser','main'])asset('scripts/'+name,'Script','',{storage:'external',source:name+'.js'});
asset('Picking','RenderTarget',{width:0,height:0,format:'R32Uint'});
asset('shaders/Standard','Shader',`SurfaceData s = DefaultSurface(ctx);
s.albedo = properties.baseColor;
s.roughness = properties.roughness;
s.metallic = properties.metallic;
s.emission = properties.emission;
return s;`,{metadata:{materialModel:'metallicRoughness',properties:[{name:'baseColor',type:'vec3',default:[0.5,0.5,0.5]},{name:'roughness',type:'float',default:0.5},{name:'emission',type:'vec3',default:[0,0,0]},{name:'metallic',type:'float',default:0}],textures:[],renderState:{surface:'opaque',cull:'none',alphaCutoff:0.5}}});
const shader={id:id('shaders/Standard'),path:'/Game/shaders/Standard'};
const material=(color,roughness=0.5,metallic=0)=>({shader,properties:{baseColor:color,roughness,metallic,emission:[0,0,0]},textures:{}});
const entities=[];
function entity(name,components){entities.push({id:id(name),name,enabled:true,components});}
entity('Ground',{transform:{position:[0,-0.2,0]},render:{shape:'box',scale:[12,0.3,12],material:0}});
entity('Cube',{transform:{position:[0,0.65,0]},render:{shape:'box',scale:[1.3,1.3,1.3],material:1}});
entity('Capsule',{transform:{position:[2,1,0]},render:{shape:'capsule',scale:[0.7,1.2,0.7],material:2}});
entity('Key light',{transform:{position:[1,5,3]},light:{type:'point',intensity:1500,color:[1,0.85,0.65],radius:0.35}});
entity('Fill light',{transform:{position:[-4,3,-2]},light:{type:'point',intensity:700,color:[0.45,0.65,1],radius:0.5}});
asset('Maps/Workbench','Map',{version:7,entities,materials:[material([0.18,0.20,0.23]),material([0.65,0.32,0.12],0.3,0.25),material([0.15,0.35,0.55],0.25,0.4)],materialAssets:[],camera:{target:[0,0.7,0],yaw:0.5,pitch:0.45,distance:13,fov:0.62},navigation:{min:[-20,-5,-20],max:[20,20,20],cellSize:0.5,planeTolerance:0.2},scripts:[],player:'',references:{},data:{}});
asset('Settings','Data',{project:'Projects/Afterlight/.project'});
console.log('HumanEditor assets ready.');

