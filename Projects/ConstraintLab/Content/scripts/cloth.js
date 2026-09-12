// Experiment 01 — a sheet is a grid of R3 variables woven by three families of
// distance relations: stiff structural threads hold the weave, soft diagonals carry
// shear, and relations across two cells resist creasing. Folds come out of the mix.
Lab.add((function(){
var cloth={id:'cloth',tab:'布料',eyebrow:'实验 01',title:'布料实验',
    subtitle:'三族距离关系织成的方布，落到球面上。',
    equation:'C(q) = ‖qᵢ − qⱼ‖ − L,  L ∈ {d, √2 d, 2d}',
    hint:'↑ ↓ 调整提起高度',
    legend:'<span class="rose">■</span>布面<span class="gold">■</span>抓取角点',
    panelTitle:'布料与抓取',
    side:64,span:5.6,height:3.7,patches:0,
    held:false,wind:false,gust:1,phase:0,lift:0,target:0,kick:false,
    forceInput:false,gripDirty:false,base:null};
var strength=[3.5,8,15],gustName=['微风','阵风','强风'];

cloth.sizes={label:'网格',options:[{label:'32²',value:32},{label:'64²',value:64},{label:'96²',value:96}],
    get:function(){return cloth.side;},
    set:function(value){if(cloth.side===value)return '';cloth.side=value;Lab.reset();return '求解 '+value+' × '+value+' 个布料节点';}};
cloth.actions=[
    {id:'grip',key:'C',text:function(){return cloth.held?'放下布料':'抓起四角';},active:function(){return cloth.held;},
     click:function(){cloth.grab();return cloth.held?'提起四角，观察悬垂的褶皱':'松开四角，布料落回展台';}},
    {id:'wind',key:'W',text:function(){return '侧向风';},active:function(){return cloth.wind;},
     click:function(){cloth.blow();return cloth.wind?'加入横向阵风':'风停下来';}}];
cloth.rows=[
    {kind:'stepper',id:'lift',label:'提起高度',text:function(){return cloth.held?cloth.target.toFixed(1)+' m':'未抓取';},
     step:function(sign){cloth.raise(sign*0.4);}},
    {kind:'cycle',id:'gust',label:'风力强度',text:function(){return gustName[cloth.gust];},
     click:function(){cloth.gust=(cloth.gust+1)%3;return '风力设为'+gustName[cloth.gust];}}];
cloth.keys={14:'grip',34:'wind',91:function(){cloth.raise(0.4);},93:function(){cloth.raise(-0.4);}};

// Friction is not a solver concept: the sheet keeps its own displacement resistance,
// and that resistance grows wherever it lies against the obstacle or the floor.
cloth.define=function(){
    cloth.contact=Lab.X.defineRelation({name:'contact grip',
        objects:[{position:Lab.space},{position:Lab.space,radius:Lab.scalar,height:Lab.scalar}],parameters:3,rows:3,history:3},function(op,a,b){
        var q=a.position;
        var gap=op.min(op.sub(op.length(op.vsub(q,b.position)),op.add(b.radius[0],op.parameter(0))),
            op.sub(q[1],op.add(b.height[0],op.parameter(1))));
        var weight=op.add(1,op.mul(op.parameter(2),op.min(1,op.max(0,op.sub(1,op.div(gap,0.1))))));
        return {residual:op.scale(op.vsub(q,[op.state(0),op.state(1),op.state(2)]),weight),update:q};
    });
};
cloth.layout=function(){return String(cloth.side);};
cloth.camera=function(){return {target:[0,1.85,0],yaw:0.38,pitch:0.28,distance:11.5,fov:0.62};};
cloth.note=function(){return cloth.side+' × '+cloth.side+' 个求解节点 · '+cloth.patches+' × '+cloth.patches+' 个可视面片';};
cloth.families=function(){
    return [{name:'结构 L = d',kind:'等式',rows:cloth.structureRows},
            {name:'剪切 L = √2 d',kind:'等式',rows:cloth.shearRows},
            {name:'弯曲 L = 2d',kind:'等式',rows:cloth.bendRows},
            {name:'地面半空间',kind:'不等式',rows:Lab.total},
            {name:'球面外部',kind:'不等式',rows:Lab.total},
            {name:'接触阻力',kind:'持久',rows:Lab.total}];
};

cloth.build=function(model){
    var X=Lab.X,n=cloth.side,total=n*n,cell=cloth.span/(n-1),i,j,k;
    // Calibrate against 64²: preserve total mass and sample spatial inputs in the same coordinates.
    cloth.inverseMass=total/(64*64);
    cloth.referenceStep=63/(n-1);
    // For this two-edge chord surrogate, pure-bending residual is O(h³).
    // Its squared energy summed over O(h⁻²) links scales as h⁴.
    var bendScale=Math.pow(cloth.referenceStep,4);
    cloth.cell=cell;cloth.held=false;cloth.lift=0;cloth.target=0;cloth.kick=false;cloth.phase=0;
    cloth.forceInput=true;cloth.gripDirty=false;cloth.base=null;
    cloth.corners=[0,n-1,(n-1)*n,total-1];
    var initial=new Float32Array(total*3),metric=new Float32Array(total*9),
        enabled=new Float32Array(total);
    cloth.acceleration=new Float32Array(total*3);
    for(j=0;j<n;++j)for(i=0;i<n;++i){
        k=j*n+i;
        initial[k*3]=(i/(n-1)-0.5)*cloth.span;
        // A whisper of relief keeps the first contact from being perfectly symmetric.
        initial[k*3+1]=cloth.height+0.01*Math.sin(i*cloth.referenceStep*1.7)*Math.cos(j*cloth.referenceStep*2.3);
        initial[k*3+2]=(j/(n-1)-0.5)*cloth.span;
        metric[k*9]=metric[k*9+4]=metric[k*9+8]=cloth.inverseMass;
        cloth.acceleration[k*3+1]=-9.81;enabled[k]=1;}
    var dofs=X.defineDofs(model,{name:'cloth positions',space:Lab.space,count:total,initial:initial,inverseMetric:metric});
    Lab.variables=dofs.set;
    var particles=X.defineObject(model,{name:'cloth nodes',kind:'collection',dofs:{position:dofs}});
    var environment=Lab.environment(model), members=[];
    for(i=0;i<total;++i)members.push(X.defineMember(particles,i));
    function links(steps){
        var rows=0,s,pairs=[];
        for(s=0;s<steps.length;++s)rows+=(n-Math.abs(steps[s][0]))*(n-Math.abs(steps[s][1]));
        for(s=0;s<steps.length;++s)for(j=0;j<n;++j)for(i=0;i<n;++i){
            var i2=i+steps[s][0],j2=j+steps[s][1];
            if(i2<0||i2>=n||j2<0||j2>=n)continue;
            pairs.push([members[j*n+i],members[j2*n+i2]]);}
        return {rows:rows,pairs:pairs};
    }
    var structure=links([[1,0],[0,1]]),shear=links([[1,1],[1,-1]]),bend=links([[2,0],[0,2]]);
    cloth.structureRows=structure.rows;cloth.shearRows=shear.rows;cloth.bendRows=bend.rows;
    cloth.structure=X.pairs(Lab.distance,structure.pairs,[cell],{compliance:[Lab.compliance()]});
    cloth.shear=X.pairs(Lab.distance,shear.pairs,[cell*Math.SQRT2],{compliance:[0.00005]});
    cloth.bend=X.pairs(Lab.distance,bend.pairs,[2*cell],{compliance:[0.0002*bendScale]});
    cloth.floorSet=X.pair(Lab.floor,particles,environment,[0.03],{enabled:enabled});
    cloth.sphereSet=X.pair(Lab.sphere,particles,environment,[0.03],{enabled:enabled});
    cloth.gripSet=X.pair(cloth.contact,particles,environment,[0.03,0.03,10],{
        history:initial,compliance:[0.02*cloth.inverseMass,0.02*cloth.inverseMass,0.02*cloth.inverseMass],enabled:enabled});
    Lab.total=total;Lab.relations=structure.rows+shear.rows+bend.rows+3*total;
    return initial;
};
cloth.stage=function(){
    var n=cloth.side,patches=n-1,half=cloth.span/2,i,j;
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
    // Each row is one instance batch: all four corner streams advance by one node.
    var mapping=Lab.patchMapping(0.028,1),cell=cloth.cell,q=Lab.sample;
    for(j=0;j<patches;++j){
        var transforms=[],first=j*n;
        for(i=0;i<patches;++i){
            var a=(first+i)*3,b=a+3,c=a+n*3,d=c+3;
            transforms.push({position:[(q[a]+q[b]+q[c]+q[d])*0.25,
                (q[a+1]+q[b+1]+q[c+1]+q[d+1])*0.25,
                (q[a+2]+q[b+2]+q[c+2]+q[d+2])*0.25],scale:[cell,0.028,cell]});
        }
        var row=Engine.create({name:'Cloth row '+j,persistent:false,components:{
            transform:{},
            render:{mesh:Lab.boxMesh,material:Lab.materials.Cloth,
                instanceCount:patches,instanceTransforms:transforms},
            dynamicsBinding:{model:Lab.modelId,direction:'output',target:'renderInstances',interpolation:0.05,
                variables:[{set:Lab.variables,index:first},{set:Lab.variables,index:first+1},
                    {set:Lab.variables,index:first+n},{set:Lab.variables,index:first+n+1}],
                mapping:mapping}
        }});
        Lab.entities.push(row);
    }
    Lab.lights(1);
    Engine.log('LAB cloth stage: '+patches*patches+' instances in '+patches+' rows over '+n*n+' nodes');
};
cloth.restage=function(){
    cloth.handles.forEach(function(handle){Engine.enabled(handle,false);});
    cloth.anchors.forEach(function(marker){Engine.visible(marker,false);});
};
cloth.apply=function(){
    var S=Lab.S,softness=new Float32Array(cloth.structureRows);
    for(var i=0;i<softness.length;++i)softness[i]=Lab.compliance();
    S.patch(Lab.owner,'compliance',cloth.structure,0,softness);
    var w=cloth.inverseMass;
    cloth.corners.forEach(function(k){
        S.patch(Lab.owner,'inverseMetric',Lab.variables,k,cloth.held?[0,0,0,0,0,0,0,0,0]:[w,0,0,0,w,0,0,0,w]);
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
            a[k]=strength[cloth.gust]*(0.55+0.45*Math.sin(cloth.phase*2.3-i*cloth.referenceStep*0.4+j*cloth.referenceStep*0.15));
            a[k+2]=strength[cloth.gust]*0.3*Math.sin(cloth.phase*1.1+j*cloth.referenceStep*0.3);}
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
