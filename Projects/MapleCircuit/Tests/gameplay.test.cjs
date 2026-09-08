// Project rules and controls in V8. Native collision/rendering are verified separately in Verification.asset.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const project = path.resolve(__dirname, '..');
const map = JSON.parse(fs.readFileSync(path.join(project, 'Content/Maps/MapleCircuit.asset'), 'utf8').split('\n').slice(2).join('\n'));

function game() {
    const objects = new Map(), nodes = new Map(), logs = [];
    const clone = x => JSON.parse(JSON.stringify(x));
    for (const o of map.entities) {
        const t=o.components.transform;
        objects.set(o.name, {position:{x:t.position[0],y:t.position[1],z:t.position[2]},
            rotation:{x:t.rotation[0],y:t.rotation[1],z:t.rotation[2],w:t.rotation[3]},enabled:o.enabled});
    }
    function node(id) {
        if (!nodes.has(id)) nodes.set(id, {text:'',properties:{},setText(v){this.text=v;},setProperty(k,v){this.properties[k]=v;},
            setClass(){},blur(){this.blurred=true;},setInnerRML(v){this.rml=v;},on(event,fn){this[event]=fn;}});
        return nodes.get(id);
    }
    const context = vm.createContext({console, Engine: {
        sceneData:()=>clone(map.data), sceneObject:name=>name,
        transform(id, pose){Object.assign(objects.get(id), clone(pose));},
        localTransform(id, pose){objects.get(id).local=clone(pose);},
        enabled(id, enabled){objects.get(id).enabled=enabled;},
        visualPose(id, offset, scale){objects.get(id).scale=clone(scale);},
        moveBody(id, delta, rotation){const o=objects.get(id);o.position.x+=delta.x;o.position.y+=delta.y;o.position.z+=delta.z;
            o.rotation=clone(rotation);return {position:clone(o.position),rotation:clone(rotation),applied:clone(delta),blocked:false,contacts:[]};},
        camera(){},log:text=>logs.push(text),
        ui:{loadDocument:()=>({show(){},getElementById:node})}
    }});
    for(const script of ['track','vehicle','race','hud','main']) vm.runInContext(
        fs.readFileSync(path.join(project,'Content/scripts',script+'.js'),'utf8'),context,{filename:script+'.js'});
    context.initialize();
    context.Race.ready=false;
    return {g:context,objects,nodes,logs};
}
function input(keys=[], pressed=[]) {const k=[],p=[];keys.forEach(x=>k[x]=true);pressed.forEach(x=>p[x]=true);return {keys:k,pressed:p,keyboardCaptured:false};}
function step(g, seconds, controls=input()) {for(let i=0;i<Math.round(seconds*60);i++)g.fixedUpdate(1/60,controls);}
const tests=[];
function test(name, fn){tests.push([name,fn]);}

test('countdown locks all cars; pause freezes the race and timers',()=>{
    const {g}=game(),start=g.Race.cars.map(c=>[c.x,c.z]);step(g,2,input([87]));
    g.Race.cars.forEach((c,i)=>assert.deepEqual([c.x,c.z],Array.from(start[i])));
    g.fixedUpdate(1/60,input([],[27]));const countdown=g.Race.countdown;step(g,4,input([87]));
    assert.equal(g.Race.countdown,countdown);assert.equal(g.Race.elapsed,0);
});
test('the grid waits for the player while shaders and rendering initialize',()=>{
    const {g,nodes}=game();g.Race.reset(true);step(g,10);assert.equal(g.Race.countdown,3);assert.equal(g.Race.elapsed,0);
    nodes.get('continue').click();step(g,1);assert.ok(g.Race.countdown<2.1);
});
test('W accelerates, S brakes, and captured keyboard never accelerates',()=>{
    const {g}=game();g.Race.countdown=0;step(g,1,input([87]));assert.ok(g.Race.cars[0].speed>10);
    step(g,.5,input([83]));assert.ok(g.Race.cars[0].speed<1);
    g.Race.reset();g.Race.countdown=0;const captured=input([87]);captured.keyboardCaptured=true;step(g,1,captured);
    assert.equal(g.Race.cars[0].speed,0);
});
test('A and D steer in opposite chase-camera directions',()=>{
    const a=game().g,b=game().g;a.Race.countdown=b.Race.countdown=0;
    step(a,.8,input([87]));step(b,.8,input([87]));const ya=a.Race.cars[0].yaw,yb=b.Race.cars[0].yaw;
    step(a,.25,input([87,65]));step(b,.25,input([87,68]));
    assert.ok(a.MathUtil.angle(a.Race.cars[0].yaw-ya)>0);assert.ok(b.MathUtil.angle(b.Race.cars[0].yaw-yb)<0);
});
test('reverse driving and skipped sectors cannot award a lap',()=>{
    const {g}=game(),c=g.Race.cars[0],L=g.Track.length;g.Race.countdown=0;
    c.track.s=1;c.track.distance=0;g.Race.checkpoint(c,L-1);assert.equal(c.started,true);assert.equal(c.completed,0);
    c.track.s=L-1;g.Race.checkpoint(c,1);assert.equal(c.completed,0);
    c.track.s=1;g.Race.checkpoint(c,L-1);assert.equal(c.completed,0);assert.equal(c.nextGate,1);
    c.track.s=L/16+1;c.track.distance=20;g.Race.checkpoint(c,L/16-1);assert.equal(c.nextGate,1);
});
test('exact segment endpoints and the length/zero seam never drop checkpoints',()=>{
    const {g}=game(),L=g.Track.length;
    assert.equal(g.Track.crossed(L,.1,0),true);
    assert.equal(g.Track.crossed(L/4,L/4+.1,L/4),true);
    assert.equal(g.Track.crossed(L/4-.1,L/4,L/4),true);
    assert.equal(g.Track.crossed(.1,L,0),false);
});
test('ordered checkpoints award exactly three laps and stable finish order',()=>{
    const {g}=game(),c=g.Race.cars[0],L=g.Track.length;c.track.distance=0;
    function cross(gate){const s=gate*L/16;g.Race.elapsed+=1;c.track.s=g.MathUtil.wrap(s+.1,L);g.Race.checkpoint(c,g.MathUtil.wrap(s-.1,L));}
    cross(0);for(let lap=0;lap<3;lap++){for(let gate=1;gate<16;gate++)cross(gate);cross(0);}
    assert.equal(c.completed,3);assert.equal(c.gatesPassed,49);assert.equal(g.Race.finished,true);assert.equal(c.lastLap,16);
    cross(0);assert.equal(g.Race.finishOrder.length,1);
});
test('opaque smoke expires, object pool stays bounded, restart clears effects and results',()=>{
    const {g,objects,nodes}=game();g.Race.countdown=0;step(g,.8,input([87]));step(g,.5,input([87,32,65]));
    assert.ok(g.Effects.emitted>0);assert.ok(g.Effects.smoke.some(p=>p.life>0));
    const count=objects.size;g.Race.cars[0].drifting=false;for(let i=0;i<70;i++)g.Effects.update(g.Race.cars[0],1/60);
    assert.ok(g.Effects.smoke.every(p=>p.life<=0));assert.equal(objects.size,count);
    nodes.get('restart').click();assert.equal(g.Race.countdown,3);assert.equal(g.Race.elapsed,0);
    assert.ok(g.Effects.skids.every(id=>objects.get(id).enabled===false));
});
test('AI can complete all ordered sectors using the same vehicle integration',()=>{
    const {g}=game();g.Race.autopilot=true;
    for(let i=0;i<60*100&&!g.Race.finished;i++)g.fixedUpdate(1/60,input());
    assert.equal(g.Race.cars[0].completed,3);
    assert.ok(g.Race.cars[0].distanceDriven>g.Track.length*2.85);
});
test('HUD restart, AI toggle and pause resume buttons change real gameplay state',()=>{
    const {g,nodes}=game();nodes.get('watch').click();assert.equal(g.Race.autopilot,true);
    g.Race.paused=true;g.updateUI(0);assert.equal(nodes.get('overlay').properties.display,'block');
    nodes.get('continue').click();assert.equal(g.Race.paused,false);g.updateUI(0);assert.equal(nodes.get('overlay').properties.display,'none');
    assert.equal(nodes.get('continue').blurred,true);assert.equal(nodes.get('watch').blurred,true);
});

module.exports={game,input,step};
if(require.main===module){
    let failed=0;
    for(const [name,fn]of tests){try{fn();console.log('PASS '+name);}catch(e){failed++;console.error('FAIL '+name+'\n'+e.stack);}}
    if(failed)process.exitCode=1;
    else console.log('PASS: '+tests.length+' project gameplay tests. Collision and Vulkan checks are separate native runs.');
}
