// Experiment 03 — small cloth patches and short ropes form one suspended grid.
// Rope endpoints reuse patch-edge variables; four longer chains reuse the four
// outer corner variables. Everything is one connected R3 relation graph.
Lab.add((function(){
var net={id:'woven',tab:'绳布拼网',eyebrow:'实验 03',title:'绳布拼接网',
    subtitle:'布块之间由短绳逐格连接，整张网再由四根角绳悬挂。',
    equation:'G = (⋃ Pᵢⱼ) ∪ R短绳 ∪ R角绳',
    hint:'↑ ↓ 收紧或放松布块间短绳',
    legend:'<span class="rose">■</span>独立布块<span class="teal">■</span>短连接绳<span class="gold">■</span>四根角绳',
    panelTitle:'绳布网格',
    tiles:4,resolution:6,tileSize:1.25,gap:0.44,shortScale:1,
    patches:0,wind:false,drive:false,phase:0,kick:false,forceInput:false,anchorDirty:false};
var windStrength=4.5;

net.sizes={label:'布块阵列',options:[{label:'2 × 2',value:2},{label:'3 × 3',value:3},{label:'4 × 4',value:4}],
    get:function(){return net.tiles;},
    set:function(value){if(net.tiles===value)return '';net.tiles=value;Lab.reset();
        return '重新编译 '+value+' × '+value+' 块布组成的绳网';}};
net.actions=[
    {id:'wind',key:'C',text:function(){return '侧向风';},active:function(){return net.wind;},
     click:function(){net.wind=!net.wind;net.forceInput=true;return net.wind?'风吹动每块布与连接绳':'侧向风停止';}},
    {id:'drive',key:'W',text:function(){return '摆动四角';},active:function(){return net.drive;},
     click:function(){net.drive=!net.drive;net.anchorDirty=true;
        return net.drive?'四根角绳开始交替牵动整张网':'四个吊点回到固定位置';}}];
net.rows=[
    {kind:'stepper',id:'short-length',label:'短绳长度',text:function(){return net.shortScale.toFixed(2)+' ×';},
     step:function(sign){net.adjust(sign*0.04);}}];
net.keys={14:'wind',34:'drive',91:function(){net.adjust(-0.04);},93:function(){net.adjust(0.04);}};

net.node=function(tx,tz,u,v){
    var r=net.resolution;
    return (tz*net.tiles+tx)*r*r+v*r+u;
};
net.layout=function(){return String(net.tiles);};
net.camera=function(){return {target:[0,3.15,0],yaw:0.5,pitch:0.46,distance:14.5,fov:0.64};};
net.note=function(){
    return net.tiles+' × '+net.tiles+' 块布 · '+net.shortRopes+' 根短绳 · 4 根角绳 · '+Lab.total+' 个 R³ 变量';
};
net.families=function(){
    return [{name:'各布块结构 L = d',kind:'等式',rows:net.structureRows},
            {name:'各布块剪切 L = √2 d',kind:'等式',rows:net.shearRows},
            {name:'各布块弯曲 L = 2d',kind:'等式',rows:net.bendRows},
            {name:net.shortRopes+' 根布块间短绳',kind:'等式',rows:net.shortRows},
            {name:'四根外角悬绳',kind:'等式',rows:net.cornerRows},
            {name:'地面半空间',kind:'不等式',rows:Lab.total},
            {name:'整体位移阻力',kind:'持久',rows:Lab.total}];
};

net.build=function(model){
    var X=Lab.X,t=net.tiles,r=net.resolution,positions=[],fixed=[],
        span=t*net.tileSize+(t-1)*net.gap,pitch=net.tileSize+net.gap;
    var tx,tz,u,v,i,k,lane;
    function addPoint(x,y,z,isFixed){
        var id=positions.length;positions.push([x,y,z]);fixed.push(!!isFixed);return id;
    }
    for(tz=0;tz<t;++tz)for(tx=0;tx<t;++tx){
        var cx=(tx-(t-1)/2)*pitch,cz=(tz-(t-1)/2)*pitch;
        for(v=0;v<r;++v)for(u=0;u<r;++u)
            addPoint(cx+(u/(r-1)-0.5)*net.tileSize,
                     4+0.012*Math.sin((tx*r+u)*0.9)*Math.cos((tz*r+v)*0.7),
                     cz+(v/(r-1)-0.5)*net.tileSize,false);
    }
    net.fabricCount=positions.length;
    var ropeA=[],ropeB=[],ropeRest=[];
    net.shortPaths=[];net.cornerPaths=[];
    function addSegment(a,b){
        var pa=positions[a],pb=positions[b],dx=pb[0]-pa[0],dy=pb[1]-pa[1],dz=pb[2]-pa[2];
        ropeA.push(a);ropeB.push(b);ropeRest.push(Math.sqrt(dx*dx+dy*dy+dz*dz));
    }
    function shortRope(a,b){
        var pa=positions[a],pb=positions[b],
            middle=addPoint((pa[0]+pb[0])/2,(pa[1]+pb[1])/2-0.055,(pa[2]+pb[2])/2,false),
            path=[a,middle,b];
        addSegment(a,middle);addSegment(middle,b);net.shortPaths.push(path);
    }
    var lanes=[0,Math.floor((r-1)/2),r-1];
    for(tz=0;tz<t;++tz)for(tx=0;tx+1<t;++tx)
        for(k=0;k<lanes.length;++k){
            lane=lanes[k];
            shortRope(net.node(tx,tz,r-1,lane),net.node(tx+1,tz,0,lane));
        }
    for(tz=0;tz+1<t;++tz)for(tx=0;tx<t;++tx)
        for(k=0;k<lanes.length;++k){
            lane=lanes[k];
            shortRope(net.node(tx,tz,lane,r-1),net.node(tx,tz+1,lane,0));
        }
    net.shortRopes=net.shortPaths.length;net.shortRows=ropeA.length;

    var cornerNodes=[
        net.node(0,0,0,0),net.node(t-1,0,r-1,0),
        net.node(0,t-1,0,r-1),net.node(t-1,t-1,r-1,r-1)];
    net.anchorBase=[
        [-span/2-1.15,7.2,-span/2-0.95],[span/2+1.15,7.2,-span/2-0.95],
        [-span/2-1.15,7.2,span/2+0.95],[span/2+1.15,7.2,span/2+0.95]];
    net.anchorIndices=[];
    var cornerSegments=14;
    for(i=0;i<4;++i){
        var anchor=net.anchorBase[i],end=positions[cornerNodes[i]],
            anchorId=addPoint(anchor[0],anchor[1],anchor[2],true),path=[anchorId];
        net.anchorIndices.push(anchorId);
        for(k=1;k<cornerSegments;++k){
            var f=k/cornerSegments;
            path.push(addPoint(anchor[0]+(end[0]-anchor[0])*f,
                anchor[1]+(end[1]-anchor[1])*f-0.28*Math.sin(Math.PI*f),
                anchor[2]+(end[2]-anchor[2])*f,false));
        }
        path.push(cornerNodes[i]);
        for(k=0;k<cornerSegments;++k)addSegment(path[k],path[k+1]);
        net.cornerPaths.push(path);
    }
    net.cornerNodes=cornerNodes;net.cornerRows=ropeA.length-net.shortRows;

    var total=positions.length,initial=new Float32Array(total*3),metric=new Float32Array(total*9),
        enabled=new Float32Array(total);
    net.acceleration=new Float32Array(total*3);net.movable=new Float32Array(total);
    for(i=0;i<total;++i){
        initial[i*3]=positions[i][0];initial[i*3+1]=positions[i][1];initial[i*3+2]=positions[i][2];
        if(!fixed[i]){
            metric[i*9]=metric[i*9+4]=metric[i*9+8]=1;
            net.acceleration[i*3+1]=-9.81;net.movable[i]=1;enabled[i]=1;
        }
    }
    var dofs=X.defineDofs(model,{name:'woven positions',space:Lab.space,count:total,initial:initial,inverseMetric:metric});
    Lab.variables=dofs.set;
    var particles=X.defineObject(model,{name:'woven nodes',kind:'collection',dofs:{position:dofs}});
    var environment=Lab.environment(model), members=[];
    for(i=0;i<total;++i)members.push(X.defineMember(particles,i));

    function clothLinks(steps){
        var a=[],b=[],pairs=[],step;
        for(tz=0;tz<t;++tz)for(tx=0;tx<t;++tx)
            for(step=0;step<steps.length;++step)for(v=0;v<r;++v)for(u=0;u<r;++u){
                var u2=u+steps[step][0],v2=v+steps[step][1];
                if(u2>=0&&u2<r&&v2>=0&&v2<r){
                    a.push(net.node(tx,tz,u,v));b.push(net.node(tx,tz,u2,v2));
                    pairs.push([members[a[a.length-1]],members[b[b.length-1]]]);
                }
            }
        return {rows:a.length,a:new Uint32Array(a),b:new Uint32Array(b),
                pairs:pairs};
    }
    var structure=clothLinks([[1,0],[0,1]]),shear=clothLinks([[1,1],[1,-1]]),
        bend=clothLinks([[2,0],[0,2]]),cell=net.tileSize/(r-1);
    net.structureRows=structure.rows;net.structureA=structure.a;net.structureB=structure.b;
    net.shearRows=shear.rows;net.bendRows=bend.rows;
    net.structureSet=X.pairs(Lab.distance,structure.pairs,[cell],{compliance:[Lab.compliance()]});
    net.shearSet=X.pairs(Lab.distance,shear.pairs,[cell*Math.SQRT2],{compliance:[0.00005]});
    net.bendSet=X.pairs(Lab.distance,bend.pairs,[2*cell],{compliance:[0.0002]});

    net.ropeRows=ropeA.length;net.ropeA=new Uint32Array(ropeA);net.ropeB=new Uint32Array(ropeB);
    net.baseRests=new Float32Array(ropeRest);net.currentRests=new Float32Array(ropeRest.length);
    for(i=0;i<net.ropeRows;++i)
        net.currentRests[i]=net.baseRests[i]*(i<net.shortRows?net.shortScale:1);
    var ropePairs=[];
    for(i=0;i<net.ropeRows;++i)ropePairs.push([members[net.ropeA[i]],members[net.ropeB[i]]]);
    net.ropeSet=X.pairs(Lab.distance,ropePairs,net.currentRests,{compliance:[Lab.compliance()]});
    net.floorSet=X.pair(Lab.floor,particles,environment,[0.04],{enabled:enabled});
    net.dampingSet=X.pair(Lab.damping,particles,environment,[],{history:initial,
        compliance:[0.035,0.035,0.035],enabled:enabled});
    net.wind=false;net.drive=false;net.phase=0;net.kick=false;
    net.forceInput=true;net.anchorDirty=true;
    Lab.total=total;
    Lab.relations=net.structureRows+net.shearRows+net.bendRows+net.ropeRows+2*total;
    return initial;
};

net.stage=function(){
    var t=net.tiles,r=net.resolution,span=t*net.tileSize+(t-1)*net.gap,i,k,tx,tz,u,v;
    net.patches=t*t*(r-1)*(r-1);
    Lab.mesh('Patch net platform',[0,-0.18,0],[11,0.35,10],'Floor');
    for(k=-5;k<=5;++k)Lab.mesh('Floor line',[k,0,0],[0.012,0.012,10],'Grid');
    for(k=-5;k<=5;++k)Lab.mesh('Floor line',[0,0,k],[11,0.012,0.012],'Grid');
    var ax=span/2+1.15,az=span/2+0.95;
    [[-ax,-az],[ax,-az],[-ax,az],[ax,az]].forEach(function(p,index){
        Lab.mesh('Suspension post '+index,[p[0],3.6,p[1]],[0.06,7.2,0.06],'Frame');
    });
    for(i=-1;i<=1;i+=2){
        Lab.mesh('Suspension beam X '+i,[0,7.2,i*az],[2*ax+0.08,0.07,0.07],'Frame');
        Lab.mesh('Suspension beam Z '+i,[i*ax,7.2,0],[0.07,0.07,2*az+0.08],'Frame');
    }
    net.controllers=[];
    net.anchorIndices.forEach(function(index,corner){
        var p=net.anchorBase[corner],input=Lab.X.expression(10);
        var controller=Engine.create({name:'Corner rope input '+corner,persistent:false,components:{
            transform:{position:p},
            dynamicsBinding:{model:Lab.modelId,direction:'input',variables:[{set:Lab.variables,index:index}],
                mapping:input.finish([input.input(0),input.input(1),input.input(2)])}}});
        Lab.entities.push(controller);net.controllers.push(controller);
        var marker=Lab.mesh('Fixed corner rope '+corner,p,[0.13,0.13,0.13],'Anchor',Lab.sphereMesh);
        Lab.bindPoint(marker,index,0,0,0.13);
        var shared=Lab.mesh('Shared net corner '+corner,[0,4,0],[0.1,0.1,0.1],'Anchor',Lab.sphereMesh);
        Lab.bindPoint(shared,net.cornerNodes[corner],0,0,0.1);
    });
    for(tz=0;tz<t;++tz)for(tx=0;tx<t;++tx)
        for(v=0;v+1<r;++v)for(u=0;u+1<r;++u){
            var plate=Lab.mesh('Cloth tile '+tz+'/'+tx+' patch '+v+'/'+u,[0,4,0],
                [net.tileSize/(r-1),0.026,net.tileSize/(r-1)],'Cloth');
            Lab.bindPatch(plate,net.node(tx,tz,u,v),net.node(tx,tz,u+1,v),
                net.node(tx,tz,u,v+1),net.node(tx,tz,u+1,v+1),0.026,1);
        }
    function drawRope(path,name,radius,material){
        for(var p=0;p+1<path.length;++p){
            var wire=Lab.mesh(name+' segment '+p,[0,4,0],[radius,0.2,radius],material,Lab.rodMesh);
            Lab.bindSpan(wire,path[p],path[p+1],0,0,radius);
        }
        for(var p=1;p+1<path.length;++p){
            var node=Lab.mesh(name+' joint '+p,[0,4,0],[radius*0.9,radius*0.9,radius*0.9],material,Lab.sphereMesh);
            Lab.bindPoint(node,path[p],0,0,radius*0.9);
        }
    }
    net.shortPaths.forEach(function(path,index){drawRope(path,'Short connector '+index,0.045,'Obstacle');});
    net.cornerPaths.forEach(function(path,index){drawRope(path,'Corner rope '+index,0.06,'Edge');});
    Lab.lights(1.15);
    Engine.log('LAB woven stage: '+t*t+' cloth tiles, '+net.shortRopes+' short ropes, 4 corner ropes');
};
net.restage=function(){
    net.controllers.forEach(function(controller,index){
        var p=net.anchorBase[index];Engine.enabled(controller,true);
        Engine.transform(controller,{position:{x:p[0],y:p[1],z:p[2]}});
    });
};
net.adjust=function(delta){
    net.shortScale=Math.max(0.82,Math.min(1.35,net.shortScale+delta));Lab.dirty=true;
};
net.apply=function(){
    var structureCompliance=new Float32Array(net.structureRows),
        ropeCompliance=new Float32Array(net.ropeRows),i;
    for(i=0;i<net.structureRows;++i)structureCompliance[i]=Lab.compliance();
    for(i=0;i<net.ropeRows;++i){
        ropeCompliance[i]=Lab.compliance();
        net.currentRests[i]=net.baseRests[i]*(i<net.shortRows?net.shortScale:1);
    }
    Lab.S.patch(Lab.owner,'compliance',net.structureSet,0,structureCompliance);
    Lab.S.patch(Lab.owner,'parameters',net.ropeSet,0,net.currentRests);
    Lab.S.patch(Lab.owner,'compliance',net.ropeSet,0,ropeCompliance);
};
net.push=function(){net.kick=true;return '冲量从布块经过短绳传遍整张拼接网';};
net.inputs=function(writes){
    var i;
    if(net.wind||net.forceInput){
        for(i=0;i<Lab.total;++i)
            net.acceleration[i*3]=net.wind&&net.movable[i]?
                windStrength*(0.6+0.4*Math.sin(net.phase*2+i*0.09)):0;
        net.forceInput=true;
    }
    if(net.wind||net.drive)net.phase+=1/30;
    if(net.forceInput){
        writes.push({field:'acceleration',set:Lab.variables,first:0,values:net.acceleration});
        net.forceInput=net.wind;
    }
    if(net.drive)net.anchorDirty=true;
    if(net.anchorDirty){
        var angle=net.drive?0.1*Math.sin(net.phase*1.25):0,c=Math.cos(angle),s=Math.sin(angle);
        net.controllers.forEach(function(controller,index){
            var b=net.anchorBase[index],x=c*b[0]-s*b[2],z=s*b[0]+c*b[2],
                y=b[1]+(net.drive?0.22*Math.sin(net.phase*1.7+index*Math.PI/2):0);
            Engine.transform(controller,{position:{x:x,y:y,z:z}});
        });
    }
    if(net.kick){
        var velocity=new Float32Array(Lab.total*3);
        for(i=0;i<Lab.total;++i)if(net.movable[i]){
            velocity[i*3]=2.8;velocity[i*3+1]=0.7;velocity[i*3+2]=0.9;
        }
        writes.push({field:'velocity',set:Lab.variables,first:0,values:velocity});
    }
    var step=net.kick||net.anchorDirty;net.kick=false;net.anchorDirty=false;
    return step;
};
net.measure=function(q){
    var sum=0,rows=0,i,cell=net.tileSize/(net.resolution-1);
    function error(a,b,rest){
        a*=3;b*=3;var dx=q[b]-q[a],dy=q[b+1]-q[a+1],dz=q[b+2]-q[a+2];
        sum+=Math.abs(Math.sqrt(dx*dx+dy*dy+dz*dz)-rest)/rest;++rows;
    }
    for(i=0;i<net.structureRows;++i)error(net.structureA[i],net.structureB[i],cell);
    for(i=0;i<net.ropeRows;++i)error(net.ropeA[i],net.ropeB[i],net.currentRests[i]);
    return sum/rows;
};
return net;
}()));
