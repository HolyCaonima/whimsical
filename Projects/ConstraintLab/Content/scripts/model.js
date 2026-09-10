// Project-owned rope: R3 variables and mathematical relations only.
var Lab={count:64,copies:1,stiffness:2,mode:'hybrid',paused:false,drive:false,cut:false,
    stage:'',handle:0,tick:0,time:0,gpu:0,invalid:0,singular:0,dirty:false,kick:false,
    oneStep:false,error:'',sample:null,
    reads:0,right:[3.5,5,0],anchorDirty:false,resetPending:false};
Lab.X=Engine.dynamics;Lab.S=Lab.X.scene;Lab.modelId=Engine.content.newId();Lab.owner=0;
Lab.definitions=function(){
    var X=Lab.X;Lab.space=X.space(3);
    Lab.distance=X.defineRelation({name:'distance',spaces:[Lab.space,Lab.space],parameters:1},function(e){
        return {residual:[e.sub(e.length(e.vsub(e.endpoints[0],e.endpoints[1])),e.parameter(0))]};
    });
    Lab.floor=X.defineRelation({name:'ground half space',spaces:[Lab.space],kind:'greaterEqual'},function(e){
        return {residual:[e.sub(e.endpoints[0][1],0.06)]};
    });
    Lab.sphere=X.defineRelation({name:'sphere exterior',spaces:[Lab.space],kind:'greaterEqual'},function(e){
        return {residual:[e.sub(e.length(e.vsub(e.endpoints[0],[0,1.8,0])),1.16)]};
    });
    Lab.damping=X.defineRelation({name:'displacement resistance',spaces:[Lab.space],rows:3,history:3},function(e){
        return {residual:e.vsub(e.endpoints[0],[e.state(0),e.state(1),e.state(2)]),update:e.endpoints[0]};
    });
    // Uniform arc-length sampling makes segment lengths comparable.
    var curve=[],lengths=[0],total=0;
    for(var i=0;i<=512;++i){var t=i/512,p=[-3.5+7*t,5-1.9*Math.sin(Math.PI*t),0.15*Math.sin(2*Math.PI*t)];
        if(i){var q=curve[i-1];total+=Math.sqrt(Math.pow(p[0]-q[0],2)+Math.pow(p[1]-q[1],2)+Math.pow(p[2]-q[2],2));lengths.push(total);}curve.push(p);}
    Lab.initials=[];var j=1;
    for(i=0;i<Lab.count;++i){var at=total*i/(Lab.count-1);while(j<512&&lengths[j]<at)++j;
        var f=(at-lengths[j-1])/(lengths[j]-lengths[j-1]);
        Lab.initials.push(curve[j-1].map(function(v,k){return v+(curve[j][k]-v)*f;}));}
    Lab.rest=total/(Lab.count-1);
};
Lab.initial=function(i){return Lab.initials[i];};
Lab.compliance=function(){return [0.0005,0.00001,0.0000001][Lab.stiffness];};
Lab.reset=function(){Lab.resetPending=true;};
Lab.rebuild=function(){
    var X=Lab.X,n=Lab.count*Lab.copies;if(Lab.owner)Engine.destroy(Lab.owner);
    Lab.handle=X.model();Lab.tick=0;Lab.time=0;Lab.gpu=0;Lab.invalid=0;Lab.singular=0;Lab.reads=0;
    Lab.error='';Lab.lastSampleTick=0;Lab.dirty=false;Lab.kick=false;Lab.oneStep=false;Lab.resetPending=false;
    Lab.cut=false;Lab.right=[3.5,5,0];Lab.anchorDirty=false;
    var initial=new Float32Array(n*3),metric=new Float32Array(n*9),enabled=new Float32Array(n),ids=new Uint32Array(n);
    Lab.acceleration=new Float32Array(n*3);
    for(var i=0;i<n;++i){var local=i%Lab.count,p=Lab.initial(local),free=local!==0&&local!==Lab.count-1;
        initial[i*3]=p[0];initial[i*3+1]=p[1];initial[i*3+2]=p[2];ids[i]=i;
        if(free){metric[i*9]=metric[i*9+4]=metric[i*9+8]=1;Lab.acceleration[i*3+1]=-9.81;enabled[i]=1;}}
    Lab.variables=X.variables(Lab.handle,Lab.space,{count:n,initial:initial,inverseMetric:metric});
    var m=(Lab.count-1)*Lab.copies,a=new Uint32Array(m),b=new Uint32Array(m);
    for(i=0;i<m;++i){a[i]=Math.floor(i/(Lab.count-1))*Lab.count+i%(Lab.count-1);b[i]=a[i]+1;}
    Lab.links=X.relations(Lab.handle,Lab.distance,{endpoints:[{set:Lab.variables,indices:a},{set:Lab.variables,indices:b}],parameters:[Lab.rest],compliance:[Lab.compliance()]});
    var endpoints=[{set:Lab.variables,indices:ids}];
    Lab.floorSet=X.relations(Lab.handle,Lab.floor,{endpoints:endpoints,enabled:enabled});
    Lab.sphereSet=X.relations(Lab.handle,Lab.sphere,{endpoints:endpoints,enabled:enabled});
    // Retained relation history resists displacement, updated by the solver itself.
    Lab.dampingSet=X.relations(Lab.handle,Lab.damping,{endpoints:endpoints,history:initial,compliance:[0.025,0.025,0.025],enabled:enabled});
    Lab.total=n;Lab.relations=m+3*n;
    Lab.sample=new Float32Array(n*3);for(i=0;i<Lab.sample.length;++i)Lab.sample[i]=initial[i];
    Lab.owner=Engine.create({id:Lab.modelId,name:'Rope dynamics batch',persistent:false,components:{dynamics:{
        model:X.describe(Lab.handle),policy:{mode:Lab.mode,substeps:4,iterations:12,colorBudget:12},
        stepTime:1/30,paused:Lab.paused,
        // Explicit CPU observation for the strain meter. Rendering bindings declare their own demand.
        observe:[{set:Lab.variables,first:0,count:n}]
    }}});
    X.destroy(Lab.handle);Lab.handle=0;
    Lab.arrange(Lab.copies);Lab.stage='compile';Lab.forceInput=true;Lab.anchorDirty=true;
    Engine.log('ROPE_LAB model: '+n+' variables / '+Lab.relations+' relations; '+Lab.mode);
};
Lab.release=function(){Lab.cut=!Lab.cut;Lab.dirty=true;Lab.anchorDirty=!Lab.cut;};
Lab.move=function(dx,dy){if(Lab.cut)return;Lab.right[0]=Math.max(1.5,Math.min(4.5,Lab.right[0]+dx));Lab.right[1]=Math.max(3,Math.min(6,Lab.right[1]+dy));Lab.anchorDirty=true;};
Lab.apply=function(){
    var X=Lab.S;
    var softness=new Float32Array((Lab.count-1)*Lab.copies);
    for(var i=0;i<softness.length;++i)softness[i]=Lab.compliance();
    X.patch(Lab.owner,'compliance',Lab.links,0,softness);
    for(var rope=0;rope<Lab.total/Lab.count;++rope){
        var end=(rope+1)*Lab.count-1;
        X.patch(Lab.owner,'inverseMetric',Lab.variables,end,Lab.cut?[1,0,0,0,1,0,0,0,1]:[0,0,0,0,0,0,0,0,0]);
        [Lab.floorSet,Lab.sphereSet,Lab.dampingSet].forEach(function(set){X.patch(Lab.owner,'relationEnabled',set,end,[Lab.cut?1:0]);});
        Lab.acceleration[end*3+1]=Lab.cut?-9.81:0;
    }
    Lab.forceInput=true;
    Lab.dirty=false;
    Lab.ropeViews.forEach(function(view){Engine.enabled(view.controller,!Lab.cut);Engine.visible(view.anchor,!Lab.cut);});
};
Lab.inputs=function(){
    var writes=[];
    if(Lab.drive&&!Lab.cut){Lab.right[2]=1.15*Math.sin((Lab.time+1/30)*1.8);Lab.anchorDirty=true;}
    if(Lab.forceInput){writes.push({field:'acceleration',set:Lab.variables,first:0,values:Lab.acceleration});Lab.forceInput=false;}
    if(Lab.anchorDirty){
        Lab.ropeViews.forEach(function(view){Engine.transform(view.controller,{position:{x:Lab.right[0],y:Lab.right[1],z:Lab.right[2]}});});
        Lab.anchorDirty=false;
    }
    if(Lab.kick){var velocity=new Float32Array(Lab.total*3);
        for(var i=0;i<Lab.total;++i){var local=i%Lab.count;
            if(local>0&&(local<Lab.count-1||Lab.cut)){
                var amount=Math.sin(Math.PI*local/(Lab.count-1));
                velocity[i*3+1]=2*amount;velocity[i*3+2]=5*amount;}}
        writes.push({field:'velocity',set:Lab.variables,first:0,values:velocity});Lab.kick=false;}
    writes.forEach(function(w){Lab.S.write(Lab.owner,w.field,w.set,w.first,w.values);});
};
Lab.pump=function(){
    if(Lab.resetPending)Lab.rebuild();
    if(!Lab.owner)return;
    var result=Lab.S.state(Lab.owner);
    Lab.tick=result.tick;Lab.time=result.time;Lab.gpu=result.gpuMilliseconds;
    Lab.invalid=result.invalidEvaluations;Lab.singular=result.singularSystems;Lab.stage=result.ready?'':'compile';
    if(result.error){if(!Lab.error)Engine.log('ROPE_LAB ERROR: '+result.error);Lab.error=result.error;Lab.paused=true;return;}
    if(result.sampleTick!==Lab.lastSampleTick&&result.sampleTick){
        Lab.lastSampleTick=result.sampleTick;Lab.sample=Lab.S.values(Lab.owner,Lab.variables,0,Lab.total);++Lab.reads;
        if(Lab.reads===1)Engine.log('ROPE_LAB LIVE: ECS sample received; diagnostics='+Lab.invalid+'/'+Lab.singular);
    }
    if(Lab.dirty)Lab.apply();
    var step=Lab.oneStep||Lab.kick||Lab.anchorDirty;
    Lab.inputs();Lab.S.control(Lab.owner,Lab.paused,step);Lab.oneStep=false;
};
