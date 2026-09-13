// Experiment 07 — density is a relation expression over the particle collection.
// The compiler differentiates the sum; the project supplies no solver or Jacobian.
Lab.add((function(){
var fluid={id:'fluid',tab:'Fluid',eyebrow:'实验 07',title:'粒子流体',
    subtitle:'水柱落入容器，观察水体铺开与回流。',
    equation:'ρᵢ / ρ₀ = Σⱼ V·315/(64πh³)·max(0, 1 − rᵢⱼ²/h²)³ ≤ 1',
    hint:'I 推动水体，观察回流',
    legend:'<span class="teal">■</span>流体<span class="gold">■</span>容器',
    panelTitle:'密度约束',strainLabel:'平均密度超限',
    count:2000,spacing:0.11,radius:0.05,support:0.22,halfWidth:1.5,halfDepth:0.75};
fluid.sizes={label:'粒子',options:[{label:'2,000 个',value:2000}],
    get:function(){return fluid.count;},set:function(){return '';}};
fluid.actions=[];fluid.keys={};
fluid.rows=[{id:'column',label:'初始水柱',kind:'enum',text:function(){return '10 × 20 × 10';},
    click:function(){Lab.reset();return '重新释放水柱';}}];
fluid.define=function(){
    var X=Lab.X;
    fluid.density=X.defineRelation({name:'relative density bound',
        objects:[{position:Lab.space},{position:Lab.space}],
        parameters:{support:'scalar',volume:'scalar'},kind:'lessEqual'},function(op,a,b,p){
        var delta=op.vsub(a.position,b.position);
        var h2=op.mul(p.support,p.support);
        var shape=op.max(0,op.sub(1,op.div(op.dot(delta,delta),h2)));
        var normalization=op.div(315/(64*Math.PI),op.mul(h2,p.support));
        var contribution=op.mul(op.mul(p.volume,normalization),op.mul(op.mul(shape,shape),shape));
        var density=op.sum(contribution,b);
        return {residual:[op.sub(density,1)]};
    });
    fluid.containment=X.defineRelation({name:'fluid container interior',
        objects:[{position:Lab.space},{normal:Lab.space,offset:Lab.scalar}],
        parameters:{radius:'scalar'},kind:'greaterEqual'},function(op,a,b,p){
        return {residual:[op.sub(op.sub(op.dot(a.position,b.normal),b.offset[0]),p.radius)]};
    });
};
fluid.build=function(model){
    var X=Lab.X,n=fluid.count,initial=new Float32Array(3*n);
    fluid.acceleration=new Float32Array(3*n);
    // A regular water column starts at rest; small deterministic offsets break
    // lattice symmetry without creating coincident particles or a dense random cloud.
    for(var y=0;y<20;++y)for(var z=0;z<10;++z)for(var x=0;x<10;++x){
        var i=(y*10+z)*10+x,at=3*i;
        initial[at]=-1.32+x*fluid.spacing+0.002*Math.sin(i*1.7);
        initial[at+1]=0.14+y*fluid.spacing+0.002*Math.cos(i*2.3);
        initial[at+2]=(z-4.5)*fluid.spacing+0.002*Math.sin(i*0.9);
        fluid.acceleration[at+1]=-9.81;
    }
    var positions=X.defineDofs(model,{name:'fluid positions',space:Lab.space,count:n,initial:initial});
    var normals=X.defineDofs(model,{name:'fluid wall normals',space:Lab.space,count:5,
        initial:[0,1,0,1,0,0,-1,0,0,0,0,1,0,0,-1],readOnly:true});
    var offsets=X.defineDofs(model,{name:'fluid wall offsets',space:Lab.scalar,count:5,
        initial:[0,-fluid.halfWidth,-fluid.halfWidth,-fluid.halfDepth,-fluid.halfDepth],readOnly:true});
    var particles=X.defineObject(model,{name:'fluid particles',kind:'collection',dofs:{position:positions}});
    var walls=X.defineObject(model,{name:'fluid container',kind:'collection',dofs:{normal:normals,offset:offsets}});
    var frame=X.defineObject(model,{name:'fluid rest frame',kind:'single',dofs:{}});
    fluid.densities=X.pair(fluid.density,particles,particles,
        {support:fluid.support,volume:Math.pow(fluid.spacing,3)},
        {self:'directed',includeSelf:true,compliance:[Lab.compliance()]});
    fluid.boundaries=X.pair(fluid.containment,particles,walls,{radius:fluid.radius});
    // A weak displacement resistance dissipates motion; it is not a viscosity solver.
    fluid.damping=X.pair(Lab.damping,particles,frame,[],{history:initial,compliance:[0.4,0.4,0.4]});
    Lab.variables=positions.set;Lab.total=n;Lab.relations=n+5*n+n;
    fluid.forceInput=true;fluid.kick=false;
    return initial;
};
fluid.layout=function(){return '2000';};
fluid.camera=function(){return {target:[0,1.1,0],yaw:0.3,pitch:0.48,distance:7.6,fov:0.65};};
fluid.note=function(){return '2,000 个粒子 · 2,000 条密度约束';};
fluid.families=function(){return [
    {name:'粒子密度上限',kind:'不等式',rows:fluid.count},
    {name:'容器半空间',kind:'不等式',rows:5*fluid.count},
    {name:'位移阻力',kind:'持久',rows:fluid.count}];};
fluid.stage=function(){
    Lab.mesh('Fluid basin floor',[0,-0.07,0],[3.2,0.14,1.7],'Floor');
    for(var side=-1;side<=1;side+=2){
        Lab.mesh('Fluid basin rim X '+side,[0,0.06,side*0.81],[3.3,0.12,0.12],'Anchor');
        Lab.mesh('Fluid basin rim Z '+side,[side*1.56,0.06,0],[0.12,0.12,1.7],'Anchor');
        for(var z=-1;z<=1;z+=2)
            Lab.mesh('Fluid basin corner '+side+' '+z,[side*1.56,1.25,z*0.81],[0.035,2.5,0.035],'Frame');
    }
    Lab.materials.Fluid=Engine.asset('/Game/Materials/Fluid');
    var rows=[];for(var i=0;i<fluid.count;++i)rows.push([i]);
    var r=fluid.radius;
    Lab.instanceBatches('Fluid',Lab.sphereMesh,'Fluid',rows,Lab.pointMapping(0,0,r),[r,r,r],0.07);
    Lab.lights(0.8);
};
fluid.restage=function(){};
fluid.apply=function(){
    var compliance=new Float32Array(fluid.count);
    for(var i=0;i<fluid.count;++i)compliance[i]=Lab.compliance();
    Lab.S.patch(Lab.owner,'compliance',fluid.densities,0,compliance);
};
fluid.push=function(){fluid.kick=true;return '向右推动水体';};
fluid.inputs=function(writes){
    if(fluid.forceInput){
        writes.push({field:'acceleration',set:Lab.variables,first:0,values:fluid.acceleration});
        fluid.forceInput=false;
    }
    if(fluid.kick){
        var velocity=new Float32Array(3*fluid.count);
        for(var i=0;i<fluid.count;++i){velocity[3*i]=1.5;velocity[3*i+1]=0.6;}
        writes.push({field:'velocity',set:Lab.variables,first:0,values:velocity});
    }
    var step=fluid.kick;fluid.kick=false;return step;
};
fluid.beginMeasure=function(q){
    // Display-only density telemetry uses the same polynomial, on a fixed sample.
    // It never supplies neighbours, density state or corrections to the solver.
    var n=fluid.count,h2=fluid.support*fluid.support;
    var factor=315/(64*Math.PI)*Math.pow(fluid.spacing/fluid.support,3),i=0,total=0;
    return {step:function(){
        var deadline=Date.now()+2;
        for(;i<n;++i){
            var at=3*i,density=0;
            for(var j=0;j<n;++j){
                var bt=3*j,dx=q[at]-q[bt],dy=q[at+1]-q[bt+1],dz=q[at+2]-q[bt+2];
                var shape=1-(dx*dx+dy*dy+dz*dz)/h2;
                if(shape>0)density+=factor*shape*shape*shape;
            }
            total+=Math.max(0,density-1);
            if((i&7)===7&&Date.now()>=deadline){++i;return;}
        }
        return total/n;
    }};
};
return fluid;
}()));
