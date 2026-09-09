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
for(const name of ['editor','viewport','gizmo','helpers','panels','inspector','browser','main'])asset('scripts/'+name,'Script','',{storage:'external',source:name+'.js'});
asset('Picking','RenderTarget',{width:0,height:0,format:'R32Uint'});
const shader=asset('shaders/Standard','Shader',`SurfaceData s = DefaultSurface(ctx);
s.albedo = properties.baseColor;
s.roughness = properties.roughness;
s.metallic = properties.metallic;
s.emission = properties.emission;
return s;`,{metadata:{materialModel:'metallicRoughness',properties:[{name:'baseColor',type:'vec3',default:[0.5,0.5,0.5]},{name:'roughness',type:'float',default:0.5},{name:'emission',type:'vec3',default:[0,0,0]},{name:'metallic',type:'float',default:0}],textures:[]}});
const material=(color,roughness=0.5,metallic=0)=>({shader,properties:{baseColor:color,roughness,metallic,emission:[0,0,0]},textures:{}});
const gizmoShader=asset('shaders/Gizmo','Shader',`SurfaceData s = DefaultSurface(ctx);
s.albedo = properties.baseColor * ctx.vertexColor;
return s;`,{metadata:{materialModel:'metallicRoughness',properties:[{name:'baseColor',type:'vec3',default:[1,1,1]}],textures:[]}});
for(const [name,color] of Object.entries({Gizmo:[1,1,1],GizmoX:[.94,.19,.16],GizmoY:[.30,.86,.20],GizmoZ:[.18,.42,1],GizmoHighlight:[1,.87,.1]}))
  asset('Materials/'+name,'Material',{shader:gizmoShader,properties:{baseColor:color},textures:{},renderState:{domain:'display',layer:1,rayVisible:false,depthTest:true,depthWrite:true}});
asset('Materials/HelperGrid','Material',{shader:gizmoShader,properties:{baseColor:[1,1,1]},textures:{},renderState:{domain:'display',layer:0,cull:'none',rayVisible:false,depthTest:true,depthWrite:false}});
// Light wires only exist while a light is selected, so they wear the selection colour from
// HE.syncOutlines rather than a warm hue of their own, which also keeps them off the warm
// floors they are usually drawn over.
asset('Materials/HelperLight','Material',{shader:gizmoShader,properties:{baseColor:[0.42,0.96,0.82]},textures:{},renderState:{domain:'display',layer:0,cull:'none',rayVisible:false,depthTest:true,depthWrite:false}});
// Light billboards are sprites, as they are in every editor that has to show a light with no
// geometry: masked so the picking pass answers for the glyph, blended so its edges stay smooth.
const iconShader=asset('shaders/Icon','Shader',`vec4 sprite = SampleTexture(ctx, textures.sprite, ctx.uv);
SurfaceData s = DefaultSurface(ctx);
s.albedo = sprite.rgb * properties.tint * ctx.vertexColor;
s.opacity = sprite.a * properties.opacity;
return s;`,{metadata:{materialModel:'metallicRoughness',properties:[{name:'tint',type:'vec3',default:[1,1,1]},{name:'opacity',type:'float',default:1}],textures:['sprite']}});
const external=name=>({id:JSON.parse(fs.readFileSync(path.join(root,'Content',name+'.asset'),'utf8').split('\n')[1]).id,path:'/Game/'+name});
const sprite=(name,tint,opacity,layer,cutoff)=>asset('Materials/'+name,'Material',{shader:iconShader,
  properties:{tint,opacity},textures:{sprite:external('Textures/LightIcons')},
  renderState:{domain:'display',layer,cull:'none',surface:'masked',alphaCutoff:cutoff,blend:'alpha',rayVisible:false,depthTest:true,depthWrite:false}});
// The atlas is grey, so selection is a tint swap rather than another sprite stacked behind:
// the icon itself goes white, which is legible on every background the amber has to survive.
const AMBER=[1,0.74,0.30],WHITE=[1,1,1];
// Depth tells you where a light sits, but a half eaten sprite tells you nothing, so the icon is
// drawn twice: solid where it is visible and faint on the overlay layer where geometry covers it.
sprite('HelperIcon',AMBER,1,0,0.06);
sprite('HelperIconGhost',AMBER,0.3,1,0.02);
sprite('HelperIconActive',WHITE,1,0,0.06);
sprite('HelperIconActiveGhost',WHITE,0.45,1,0.02);
const surface=(name,color,roughness=0.5,metallic=0)=>{const path='Materials/Workbench/'+name;return asset(path,'Material',material(color,roughness,metallic));};
const ground=surface('Ground',[0.18,0.20,0.23]),cube=surface('Cube',[0.65,0.32,0.12],0.3,0.25),capsule=surface('Capsule',[0.15,0.35,0.55],0.25,0.4);
const mesh=name=>({id:JSON.parse(fs.readFileSync(path.join(root,'../../engine/Content/Meshes',name+'.asset'),'utf8').split('\n')[1]).id,path:'/Engine/Meshes/'+name});
const entities=[];
function entity(name,components){entities.push({id:id(name),name,enabled:true,components});}
entity('Ground',{transform:{position:[0,-0.2,0],scale:[12,0.3,12]},render:{mesh:mesh('Box'),material:ground}});
entity('Cube',{transform:{position:[0,0.65,0],scale:[1.3,1.3,1.3]},render:{mesh:mesh('Box'),material:cube}});
entity('Capsule',{transform:{position:[2,1,0],scale:[0.7,1.2,0.7]},render:{mesh:mesh('Capsule'),material:capsule}});
entity('Key light',{transform:{position:[1,5,3]},light:{type:'point',intensity:1500,color:[1,0.85,0.65],radius:0.35}});
entity('Fill light',{transform:{position:[-4,3,-2]},light:{type:'point',intensity:700,color:[0.45,0.65,1],radius:0.5}});
asset('Maps/Workbench','Map',{version:11,entities,camera:{target:[0,0.7,0],yaw:0.5,pitch:0.45,distance:13,fov:0.62},navigation:{min:[-20,-5,-20],max:[20,20,20],cellSize:0.5,planeTolerance:0.2},scripts:[],references:{},data:{}});
asset('Settings','Data',{project:'Projects/Afterlight/.project'});
console.log('HumanEditor assets ready.');

