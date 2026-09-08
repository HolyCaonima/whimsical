HE.camera = {target:[0,1,0],yaw:0.65,pitch:0.45,distance:15,fov:0.62};
HE.rect = {x:224,y:106,width:900,height:600};
HE.pickTicket = null; HE.pickEpoch = 0; HE.drag = null; HE.navigation = null;
// The viewport is an interactive RmlUi surface: use its event stream, because
// Engine intentionally consumes gameplay mouse deltas and keys over UI.
HE.pointer={x:0,y:0,dx:0,dy:0,wheel:0};HE.keys={};HE.pressed={};HE.editingText=false;
HE.pointerDown=function(ev){
    var p=ev.parameters;HE.pointer.x=p.mouse_x;HE.pointer.y=p.mouse_y;HE.pointer.dx=HE.pointer.dy=0;
    HE.cancelPick();
    if(p.button===1){HE.navigation='orbit';HE.rightGesture={x:p.mouse_x,y:p.mouse_y,moved:false};}
    else if(p.button===2)HE.navigation='pan';
    else if(p.button===0)HE.pick(p.mouse_x,p.mouse_y,!!p.ctrl_key);
    HE.editingText=false;HE.el('viewport').focus();
};
HE.pointerMove=function(ev){var p=ev.parameters;if(HE.panelDrag)HE.movePanel(p.mouse_x,p.mouse_y);if(HE.rightGesture&&Math.abs(p.mouse_x-HE.rightGesture.x)+Math.abs(p.mouse_y-HE.rightGesture.y)>4)HE.rightGesture.moved=true;HE.pointer.dx+=p.mouse_x-HE.pointer.x;HE.pointer.dy+=p.mouse_y-HE.pointer.y;HE.pointer.x=p.mouse_x;HE.pointer.y=p.mouse_y;};
HE.pointerUp=function(ev){
    var p=ev.parameters,g=HE.rightGesture;
    HE.panelDrag=null;HE.navigationEnded=HE.navigation;HE.navigation=null;
    if(HE.drag)HE.dragTo(p.mouse_x,p.mouse_y);HE.endDrag(false);
    if(p.button!==1)return;
    HE.rightGesture=null;
    if(g&&!g.moved&&Math.abs(p.mouse_x-g.x)+Math.abs(p.mouse_y-g.y)<=4){
        HE.navigationEnded=null;HE.pointer.dx=HE.pointer.dy=0;
        HE.pick(p.mouse_x,p.mouse_y,false,function(entity){HE.actorContext(entity,p.mouse_x,p.mouse_y);});
    }
};
HE.pointerWheel=function(ev){HE.pointer.wheel-=ev.parameters.wheel_delta_y;ev.stopPropagation();};
HE.keyEvent=function(ev,down){
    var p=ev.parameters,k=p.key_identifier,code=k>=12&&k<=37?k+53:({1:32,69:8,70:9,72:13,81:27,99:46,138:16,139:16,140:17,141:17})[k];
    HE.keys[16]=!!p.shift_key;HE.keys[17]=!!p.ctrl_key;
    if(down&&HE.rightGesture&&[87,65,83,68,81,69].indexOf(code)>=0)HE.rightGesture.moved=true;
    if(code){if(down&&!HE.keys[code]&&!HE.browserFocused)HE.pressed[code]=true;HE.keys[code]=down;}
};
HE.cancelPick = function () { if (HE.pickTicket) Engine.cancelPixels(HE.pickTicket); HE.pickTicket = null; HE.pickCallback=null; ++HE.pickEpoch; };
HE.createTools = function () {
    HE.cancelPick();
    var asset = Engine.asset('/Game/Picking');
    HE.rt = Engine.renderTarget(asset);
    HE.writer = Engine.create({name:'HumanEditor ID output',persistent:false,components:{drawEntityID:{target:asset}}});
};
HE.pick = function (x,y,additive,callback) {
    HE.cancelPick();
    var info = Engine.renderTargetInfo(HE.rt);
    if (!info.width || !info.height) return;
    var p = Engine.view.pixel(x,y,HE.width,HE.height,info.width,info.height);
    if (!p) return;
    HE.pickAdditive = additive;
    HE.pickCallback=callback;
    HE.pickTicket = Engine.readPixels(HE.rt,{x:p.x,y:p.y,width:1,height:1,rtVersion:info.rtVersion});
};
HE.pollPick = function () {
    if (!HE.pickTicket) return;
    var result = Engine.pollPixels(HE.pickTicket);
    if (!result) return;
    HE.pickTicket = null;
    if (result.status === 'ready') {
        HE.lastPick = {entity:result.data[0],frame:result.renderFrame,tick:result.sourceTick};
        var callback=HE.pickCallback;HE.pickCallback=null;
        if(callback)callback(result.data[0]);else HE.select(result.data[0], HE.pickAdditive);
    } else if (result.status !== 'stale-version' && result.status !== 'invalidated') HE.log('Picking: ' + result.status);
};
HE.dot = function(a,b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
HE.basis = function () {
    var c=HE.camera, sy=Math.sin(c.yaw),cy=Math.cos(c.yaw),sp=Math.sin(c.pitch),cp=Math.cos(c.pitch);
    return {right:[cy,0,-sy], up:[-sy*sp,cp,-cy*sp], forward:[-sy*cp,-sp,-cy*cp]};
};
HE.project = function (p) {
    var c=HE.camera,b=HE.basis(),v=[p[0]-c.target[0],p[1]-c.target[1],p[2]-c.target[2]],z=HE.dot(v,b.forward)+c.distance;
    if(z<=0.1)return null;
    var scale=HE.rect.height/(2*Math.tan(c.fov/2)*z);
    return {x:HE.rect.x+HE.rect.width/2+HE.dot(v,b.right)*scale,y:HE.rect.y+HE.rect.height/2-HE.dot(v,b.up)*scale,scale:scale};
};
HE.qmul = function(a,b){return [a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]];};
HE.rotateVector = function(q,v){var r=HE.qmul(HE.qmul(q,[v[0],v[1],v[2],0]),[-q[0],-q[1],-q[2],q[3]]);return r.slice(0,3);};
HE.axisVector = function (axis, e) {
    var v=axis==='x'?[1,0,0]:axis==='y'?[0,1,0]:[0,0,1];
    if(HE.space==='local') {var q=Engine.position(e).rotation;v=HE.rotateVector([q.x,q.y,q.z,q.w],v);}
    return v;
};
HE.focus = function () {
    var selected=HE.selected.filter(function(e){return Engine.hasComponent(e,'transform');});
    if(!selected.length)return;
    var center=[0,0,0],size=2;
    selected.forEach(function(e){var p=Engine.position(e);center[0]+=p.x;center[1]+=p.y;center[2]+=p.z;
        if(Engine.hasComponent(e,'render'))size=Math.max(size,Math.max.apply(Math,Engine.component(e,'render').scale));});
    HE.camera.target=center.map(function(v){return v/selected.length;});HE.camera.distance=Math.max(4,size*3);HE.applyCamera();
};
HE.applyCamera = function(){HE.cancelPick();Engine.view.set({camera:HE.camera});};
HE.setMode = function(mode){HE.mode=mode;['move','rotate','scale'].forEach(function(m){HE.el('tool-'+m).setClass('active',m===mode);});};
HE.beginDrag = function(axis,x,y){
    HE.editable();
    var roots=HE.roots(HE.selected).filter(function(e){return Engine.hasComponent(e,'transform');});
    if(!roots.length)return;
    HE.cancelPick(); HE.axis=axis;
    HE.drag={before:HE.checkpoint(),x:x,y:y,changed:false,rows:roots.map(function(e){return {e:e,pose:Engine.position(e),render:Engine.hasComponent(e,'render')?Engine.component(e,'render'):null,axis:HE.axisVector(axis,e)};})};
};
HE.dragTo = function(x,y){
    var d=HE.drag;if(!d)return;
    var dx=x-d.x,dy=y-d.y;if(Math.abs(dx)+Math.abs(dy)<2&&!d.changed)return;
    d.changed=true;
    d.rows.forEach(function(row){
        var p=row.pose,v=row.axis,origin=HE.project([p.x,p.y,p.z]),b=HE.basis();
        if(!origin)return;
        var sx=HE.dot(v,b.right),sy=-HE.dot(v,b.up),len=sx*sx+sy*sy;
        var amount=len>0.04?(dx*sx+dy*sy)/(len*origin.scale):(dx-dy)/origin.scale;
        if(HE.mode==='move'){
            if(HE.snapEnabled)amount=Math.round(amount/HE.snap)*HE.snap;
            Engine.transform(row.e,{position:{x:p.x+v[0]*amount,y:p.y+v[1]*amount,z:p.z+v[2]*amount},rotation:p.rotation});
        }else if(HE.mode==='rotate'){
            var angle=(dx-dy)*0.01,step=HE.rotationSnap*Math.PI/180;if(HE.snapEnabled)angle=Math.round(angle/step)*step;
            var q=[v[0]*Math.sin(angle/2),v[1]*Math.sin(angle/2),v[2]*Math.sin(angle/2),Math.cos(angle/2)];
            var r=HE.qmul(q,[p.rotation.x,p.rotation.y,p.rotation.z,p.rotation.w]);
            Engine.transform(row.e,{position:{x:p.x,y:p.y,z:p.z},rotation:{x:r[0],y:r[1],z:r[2],w:r[3]}});
        }else if(row.render){
            var render=HE.copy(row.render),i=HE.axis==='x'?0:HE.axis==='y'?1:2,delta=(dx-dy)*0.01;
            if(HE.snapEnabled)delta=Math.round(delta/HE.scaleSnap)*HE.scaleSnap;
            render.scale[i]=Math.max(0.01,render.scale[i]+delta);Engine.setComponent(row.e,'render',render);
        }
    });
};
HE.endDrag = function(cancel){
    var d=HE.drag;if(!d)return;HE.drag=null;
    if(!d.changed)return;
    if(cancel){HE.pending={kind:'rollback',selection:d.before.selection,revision:d.before.revision,saveTarget:HE.source};Engine.scene.restore(d.before.snapshot);}
    else HE.commit(HE.mode+' '+HE.axis.toUpperCase(),d.before);
};
HE.updateGizmo = function(){
    var root=HE.el('gizmo'),e=HE.selected.length?HE.selected[HE.selected.length-1]:0;
    function hide(){if(HE.gizmoVisible){root.setProperty('display','none');HE.gizmoVisible=false;}HE.gizmoKey=null;}
    if(!e||!Engine.alive(e)||!Engine.hasComponent(e,'transform')||Engine.simulation.state().running){hide();return;}
    var p=Engine.position(e),point=HE.project([p.x,p.y,p.z]);
    if(!point||point.x<HE.rect.x+75||point.x>HE.rect.x+HE.rect.width-75||point.y<HE.rect.y+75||point.y>HE.rect.y+HE.rect.height-75){hide();return;}
    var key=JSON.stringify([e,p,HE.camera,HE.rect,HE.mode,HE.space]);if(HE.gizmoKey===key)return;HE.gizmoKey=key;HE.gizmoVisible=true;
    root.setProperty('display','block');root.setProperty('left',Math.round(point.x)+'px');root.setProperty('top',Math.round(point.y)+'px');
    var basis=HE.basis();
    ['x','y','z'].forEach(function(axis){
        var v=HE.axisVector(axis,e),x=HE.dot(v,basis.right)*65,y=-HE.dot(v,basis.up)*65;
        if(Math.abs(x)+Math.abs(y)<18){x=axis==='z'?-24:24;y=24;}
        var line=HE.el('line-'+axis),handle=HE.el('axis-'+axis);
        line.setProperty('width',Math.sqrt(x*x+y*y)+'px');line.setProperty('transform','rotate('+Math.atan2(y,x)+'rad)');
        handle.setProperty('left',Math.round(x-11)+'px');handle.setProperty('top',Math.round(y-12)+'px');
    });
    var label=HE.mode.toUpperCase()+' / '+HE.space;
    if(HE.gizmoLabel!==label){HE.el('gizmo-mode').setText(label);HE.gizmoLabel=label;}
};
HE.layout={left:250,right:360,bottom:270,details:0.43,content:true,maximized:false};
HE.rotationSnap=10;HE.scaleSnap=0.1;HE.cameraSpeed=1;
HE.movePanel=function(x,y){
    var l=HE.layout;
    if(HE.panelDrag==='left')l.left=x;
    if(HE.panelDrag==='right')l.right=HE.width-x;
    if(HE.panelDrag==='bottom')l.bottom=HE.height-y-28;
    if(HE.panelDrag==='details')l.details=(y-80)/(HE.height-112);
    HE.resize(HE.width,HE.height,true);
};
HE.toggleViewport=function(){HE.layout.maximized=!HE.layout.maximized;HE.resize(HE.width,HE.height,true);};
HE.toggleContent=function(){HE.layout.content=!HE.layout.content;HE.layout.maximized=false;HE.resize(HE.width,HE.height,true);};
HE.showLog=function(show){HE.logVisible=show;HE.layout.content=true;HE.layout.maximized=false;HE.resize(HE.width,HE.height,true);};
HE.resize = function(w,h,force){
    if(w===HE.width&&h===HE.height&&!force)return;
    HE.closeContext();
    HE.width=w;HE.height=h;
    var l=HE.layout,full=l.maximized;
    l.left=Math.round(Math.max(190,Math.min(l.left,w*0.25)));
    l.right=Math.round(Math.max(290,Math.min(l.right,w*0.34)));
    l.bottom=Math.round(Math.max(180,Math.min(l.bottom,h*0.48)));
    l.details=Math.max(0.25,Math.min(0.65,l.details));
    var left=full?0:l.left,right=full?0:l.right,bottom=!full&&l.content?l.bottom:0;
    HE.detailsWidth=right;
    HE.rect={x:left+4,y:108,width:Math.max(1,w-left-right-12),height:Math.max(1,h-bottom-140)};
    function box(id,x,y,bw,bh){var el=HE.el(id);el.setProperty('left',x+'px');el.setProperty('top',y+'px');el.setProperty('width',bw+'px');el.setProperty('height',bh+'px');}
    box('place',4,80,left-4,h-bottom-112);box('viewport',HE.rect.x,108,HE.rect.width,HE.rect.height);
    HE.sizePalette();
    box('viewbar',HE.rect.x,80,HE.rect.width,28);box('viewhint',HE.rect.x,108+HE.rect.height-26,HE.rect.width,26);
    var treeHeight=Math.round((h-112)*l.details);
    box('outliner',w-right-4,80,right,treeHeight);
    box('details',w-right-4,84+treeHeight,right,h-114-treeHeight);
    box('content',4,h-bottom-28,w-right-12,bottom);box('log-panel',4,h-bottom-28,w-right-12,bottom);
    HE.sizeBrowser();
    box('split-left',left,80,4,h-bottom-112);box('split-right',w-right-9,80,5,h-110);
    box('split-bottom',4,h-bottom-32,w-right-12,4);box('split-details',w-right-4,80+treeHeight,right,4);
    ['place','outliner','details','split-left','split-right','split-details'].forEach(function(id){HE.el(id).setProperty('display',full?'none':'block');});
    HE.el('content').setProperty('display',bottom?'block':'none');
    HE.el('log-panel').setProperty('display',bottom&&HE.logVisible?'block':'none');
    HE.el('split-bottom').setProperty('display',bottom?'block':'none');
    HE.el('maximize-view').setText(full?'Restore':'Maximize');
    HE.el('content-drawer').setClass('active',!!bottom);
    HE.sizeInspector();
    HE.sizeTree();
    HE.cancelPick();Engine.view.set({rectangle:HE.rect});
};
HE.input = function(dt,i){
    HE.resize(i.width,i.height);HE.pollPick();
    var motion=HE.pointer,dx=motion.dx,dy=motion.dy,wheel=motion.wheel,keys=HE.keys,pressed=HE.pressed;
    motion.dx=motion.dy=motion.wheel=0;HE.pressed={};
    var playing=Engine.simulation.state().running;
    if(HE.wasPlaying!==playing){HE.el('viewport').setProperty('pointer-events',playing?'none':'auto');HE.wasPlaying=playing;HE.doc.show();}
    if(playing)return;
    if(!i.focused){HE.closeContext();HE.rightGesture=null;HE.navigation=null;HE.panelDrag=null;HE.keys={};HE.endDrag(false);return;}
    if(HE.contextDoc)return;
    if(HE.drag)HE.dragTo(i.x,i.y);
    var nav=HE.navigation||HE.navigationEnded,b=HE.basis(),c=HE.camera,changed=false;HE.navigationEnded=null;
    if(nav){
        if(nav==='orbit'){if(HE.rightGesture&&!HE.rightGesture.moved)dx=dy=0;c.yaw-=dx*0.006;c.pitch=Math.max(-1.5,Math.min(1.5,c.pitch+dy*0.006));}
        else {var unit=c.distance*0.0015;for(var a=0;a<3;a++)c.target[a]+=(-dx*b.right[a]+dy*b.up[a])*unit;}
        changed=!!(dx||dy);
    }
    var inside=i.x>=HE.rect.x&&i.y>=HE.rect.y&&i.x<HE.rect.x+HE.rect.width&&i.y<HE.rect.y+HE.rect.height;
    if(inside&&wheel){c.distance=Math.max(0.4,Math.min(140,c.distance*Math.exp(-wheel*0.12)));changed=true;}
    if(nav==='orbit'&&!HE.editingText){
        var speed=dt*c.distance*HE.cameraSpeed*(keys[16]?1.8:0.5),f=(keys[87]?1:0)-(keys[83]?1:0),r=(keys[68]?1:0)-(keys[65]?1:0);
        for(var k=0;k<3;k++)c.target[k]+=speed*(b.forward[k]*f+b.right[k]*r);
        c.target[1]+=speed*((keys[69]?1:0)-(keys[81]?1:0));changed=changed||!!(f||r||keys[69]||keys[81]);
    }
    if(changed){if(HE.rightGesture)HE.rightGesture.moved=true;HE.applyCamera();}
    if(!HE.editingText&&!HE.modalDoc&&!HE.browserFocused){
        var ctrl=keys[17];
        if(ctrl&&pressed[32])HE.toggleContent();
        if(ctrl&&pressed[83]){if(HE.source)HE.save();else HE.saveAs();}
        if(ctrl&&pressed[90])HE.history(false);
        if(ctrl&&pressed[89])HE.history(true);
        if(ctrl&&pressed[68])HE.duplicate();
        if(ctrl&&pressed[67])HE.copySelection();
        if(ctrl&&pressed[86])HE.paste();
        if(pressed[46])HE.remove();
        if(pressed[70])HE.focus();
        if(pressed[27]){if(HE.drag)HE.endDrag(true);else HE.select(0);}
        if(!ctrl&&!nav){if(pressed[87])HE.setMode('move');if(pressed[69])HE.setMode('rotate');if(pressed[82])HE.setMode('scale');}
    }
    HE.updateGizmo();
};
