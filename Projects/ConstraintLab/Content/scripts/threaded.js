// Experiment 04 — one continuous rope repeatedly enters, traverses and leaves a sheet.
// Rope rows reuse the sheet's variables while exposed side loops add variables to
// the same R3 set. The two fixed endpoints are simultaneously cloth corners.
Lab.add((function(){
var thread={id:'threaded',tab:'连续穿布',eyebrow:'实验 04',title:'连续穿绳布',
    subtitle:'一根绳从布角出发，反复穿入布面、露出绳环并最终回到另一布角。',
    equation:'G = (V, E布面 ∪ E连续绳),  V穿入处共享',
    hint:'↑ ↓ 收紧或放松整根穿绳',
    legend:'<span class="rose">■</span>整块布面<span class="gold">■</span>布前穿绳<span class="teal">■</span>侧边绳环',
    panelTitle:'连续穿绳',
    side:32,width:5.8,height:4.5,ropeScale:1,
    patches:0,wind:false,drive:false,phase:0,kick:false,forceInput:false,anchorDirty:false};
var windStrength=4.2;

thread.sizes={label:'布面网格',options:[{label:'16²',value:16},{label:'24²',value:24},{label:'32²',value:32}],
    get:function(){return thread.side;},
    set:function(value){if(thread.side===value)return '';thread.side=value;Lab.reset();
        return '重新编译 '+value+' × '+value+' 布面中的连续穿绳';}};
thread.actions=[
    {id:'wind',key:'C',text:function(){return '侧向风';},active:function(){return thread.wind;},
     click:function(){thread.wind=!thread.wind;thread.forceInput=true;
        return thread.wind?'风穿过连续绳布结构':'侧向风停止';}},
    {id:'drive',key:'W',text:function(){return '抽动穿绳';},active:function(){return thread.drive;},
     click:function(){thread.drive=!thread.drive;thread.anchorDirty=true;
        return thread.drive?'穿绳两端开始交替抽动':'穿绳两端回到固定位置';}}];
thread.rows=[
    {kind:'stepper',id:'rope-length',label:'穿绳长度',text:function(){return thread.ropeScale.toFixed(2)+' ×';},
     step:function(sign){thread.adjust(sign*0.04);}}];
thread.keys={14:'wind',34:'drive',91:function(){thread.adjust(-0.04);},93:function(){thread.adjust(0.04);}};

thread.layout=function(){return String(thread.side);};
thread.camera=function(){return {target:[0,3.35,0],yaw:0.24,pitch:0.08,distance:11.8,fov:0.62};};
thread.note=function(){
    return '一根连续绳 · '+thread.ropeRows+' 段 · '+thread.sharedRows.length+' 次横穿布面 · '+Lab.total+' 个 R³ 变量';
};
thread.families=function(){
    return [{name:'布面结构 L = d',kind:'等式',rows:thread.structureRows},
            {name:'布面剪切 L = √2 d',kind:'等式',rows:thread.shearRows},
            {name:'布面弯曲 L = 2d',kind:'等式',rows:thread.bendRows},
            {name:'单根连续穿绳',kind:'等式',rows:thread.ropeRows},
            {name:'地面半空间',kind:'不等式',rows:Lab.total},
            {name:'整体位移阻力',kind:'持久',rows:Lab.total}];
};

thread.build=function(model){
    var X=Lab.X,n=thread.side,positions=[],fixed=[],i,j,k,rank,s;
    var top=5.8,cellX=thread.width/(n-1),cellY=thread.height/(n-1);
    function addPoint(x,y,z,isFixed){
        var id=positions.length;positions.push([x,y,z]);fixed.push(!!isFixed);return id;
    }
    for(j=0;j<n;++j)for(i=0;i<n;++i)
        addPoint((i/(n-1)-0.5)*thread.width,top-j*cellY,
                 0.025*Math.sin(i*0.73)*Math.sin(j*0.61),j===0&&(i===0||i===n-1));
    thread.fabricCount=n*n;
    thread.sharedRows=[0.1,0.3,0.5,0.7,0.9].map(function(f){return Math.round(f*(n-1));});
    thread.rowLayer=[];
    thread.sharedRows.forEach(function(row,index){thread.rowLayer[row]=index;});
    thread.anchorBase=[positions[0].slice(),positions[n-1].slice()];
    thread.leftAnchor=0;thread.path=[thread.leftAnchor];
    var firstRow=thread.sharedRows[0],firstCloth=firstRow*n,
        from=positions[thread.leftAnchor],to=positions[firstCloth];
    for(s=1;s<=4;++s){
        var f=s/5;
        thread.path.push(addPoint(from[0]+(to[0]-from[0])*f,from[1]+(to[1]-from[1])*f,
                                   0.12*Math.sin(Math.PI*f),false));
    }
    thread.path.push(firstCloth);
    for(rank=0;rank<thread.sharedRows.length;++rank){
        var row=thread.sharedRows[rank],direction=rank%2?-1:1;
        for(s=1;s<n;++s)thread.path.push(row*n+(direction>0?s:n-1-s));
        if(rank+1<thread.sharedRows.length){
            var edge=positions[thread.path[thread.path.length-1]],
                nextRow=thread.sharedRows[rank+1],next=nextRow*n+(direction>0?n-1:0),
                target=positions[next];
            for(s=1;s<=5;++s){
                f=s/6;
                thread.path.push(addPoint(edge[0]+direction*0.62*Math.sin(Math.PI*f),
                    edge[1]+(target[1]-edge[1])*f,
                    (rank%2?-0.18:0.18)*Math.sin(Math.PI*f),false));
            }
            thread.path.push(next);
        }
    }
    var last=positions[thread.path[thread.path.length-1]],right=thread.anchorBase[1];
    for(s=1;s<=8;++s){
        f=s/9;
        thread.path.push(addPoint(last[0]+(right[0]-last[0])*f+0.55*Math.sin(Math.PI*f),
            last[1]+(right[1]-last[1])*f,-0.14*Math.sin(Math.PI*f),false));
    }
    thread.rightAnchor=n-1;thread.path.push(thread.rightAnchor);
    thread.anchorIndices=[thread.leftAnchor,thread.rightAnchor];

    var total=positions.length,initial=new Float32Array(total*3),metric=new Float32Array(total*9),
        enabled=new Float32Array(total);
    thread.acceleration=new Float32Array(total*3);thread.movable=new Float32Array(total);
    for(i=0;i<total;++i){
        initial[i*3]=positions[i][0];initial[i*3+1]=positions[i][1];initial[i*3+2]=positions[i][2];
        if(!fixed[i]){
            metric[i*9]=metric[i*9+4]=metric[i*9+8]=1;
            thread.acceleration[i*3+1]=-9.81;thread.movable[i]=1;enabled[i]=1;
        }
    }
    var dofs=X.defineDofs(model,{name:'threaded positions',space:Lab.space,count:total,initial:initial,inverseMetric:metric});
    Lab.variables=dofs.set;
    var particles=X.defineObject(model,{name:'threaded nodes',kind:'collection',dofs:{position:dofs}});
    var environment=Lab.environment(model), members=[];
    for(i=0;i<total;++i)members.push(X.defineMember(particles,i));
    function links(steps){
        var a=[],b=[],pairs=[],step;
        for(step=0;step<steps.length;++step)for(j=0;j<n;++j)for(i=0;i<n;++i){
            var i2=i+steps[step][0],j2=j+steps[step][1];
            if(i2>=0&&i2<n&&j2>=0&&j2<n){a.push(j*n+i);b.push(j2*n+i2);pairs.push([members[j*n+i],members[j2*n+i2]]);}
        }
        return {rows:a.length,a:new Uint32Array(a),b:new Uint32Array(b),
                pairs:pairs};
    }
    var structure=links([[1,0],[0,1]]),shear=links([[1,1],[1,-1]]),bend=links([[2,0],[0,2]]);
    thread.structureRows=structure.rows;thread.structureA=structure.a;thread.structureB=structure.b;
    thread.shearRows=shear.rows;thread.bendRows=bend.rows;
    thread.structureRests=new Float32Array(structure.rows);
    thread.bendRests=new Float32Array(bend.rows);
    var horizontalRows=n*(n-1),horizontalBend=n*(n-2);
    for(i=0;i<structure.rows;++i)thread.structureRests[i]=i<horizontalRows?cellX:cellY;
    for(i=0;i<bend.rows;++i)thread.bendRests[i]=i<horizontalBend?2*cellX:2*cellY;
    thread.structureSet=X.pairs(Lab.distance,structure.pairs,thread.structureRests,{compliance:[Lab.compliance()]});
    thread.shearSet=X.pairs(Lab.distance,shear.pairs,[Math.sqrt(cellX*cellX+cellY*cellY)],{compliance:[0.00005]});
    thread.bendSet=X.pairs(Lab.distance,bend.pairs,thread.bendRests,{compliance:[0.0002]});

    thread.ropeRows=thread.path.length-1;
    thread.ropeA=new Uint32Array(thread.ropeRows);thread.ropeB=new Uint32Array(thread.ropeRows);
    thread.baseRests=new Float32Array(thread.ropeRows);thread.currentRests=new Float32Array(thread.ropeRows);
    for(k=0;k<thread.ropeRows;++k){
        var a=thread.path[k],b=thread.path[k+1],pa=positions[a],pb=positions[b],
            dx=pb[0]-pa[0],dy=pb[1]-pa[1],dz=pb[2]-pa[2];
        thread.ropeA[k]=a;thread.ropeB[k]=b;
        thread.baseRests[k]=Math.sqrt(dx*dx+dy*dy+dz*dz);
        thread.currentRests[k]=thread.baseRests[k]*thread.ropeScale;
    }
    var ropePairs=[];
    for(i=0;i<thread.ropeRows;++i)ropePairs.push([members[thread.ropeA[i]],members[thread.ropeB[i]]]);
    thread.ropeSet=X.pairs(Lab.distance,ropePairs,thread.currentRests,{compliance:[Lab.compliance()]});
    thread.floorSet=X.pair(Lab.floor,particles,environment,[0.04],{enabled:enabled});
    thread.dampingSet=X.pair(Lab.damping,particles,environment,[],{history:initial,
        compliance:[0.04,0.04,0.04],enabled:enabled});
    thread.wind=false;thread.drive=false;thread.phase=0;thread.kick=false;
    thread.forceInput=true;thread.anchorDirty=true;
    Lab.total=total;
    Lab.relations=thread.structureRows+thread.shearRows+thread.bendRows+thread.ropeRows+2*total;
    return initial;
};

thread.stage=function(){
    var n=thread.side,patches=Math.min(n-1,22),i,j,k;
    thread.patches=patches;
    Lab.mesh('Threaded platform',[0,-0.18,0],[9,0.35,5.5],'Floor');
    for(k=-4;k<=4;++k)Lab.mesh('Floor line',[k,0,0],[0.012,0.012,5.5],'Grid');
    for(k=-2;k<=2;++k)Lab.mesh('Floor line',[0,0,k],[9,0.012,0.012],'Grid');
    for(i=-1;i<=1;i+=2)
        Lab.mesh('Thread frame post '+i,[i*thread.width/2,2.9,0],[0.065,5.8,0.065],'Frame');
    Lab.mesh('Thread frame beam',[0,5.8,0],[thread.width+0.08,0.075,0.075],'Frame');
    thread.controllers=[];
    thread.anchorIndices.forEach(function(index,side){
        var p=thread.anchorBase[side],input=Lab.X.expression(10);
        var controller=Engine.create({name:'Thread end input '+side,persistent:false,components:{
            transform:{position:p},
            dynamicsBinding:{model:Lab.modelId,direction:'input',variables:[{set:Lab.variables,index:index}],
                mapping:input.finish([input.input(0),input.input(1),input.input(2)])}}});
        Lab.entities.push(controller);thread.controllers.push(controller);
        var marker=Lab.mesh('Shared thread corner '+side,p,[0.13,0.13,0.13],'Anchor',Lab.sphereMesh);
        Lab.bindPoint(marker,index,0,0,0.13);
    });
    for(j=0;j<patches;++j)for(i=0;i<patches;++i){
        var i0=Math.round(i*(n-1)/patches),i1=Math.round((i+1)*(n-1)/patches),
            j0=Math.round(j*(n-1)/patches),j1=Math.round((j+1)*(n-1)/patches);
        var plate=Lab.mesh('Threaded patch '+j+'/'+i,[0,3.5,0],
            [thread.width/patches,0.027,thread.height/patches],'Cloth');
        Lab.bindPatch(plate,j0*n+i0,j0*n+i1,j1*n+i0,j1*n+i1,0.027,1);
    }
    for(k=0;k<thread.ropeRows;++k){
        var a=thread.ropeA[k],b=thread.ropeB[k],offset=0;
        if(a<thread.fabricCount&&b<thread.fabricCount&&Math.floor(a/n)===Math.floor(b/n)){
            var layer=thread.rowLayer[Math.floor(a/n)];offset=layer%2?-0.075:0.075;
        }
        var wire=Lab.mesh('Continuous thread '+k,[0,3.5,0],[0.055,0.2,0.055],'Edge',Lab.rodMesh);
        Lab.bindSpan(wire,a,b,0,offset,0.055);
    }
    for(k=thread.fabricCount;k<Lab.total;++k){
        var node=Lab.mesh('Exposed thread joint '+k,[0,3.5,0],[0.052,0.052,0.052],'Obstacle',Lab.sphereMesh);
        Lab.bindPoint(node,k,0,0,0.052);
    }
    Lab.lights(1);
    Engine.log('LAB threaded stage: one rope, '+thread.ropeRows+' spans, '+thread.patches*thread.patches+' cloth patches');
};
thread.restage=function(){
    thread.controllers.forEach(function(controller,index){
        var p=thread.anchorBase[index];Engine.enabled(controller,true);
        Engine.transform(controller,{position:{x:p[0],y:p[1],z:p[2]}});
    });
};
thread.adjust=function(delta){
    thread.ropeScale=Math.max(0.86,Math.min(1.35,thread.ropeScale+delta));Lab.dirty=true;
};
thread.apply=function(){
    var structureCompliance=new Float32Array(thread.structureRows),
        ropeCompliance=new Float32Array(thread.ropeRows),i;
    for(i=0;i<thread.structureRows;++i)structureCompliance[i]=Lab.compliance();
    for(i=0;i<thread.ropeRows;++i){
        ropeCompliance[i]=Lab.compliance();
        thread.currentRests[i]=thread.baseRests[i]*thread.ropeScale;
    }
    Lab.S.patch(Lab.owner,'compliance',thread.structureSet,0,structureCompliance);
    Lab.S.patch(Lab.owner,'parameters',thread.ropeSet,0,thread.currentRests);
    Lab.S.patch(Lab.owner,'compliance',thread.ropeSet,0,ropeCompliance);
};
thread.push=function(){thread.kick=true;return '侧向冲量沿连续穿绳与整块布面共同传播';};
thread.inputs=function(writes){
    var i;
    if(thread.wind||thread.forceInput){
        for(i=0;i<Lab.total;++i)
            thread.acceleration[i*3+2]=thread.wind&&thread.movable[i]?
                windStrength*(0.62+0.38*Math.sin(thread.phase*2.1+i*0.07)):0;
        thread.forceInput=true;
    }
    if(thread.wind||thread.drive)thread.phase+=1/30;
    if(thread.forceInput){
        writes.push({field:'acceleration',set:Lab.variables,first:0,values:thread.acceleration});
        thread.forceInput=thread.wind;
    }
    if(thread.drive)thread.anchorDirty=true;
    if(thread.anchorDirty){
        var lift=thread.drive?0.34*Math.sin(thread.phase*1.8):0,
            pull=thread.drive?0.22*Math.sin(thread.phase*1.1):0;
        thread.controllers.forEach(function(controller,index){
            var b=thread.anchorBase[index],sign=index?1:-1;
            Engine.transform(controller,{position:{x:b[0]+sign*pull,y:b[1]+sign*lift,z:b[2]}});
        });
    }
    if(thread.kick){
        var velocity=new Float32Array(Lab.total*3);
        for(i=0;i<Lab.total;++i)if(thread.movable[i]){
            velocity[i*3]=0.5;velocity[i*3+1]=0.25;velocity[i*3+2]=3.4;
        }
        writes.push({field:'velocity',set:Lab.variables,first:0,values:velocity});
    }
    var step=thread.kick||thread.anchorDirty;thread.kick=false;thread.anchorDirty=false;
    return step;
};
thread.measure=function(q){
    var sum=0,rows=0,i;
    function error(a,b,rest){
        a*=3;b*=3;var dx=q[b]-q[a],dy=q[b+1]-q[a+1],dz=q[b+2]-q[a+2];
        sum+=Math.abs(Math.sqrt(dx*dx+dy*dy+dz*dz)-rest)/rest;++rows;
    }
    for(i=0;i<thread.structureRows;++i)
        error(thread.structureA[i],thread.structureB[i],thread.structureRests[i]);
    for(i=0;i<thread.ropeRows;++i)
        error(thread.ropeA[i],thread.ropeB[i],thread.currentRests[i]);
    return sum/rows;
};
return thread;
}()));
