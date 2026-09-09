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
    if(!part){part={entity:HE.helperEntity(mesh),geometry:null};pool.push(part);}
    h.byWire[part.entity]=owner;
    return part;
};
HE.wireEnd=function(){
    var h=HE.helpers;
    HE.wireMeshes.forEach(function(name){
        var pool=h.wires[name],used=h.used[name],live=h.live[name]||0,i;
        for(i=used;i<live;i++)Engine.enabled(pool[i].entity,false);
        for(i=live;i<used;i++)Engine.enabled(pool[i].entity,true);
        h.live[name]=used;
    });
};
// One camera basis per helper update; line width needs depth, not a full screen projection.
HE.helperProjectionScale=function(p){
    var view=HE.helpers.view,c=HE.camera;
    var z=(p[0]-c.target[0])*view.forward[0]+(p[1]-c.target[1])*view.forward[1]+(p[2]-c.target[2])*view.forward[2]+c.distance;
    return z>0.1?view.focal/z:0;
};
HE.screenSize=function(p,pixels){var scale=HE.helperProjectionScale(p);return scale?pixels/scale:pixels*0.02;};
// A billboard pinned to forty pixels turns a wide shot into a contact sheet of icons, so the
// pixel size is a ceiling rather than a target: it stops a near light from filling the screen,
// and past the distance where the quad would grow beyond a fixture the icon is simply an
// object in the room and shrinks with everything else.
HE.iconSize=function(p){return Math.min(HE.iconMetres,HE.screenSize(p,HE.iconPixels));};
// Conservative group culling: test the whole shape, not just the emitter. Keep
// depth unbounded at the far end so this does not duplicate engine clip settings.
HE.helperInView=function(p,radius,pixels){
    var view=HE.helpers.view,c=HE.camera,dx=p[0]-c.target[0],dy=p[1]-c.target[1],dz=p[2]-c.target[2];
    var z=dx*view.forward[0]+dy*view.forward[1]+dz*view.forward[2]+c.distance;
    radius+=pixels*Math.max(0.02,(z+radius)/view.focal);
    if(z+radius<=0)return false;
    var x=dx*view.right[0]+dy*view.right[1]+dz*view.right[2];
    var y=dx*view.up[0]+dy*view.up[1]+dz*view.up[2];
    return Math.abs(x)<=z*view.tanX+radius*view.planeX&&
        Math.abs(y)<=z*view.tanY+radius*view.planeY;
};
HE.wireLine=function(owner,a,b,mesh){
    var d=[b[0]-a[0],b[1]-a[1],b[2]-a[2]],length=Math.sqrt(HE.dot(d,d));
    if(!(length>0.00001))return;
    var mid=[(a[0]+b[0])/2,(a[1]+b[1])/2,(a[2]+b[2])/2];
    HE.helpers.build.parts.push({mesh:mesh,position:mid,
        pose:HE.pose(mid,HE.qfromY([d[0]/length,d[1]/length,d[2]/length])),length:length});
};
HE.wireCircleSteps=function(center,radius){
    var scale=HE.helperProjectionScale(center);
    return scale?Math.max(8,Math.min(40,Math.round(radius*scale*0.4))):12;
};
// Segment count follows the on-screen radius: a distant circle costs a handful of rods and a
// near one stays round.
HE.wireCircle=function(owner,center,u,v,radius,mesh){
    if(!(radius>0.0001))return;
    var steps=HE.wireCircleSteps(center,radius);
    HE.helpers.build.lod.push({center:center,radius:radius,steps:steps});
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
    HE.helpers.build.parts.push({mesh:tip,position:base,pose:HE.pose(base,HE.qfromY(direction)),length:head,radius:radius});
};
HE.wireSphereVisible=function(center,radius){
    var scale=HE.helperProjectionScale(center);
    return radius>0&&(!scale||radius*scale>=5);
};
HE.wireSphere=function(owner,center,radius,mesh){
    var visible=HE.wireSphereVisible(center,radius);
    HE.helpers.build.lod.push({center:center,radius:radius,visible:visible});
    if(!visible)return;
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
HE.lightWireRadius=function(light){
    if(light.type==='spot')return Math.max(HE.lightReach(light),light.radius);
    if(light.type==='rect'){
        var w=light.width/2,h=light.height/2,thrown=Math.min(HE.lightReach(light),Math.max(w,h)*2);
        // Include arrow heads as well as the emitter rectangle.
        return Math.max(Math.sqrt(w*w+h*h),thrown*1.15);
    }
    if(light.type==='capsule')return Math.sqrt(light.length*light.length/4+light.radius*light.radius);
    return light.radius;
};
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
// Cache world-space geometry separately from its camera-dependent width. Only a LOD
// transition, light edit or pose edit rebuilds cones/circles. Arrows have screen-sized
// heads (and directional lights a screen-sized span), so those shapes follow the view.
HE.updateLightWires=function(record,pose){
    var h=HE.helpers,key=JSON.stringify([record.light,pose]),geometry=record.geometry;
    var rebuild=!geometry||geometry.key!==key||record.light.type==='directional'||record.light.type==='rect';
    if(!rebuild){
        for(var i=0;i<geometry.lod.length;i++){
            var lod=geometry.lod[i];
            if(lod.steps!==undefined?HE.wireCircleSteps(lod.center,lod.radius)!==lod.steps:
                HE.wireSphereVisible(lod.center,lod.radius)!==lod.visible){rebuild=true;break;}
        }
    }
    if(rebuild){
        geometry={key:key,parts:[],lod:[]};h.build=geometry;
        HE.lightWires(record,pose);h.build=null;record.geometry=geometry;
    }
    for(var j=0;j<geometry.parts.length;j++){
        var line=geometry.parts[j],part=HE.wirePart(line.mesh,record.entity);
        var thin=line.radius!==undefined?line.radius:HE.screenSize(line.position,HE.wirePixels*HE.wireWeight[line.mesh]);
        if(part.geometry!==line){
            line.pose.scale={x:thin,y:line.length,z:thin};
            Engine.transform(part.entity,line.pose);part.geometry=line;part.thin=thin;
        }else if(part.thin!==thin){
            Engine.scale(part.entity,{x:thin,y:line.length,z:thin});part.thin=thin;
        }
    }
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
        record.pose=Engine.position(e);record.position=[record.pose.x,record.pose.y,record.pose.z];
        record.wireRadius=HE.lightWireRadius(light);
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
    var basis=HE.basis(),tanY=Math.tan(HE.camera.fov/2),tanX=tanY*HE.rect.width/HE.rect.height;
    h.view={forward:basis.forward,right:basis.right,up:basis.up,focal:HE.rect.height/(2*tanY),
        tanX:tanX,tanY:tanY,planeX:Math.sqrt(1+tanX*tanX),planeY:Math.sqrt(1+tanY*tanY)};
    var billboard=HE.billboard(),selected={};
    HE.selected.forEach(function(e){selected[e]=true;});
    HE.wireBegin();
    h.lights.forEach(function(record){
        // Camera motion uses cached world poses. Drags can move a light through
        // an ancestor, so refresh all light poses while an edit is in progress.
        if(HE.drag){record.pose=Engine.position(record.entity);record.position=[record.pose.x,record.pose.y,record.pose.z];}
        var pose=record.pose,pos=record.position;
        var size=HE.iconSize(pos),active=!!selected[record.entity];
        var visible=HE.helperInView(pos,size*Math.SQRT1_2,2);
        if(record.iconVisible!==visible){
            Engine.enabled(record.icon,visible);Engine.enabled(record.ghost,visible);record.iconVisible=visible;
        }
        if(visible){
            var billboarded=HE.pose(pos,billboard);billboarded.scale={x:size,y:size,z:size};
            Engine.transform(record.icon,billboarded);
            Engine.transform(record.ghost,billboarded);
            if(record.active!==active){record.active=active;HE.paintIcon(record);}
        }
        if(active){
            var radius=record.light.type==='directional'?HE.screenSize(pos,120)*1.2:record.wireRadius;
            if(HE.helperInView(pos,radius,HE.wirePixels+2))HE.updateLightWires(record,pose);
        }
    });
    HE.wireEnd();
};
