HE.camera = {target:[0,1,0],yaw:0.65,pitch:0.45,distance:15,fov:0.62};
HE.rect = {x:224,y:106,width:900,height:600};
HE.pickTicket = null; HE.pickEpoch = 0; HE.drag = null; HE.navigation = null;
// The viewport is an interactive RmlUi surface: use its event stream, because
// Engine intentionally consumes gameplay mouse deltas and keys over UI.
HE.pointer={x:0,y:0,dx:0,dy:0,wheel:0};HE.keys={};HE.pressed={};HE.editingText=false;
HE.pointerDown=function(ev){
    var p=ev.parameters;HE.pointer.x=p.mouse_x;HE.pointer.y=p.mouse_y;HE.pointer.dx=HE.pointer.dy=0;
    if(HE.drag)return;
    HE.cancelPick();
    if(p.button===1){HE.navigation='orbit';HE.rightGesture={x:p.mouse_x,y:p.mouse_y,moved:false};}
    else if(p.button===2)HE.navigation='pan';
    else if(p.button===0){
        HE.pointer.left=true;
        HE.pick(p.mouse_x,p.mouse_y,!!p.ctrl_key,function(entity){
            var axis=HE.gizmoAxis(entity);
            if(axis){
                if(HE.pointer.left){HE.beginDrag(axis,p.mouse_x,p.mouse_y);HE.dragTo(HE.pointer.x,HE.pointer.y);}
            }else HE.select(entity,!!p.ctrl_key);
        });
    }
    HE.editingText=false;HE.el('viewport').focus();
};
HE.pointerMove=function(ev){var p=ev.parameters;if(HE.panelDrag)HE.movePanel(p.mouse_x,p.mouse_y);if(HE.rightGesture&&Math.abs(p.mouse_x-HE.rightGesture.x)+Math.abs(p.mouse_y-HE.rightGesture.y)>4)HE.rightGesture.moved=true;HE.pointer.dx+=p.mouse_x-HE.pointer.x;HE.pointer.dy+=p.mouse_y-HE.pointer.y;HE.pointer.x=p.mouse_x;HE.pointer.y=p.mouse_y;if(HE.drag)HE.dragTo(p.mouse_x,p.mouse_y);};
HE.pointerUp=function(ev){
    var p=ev.parameters,g=HE.rightGesture;
    HE.panelDrag=null;HE.navigationEnded=HE.navigation;HE.navigation=null;
    if(p.button===0){HE.pointer.left=false;if(HE.drag)HE.dragTo(p.mouse_x,p.mouse_y);HE.endDrag(false);}
    if(p.button!==1)return;
    HE.rightGesture=null;
    if(g&&!g.moved&&Math.abs(p.mouse_x-g.x)+Math.abs(p.mouse_y-g.y)<=4){
        HE.navigationEnded=null;HE.pointer.dx=HE.pointer.dy=0;
        HE.pick(p.mouse_x,p.mouse_y,false,function(entity){HE.actorContext(HE.gizmoAxis(entity)?HE.selected[HE.selected.length-1]:entity,p.mouse_x,p.mouse_y);});
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
    HE.createGizmo();
};
HE.pick = function (x,y,additive,callback) {
    HE.cancelPick();
    var info = Engine.renderTargetInfo(HE.rt);
    if (!info.width || !info.height) return;
    var p = Engine.view.pixel(x,y,HE.width,HE.height,info.width,info.height);
    if (!p) return;
    HE.pickAdditive = additive;
    HE.pickCallback=callback;
    // Small screen-space tolerance for thin mesh handles. Colour and ID use identical geometry.
    var radius=HE.gizmo&&HE.gizmo.visible?4:0,left=Math.max(0,p.x-radius),top=Math.max(0,p.y-radius);
    var width=Math.min(info.width,p.x+radius+1)-left,height=Math.min(info.height,p.y+radius+1)-top;
    HE.pickRegion={x:p.x-left,y:p.y-top,width:width};
    HE.pickTicket = Engine.readPixels(HE.rt,{x:left,y:top,width:width,height:height,rtVersion:info.rtVersion});
};
HE.pollPick = function () {
    if (!HE.pickTicket) return;
    var result = Engine.pollPixels(HE.pickTicket);
    if (!result) return;
    HE.pickTicket = null;
    if (result.status === 'ready') {
        var region=HE.pickRegion,entity=result.data[region.y*region.width+region.x],nearest=Infinity;
        for(var i=0;i<result.data.length;i++){
            var dx=i%region.width-region.x,dy=Math.floor(i/region.width)-region.y,distance=dx*dx+dy*dy;
            if(HE.gizmoAxis(result.data[i])&&distance<nearest){nearest=distance;entity=result.data[i];}
        }
        HE.lastPick = {entity:entity,frame:result.renderFrame,tick:result.sourceTick};
        var callback=HE.pickCallback;HE.pickCallback=null;
        if(callback)callback(entity);else HE.select(entity, HE.pickAdditive);
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
HE.gizmoSpace = function () { return HE.mode==='scale'?'local':HE.space; };
HE.axisVector = function (axis, e) {
    var v=axis==='x'?[1,0,0]:axis==='y'?[0,1,0]:[0,0,1];
    if(HE.gizmoSpace()==='local') {var q=Engine.position(e).rotation;v=HE.rotateVector([q.x,q.y,q.z,q.w],v);}
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
    if(playing){HE.updateGizmo();return;}
    if(!i.focused){HE.closeContext();HE.rightGesture=null;HE.navigation=null;HE.panelDrag=null;HE.keys={};HE.pointer.left=false;HE.endDrag(false);return;}
    if(HE.contextDoc)return;
    if(HE.drag){if(pressed[27])HE.endDrag(true);HE.updateGizmo();return;}
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
    HE.updateGizmoHover();
};
