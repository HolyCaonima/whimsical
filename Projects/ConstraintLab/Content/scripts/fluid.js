// Experiment 07 — density is a relation expression over the particle collection.
// The compiler differentiates the sum; the project supplies no solver or Jacobian.
Lab.add((function(){
var fluid={id:'fluid',tab:'Fluid',eyebrow:'实验 07',title:'粒子流体',
    subtitle:'水柱落入容器，观察水体铺开与回流。',
    equation:'ρᵢ / ρ₀ = Σⱼ V·315/(64πh³)·max(0, 1 − rᵢⱼ²/h²)³ ≤ 1',
    hint:'I 推动水体，观察回流',
    legend:'<span class="teal">■</span>流体<span class="gold">■</span>容器',
    panelTitle:'密度约束',strainLabel:'平均密度超限（采样）',
    count:10000,spacing:0.065,radius:0.03,support:0.13,halfWidth:1.5,halfDepth:0.75};
fluid.sizes={label:'粒子',options:[{label:'10,000 个',value:10000}],
    get:function(){return fluid.count;},set:function(){return '';}};
fluid.actions=[];fluid.keys={};
fluid.rows=[{id:'column',label:'初始水柱',kind:'enum',text:function(){return '20 × 25 × 20';},
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
    for(var y=0;y<25;++y)for(var z=0;z<20;++z)for(var x=0;x<20;++x){
        var i=(y*20+z)*20+x,at=3*i;
        initial[at]=-1.32+x*fluid.spacing+0.0012*Math.sin(i*1.7);
        initial[at+1]=0.14+y*fluid.spacing+0.0012*Math.cos(i*2.3);
        initial[at+2]=(z-9.5)*fluid.spacing+0.0012*Math.sin(i*0.9);
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
fluid.layout=function(){return '10000';};
fluid.camera=function(){return {target:[0,1.1,0],yaw:0.3,pitch:0.48,distance:7.6,fov:0.65};};
fluid.note=function(){return '10,000 个粒子 · 10,000 条密度约束';};
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
    // Display-only index over an immutable snapshot. Each sampled density still
    // includes every neighbour; this index never supplies data to the solver.
    var n=fluid.count,h=fluid.support,h2=h*h,grid=fluid.measureGrid;
    if(!grid||grid.count!==n){
        var buckets=1;while(buckets<2*n)buckets*=2;
        grid=fluid.measureGrid={count:n,mask:buckets-1,epoch:0,
            heads:new Uint32Array(buckets),marks:new Uint32Array(buckets),
            next:new Uint32Array(n),x:new Int32Array(n),y:new Int32Array(n),z:new Int32Array(n)};
    }
    var epoch=++grid.epoch,heads=grid.heads,marks=grid.marks,next=grid.next;
    var gx=grid.x,gy=grid.y,gz=grid.z,mask=grid.mask;
    function bucket(x,y,z){return (x*73856093^y*19349663^z*83492791)&mask;}
    var factor=315/(64*Math.PI)*Math.pow(fluid.spacing/h,3),built=0,total=0;
    var samples=Math.min(n,256),sample=0,cell=27,member=0,density=0;
    var ax,ay,az,cx,cy,cz,bx,by,bz;
    return {step:function(){
        var deadline=Date.now()+2;
        for(var work=0;;++work){
            // Yield inside index construction and bucket traversal as well as
            // between samples, including snapshots with heavily occupied cells.
            if(work&&!(work&63)&&Date.now()>=deadline)return;
            if(built<n){
                var at=3*built,x=Math.floor(q[at]/h),y=Math.floor(q[at+1]/h),z=Math.floor(q[at+2]/h);
                var key=bucket(x,y,z);
                gx[built]=x;gy[built]=y;gz[built]=z;
                next[built]=marks[key]===epoch?heads[key]:0;
                heads[key]=++built;marks[key]=epoch;
                continue;
            }
            if(member){
                var j=member-1;member=next[j];
                if(gx[j]!==bx||gy[j]!==by||gz[j]!==bz)continue;
                var bt=3*j,dx=ax-q[bt],dy=ay-q[bt+1],dz=az-q[bt+2];
                var shape=1-(dx*dx+dy*dy+dz*dz)/h2;
                if(shape>0)density+=factor*shape*shape*shape;
                continue;
            }
            if(cell===27){
                if(sample)total+=Math.max(0,density-1);
                if(sample===samples)return total/samples;
                var i=Math.floor(sample++*n/samples),offset=3*i;
                ax=q[offset];ay=q[offset+1];az=q[offset+2];
                cx=gx[i];cy=gy[i];cz=gz[i];density=0;cell=0;
            }
            bx=cx+cell%3-1;by=cy+Math.floor(cell/3)%3-1;bz=cz+Math.floor(cell/9)-1;++cell;
            var key=bucket(bx,by,bz);member=marks[key]===epoch?heads[key]:0;
        }
    }};
};
return fluid;
}()));
