// Experiment 05 — one connected scalar graph combines a history ratchet,
// nonlinear transmission and a three-cable differential. The names describe
// the exhibit only; every behavior below is authored as a user relation.
Lab.add((function(){
var clock={id:'clockwork',tab:'棘轮星仪',eyebrow:'实验 05',title:'棘轮差动星仪',
    subtitle:'棘轮记忆输入相位，非圆传动改变速比，再由一个三行差动关系升降三枚悬星。',
    equation:'θ → f(θ)； Aℓ + sθ = L； Cᵣ = θ₀ − h ≥ 0, h′ = max(h, θ₀)',
    hint:'I 推动棘轮 · C 脱开回程',
    legend:'<span class="gold">■</span>棘轮输入<span class="teal">■</span>非圆从动<span class="pink">■</span>差动悬星',
    panelTitle:'统一关系图',
    profile:1,kickSpeed:1.25,auto:true,engaged:true,kick:false,nextPump:24,
    forceInput:false,lastTheta:0,observedHistory:0,anchorY:4.35,maxTheta:3.45};
var profiles=[
    {label:'均衡',gear:[1.18,0.92,0.10,0.06],ratio:0.48,spool:0.18},
    {label:'偏心',gear:[1.30,0.86,0.20,0.11],ratio:0.55,spool:0.23},
    {label:'强耦合',gear:[1.40,0.82,0.27,0.14],ratio:0.62,spool:0.26}];

clock.sizes={label:'机构轮廓',options:profiles.map(function(p,i){return {label:p.label,value:i};}),
    get:function(){return clock.profile;},
    set:function(value){if(clock.profile===value)return '';clock.profile=value;Lab.reset();
        return '星仪重编译为'+profiles[value].label+'轮廓';}};
clock.actions=[
    {id:'clutch',key:'C',text:function(){return clock.engaged?'脱开棘轮':'扣上棘轮';},
     active:function(){return !clock.engaged;},
     click:function(){clock.engaged=!clock.engaged;Lab.dirty=true;
        return clock.engaged?'棘轮重新扣合，记住当前相位':'棘轮脱开，回程力矩释放整套机构';}},
    {id:'auto',key:'W',text:function(){return clock.auto?'停止自动上弦':'自动上弦';},
     active:function(){return clock.auto;},
     click:function(){clock.auto=!clock.auto;clock.nextPump=Lab.tick+20;
        return clock.auto?'棘轮开始周期上弦':'停止自动上弦';}}];
clock.rows=[
    {kind:'stepper',id:'stroke',label:'上弦冲程',text:function(){return clock.kickSpeed.toFixed(2)+' rad/s';},
     step:function(sign){clock.kickSpeed=Math.max(0.65,Math.min(2.1,clock.kickSpeed+sign*0.15));}}];
clock.keys={14:'clutch',34:'auto',
    91:function(){clock.kickSpeed=Math.min(2.1,clock.kickSpeed+0.15);},
    93:function(){clock.kickSpeed=Math.max(0.65,clock.kickSpeed-0.15);}};

clock.define=function(){
    var X=Lab.X;
    clock.scalar=X.space(1);
    clock.transmission=X.defineRelation({
        name:'noncircular phase transmission',objects:[{input:clock.scalar},{middle:clock.scalar,output:clock.scalar}],
        parameters:4,rows:2
    },function(op,driver,followers){
        var a=driver.input[0],b=followers.middle[0],c=followers.output[0];
        return {residual:[
            op.sub(op.sub(b,op.mul(op.parameter(0),a)),op.mul(op.parameter(2),op.sin(op.mul(2,a)))),
            op.sub(op.sub(c,op.mul(op.parameter(1),b)),op.mul(op.parameter(3),op.sin(op.mul(3,b))))
        ]};
    });
    clock.differential=X.defineRelation({
        name:'phase driven cable differential',
        objects:[{input:clock.scalar,middle:clock.scalar,output:clock.scalar},
                 {left:clock.scalar,centre:clock.scalar,right:clock.scalar}],parameters:6,rows:3
    },function(op,phases,payloads){
        var a=op.parameter(0),r=op.parameter(1),s=op.parameter(2),
            l0=op.sub(a,payloads.left[0]),l1=op.sub(a,payloads.centre[0]),l2=op.sub(a,payloads.right[0]);
        return {residual:[
            op.sub(op.add(op.add(l0,op.mul(r,l1)),op.mul(s,phases.middle[0])),op.parameter(3)),
            op.sub(op.add(op.add(l1,op.mul(r,l2)),op.mul(s,phases.output[0])),op.parameter(4)),
            op.sub(op.add(op.add(op.add(l0,l1),l2),op.mul(s,phases.input[0])),op.parameter(5))
        ]};
    });
    clock.ratchet=X.defineRelation({
        name:'phase history ratchet',objects:[{value:clock.scalar},{}],history:1,kind:'greaterEqual'
    },function(op,a,b){
        var angle=a.value[0];
        return {residual:[op.sub(angle,op.state(0))],update:[op.max(op.state(0),angle)]};
    });
    clock.lower=X.defineRelation({
        name:'scalar lower bound',objects:[{value:clock.scalar},{value:clock.scalar}],kind:'greaterEqual'
    },function(op,a,b){return {residual:[op.sub(a.value[0],b.value[0])]};});
    clock.upper=X.defineRelation({
        name:'scalar upper bound',objects:[{value:clock.scalar},{value:clock.scalar}],kind:'lessEqual'
    },function(op,a,b){return {residual:[op.sub(a.value[0],b.value[0])]};});
};
clock.layout=function(){return String(clock.profile);};
clock.camera=function(){return {target:[0,3.15,0],yaw:0.12,pitch:0.12,distance:12.7,fov:0.61};};
clock.note=function(){return '6 个 R¹ 自由度 · 8 个关系实例 · 同一张连通执行图';};
clock.families=function(){
    return [{name:'非圆相位传动（2 行）',kind:'等式',rows:1},
            {name:'三索差动矩阵（3 行）',kind:'等式',rows:1},
            {name:'输入相位历史棘轮',kind:'持久',rows:1},
            {name:'标量行程下界',kind:'不等式',rows:4},
            {name:'输入相位上界',kind:'不等式',rows:1}];
};
clock.build=function(model){
    var X=Lab.X,p=profiles[clock.profile],
        initial=new Float32Array([0,0,0,1.80,2.30,1.50]),
        metric=new Float32Array([0.52,0.78,0.92,1,0.82,1.16]),
        acceleration=new Float32Array([-1.25,0,0,-9.81,-9.81,-9.81]);
    var dofs=X.defineDofs(model,{name:'mechanism coordinates',space:clock.scalar,count:6,initial:initial,inverseMetric:metric});
    Lab.variables=dofs.set;
    var coordinates=X.defineObject(model,{name:'coordinates',kind:'collection',dofs:{value:dofs}});
    var input=X.defineMember(coordinates,0);
    var driver=X.defineObject(model,{name:'input shaft',kind:'single',dofs:{input:X.dof(dofs,0)}});
    var followers=X.defineObject(model,{name:'follower shafts',kind:'single',dofs:{
        middle:X.dof(dofs,1),output:X.dof(dofs,2)}});
    var phases=X.defineObject(model,{name:'phase train',kind:'single',dofs:{
        input:X.dof(dofs,0),middle:X.dof(dofs,1),output:X.dof(dofs,2)}});
    var payloads=X.defineObject(model,{name:'three payloads',kind:'single',dofs:{
        left:X.dof(dofs,3),centre:X.dof(dofs,4),right:X.dof(dofs,5)}});
    // The frame has no evolving DOF; the ratchet keeps history on its pair.
    var frame=X.defineObject(model,{name:'ratchet frame',kind:'single',dofs:{}});
    var limits=X.defineDofs(model,{name:'travel limits',space:clock.scalar,count:3,initial:[0,0.42,clock.maxTheta],readOnly:true});
    var lowerAngle=X.defineObject(model,{name:'angle lower stop',kind:'single',dofs:{value:X.dof(limits,0)}});
    var lowerHeight=X.defineObject(model,{name:'height lower stop',kind:'single',dofs:{value:X.dof(limits,1)}});
    var upperAngle=X.defineObject(model,{name:'angle upper stop',kind:'single',dofs:{value:X.dof(limits,2)}});
    clock.gearParameters=new Float32Array(p.gear);
    // Both sides may expose several named DOFs, including aliases used elsewhere.
    clock.gearSet=X.pair(clock.transmission,driver,followers,clock.gearParameters,
        {compliance:[Lab.compliance(),Lab.compliance()]});
    var l0=clock.anchorY-initial[3],l1=clock.anchorY-initial[4],l2=clock.anchorY-initial[5];
    clock.diffParameters=new Float32Array([
        clock.anchorY,p.ratio,p.spool,l0+p.ratio*l1,l1+p.ratio*l2,l0+l1+l2]);
    clock.diffSet=X.pair(clock.differential,phases,payloads,clock.diffParameters,
        {compliance:[Lab.compliance(),Lab.compliance(),Lab.compliance()]});
    clock.ratchetSet=X.pair(clock.ratchet,input,frame,[],{history:[0],compliance:[Lab.compliance()]});
    clock.lowerSet=X.pairs(clock.lower,[[input,lowerAngle],
        [X.defineMember(coordinates,3),lowerHeight],[X.defineMember(coordinates,4),lowerHeight],
        [X.defineMember(coordinates,5),lowerHeight]],[]);
    clock.upperSet=X.pair(clock.upper,input,upperAngle,[]);
    clock.acceleration=acceleration;clock.auto=true;clock.engaged=true;clock.kick=false;
    clock.nextPump=24;clock.forceInput=true;clock.lastTheta=0;clock.observedHistory=0;
    Lab.total=6;Lab.relations=8;
    return initial;
};
clock.bindSpoke=function(entity,index,cx,cy,cz,radius,offset){
    var e=Lab.X.expression(1),angle=e.add(e.input(0),offset),half=e.mul(angle,0.5);
    Engine.addComponent(entity,'dynamicsBinding',{model:Lab.modelId,direction:'output',interpolation:0.04,
        variables:[{set:Lab.variables,index:index}],
        mapping:e.finish([cx,cy,cz,e.cos(half),0,0,e.sin(half),radius*2,0.10,0.13])});
};
clock.bindMarker=function(entity,index,cx,cy,cz,radius){
    var e=Lab.X.expression(1),angle=e.input(0);
    Engine.addComponent(entity,'dynamicsBinding',{model:Lab.modelId,direction:'output',interpolation:0.04,
        variables:[{set:Lab.variables,index:index}],
        mapping:e.finish([e.add(cx,e.mul(radius,e.cos(angle))),e.add(cy,e.mul(radius,e.sin(angle))),cz+0.12,
            1,0,0,0,0.14,0.14,0.14])});
};
clock.bindPhaseLink=function(entity,a,b,ca,cb,ra,rb){
    var e=Lab.X.expression(2),aa=e.input(0),bb=e.input(1),
        p=[e.add(ca[0],e.mul(ra,e.cos(aa))),e.add(ca[1],e.mul(ra,e.sin(aa))),0.48],
        q=[e.add(cb[0],e.mul(rb,e.cos(bb))),e.add(cb[1],e.mul(rb,e.sin(bb))),0.48],
        d=e.vsub(q,p),length=e.max(e.length(d),0.001),
        rotation=Lab.swing(e,e.scale(d,e.div(1,length))),mid=e.scale(e.vadd(p,q),0.5);
    Engine.addComponent(entity,'dynamicsBinding',{model:Lab.modelId,direction:'output',interpolation:0.04,
        variables:[{set:Lab.variables,index:a},{set:Lab.variables,index:b}],
        mapping:e.finish([mid[0],mid[1],mid[2],rotation[0],rotation[1],rotation[2],rotation[3],
            0.035,length,0.035])});
};
clock.bindPayload=function(entity,index,x,z,size){
    var e=Lab.X.expression(1);
    Engine.addComponent(entity,'dynamicsBinding',{model:Lab.modelId,direction:'output',interpolation:0.06,
        variables:[{set:Lab.variables,index:index}],
        mapping:e.finish([x,e.input(0),z,1,0,0,0,size,size,size])});
};
clock.bindCable=function(entity,index,x,z){
    var e=Lab.X.expression(1),y=e.input(0),length=e.max(e.sub(clock.anchorY,y),0.001);
    Engine.addComponent(entity,'dynamicsBinding',{model:Lab.modelId,direction:'output',interpolation:0.06,
        variables:[{set:Lab.variables,index:index}],
        mapping:e.finish([x,e.mul(e.add(clock.anchorY,y),0.5),z,1,0,0,0,0.045,length,0.045])});
};
clock.bindMoon=function(entity,angleIndex,heightIndex,x,z,orbit,size){
    var e=Lab.X.expression(2),angle=e.input(0),height=e.input(1);
    Engine.addComponent(entity,'dynamicsBinding',{model:Lab.modelId,direction:'output',interpolation:0.06,
        variables:[{set:Lab.variables,index:angleIndex},{set:Lab.variables,index:heightIndex}],
        mapping:e.finish([e.add(x,e.mul(orbit,e.cos(angle))),e.add(height,e.mul(orbit,e.sin(angle))),
            z+0.18,1,0,0,0,size,size,size])});
};
clock.stage=function(){
    Lab.mesh('Orrery platform',[0,-0.18,0],[9,0.35,4.2],'Floor');
    Lab.mesh('Orrery crown',[0,4.48,0.58],[7.4,0.12,0.12],'Frame');
    var centres=[[-3,5.85],[0,5.85],[3,5.85]],radii=[1.02,0.86,0.74],
        wheelMaterials=['Anchor','Obstacle','Obstacle'],payloadMaterials=['Cloth','Obstacle','Cloth'];
    for(var i=0;i<3;++i){
        var x=centres[i][0],cy=centres[i][1],radius=radii[i];
        Lab.mesh('Orrery tower '+i,[x,2.25,0.72],[0.12,4.5,0.12],'Frame');
        Lab.mesh('Cable eye '+i,[x,clock.anchorY,0],[0.17,0.17,0.17],'Anchor',Lab.sphereMesh);
        for(var spoke=0;spoke<3;++spoke){
            var beam=Lab.mesh('Orrery wheel '+i+' spoke '+spoke,[x,cy,0.62],
                [radius*2,0.1,0.13],wheelMaterials[i]);
            clock.bindSpoke(beam,i,x,cy,0.62,radius,spoke*Math.PI/3);
        }
        var marker=Lab.mesh('Orrery phase '+i,[x,cy,0.75],[0.14,0.14,0.14],wheelMaterials[i],Lab.sphereMesh);
        clock.bindMarker(marker,i,x,cy,0.62,radius);
        var cable=Lab.mesh('Differential cable '+i,[x,3,0],[0.045,2,0.045],'Edge',Lab.rodMesh);
        clock.bindCable(cable,i+3,x,0);
        var body=Lab.mesh('Suspended star '+i,[x,2,0],[0.34,0.34,0.34],payloadMaterials[i],Lab.sphereMesh);
        clock.bindPayload(body,i+3,x,0,0.34);
        var moon=Lab.mesh('Phase moon '+i,[x,2,0.18],[0.11,0.11,0.11],wheelMaterials[i],Lab.sphereMesh);
        clock.bindMoon(moon,i,i+3,x,0,0.52,0.11);
    }
    for(var link=0;link<2;++link){
        var trace=Lab.mesh('Noncircular phase link '+link,[0,5.8,0.48],[0.035,3,0.035],link?'Frame':'Edge',Lab.rodMesh);
        clock.bindPhaseLink(trace,link,link+1,centres[link],centres[link+1],radii[link],radii[link+1]);
    }
    for(var tooth=0;tooth<7;++tooth)
        Lab.mesh('Ratchet crown tooth '+tooth,[-4.25+tooth*0.18,5.13+tooth*0.12,0.62],[0.11,0.05,0.11],'Anchor');
    Lab.lights(0.95);
};
clock.restage=function(){};
clock.apply=function(){
    var c=Lab.compliance();
    Lab.S.patch(Lab.owner,'compliance',clock.gearSet,0,[c,c]);
    Lab.S.patch(Lab.owner,'compliance',clock.diffSet,0,[c,c,c]);
    Lab.S.patch(Lab.owner,'compliance',clock.ratchetSet,0,[c]);
    Lab.S.patch(Lab.owner,'relationEnabled',clock.ratchetSet,0,[clock.engaged?1:0]);
};
clock.push=function(){clock.kick=true;return '棘轮输入推进，非圆相位经差动矩阵传到三枚悬星';};
clock.inputs=function(writes){
    if(Lab.sample)clock.lastTheta=Lab.sample[0];
    if(clock.forceInput){
        writes.push({field:'acceleration',set:Lab.variables,first:0,values:clock.acceleration});
        clock.forceInput=false;
    }
    if(!clock.engaged)
        writes.push({field:'history',set:clock.ratchetSet,first:0,values:new Float32Array([clock.lastTheta])});
    if(clock.auto&&clock.engaged&&Lab.tick>=clock.nextPump&&clock.lastTheta<clock.maxTheta-0.12){
        clock.kick=true;clock.nextPump=Lab.tick+52;
    }
    if(clock.kick)
        writes.push({field:'velocity',set:Lab.variables,first:0,values:new Float32Array([clock.kickSpeed])});
    var step=clock.kick||!clock.engaged;clock.kick=false;return step;
};
clock.measure=function(q){
    clock.lastTheta=q[0];
    if(clock.engaged)clock.observedHistory=Math.max(clock.observedHistory,q[0]);
    else clock.observedHistory=q[0];
    var g=clock.gearParameters,p=clock.diffParameters,a=p[0],r=p[1],s=p[2],
        l0=a-q[3],l1=a-q[4],l2=a-q[5],
        e0=q[1]-g[0]*q[0]-g[2]*Math.sin(2*q[0]),
        e1=q[2]-g[1]*q[1]-g[3]*Math.sin(3*q[1]),
        d0=l0+r*l1+s*q[1]-p[3],d1=l1+r*l2+s*q[2]-p[4],
        d2=l0+l1+l2+s*q[0]-p[5],
        h=Math.max(0,clock.observedHistory-q[0]);
    return (Math.abs(e0)+Math.abs(e1)+Math.abs(d0)+Math.abs(d1)+Math.abs(d2)+h)/
        (3+p[3]+p[4]+p[5]);
};
return clock;
}()));
