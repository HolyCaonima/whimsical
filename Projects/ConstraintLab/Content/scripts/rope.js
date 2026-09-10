// Experiment 01 — a rope is a chain of distance relations between R3 variables.
// Each solver instance has a local frame; the gallery places those frames in a grid,
// and every rope reads its own GPU result rather than a copy of the first rope's positions.
Lab.add((function(){
var rope={id:'rope',tab:'绳索',eyebrow:'实验 01',title:'绳索实验',
    subtitle:'距离关系连成的绳子，右端可移动或松开。',
    equation:'C(q) = ‖qᵢ − qⱼ‖ − L',
    hint:'方向键移动右端点',
    legend:'<span class="teal">■</span>绳子<span class="gold">■</span>固定端点',
    panelTitle:'绳子与端点',
    count:64,copies:1,cut:false,drive:false,kick:false,
    right:[3.5,5,0],anchorDirty:false,forceInput:false,views:[]};

rope.sizes={label:'条数',options:[{label:'1 条',value:1},{label:'16 条',value:16},{label:'64 条',value:64}],
    get:function(){return rope.copies;},
    set:function(value){if(rope.copies===value)return '';rope.copies=value;Lab.reset();return '求解并显示 '+value+' 条绳子';}};
rope.actions=[
    {id:'cut',key:'C',text:function(){return rope.cut?'接回右端':'松开右端';},active:function(){return rope.cut;},
     click:function(){rope.cut=!rope.cut;Lab.dirty=true;rope.anchorDirty=!rope.cut;
        return rope.cut?'松开右端，观察重力与碰撞':'固定右端到控制位置';}},
    {id:'drive',key:'W',text:function(){return '端点摆动';},active:function(){return rope.drive;},
     click:function(){rope.drive=!rope.drive;return rope.drive?'右端点沿前后方向摆动':'端点停止主动摆动';}}];
rope.rows=[
    {kind:'stepper',id:'height',label:'右端高度',text:function(){return rope.right[1].toFixed(1)+' m';},
     step:function(sign){rope.move(0,sign*0.3);}}];
rope.keys={14:'cut',34:'drive',
    90:function(){rope.move(-0.3,0);},91:function(){rope.move(0,0.3);},
    92:function(){rope.move(0.3,0);},93:function(){rope.move(0,-0.3);}};

// Uniform arc-length sampling makes segment lengths comparable.
rope.shape=function(){
    if(rope.initials)return;
    var curve=[],lengths=[0],total=0,i,j=1;
    for(i=0;i<=512;++i){var t=i/512,p=[-3.5+7*t,5-1.9*Math.sin(Math.PI*t),0.15*Math.sin(2*Math.PI*t)];
        if(i){var q=curve[i-1];total+=Math.sqrt(Math.pow(p[0]-q[0],2)+Math.pow(p[1]-q[1],2)+Math.pow(p[2]-q[2],2));lengths.push(total);}
        curve.push(p);}
    rope.initials=[];
    for(i=0;i<rope.count;++i){var at=total*i/(rope.count-1);while(j<512&&lengths[j]<at)++j;
        var f=(at-lengths[j-1])/(lengths[j]-lengths[j-1]);
        rope.initials.push(curve[j-1].map(function(v,k){return v+(curve[j][k]-v)*f;}));}
    rope.rest=total/(rope.count-1);
};
rope.initial=function(i){return rope.initials[i];};
rope.layout=function(){return String(rope.copies);};
rope.camera=function(){
    var side=Math.sqrt(rope.copies);
    return side===1?{target:[0,2.7,0],yaw:0.28,pitch:0.23,distance:14,fov:0.65}:
        {target:[0,1.8,0],yaw:0.08,pitch:0.95,distance:side*14,fov:0.75};
};
rope.note=function(){return rope.copies+' 条绳子全部显示 · 每条 '+rope.count+' 个求解节点';};
rope.families=function(){
    return [{name:'距离 L = '+rope.rest.toFixed(3)+' m',kind:'等式',rows:(rope.count-1)*rope.copies},
            {name:'地面半空间',kind:'不等式',rows:Lab.total},
            {name:'球面外部',kind:'不等式',rows:Lab.total},
            {name:'位移阻力',kind:'持久',rows:Lab.total}];
};

rope.build=function(model){
    var X=Lab.X,n=rope.count*rope.copies,i;
    rope.shape();
    rope.cut=false;rope.right=[3.5,5,0];rope.anchorDirty=true;rope.forceInput=true;rope.kick=false;
    var initial=new Float32Array(n*3),metric=new Float32Array(n*9),enabled=new Float32Array(n),ids=new Uint32Array(n);
    rope.acceleration=new Float32Array(n*3);
    for(i=0;i<n;++i){var local=i%rope.count,p=rope.initial(local),free=local!==0&&local!==rope.count-1;
        initial[i*3]=p[0];initial[i*3+1]=p[1];initial[i*3+2]=p[2];ids[i]=i;
        if(free){metric[i*9]=metric[i*9+4]=metric[i*9+8]=1;rope.acceleration[i*3+1]=-9.81;enabled[i]=1;}}
    Lab.variables=X.variables(model,Lab.space,{count:n,initial:initial,inverseMetric:metric});
    var m=(rope.count-1)*rope.copies,a=new Uint32Array(m),b=new Uint32Array(m);
    for(i=0;i<m;++i){a[i]=Math.floor(i/(rope.count-1))*rope.count+i%(rope.count-1);b[i]=a[i]+1;}
    rope.links=X.relations(model,Lab.distance,{endpoints:[{set:Lab.variables,indices:a},{set:Lab.variables,indices:b}],
        parameters:[rope.rest],compliance:[Lab.compliance()]});
    var points=[{set:Lab.variables,indices:ids}];
    rope.floorSet=X.relations(model,Lab.floor,{endpoints:points,parameters:[0.06],enabled:enabled});
    rope.sphereSet=X.relations(model,Lab.sphere,{endpoints:points,parameters:[0,1.8,0,1.16],enabled:enabled});
    // Retained relation history resists displacement, updated by the solver itself.
    rope.dampingSet=X.relations(model,Lab.damping,{endpoints:points,history:initial,compliance:[0.025,0.025,0.025],enabled:enabled});
    Lab.total=n;Lab.relations=m+3*n;
    return initial;
};
rope.stage=function(){
    var copies=rope.copies,side=Math.sqrt(copies),single=copies===1,i;
    // Distant gallery ropes use fewer visual spans, while all 64 solver nodes remain active.
    var spans=single?63:12;
    rope.views=[];
    if(single)Lab.platform();
    for(var index=0;index<copies;++index){
        var ox=(index%side-(side-1)/2)*10,oz=(Math.floor(index/side)-(side-1)/2)*8;
        var view={first:index*rope.count};rope.views.push(view);
        var prop=function(name,p,s,material,mesh){return Lab.mesh(name,[p[0]+ox,p[1],p[2]+oz],s,material,mesh);};
        if(!single){
            prop('Stand '+index,[0,0,0],[1,1,1],'Floor',Lab.standMesh);
            prop('Collision sphere '+index,[0,1.8,0],[1.1,1.1,1.1],'Obstacle',Lab.sphereMesh);
        }
        for(var direction=-1;direction<=1;direction+=2){
            if(single)prop('Support foot',[direction*3.5,0.1,0],[0.7,0.2,0.7],'Frame');
            if(single)prop('Support pole',[direction*3.5,2.5,0.22],[0.045,5,0.045],'Frame');
            if(!single&&direction===-1)continue;
            var anchor=prop('Attachment',[direction*3.5,5,0],[0.13,0.13,0.13],'Anchor',Lab.sphereMesh);
            if(direction===1){view.anchor=anchor;Lab.bindPoint(anchor,view.first+rope.count-1,ox,oz,0.13);}
        }
        var input=Lab.X.expression(10);
        view.controller=Engine.create({name:'Attachment input',persistent:false,components:{transform:{position:rope.right},
            dynamicsBinding:{model:Lab.modelId,direction:'input',variables:[{set:Lab.variables,index:view.first+rope.count-1}],
                mapping:input.finish([input.input(0),input.input(1),input.input(2)])}}});
        Lab.entities.push(view.controller);
        if(single)for(i=0;i<rope.count;++i){var node=prop('Rope joint '+i,rope.initial(i),[0.06,0.06,0.06],'Node',Lab.sphereMesh);
            Lab.bindPoint(node,view.first+i,ox,oz,0.06);}
        for(var span=0;span<spans;++span){
            var a=Math.round(span*(rope.count-1)/spans),b=Math.round((span+1)*(rope.count-1)/spans);
            var wire=prop('Rope '+index+' segment '+span,[0,3,0],[0.06,rope.rest*(b-a),0.06],'Edge',Lab.rodMesh);
            Lab.bindSpan(wire,view.first+a,view.first+b,ox,oz,0.06);
        }
    }
    Lab.lights(single?1:side*0.9);
    Engine.log('LAB rope stage: '+copies+' visible ropes; '+copies*spans+' rendered spans');
};
rope.restage=function(){
    rope.views.forEach(function(view){Engine.enabled(view.controller,true);Engine.visible(view.anchor,true);});
};
rope.apply=function(){
    var S=Lab.S,softness=new Float32Array((rope.count-1)*rope.copies);
    for(var i=0;i<softness.length;++i)softness[i]=Lab.compliance();
    S.patch(Lab.owner,'compliance',rope.links,0,softness);
    for(var index=0;index<rope.copies;++index){
        var end=(index+1)*rope.count-1;
        S.patch(Lab.owner,'inverseMetric',Lab.variables,end,rope.cut?[1,0,0,0,1,0,0,0,1]:[0,0,0,0,0,0,0,0,0]);
        [rope.floorSet,rope.sphereSet,rope.dampingSet].forEach(function(set){S.patch(Lab.owner,'relationEnabled',set,end,[rope.cut?1:0]);});
        rope.acceleration[end*3+1]=rope.cut?-9.81:0;
    }
    rope.forceInput=true;
    rope.views.forEach(function(view){Engine.enabled(view.controller,!rope.cut);Engine.visible(view.anchor,!rope.cut);});
};
rope.move=function(dx,dy){
    if(rope.cut)return;
    rope.right[0]=Math.max(1.5,Math.min(4.5,rope.right[0]+dx));
    rope.right[1]=Math.max(3,Math.min(6,rope.right[1]+dy));
    rope.anchorDirty=true;
};
rope.push=function(){rope.kick=true;return '给绳子一个侧向冲量';};
rope.inputs=function(writes){
    if(rope.drive&&!rope.cut){rope.right[2]=1.15*Math.sin((Lab.time+1/30)*1.8);rope.anchorDirty=true;}
    if(rope.forceInput){writes.push({field:'acceleration',set:Lab.variables,first:0,values:rope.acceleration});rope.forceInput=false;}
    if(rope.anchorDirty)
        rope.views.forEach(function(view){Engine.transform(view.controller,{position:{x:rope.right[0],y:rope.right[1],z:rope.right[2]}});});
    if(rope.kick){var velocity=new Float32Array(Lab.total*3);
        for(var i=0;i<Lab.total;++i){var local=i%rope.count;
            if(local>0&&(local<rope.count-1||rope.cut)){
                var amount=Math.sin(Math.PI*local/(rope.count-1));
                velocity[i*3+1]=2*amount;velocity[i*3+2]=5*amount;}}
        writes.push({field:'velocity',set:Lab.variables,first:0,values:velocity});}
    var step=rope.kick||rope.anchorDirty;rope.kick=false;rope.anchorDirty=false;
    return step;
};
// Error covers every physical segment in every completed rope sample.
rope.measure=function(q){
    var sum=0;
    for(var i=0;i<Lab.total-1;++i)if(i%rope.count!==rope.count-1){
        var a=i*3,dx=q[a+3]-q[a],dy=q[a+4]-q[a+1],dz=q[a+5]-q[a+2];
        sum+=Math.abs(Math.sqrt(dx*dx+dy*dy+dz*dz)-rope.rest)/rope.rest;
    }
    return sum/(rope.copies*(rope.count-1));
};
return rope;
}()));
