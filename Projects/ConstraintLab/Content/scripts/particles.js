// Experiment 06 — high-level collection pairs. Compiler expands every member
// combination; separation and containment are ordinary user-authored op formulas.
Lab.add((function(){
var cloud={id:'particles',tab:'粒子群',eyebrow:'实验 06',title:'集合关系实验',
    subtitle:'粒子落入容器，观察间距与边界约束。',
    equation:'C₁ = (pᵢ − pⱼ)² − d² ≥ 0； C₂ = n·p − h − r ≥ 0',
    hint:'I 给粒子一个向上冲量',
    legend:'<span class="teal">■</span>粒子<span class="gold">■</span>容器边界',
    panelTitle:'集合 pair',count:4000,radius:0.05,kick:false,forceInput:false};
cloud.sizes={label:'数量',options:[{label:'4,000 个',value:4000}],
    get:function(){return 4000;},set:function(){return '';}};
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
    var halfWidth=1.5-cloud.radius;
    cloud.acceleration=new Float32Array(n*3);
    cloud.measureGrid={stamp:0,marks:new Uint32Array(1),heads:new Uint32Array(1),
        x:new Int32Array(n),y:new Int32Array(n),z:new Int32Array(n),next:new Uint32Array(n)};
    for(var i=0;i<n;++i){
        initial[i*3]=(Math.random()*2-1)*halfWidth;
        initial[i*3+1]=1+Math.random()*2;
        initial[i*3+2]=(Math.random()*2-1)*halfWidth;
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
cloud.layout=function(){return cloud.count+'/'+cloud.radius;};
cloud.camera=function(){return {target:[0,1.2,0],yaw:0.48,pitch:0.55,distance:8,fov:0.65};};
cloud.note=function(){return '4,000 个粒子 · 7,998,000 个无向成员组合 · 20,000 个粒子—边界组合';};
cloud.families=function(){return [
    {name:'粒子集合 × 自身',kind:'不等式',rows:cloud.count*(cloud.count-1)/2},
    {name:'粒子集合 × 边界集合',kind:'不等式',rows:cloud.count*5},
    {name:'粒子集合 × 容器框架',kind:'持久',rows:cloud.count}];};
cloud.stage=function(){
    Lab.mesh('Container floor',[0,-0.09,0],[3.2,0.18,3.2],'Floor');
    for(var side=-1;side<=1;side+=2){
        Lab.mesh('Container rail X '+side,[0,0.1,side*1.58],[3.3,0.2,0.16],'Anchor');
        Lab.mesh('Container rail Z '+side,[side*1.58,0.1,0],[0.16,0.2,3.3],'Anchor');
    }
    var transforms=[];
    for(var i=0;i<cloud.count;++i)
        transforms.push({position:[Lab.sample[i*3],Lab.sample[i*3+1],Lab.sample[i*3+2]],
            scale:[cloud.radius,cloud.radius,cloud.radius]});
    var e=Lab.X.expression(3),size=cloud.radius;
    cloud.entity=Engine.create({name:'Particles',persistent:false,components:{
        transform:{},
        render:{mesh:Lab.sphereMesh,material:Lab.materials.Edge,
            instanceCount:cloud.count,instanceTransforms:transforms},
        dynamicsBinding:{model:Lab.modelId,direction:'output',target:'renderInstances',interpolation:0.07,
            variables:[{set:Lab.variables,index:0}],
            mapping:e.finish([e.input(0),e.input(1),e.input(2),1,0,0,0,size,size,size])}
    }});
    Lab.entities.push(cloud.entity);
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
    // A pair can overlap only in the same diameter-sized cell or one of its
    // 26 neighbours. This preserves the exact maximum without an all-pairs scan.
    var overlap=0,d=2*cloud.radius,d2=d*d,inverse=1/d,grid=cloud.measureGrid;
    var cellX=grid.x,cellY=grid.y,cellZ=grid.z,next=grid.next;
    var minX=Infinity,minY=Infinity,minZ=Infinity,maxX=-Infinity,maxY=-Infinity,maxZ=-Infinity;
    for(var i=0;i<cloud.count;++i){
        var at=3*i,cx=Math.floor(q[at]*inverse),cy=Math.floor(q[at+1]*inverse),
            cz=Math.floor(q[at+2]*inverse);
        cellX[i]=cx;cellY[i]=cy;cellZ[i]=cz;
        minX=Math.min(minX,cx);minY=Math.min(minY,cy);minZ=Math.min(minZ,cz);
        maxX=Math.max(maxX,cx);maxY=Math.max(maxY,cy);maxZ=Math.max(maxZ,cz);
    }
    var originX=minX-1,originY=minY-1,originZ=minZ-1;
    var sizeX=maxX-minX+3,sizeY=maxY-minY+3,sizeZ=maxZ-minZ+3;
    var volume=sizeX*sizeY*sizeZ,capacity=grid.marks.length;
    if(capacity<volume){
        while(capacity<volume)capacity*=2;
        grid.marks=new Uint32Array(capacity);grid.heads=new Uint32Array(capacity);
    }
    var stamp=++grid.stamp,marks=grid.marks,heads=grid.heads;
    for(var i=0;i<cloud.count;++i){
        var cx=cellX[i],cy=cellY[i],cz=cellZ[i];
        var slot=((cx-originX)*sizeY+(cy-originY))*sizeZ+cz-originZ;
        if(marks[slot]!==stamp){marks[slot]=stamp;heads[slot]=0;}
        next[i]=heads[slot];heads[slot]=i+1;
    }
    // The positive half-neighbourhood visits each cross-cell pair exactly once.
    for(var i=0;i<cloud.count;++i){
        var at=3*i,px=q[at],py=q[at+1],pz=q[at+2],cx=cellX[i],cy=cellY[i],cz=cellZ[i];
        var slot=((cx-originX)*sizeY+(cy-originY))*sizeZ+cz-originZ;
        var entry=heads[slot];
        while(entry&&entry-1>i){
            var j=entry-1,jat=3*j,x=px-q[jat],y=py-q[jat+1],z=pz-q[jat+2];
            var distance2=x*x+y*y+z*z;
            if(distance2<d2)overlap=Math.max(overlap,1-Math.sqrt(distance2)*inverse);
            entry=next[j];
        }
        for(var ox=0;ox<=1;++ox)for(var oy=ox?-1:0;oy<=1;++oy){
            var firstZ=ox===0&&oy===0?1:-1;
            var base=((cx+ox-originX)*sizeY+(cy+oy-originY))*sizeZ+cz-originZ;
            for(var oz=firstZ;oz<=1;++oz){
                entry=marks[base+oz]===stamp?heads[base+oz]:0;
                while(entry){
                    var j=entry-1,jat=3*j,x=px-q[jat],y=py-q[jat+1],z=pz-q[jat+2];
                    var distance2=x*x+y*y+z*z;
                    if(distance2<d2)overlap=Math.max(overlap,1-Math.sqrt(distance2)*inverse);
                    entry=next[j];
                }
            }
        }
    }
    return overlap;
};
return cloud;
}()));
