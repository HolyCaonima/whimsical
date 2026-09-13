// Constraint Lab shell.
// The shell owns the solver entity, the stage, the camera and the panel frame.
// An experiment owns only its mathematics, its own props and the rows of its panel.
var Lab={experiments:{},order:[],current:'fluid',test:null,
    stiffness:2,mode:'hybrid',paused:false,
    phase:'',tick:0,time:0,gpu:0,invalid:0,singular:0,reads:0,strain:0,drawClock:0,
    total:0,relations:0,variables:0,sample:null,lastSampleTick:0,measuredTick:0,measureJob:null,
    dirty:false,oneStep:false,resetPending:false,error:'',
    owner:0,entities:[],stageKey:''};
Lab.X=Engine.dynamics;Lab.S=Lab.X.scene;Lab.modelId=Engine.content.newId();
Lab.camera={target:[0,2.7,0],yaw:0.28,pitch:0.23,distance:14,fov:0.65};
Lab.add=function(test){Lab.experiments[test.id]=test;Lab.order.push(test.id);if(!Lab.current)Lab.current=test.id;};

// A small library of parameterised relations. Experiments reach for these; they do not redefine them.
Lab.definitions=function(){
    var X=Lab.X,space=Lab.space=X.space(3),scalar=Lab.scalar=X.space(1);
    Lab.distance=X.defineRelation({name:'distance',objects:[{position:space},{position:space}],parameters:1},function(op,a,b){
        return {residual:[op.sub(op.length(op.vsub(a.position,b.position)),op.parameter(0))]};
    });
    Lab.floor=X.defineRelation({name:'ground half space',objects:[{position:space},{height:scalar}],
        parameters:1,kind:'greaterEqual'},function(op,a,b){
        return {residual:[op.sub(op.sub(a.position[1],b.height[0]),op.parameter(0))]};
    });
    Lab.sphere=X.defineRelation({name:'sphere exterior',objects:[{position:space},{position:space,radius:scalar}],
        parameters:1,kind:'greaterEqual'},function(op,a,b){
        return {residual:[op.sub(op.length(op.vsub(a.position,b.position)),op.add(b.radius[0],op.parameter(0)))]};
    });
    Lab.damping=X.defineRelation({name:'displacement resistance',objects:[{position:space},{}],rows:3,history:3},function(op,a,b){
        return {residual:op.vsub(a.position,[op.state(0),op.state(1),op.state(2)]),update:a.position};
    });
    Lab.order.forEach(function(id){if(Lab.experiments[id].define)Lab.experiments[id].define();});
};
// The stage is one explicit, stationary object exposing several named DOFs.
Lab.environment=function(model){
    var X=Lab.X;
    var centre=X.defineDofs(model,{name:'obstacle centre',space:Lab.space,count:1,initial:[0,1.8,0],readOnly:true});
    var dimensions=X.defineDofs(model,{name:'stage dimensions',space:Lab.scalar,count:2,initial:[0,1.1],readOnly:true});
    return X.defineObject(model,{name:'stage',kind:'single',dofs:{
        position:centre,height:X.dof(dimensions,0),radius:X.dof(dimensions,1)}});
};
Lab.compliance=function(){return [0.0005,0.00001,0.0000001][Lab.stiffness];};

Lab.reset=function(){Lab.resetPending=true;};
Lab.select=function(id){if(Lab.current!==id){Lab.current=id;Lab.reset();}};
Lab.rebuild=function(){
    var X=Lab.X,test=Lab.test=Lab.experiments[Lab.current];
    if(Lab.owner)Engine.destroy(Lab.owner);
    Lab.tick=0;Lab.time=0;Lab.gpu=0;Lab.invalid=0;Lab.singular=0;Lab.reads=0;Lab.strain=0;
    Lab.lastSampleTick=0;Lab.measuredTick=0;Lab.drawClock=0;Lab.measureJob=null;
    Lab.dirty=false;Lab.oneStep=false;Lab.resetPending=false;Lab.error='';
    var handle=X.model();
    Lab.sample=test.build(handle);
    Lab.owner=Engine.create({id:Lab.modelId,name:test.title,persistent:false,components:{dynamics:{
        model:X.describe(handle),
        policy:{mode:Lab.mode,substeps:4,iterations:12,colorBudget:12},
        stepTime:1/30,paused:Lab.paused,
        // Explicit CPU observation for the strain meter. Rendering bindings declare their own demand.
        observe:[{set:Lab.variables,first:0,count:Lab.total}]
    }}});
    X.destroy(handle);
    Lab.stage();
    Lab.phase='compile';
    Engine.log('LAB '+test.id+': '+Lab.total+' variables / '+Lab.relations+' relations; '+Lab.mode);
};
// Props survive a rebuild whenever the layout is unchanged; bindings address the model by persistent id.
Lab.stage=function(){
    var key=Lab.current+'/'+Lab.test.layout();
    if(key===Lab.stageKey){Lab.test.restage();return;}
    Lab.entities.forEach(function(id){Engine.destroy(id);});Lab.entities=[];Lab.stageKey=key;
    Lab.test.stage();
    Lab.camera=Lab.test.camera();Engine.view.set({camera:Lab.camera});
};
Lab.pump=function(){
    if(Lab.resetPending)Lab.rebuild();
    if(!Lab.owner)return;
    var result=Lab.S.state(Lab.owner);
    Lab.tick=result.tick;Lab.time=result.time;Lab.gpu=result.gpuMilliseconds;
    Lab.invalid=result.invalidEvaluations;Lab.singular=result.singularSystems;
    Lab.phase=result.ready?'':'compile';
    if(result.error){if(!Lab.error)Engine.log('LAB ERROR: '+result.error);Lab.error=result.error;Lab.paused=true;return;}
    if(result.sampleTick&&result.sampleTick!==Lab.lastSampleTick){
        Lab.lastSampleTick=result.sampleTick;
        Lab.sample=Lab.S.values(Lab.owner,Lab.variables,0,Lab.total);++Lab.reads;
        if(Lab.reads===1)Engine.log('LAB LIVE: ECS sample received; diagnostics='+Lab.invalid+'/'+Lab.singular);
    }
    if(Lab.dirty){Lab.test.apply();Lab.dirty=false;}
    var writes=[],step=Lab.test.inputs(writes)||Lab.oneStep;
    writes.forEach(function(w){Lab.S.write(Lab.owner,w.field,w.set,w.first,w.values);});
    Lab.S.control(Lab.owner,Lab.paused,step);Lab.oneStep=false;
};
Lab.draw=function(dt){
    Lab.drawClock=Math.min(0.1,Lab.drawClock+dt);
    // Expensive display-only diagnostics can consume one immutable sample in
    // bounded chunks. Publish only a finished result; resets discard the job.
    if(Lab.measureJob){
        var strain=Lab.measureJob.step();
        if(strain!==undefined){Lab.strain=strain;Lab.measureJob=null;}
        return;
    }
    if(Lab.lastSampleTick===Lab.measuredTick||Lab.drawClock<0.1)return;
    Lab.drawClock=0;Lab.measuredTick=Lab.lastSampleTick;
    if(Lab.test.beginMeasure)Lab.measureJob=Lab.test.beginMeasure(Lab.sample);
    else Lab.strain=Lab.test.measure(Lab.sample);
};
Lab.input=function(input){
    if(input.pointerCaptured||!Lab.test)return;
    var moved=false;
    if(input.middle){Lab.camera.yaw-=input.dx*0.006;Lab.camera.pitch=Math.max(-0.1,Math.min(1.35,Lab.camera.pitch+input.dy*0.005));moved=true;}
    if(input.wheel){var maxDistance=Math.min(120,Lab.test.camera().distance*2);
        Lab.camera.distance=Math.max(6,Math.min(maxDistance,Lab.camera.distance*Math.pow(0.9,input.wheel)));moved=true;}
    if(moved)Engine.view.set({camera:Lab.camera});
};

// --- Stage ---------------------------------------------------------------
Lab.assets=function(){
    Lab.boxMesh=Engine.asset('/Engine/Meshes/Box');Lab.rodMesh=Engine.asset('/Game/Models/Rod');
    Lab.sphereMesh=Engine.asset('/Game/Models/Sphere');Lab.standMesh=Engine.asset('/Game/Models/Stand');
    Lab.materials={};
    ['Node','Edge','Anchor','Floor','Grid','Frame','Obstacle','Cloth'].forEach(function(n){Lab.materials[n]=Engine.asset('/Game/Materials/'+n);});
};
Lab.mesh=function(name,p,s,material,mesh){
    var id=Engine.create({name:name,persistent:false,components:{transform:{position:p,scale:s},render:{mesh:mesh||Lab.boxMesh,material:Lab.materials[material]}}});
    Lab.entities.push(id);return id;
};
// The shared exhibition stand: slab, floor grid, obstacle sphere and its pedestal.
Lab.platform=function(){
    Lab.mesh('Platform',[0,-0.18,0],[9,0.35,6.5],'Floor');
    for(var k=-4;k<=4;++k)Lab.mesh('Floor line',[k,0,0],[0.012,0.012,6.5],'Grid');
    for(k=-3;k<=3;++k)Lab.mesh('Floor line',[0,0,k],[9,0.012,0.012],'Grid');
    Lab.mesh('Collision sphere',[0,1.8,0],[1.1,1.1,1.1],'Obstacle',Lab.sphereMesh);
    Lab.mesh('Sphere pedestal',[0,0.35,0],[0.8,0.7,0.8],'Frame');
};
Lab.lights=function(scale){
    Lab.entities.push(Engine.light(0,9*scale,scale,0.4*scale,0.82,0.92,1,90*scale*scale));
    Lab.entities.push(Engine.light(-7*scale,5*scale,-4*scale,0.3*scale,0.3,0.75,1,55*scale*scale));
    Lab.entities.push(Engine.light(6*scale,6*scale,6*scale,0.3*scale,1,0.65,0.34,55*scale*scale));
};

// --- Pose mappings -------------------------------------------------------
Lab.normalize=function(e,v){return e.scale(v,e.div(1,e.max(e.length(v),0.000001)));};
// Shortest arc from the mesh +Y axis onto a unit direction.
Lab.swing=function(e,n){
    var w=e.sqrt(e.max(0,e.mul(e.add(1,n[1]),0.5))),flip=e.less(w,0.00001),safe=e.max(w,0.00001);
    return [e.select(flip,0,w),e.select(flip,1,e.div(n[2],e.mul(2,safe))),0,e.select(flip,0,e.neg(e.div(n[0],e.mul(2,safe))))];
};
Lab.qmul=function(e,a,b){
    return [e.sub(e.sub(e.mul(a[0],b[0]),e.mul(a[1],b[1])),e.add(e.mul(a[2],b[2]),e.mul(a[3],b[3]))),
            e.add(e.add(e.mul(a[0],b[1]),e.mul(a[1],b[0])),e.sub(e.mul(a[2],b[3]),e.mul(a[3],b[2]))),
            e.add(e.sub(e.mul(a[0],b[2]),e.mul(a[1],b[3])),e.add(e.mul(a[2],b[0]),e.mul(a[3],b[1]))),
            e.add(e.add(e.mul(a[0],b[3]),e.mul(a[1],b[2])),e.sub(e.mul(a[3],b[0]),e.mul(a[2],b[1])))];
};
// Swing +Y onto the normal, then twist about it until +X meets the tangent.
Lab.frame=function(e,tangent,normal){
    var swing=Lab.swing(e,normal),x=swing[1],z=swing[3],w=swing[0];
    var swung=[e.sub(1,e.mul(2,e.mul(z,z))),e.mul(2,e.mul(w,z)),e.mul(2,e.mul(x,z))];
    var cosine=e.add(1,e.dot(swung,tangent)),sine=e.dot(e.cross(swung,tangent),normal);
    var scale=e.div(1,e.max(e.sqrt(e.add(e.mul(cosine,cosine),e.mul(sine,sine))),0.000001));
    var twist=[e.mul(cosine,scale),e.mul(e.mul(normal[0],sine),scale),
               e.mul(e.mul(normal[1],sine),scale),e.mul(e.mul(normal[2],sine),scale)];
    return Lab.qmul(e,twist,swing);
};
Lab.pointMapping=function(ox,oz,size){
    var e=Lab.X.expression(3);
    return e.finish([e.add(e.input(0),ox),e.input(1),e.add(e.input(2),oz),1,0,0,0,size,size,size]);
};
Lab.bindPoint=function(id,index,ox,oz,size){
    Engine.addComponent(id,'dynamicsBinding',{model:Lab.modelId,direction:'output',interpolation:0.07,variables:[{set:Lab.variables,index:index}],
        mapping:Lab.pointMapping(ox,oz,size)});
};
Lab.spanMapping=function(ox,oz,radius){
    var e=Lab.X.expression(6),p=[e.input(0),e.input(1),e.input(2)],q=[e.input(3),e.input(4),e.input(5)],d=e.vsub(q,p);
    var length=e.max(e.length(d),0.000001),rotation=Lab.swing(e,e.scale(d,e.div(1,length))),mid=e.scale(e.vadd(p,q),0.5);
    return e.finish([e.add(mid[0],ox),mid[1],e.add(mid[2],oz),rotation[0],rotation[1],rotation[2],rotation[3],radius,length,radius]);
};
Lab.bindSpan=function(id,a,b,ox,oz,radius){
    Engine.addComponent(id,'dynamicsBinding',{model:Lab.modelId,direction:'output',interpolation:0.07,variables:[{set:Lab.variables,index:a},{set:Lab.variables,index:b}],
        mapping:Lab.spanMapping(ox,oz,radius)});
};
// Four corner variables become one oriented plate; its extent follows the deforming cell.
Lab.patchMapping=function(thickness,overlap){
    var e=Lab.X.expression(12),corner=[];
    for(var i=0;i<4;++i)corner.push([e.input(i*3),e.input(i*3+1),e.input(i*3+2)]);
    var centre=e.scale(e.vadd(e.vadd(corner[0],corner[1]),e.vadd(corner[2],corner[3])),0.25);
    var u=e.scale(e.vsub(e.vadd(corner[1],corner[3]),e.vadd(corner[0],corner[2])),0.5);
    var v=e.scale(e.vsub(e.vadd(corner[2],corner[3]),e.vadd(corner[0],corner[1])),0.5);
    var tangent=Lab.normalize(e,u),normal=Lab.normalize(e,e.cross(v,u)),across=e.cross(tangent,normal);
    // A sheared cell is a parallelogram; the plate takes its extent in the plate's own axes.
    var width=e.add(e.length(u),e.abs(e.dot(v,tangent))),depth=e.abs(e.dot(v,across));
    var rotation=Lab.frame(e,tangent,normal);
    return e.finish([centre[0],centre[1],centre[2],rotation[0],rotation[1],rotation[2],rotation[3],
        e.mul(width,overlap),thickness,e.mul(depth,overlap)]);
};
Lab.bindPatch=function(id,a,b,c,d,thickness,overlap){
    Engine.addComponent(id,'dynamicsBinding',{model:Lab.modelId,direction:'output',interpolation:0.05,
        variables:[{set:Lab.variables,index:a},{set:Lab.variables,index:b},{set:Lab.variables,index:c},{set:Lab.variables,index:d}],
        mapping:Lab.patchMapping(thickness,overlap)});
};

// A binding stream has a fixed, unsigned stride. Split only where that stride changes.
Lab.instanceBatches=function(name,mesh,material,rows,mapping,scale,interpolation){
    var first=0,batch=0,q=Lab.sample;
    while(first<rows.length){
        var start=rows[first],strides=start.map(function(){return 0;}),end=first+1;
        if(end<rows.length){
            var next=rows[end],candidate=start.map(function(value,j){return next[j]-value;});
            if(candidate.every(function(value){return value>=0;})){
                strides=candidate;++end;
                while(end<rows.length&&rows[end].every(function(value,j){
                    return value===start[j]+(end-first)*strides[j];
                }))++end;
            }
        }
        var transforms=[];
        for(var at=first;at<end;++at){
            var centre=[0,0,0],nodes=rows[at];
            nodes.forEach(function(node){
                for(var axis=0;axis<3;++axis)centre[axis]+=q[node*3+axis]/nodes.length;
            });
            transforms.push({position:centre,scale:scale});
        }
        var entity=Engine.create({name:name+' batch '+batch++,persistent:false,components:{
            transform:{},render:{mesh:mesh,material:Lab.materials[material],
                instanceCount:end-first,instanceTransforms:transforms},
            dynamicsBinding:{model:Lab.modelId,direction:'output',target:'renderInstances',
                interpolation:interpolation,variables:start.map(function(index,j){
                    return {set:Lab.variables,index:index,stride:strides[j]};
                }),mapping:mapping}
        }});
        Lab.entities.push(entity);first=end;
    }
};
