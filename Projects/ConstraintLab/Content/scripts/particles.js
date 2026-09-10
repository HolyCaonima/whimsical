// Experiment 06 — high-level collection pairs. Compiler expands every member
// combination; separation and containment are ordinary user-authored op formulas.
Lab.add((function(){
var cloud={id:'particles',tab:'粒子群',eyebrow:'实验 06',title:'集合关系实验',
    subtitle:'粒子落入容器，观察间距与边界约束。',
    equation:'C₁ = (pᵢ − pⱼ)² − d² ≥ 0； C₂ = n·p − h − r ≥ 0',
    hint:'I 给粒子一个向上冲量',
    legend:'<span class="teal">■</span>粒子<span class="gold">■</span>容器边界',
    panelTitle:'集合 pair',count:100,radius:0.18,kick:false,forceInput:false};
cloud.sizes={label:'数量',options:[{label:'100 个',value:100}],
    get:function(){return 100;},set:function(){return '';}};
cloud.actions=[];cloud.rows=[];cloud.keys={};
cloud.define=function(){
    var X=Lab.X;
    cloud.separation=X.defineRelation({name:'minimum squared separation',
        objects:[{position:Lab.space},{position:Lab.space}],
        parameters:{diameter:'scalar'},kind:'greaterEqual'},function(op,a,b,p){
        var d=op.vsub(a.position,b.position);
        return {residual:[op.sub(op.dot(d,d),op.mul(p.diameter,p.diameter))]};
    });
    cloud.containment=X.defineRelation({name:'oriented plane interior',
        objects:[{position:Lab.space},{normal:Lab.space,offset:Lab.scalar}],
        parameters:{radius:'scalar'},kind:'greaterEqual'},function(op,a,b,p){
        return {residual:[op.sub(op.sub(op.dot(a.position,b.normal),b.offset[0]),p.radius)]};
    });
};
cloud.build=function(model){
    var X=Lab.X,n=cloud.count,initial=new Float32Array(n*3);
    cloud.acceleration=new Float32Array(n*3);
    for(var i=0;i<n;++i){
        initial[i*3]=(i%5-2)*0.42;
        initial[i*3+1]=2.2+Math.floor(i/25)*0.42;
        initial[i*3+2]=(Math.floor(i/5)%5-2)*0.42;
        cloud.acceleration[i*3+1]=-9.81;
    }
    // 1. Define mathematical state independently of any object.
    var positions=X.defineDofs(model,{name:'particle positions',space:Lab.space,count:n,initial:initial});
    var normals=X.defineDofs(model,{name:'wall normals',space:Lab.space,count:5,
        initial:[0,1,0, 1,0,0, -1,0,0, 0,0,1, 0,0,-1],readOnly:true});
    var offsets=X.defineDofs(model,{name:'wall offsets',space:Lab.scalar,count:5,
        initial:[0,-1.5,-1.5,-1.5,-1.5],readOnly:true});
    Lab.variables=positions.set;
    // 2. Explicit collection objects; neither DOF set automatically is an object.
    var particles=X.defineObject(model,{name:'particles',kind:'collection',dofs:{position:positions}});
    var walls=X.defineObject(model,{name:'container planes',kind:'collection',dofs:{normal:normals,offset:offsets}});
    var frame=X.defineObject(model,{name:'container frame',kind:'single',dofs:{}});
    // 3. Relation definitions above are independent of these concrete objects.
    // 4. Three high-level bindings; no particle-pair enumeration in user code.
    cloud.contacts=X.pair(cloud.separation,particles,particles,{diameter:2*cloud.radius},
        {self:'undirected',includeSelf:false});
    cloud.boundaries=X.pair(cloud.containment,particles,walls,{radius:cloud.radius});
    cloud.damping=X.pair(Lab.damping,particles,frame,[],{history:initial,compliance:[0.08,0.08,0.08]});
    cloud.forceInput=true;cloud.kick=false;
    Lab.total=n;Lab.relations=n*(n-1)/2+5*n+n;
    return initial;
};
cloud.layout=function(){return '100';};
cloud.camera=function(){return {target:[0,1.2,0],yaw:0.48,pitch:0.35,distance:8,fov:0.65};};
cloud.note=function(){return '100 个粒子 · 4,950 个无向成员组合 · 500 个粒子—边界组合';};
cloud.families=function(){return [
    {name:'粒子集合 × 自身',kind:'不等式',rows:4950},
    {name:'粒子集合 × 边界集合',kind:'不等式',rows:500},
    {name:'粒子集合 × 容器框架',kind:'持久',rows:100}];};
cloud.stage=function(){
    Lab.mesh('Container floor',[0,-0.09,0],[3.2,0.18,3.2],'Floor');
    for(var side=-1;side<=1;side+=2){
        Lab.mesh('Container rail X '+side,[0,0.1,side*1.58],[3.3,0.2,0.16],'Anchor');
        Lab.mesh('Container rail Z '+side,[side*1.58,0.1,0],[0.16,0.2,3.3],'Anchor');
    }
    for(var i=0;i<cloud.count;++i){
        var particle=Lab.mesh('Particle '+i,[0,2,0],[cloud.radius,cloud.radius,cloud.radius],'Edge',Lab.sphereMesh);
        Lab.bindPoint(particle,i,0,0,cloud.radius);
    }
    Lab.lights(0.8);
};
cloud.restage=function(){};cloud.apply=function(){};
cloud.push=function(){cloud.kick=true;return '给集合成员一个向上的冲量';};
cloud.inputs=function(writes){
    if(cloud.forceInput){writes.push({field:'acceleration',set:Lab.variables,first:0,values:cloud.acceleration});cloud.forceInput=false;}
    if(cloud.kick){
        var velocity=new Float32Array(cloud.count*3);
        for(var i=0;i<cloud.count;++i){velocity[i*3]=0.8*Math.sin(i);velocity[i*3+1]=3;velocity[i*3+2]=0.8*Math.cos(i);}
        writes.push({field:'velocity',set:Lab.variables,first:0,values:velocity});
    }
    var step=cloud.kick;cloud.kick=false;return step;
};
cloud.measure=function(q){
    var overlap=0,d=2*cloud.radius;
    for(var i=0;i<cloud.count;++i)for(var j=i+1;j<cloud.count;++j){
        var x=q[3*i]-q[3*j],y=q[3*i+1]-q[3*j+1],z=q[3*i+2]-q[3*j+2];
        overlap=Math.max(overlap,(d-Math.sqrt(x*x+y*y+z*z))/d);
    }
    return overlap;
};
return cloud;
}()));
