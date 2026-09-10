// Experiment 02 — a sheet is a grid of R3 variables woven by three families of
// distance relations: stiff structural threads hold the weave, soft diagonals carry
// shear, and relations across two cells resist creasing. Folds come out of the mix.
Lab.add((function(){
var cloth={id:'cloth',tab:'布料',eyebrow:'EXPERIMENT 02',title:'布料实验',
    subtitle:'自由下落 · 结构 / 剪切 / 弯曲关系 · 球面覆盖',
    equation:'C(q) = ‖qᵢ − qⱼ‖ − L,  L ∈ {d, √2 d, 2d}',
    legend:'<span class="rose">■</span> 布面 <span class="gold">■</span> 抓取角点',
    panelTitle:'布料与抓取',
    side:32,span:5.6,height:3.7,patches:0,
    held:false,wind:false,gust:1,phase:0,lift:0,target:0,kick:false,
    forceInput:false,gripDirty:false,base:null};
var strength=[3.5,8,15],gustName=['微风','阵风','强风'];

cloth.sizes={label:'网格',options:[{label:'16²',value:16},{label:'24²',value:24},{label:'32²',value:32}],
    get:function(){return cloth.side;},
    set:function(value){if(cloth.side===value)return '';cloth.side=value;Lab.reset();return '求解 '+value+' × '+value+' 个布料节点';}};
cloth.actions=[
    {id:'grip',text:function(){return cloth.held?'放下布料 / C':'抓起四角 / C';},active:function(){return cloth.held;},
     click:function(){cloth.grab();return cloth.held?'提起四角，观察悬垂的褶皱':'松开四角，布料落回展台';}},
    {id:'wind',text:function(){return cloth.wind?'侧向风 ON / W':'侧向风 OFF / W';},active:function(){return cloth.wind;},
     click:function(){cloth.blow();return cloth.wind?'加入横向阵风':'风停下来';}}];
cloth.rows=[
    {kind:'stepper',id:'lift',label:'提起高度',text:function(){return cloth.held?cloth.target.toFixed(1)+' m':'—';},
     step:function(sign){cloth.raise(sign*0.4);}},
    {kind:'cycle',id:'gust',label:'风力强度',text:function(){return gustName[cloth.gust];},
     click:function(){cloth.gust=(cloth.gust+1)%3;return '风力设为'+gustName[cloth.gust];}}];
cloth.keys={14:'grip',34:'wind',91:function(){cloth.raise(0.4);},93:function(){cloth.raise(-0.4);}};

// Friction is not a solver concept: the sheet keeps its own displacement resistance,
// and that resistance grows wherever it lies against the obstacle or the floor.
cloth.define=function(){
    cloth.contact=Lab.X.defineRelation({name:'contact grip',spaces:[Lab.space],parameters:6,rows:3,history:3},function(e){
        var q=e.endpoints[0],centre=[e.parameter(0),e.parameter(1),e.parameter(2)];
        var gap=e.min(e.sub(e.length(e.vsub(q,centre)),e.parameter(3)),e.sub(q[1],e.parameter(4)));
        var weight=e.add(1,e.mul(e.parameter(5),e.min(1,e.max(0,e.sub(1,e.div(gap,0.1))))));
        return {residual:e.scale(e.vsub(q,[e.state(0),e.state(1),e.state(2)]),weight),update:q};
    });
};
cloth.layout=function(){return String(cloth.side);};
cloth.camera=function(){return {target:[0,1.85,0],yaw:0.38,pitch:0.28,distance:11.5,fov:0.62};};
cloth.note=function(){return cloth.side+' × '+cloth.side+' 个求解节点 · '+cloth.patches+' × '+cloth.patches+' 个可视面片';};

cloth.build=function(model){
    var X=Lab.X,n=cloth.side,total=n*n,cell=cloth.span/(n-1),i,j,k;
    cloth.cell=cell;cloth.held=false;cloth.lift=0;cloth.target=0;cloth.kick=false;cloth.phase=0;
    cloth.forceInput=true;cloth.gripDirty=false;cloth.base=null;
    cloth.corners=[0,n-1,(n-1)*n,total-1];
    var initial=new Float32Array(total*3),metric=new Float32Array(total*9),
        enabled=new Float32Array(total),ids=new Uint32Array(total);
    cloth.acceleration=new Float32Array(total*3);
    for(j=0;j<n;++j)for(i=0;i<n;++i){
        k=j*n+i;
        initial[k*3]=(i/(n-1)-0.5)*cloth.span;
        // A whisper of relief keeps the first contact from being perfectly symmetric.
        initial[k*3+1]=cloth.height+0.01*Math.sin(i*1.7)*Math.cos(j*2.3);
        initial[k*3+2]=(j/(n-1)-0.5)*cloth.span;
        metric[k*9]=metric[k*9+4]=metric[k*9+8]=1;
        cloth.acceleration[k*3+1]=-9.81;enabled[k]=1;ids[k]=k;}
    Lab.variables=X.variables(model,Lab.space,{count:total,initial:initial,inverseMetric:metric});
    function links(steps){
        var rows=0,s,a,b,at=0;
        for(s=0;s<steps.length;++s)rows+=(n-Math.abs(steps[s][0]))*(n-Math.abs(steps[s][1]));
        a=new Uint32Array(rows);b=new Uint32Array(rows);
        for(s=0;s<steps.length;++s)for(j=0;j<n;++j)for(i=0;i<n;++i){
            var i2=i+steps[s][0],j2=j+steps[s][1];
            if(i2<0||i2>=n||j2<0||j2>=n)continue;
            a[at]=j*n+i;b[at]=j2*n+i2;++at;}
        return {rows:rows,columns:[{set:Lab.variables,indices:a},{set:Lab.variables,indices:b}]};
    }
    var structure=links([[1,0],[0,1]]),shear=links([[1,1],[1,-1]]),bend=links([[2,0],[0,2]]);
    cloth.structureRows=structure.rows;
    cloth.structure=X.relations(model,Lab.distance,{endpoints:structure.columns,parameters:[cell],compliance:[Lab.compliance()]});
    cloth.shear=X.relations(model,Lab.distance,{endpoints:shear.columns,parameters:[cell*Math.SQRT2],compliance:[0.00005]});
    cloth.bend=X.relations(model,Lab.distance,{endpoints:bend.columns,parameters:[2*cell],compliance:[0.0002]});
    var points=[{set:Lab.variables,indices:ids}];
    cloth.floorSet=X.relations(model,Lab.floor,{endpoints:points,parameters:[0.03],enabled:enabled});
    cloth.sphereSet=X.relations(model,Lab.sphere,{endpoints:points,parameters:[0,1.8,0,1.13],enabled:enabled});
    cloth.gripSet=X.relations(model,cloth.contact,{endpoints:points,parameters:[0,1.8,0,1.13,0.03,10],
        history:initial,compliance:[0.02,0.02,0.02],enabled:enabled});
    Lab.total=total;Lab.relations=structure.rows+shear.rows+bend.rows+3*total;
    return initial;
};
cloth.stage=function(){
    var n=cloth.side,patches=Math.min(n-1,26),half=cloth.span/2,i,j;
    cloth.patches=patches;
    Lab.platform();
    var grip=Lab.X.expression(10);
    cloth.handles=[];cloth.anchors=[];
    [[-1,-1],[1,-1],[-1,1],[1,1]].forEach(function(corner,index){
        var handle=Engine.create({name:'Corner grip '+index,persistent:false,components:{
            transform:{position:[corner[0]*half,cloth.height,corner[1]*half]},
            dynamicsBinding:{model:Lab.modelId,direction:'input',variables:[{set:Lab.variables,index:cloth.corners[index]}],
                mapping:grip.finish([grip.input(0),grip.input(1),grip.input(2)])}}});
        Engine.enabled(handle,false);
        Lab.entities.push(handle);cloth.handles.push(handle);
        var marker=Lab.mesh('Corner marker '+index,[corner[0]*half,cloth.height,corner[1]*half],[0.12,0.12,0.12],'Anchor',Lab.sphereMesh);
        Lab.bindPoint(marker,cloth.corners[index],0,0,0.12);
        Engine.visible(marker,false);cloth.anchors.push(marker);
    });
    // One oriented plate per visual cell; the solver keeps every node regardless of this budget.
    for(j=0;j<patches;++j)for(i=0;i<patches;++i){
        var i0=Math.round(i*(n-1)/patches),i1=Math.round((i+1)*(n-1)/patches),
            j0=Math.round(j*(n-1)/patches),j1=Math.round((j+1)*(n-1)/patches);
        var plate=Lab.mesh('Cloth patch '+j+'/'+i,[0,cloth.height,0],[cloth.span/patches,0.028,cloth.span/patches],'Cloth');
        Lab.bindPatch(plate,j0*n+i0,j0*n+i1,j1*n+i0,j1*n+i1,0.028,1);
    }
    Lab.lights(1);
    Engine.log('LAB cloth stage: '+patches*patches+' plates over '+n*n+' nodes');
};
cloth.restage=function(){
    cloth.handles.forEach(function(handle){Engine.enabled(handle,false);});
    cloth.anchors.forEach(function(marker){Engine.visible(marker,false);});
};
cloth.apply=function(){
    var S=Lab.S,softness=new Float32Array(cloth.structureRows);
    for(var i=0;i<softness.length;++i)softness[i]=Lab.compliance();
    S.patch(Lab.owner,'compliance',cloth.structure,0,softness);
    cloth.corners.forEach(function(k){
        S.patch(Lab.owner,'inverseMetric',Lab.variables,k,cloth.held?[0,0,0,0,0,0,0,0,0]:[1,0,0,0,1,0,0,0,1]);
        [cloth.floorSet,cloth.sphereSet,cloth.gripSet].forEach(function(set){S.patch(Lab.owner,'relationEnabled',set,k,[cloth.held?0:1]);});
        cloth.acceleration[k*3+1]=cloth.held?0:-9.81;});
    cloth.forceInput=true;
    cloth.handles.forEach(function(handle){Engine.enabled(handle,cloth.held);});
    cloth.anchors.forEach(function(marker){Engine.visible(marker,cloth.held);});
};
// Grabbing starts where the corners already are, so the sheet is lifted rather than snapped.
cloth.grab=function(){
    cloth.held=!cloth.held;
    if(cloth.held){
        var q=Lab.sample;
        cloth.base=cloth.corners.map(function(k){return [q[k*3],q[k*3+1],q[k*3+2]];});
        cloth.lift=0;cloth.target=3.2;
    }
    cloth.gripDirty=true;Lab.dirty=true;
};
cloth.raise=function(delta){
    if(!cloth.held)return;
    cloth.target=Math.max(0,Math.min(4.4,cloth.target+delta));
};
cloth.blow=function(){
    cloth.wind=!cloth.wind;
    if(!cloth.wind)for(var i=0;i<Lab.total;++i){cloth.acceleration[i*3]=0;cloth.acceleration[i*3+2]=0;}
    cloth.forceInput=true;
};
cloth.push=function(){cloth.kick=true;return '给布料一个侧向冲量';};
cloth.inputs=function(writes){
    var n=cloth.side,a=cloth.acceleration,i,j,k;
    if(cloth.wind){
        cloth.phase+=1/30;
        for(j=0;j<n;++j)for(i=0;i<n;++i){
            k=(j*n+i)*3;
            a[k]=strength[cloth.gust]*(0.55+0.45*Math.sin(cloth.phase*2.3-i*0.4+j*0.15));
            a[k+2]=strength[cloth.gust]*0.3*Math.sin(cloth.phase*1.1+j*0.3);}
        cloth.forceInput=true;
    }
    if(cloth.forceInput){writes.push({field:'acceleration',set:Lab.variables,first:0,values:a});cloth.forceInput=false;}
    if(cloth.held){
        var move=Math.min(Math.abs(cloth.target-cloth.lift),0.06);
        if(move>0){cloth.lift+=cloth.target>cloth.lift?move:-move;cloth.gripDirty=true;}
    }
    if(cloth.gripDirty&&cloth.base)
        cloth.handles.forEach(function(handle,index){
            var b=cloth.base[index];
            Engine.transform(handle,{position:{x:b[0],y:b[1]+cloth.lift,z:b[2]}});});
    if(cloth.kick){var velocity=new Float32Array(Lab.total*3);
        for(j=0;j<n;++j)for(i=0;i<n;++i){k=j*n+i;
            velocity[k*3+2]=3.4*Math.sin(Math.PI*i/(n-1));
            velocity[k*3+1]=1.2*Math.sin(Math.PI*j/(n-1));}
        writes.push({field:'velocity',set:Lab.variables,first:0,values:velocity});}
    var step=cloth.kick||cloth.gripDirty;cloth.kick=false;cloth.gripDirty=false;
    return step;
};
// Error covers every structural thread of the completed sample.
cloth.measure=function(q){
    var n=cloth.side,sum=0,rows=0,i,j;
    function strain(a,b){
        var dx=q[b*3]-q[a*3],dy=q[b*3+1]-q[a*3+1],dz=q[b*3+2]-q[a*3+2];
        sum+=Math.abs(Math.sqrt(dx*dx+dy*dy+dz*dz)-cloth.cell)/cloth.cell;++rows;
    }
    for(j=0;j<n;++j)for(i=0;i<n;++i){
        if(i+1<n)strain(j*n+i,j*n+i+1);
        if(j+1<n)strain(j*n+i,(j+1)*n+i);
    }
    return sum/rows;
};
return cloth;
}()));
