/* Viewport helpers are transient display meshes, same contract as gizmos. ES5 for Duktape. */
HE.helpersVisible=true;
HE.wireMeshes=['Wire','WireDim','WireTip','WireTipDim'];
HE.helperMeshes=['Grid','IconPoint','IconSpot','IconDirectional','IconRect','IconCapsule'].concat(HE.wireMeshes);
HE.lightIcons={point:'IconPoint',spot:'IconSpot',directional:'IconDirectional',rect:'IconRect',capsule:'IconCapsule'};
HE.iconPixels=40;  // billboard edge on screen, close up
HE.iconMetres=1.2; // and the world sized quad it settles into once far enough away
HE.wirePixels=0.7; // wire radius on screen
HE.lightCutoff=8;  // illuminance a beam is drawn out to
// Secondary wires read as secondary by being thinner, not darker: a dimmed amber turns to mud
// against a lit floor, while a thinner line stays the same colour everywhere.
HE.wireWeight={Wire:1,WireTip:1,WireDim:0.74,WireTipDim:0.74};
HE.createHelpers=function(){
    var root=Engine.create({name:'Editor helpers',persistent:false,enabled:false,components:{transform:{position:[0,0,0]}}});
    var meshes={},wires={};
    HE.helperMeshes.forEach(function(name){meshes[name]=Engine.asset('/Game/Models/Helper/'+name);});
    HE.wireMeshes.forEach(function(name){wires[name]=[];});
    HE.helpers={root:root,parent:Engine.entity(root).id,meshes:meshes,material:Engine.asset('/Game/Materials/HelperLight'),
        sprites:{icon:[Engine.asset('/Game/Materials/HelperIcon'),Engine.asset('/Game/Materials/HelperIconActive')],
            ghost:[Engine.asset('/Game/Materials/HelperIconGhost'),Engine.asset('/Game/Materials/HelperIconActiveGhost')]},
        records:{},byIcon:{},byWire:{},lights:[],
        wires:wires,used:{},live:{},revision:null,key:null,shown:false,serial:0};
    HE.helpers.grid=HE.helperEntity('Grid',Engine.asset('/Game/Materials/HelperGrid'),[0,0.01,0]);
};
HE.helperEntity=function(mesh,material,position){
    var h=HE.helpers;
    return Engine.create({name:'Editor helper',persistent:false,components:{
        transform:{parent:h.parent,position:position||[0,0,0]},
        render:{mesh:h.meshes[mesh],material:material||h.material}}});
};
// Selection turns the sprite white. A ring around it says "this one" too, but only the icon
// itself is guaranteed to be on screen, and a light already owns enough overlay clutter.
HE.paintIcon=function(record){
    var h=HE.helpers,active=record.active?1:0,mesh=h.meshes[record.iconMesh];
    Engine.setComponent(record.icon,'render',{mesh:mesh,material:h.sprites.icon[active]});
    Engine.setComponent(record.ghost,'render',{mesh:mesh,material:h.sprites.ghost[active]});
};
HE.helperHandle=function(entity){var h=HE.helpers;return h&&entity?h.byIcon[entity]||h.byWire[entity]||0:0;};
HE.helperTarget=function(entity){
    var h=HE.helpers;
    if(!h||!entity)return entity;
    if(entity===h.grid||entity===h.root)return 0;
    return HE.helperHandle(entity)||entity;
};
HE.toggleHelpers=function(){
    HE.helpersVisible=!HE.helpersVisible;
    HE.paintViewbar();HE.updateHelpers();
};
// Wires are instanced rods, one per edge. A single stretched mesh cannot hold a two pixel
// line while the emitter it measures changes shape, and the renderer draws no lines of its
// own, so the pool is what makes cones, circles and rectangles all read at the same weight.
HE.wireBegin=function(){var h=HE.helpers;HE.wireMeshes.forEach(function(name){h.used[name]=0;});};
HE.wirePart=function(mesh,owner){
    var h=HE.helpers,pool=h.wires[mesh],part=pool[h.used[mesh]++];
    if(!part){part=HE.helperEntity(mesh);pool.push(part);}
    h.byWire[part]=owner;
    return part;
};
HE.wireEnd=function(){
    var h=HE.helpers;
    HE.wireMeshes.forEach(function(name){
        var pool=h.wires[name],used=h.used[name],live=h.live[name]||0,i;
        for(i=used;i<live;i++)Engine.enabled(pool[i],false);
        for(i=live;i<used;i++)Engine.enabled(pool[i],true);
        h.live[name]=used;
    });
};
HE.screenSize=function(p,pixels){var point=HE.project(p);return point?pixels/point.scale:pixels*0.02;};
// A billboard pinned to forty pixels turns a wide shot into a contact sheet of icons, so the
// pixel size is a ceiling rather than a target: it stops a near light from filling the screen,
// and past the distance where the quad would grow beyond a fixture the icon is simply an
// object in the room and shrinks with everything else.
HE.iconSize=function(p){return Math.min(HE.iconMetres,HE.screenSize(p,HE.iconPixels));};
HE.wireLine=function(owner,a,b,mesh){
    var d=[b[0]-a[0],b[1]-a[1],b[2]-a[2]],length=Math.sqrt(HE.dot(d,d));
    if(!(length>0.00001))return;
    var mid=[(a[0]+b[0])/2,(a[1]+b[1])/2,(a[2]+b[2])/2],thin=HE.screenSize(mid,HE.wirePixels*HE.wireWeight[mesh]);
    Engine.setComponent(HE.wirePart(mesh,owner),'transform',{parent:HE.helpers.parent,position:mid,
        rotation:HE.qfromY([d[0]/length,d[1]/length,d[2]/length]),scale:[thin,length,thin]});
};
// Segment count follows the on-screen radius: a distant circle costs a handful of rods and a
// near one stays round.
HE.wireCircle=function(owner,center,u,v,radius,mesh){
    if(!(radius>0.0001))return;
    var point=HE.project(center),steps=point?Math.max(8,Math.min(40,Math.round(radius*point.scale*0.4))):12;
    var previous=null;
    for(var i=0;i<=steps;i++){
        var a=i*Math.PI*2/steps,c=Math.cos(a)*radius,s=Math.sin(a)*radius;
        var p=[center[0]+u[0]*c+v[0]*s,center[1]+u[1]*c+v[1]*s,center[2]+u[2]*c+v[2]*s];
        if(previous)HE.wireLine(owner,previous,p,mesh);
        previous=p;
    }
};
HE.wireArrow=function(owner,from,direction,length,mesh){
    var tip=mesh==='WireDim'?'WireTipDim':'WireTip';
    var thin=HE.screenSize(from,HE.wirePixels*HE.wireWeight[tip]),head=Math.min(length*0.4,thin*12),radius=head*0.36;
    var base=[from[0]+direction[0]*(length-head),from[1]+direction[1]*(length-head),from[2]+direction[2]*(length-head)];
    HE.wireLine(owner,from,base,mesh);
    Engine.setComponent(HE.wirePart(tip,owner),'transform',
        {parent:HE.helpers.parent,position:base,rotation:HE.qfromY(direction),scale:[radius,head,radius]});
};
HE.wireSphere=function(owner,center,radius,mesh){
    var point=HE.project(center);
    if(!(radius>0)||(point&&radius*point.scale<5))return;
    HE.wireCircle(owner,center,[1,0,0],[0,1,0],radius,mesh);
    HE.wireCircle(owner,center,[0,1,0],[0,0,1],radius,mesh);
    HE.wireCircle(owner,center,[0,0,1],[1,0,0],radius,mesh);
};
// The rim rides the sphere of radius `reach`, so every edge leaves the apex the same distance
// and the drawing states how far the beam carries instead of where some arbitrary plane sits.
HE.wireCone=function(owner,apex,u,v,axis,reach,angle,edges,mesh){
    var radius=Math.sin(angle)*reach,depth=Math.cos(angle)*reach;
    var center=[apex[0]+axis[0]*depth,apex[1]+axis[1]*depth,apex[2]+axis[2]*depth];
    HE.wireCircle(owner,center,u,v,radius,mesh);
    for(var i=0;i<edges;i++){
        var a=i*Math.PI*2/edges,c=Math.cos(a)*radius,s=Math.sin(a)*radius;
        HE.wireLine(owner,apex,[center[0]+u[0]*c+v[0]*s,center[1]+u[1]*c+v[1]*s,center[2]+u[2]*c+v[2]*s],mesh);
    }
};
// Engine lights are physical and carry no authored range, so a beam is drawn out to the
// distance where its inverse square falloff stops lighting anything.
HE.lightReach=function(light){return Math.max(0.4,Math.min(60,Math.sqrt(light.intensity/HE.lightCutoff)));};
// The billboard says where a light is; these wires say what it covers. They follow the
// selection because a room full of cones is unreadable, which is also why the sprite stays.
HE.lightWires=function(record,pose){
    var light=record.light,owner=record.entity,pos=[pose.x,pose.y,pose.z];
    var q=[pose.rotation.x,pose.rotation.y,pose.rotation.z,pose.rotation.w];
    var x=HE.rotateVector(q,[1,0,0]),y=HE.rotateVector(q,[0,1,0]),z=HE.rotateVector(q,[0,0,1]);
    function at(a,b,c){return [pos[0]+x[0]*a+y[0]*b+z[0]*c,pos[1]+x[1]*a+y[1]*b+z[1]*c,pos[2]+x[2]*a+y[2]*b+z[2]*c];}
    if(light.type==='spot'){
        var reach=HE.lightReach(light),inner=Math.min(light.innerAngle,light.outerAngle);
        HE.wireCone(owner,pos,x,y,z,reach,light.outerAngle,8,'Wire');
        if(light.outerAngle-inner>0.05)HE.wireCone(owner,pos,x,y,z,reach,inner,0,'WireDim');
        HE.wireLine(owner,pos,at(0,0,reach),'WireDim');
        HE.wireSphere(owner,pos,light.radius,'WireDim');
    }else if(light.type==='directional'){
        // Direction is all a sun has: no position, no reach, so the sheaf is screen sized.
        var span=HE.screenSize(pos,120),ring=span*0.26;
        HE.wireArrow(owner,pos,z,span,'Wire');
        HE.wireCircle(owner,pos,x,y,ring,'WireDim');
        for(var i=0;i<4;i++){
            var a=i*Math.PI/2+Math.PI/4;
            HE.wireArrow(owner,at(Math.cos(a)*ring,Math.sin(a)*ring,0),z,span*0.62,'WireDim');
        }
    }else if(light.type==='rect'){
        var w=light.width/2,h=light.height/2,corner=[at(-w,-h,0),at(w,-h,0),at(w,h,0),at(-w,h,0)];
        for(var c=0;c<4;c++)HE.wireLine(owner,corner[c],corner[(c+1)%4],'Wire');
        var thrown=Math.min(HE.lightReach(light),Math.max(w,h)*2);
        HE.wireArrow(owner,pos,z,thrown,'Wire');
        if(light.twoSided)HE.wireArrow(owner,pos,[-z[0],-z[1],-z[2]],thrown,'WireDim');
    }else if(light.type==='capsule'&&light.length>0.001){
        var half=light.length/2,r=light.radius;
        HE.wireCircle(owner,at(0,half,0),x,z,r,'Wire');
        HE.wireCircle(owner,at(0,-half,0),x,z,r,'Wire');
        HE.wireLine(owner,at(0,-half,0),at(0,half,0),'WireDim');
        [[r,0],[-r,0],[0,r],[0,-r]].forEach(function(o){HE.wireLine(owner,at(o[0],-half,o[1]),at(o[0],half,o[1]),'Wire');});
    }else HE.wireSphere(owner,pos,light.radius,'Wire');
};
// Engine.entity() materializes every component, so the light set is rescanned per edit, not per tick.
HE.rescanHelpers=function(){
    var h=HE.helpers;if(!h)return;
    h.revision=HE.revision;h.key=null;h.lights=[];
    var seen={};
    HE.entities().forEach(function(e){
        if(!Engine.hasComponent(e,'light')||!Engine.hasComponent(e,'transform'))return;
        seen[e]=true;
        var light=Engine.component(e,'light'),icon=HE.lightIcons[light.type]||HE.lightIcons.point,record=h.records[e];
        if(!record){
            record=h.records[e]={entity:e,icon:HE.helperEntity(icon,h.sprites.icon[0]),
                ghost:HE.helperEntity(icon,h.sprites.ghost[0]),iconMesh:icon,active:false};
            h.byIcon[record.icon]=e;h.byIcon[record.ghost]=e;
        }else if(record.iconMesh!==icon){
            record.iconMesh=icon;HE.paintIcon(record);
        }
        record.light=light;
        h.lights.push(record);
    });
    Object.keys(h.records).forEach(function(id){
        var record=h.records[id];
        if(seen[record.entity])return;
        Engine.destroy(record.icon);delete h.byIcon[record.icon];
        Engine.destroy(record.ghost);delete h.byIcon[record.ghost];
        delete h.records[id];
    });
};
HE.updateHelpers=function(){
    var h=HE.helpers;if(!h)return;
    var show=HE.helpersVisible&&!Engine.simulation.state().running;
    if(h.shown!==show){h.shown=show;Engine.enabled(h.root,show);}
    if(!show)return;
    if(h.revision!==HE.revision)HE.rescanHelpers();
    // Helpers only move with the camera and the selection; a drag edits poses without a revision.
    var key=HE.drag?++h.serial:JSON.stringify([HE.camera,HE.rect.width,HE.rect.height,HE.selected]);
    if(h.key===key)return;
    h.key=key;
    var billboard=HE.billboard(),selected={};
    HE.selected.forEach(function(e){selected[e]=true;});
    HE.wireBegin();
    h.lights.forEach(function(record){
        var pose=Engine.position(record.entity),pos=[pose.x,pose.y,pose.z];
        var size=HE.iconSize(pos),active=!!selected[record.entity];
        var billboarded={parent:h.parent,position:pos,scale:[size,size,size],rotation:billboard};
        Engine.setComponent(record.icon,'transform',billboarded);
        Engine.setComponent(record.ghost,'transform',billboarded);
        if(record.active!==active){record.active=active;HE.paintIcon(record);}
        if(active)HE.lightWires(record,pose);
    });
    HE.wireEnd();
};
