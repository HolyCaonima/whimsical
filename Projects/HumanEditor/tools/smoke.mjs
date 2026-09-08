import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const run=path.join(root,'Saved','Smoke-'+Date.now());
const host=path.join(run,'Host'),target=path.join(run,'Target','Content');
fs.mkdirSync(host,{recursive:true});fs.mkdirSync(target,{recursive:true});
fs.cpSync(path.join(root,'Content'),path.join(host,'Content'),{recursive:true});
fs.cpSync(path.join(root,'Content','Maps'),path.join(target,'Maps'),{recursive:true});
fs.cpSync(path.join(root,'Content','shaders'),path.join(target,'shaders'),{recursive:true});
function read(file){const lines=fs.readFileSync(file,'utf8').split('\n');return {header:JSON.parse(lines[1]),payload:JSON.parse(lines.slice(2).join('\n'))};}
function write(file,header,payload){fs.mkdirSync(path.dirname(file),{recursive:true});fs.writeFileSync(file,'ALAS1\n'+JSON.stringify(header)+'\n'+(header.storage==='external'?'':(typeof payload==='string'?payload:JSON.stringify(payload))+'\n'));}
const project=JSON.parse(fs.readFileSync(path.join(root,'.project'),'utf8'));
project.hostScripts.push('/Game/scripts/smoke');fs.writeFileSync(path.join(host,'.project'),JSON.stringify(project,null,2));
fs.copyFileSync(path.join(root,'Tests','smoke.js'),path.join(host,'Content','scripts','smoke.js'));
write(path.join(host,'Content','scripts','smoke.asset'),{id:'41111111111111111111111111111111',type:'Script',name:'Smoke',version:1,storage:'external',metadata:{},source:'smoke.js'},'');
const settings=read(path.join(host,'Content','Settings.asset'));
settings.payload.project=path.join(run,'Target','.project').replace(/\\/g,'/');settings.payload.smoke=true;
fs.writeFileSync(settings.payload.project,JSON.stringify({version:1,id:'44444444444444444444444444444444',name:'Smoke target',startupMap:'/Game/Maps/Workbench',scripts:[]}));
write(path.join(host,'Content','Settings.asset'),settings.header,settings.payload);
const script={id:'42222222222222222222222222222222',path:'/Game/scripts/simulation'};
write(path.join(target,'scripts','simulation.asset'),{id:script.id,type:'Script',name:'Simulation',version:1,storage:'embedded',metadata:{}},'function initialize(){var d=Engine.sceneData();d.playStarted=true;Engine.setSceneData(d);}');
const map=read(path.join(target,'Maps','Workbench.asset'));map.payload.scripts=[script];write(path.join(target,'Maps','Workbench.asset'),map.header,map.payload);
write(path.join(target,'Props','Architecture','Wall.asset'),{id:'43333333333333333333333333333333',type:'Data',name:'Wall',version:1,storage:'embedded',metadata:{}},{height:3});
fs.writeFileSync(path.join(root,'Saved','smoke-project.txt'),host);
console.log(host);
