// Gizmos are transient scene entities. Colour and picking share the engine's mesh rasterizer.
HE.gizmoAxes=['x','y','z'];HE.gizmoPlanes=['xy','xz','yz'];HE.gizmoRadius=86;
HE.cross=function(a,b){return [a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];};
HE.unit=function(v){var length=Math.sqrt(HE.dot(v,v));return v.map(function(x){return x/length;});};
HE.qaxis=function(v,angle){var s=Math.sin(angle/2);return [v[0]*s,v[1]*s,v[2]*s,Math.cos(angle/2)];};
HE.qfromZ=function(v){return v[2]<-0.999999?[1,0,0,0]:HE.unit4([-v[1],v[0],0,1+v[2]]);};
HE.unit4=function(q){var n=Math.sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);return q.map(function(v){return v/n;});};
HE.pose=function(p,q){return {position:{x:p[0],y:p[1],z:p[2]},rotation:{x:q[0],y:q[1],z:q[2],w:q[3]}};};
HE.eye=function(){var b=HE.basis(),c=HE.camera;return c.target.map(function(v,i){return v-b.forward[i]*c.distance;});};
HE.mouseRay=function(x,y){
    var b=HE.basis(),r=HE.rect,t=Math.tan(HE.camera.fov/2),sx=(2*(x-r.x)/r.width-1)*t*r.width/r.height,sy=(1-2*(y-r.y)/r.height)*t;
    return HE.unit(b.forward.map(function(v,i){return v+b.right[i]*sx+b.up[i]*sy;}));
};
HE.setMode=function(mode){
    if(HE.drag)return;
    HE.cancelPick();HE.mode=mode;HE.updateGizmo();
};
HE.createGizmo=function(){
    var root=Engine.create({name:'Transform gizmo',persistent:false,enabled:false,components:{transform:{position:[0,0,0]}}});
    var parent=Engine.entity(root).id;
    var g=HE.gizmo={root:root,parent:parent,handles:{},byEntity:{},meshes:{},hover:null,visible:false};
    ['Move','Scale','Arc','Ring','Plane'].forEach(function(name){g.meshes[name]=Engine.asset('/Game/Models/Gizmo/'+name);});
    HE.gizmoAxes.concat(HE.gizmoPlanes).forEach(function(axis){
        var i=HE.gizmoAxes.indexOf(axis.length===1?axis:({xy:'z',xz:'y',yz:'x'})[axis]);
        var color=i===0?[0.94,0.19,0.16]:i===1?[0.30,0.86,0.20]:[0.18,0.42,1];
        var render={mesh:g.meshes.Move,scale:[1,1,1],overlay:true,overlayColor:color,castShadow:false};
        var entity=Engine.create({name:'Gizmo '+axis.toUpperCase(),persistent:false,components:{transform:{parent:parent,position:[0,0,0]},render:render}});
        g.handles[axis]={entity:entity,color:color,render:render};g.byEntity[entity]=axis;
    });
    HE.gizmoKey=null;
};
HE.gizmoAxis=function(entity){return HE.gizmo?HE.gizmo.byEntity[entity]:null;};
HE.gizmoHighlight=function(axis){
    var g=HE.gizmo;if(g.hover===axis)return;g.hover=axis;
    Object.keys(g.handles).forEach(function(a){var row=g.handles[a];row.render.overlayColor=axis&&axis.indexOf(a)>=0?[1,0.87,0.1]:row.color;Engine.setComponent(row.entity,'render',row.render);});
};
HE.updateGizmo=function(){
    var g=HE.gizmo,e=HE.selected.length?HE.selected[HE.selected.length-1]:0;
    function hide(){
        if(g.visible){Engine.enabled(g.root,false);HE.el('gizmo').setProperty('display','none');g.visible=false;}
        HE.gizmoKey=null;g.frame=null;
    }
    if(!e||!Engine.alive(e)||!Engine.hasComponent(e,'transform')||Engine.simulation.state().running){hide();return;}
    var p=Engine.position(e),position=[p.x,p.y,p.z],point=HE.project(position);
    if(!point||point.x<HE.rect.x||point.x>HE.rect.x+HE.rect.width||point.y<HE.rect.y||point.y>HE.rect.y+HE.rect.height){hide();return;}
    // Freeze the drag frame: local rotation must not move the ring under the pointer.
    if(HE.drag){
        if(HE.drag.mode==='move'){
            Engine.transform(g.root,HE.pose(position,[0,0,0,1]));
            HE.el('gizmo').setProperty('left',Math.round(point.x)+'px');HE.el('gizmo').setProperty('top',Math.round(point.y)+'px');
        }
        return;
    }
    var key=JSON.stringify([e,p,HE.camera,HE.rect,HE.mode,HE.gizmoSpace()]);if(HE.gizmoKey===key)return;
    HE.gizmoKey=key;g.visible=true;Engine.enabled(g.root,true);HE.gizmoHighlight(null);
    var radius=HE.gizmoRadius/point.scale,eye=HE.eye(),toward=HE.unit(position.map(function(v,i){return eye[i]-v;}));
    g.frame={position:position,point:point,radius:radius,axes:{},eye:eye};
    Engine.transform(g.root,HE.pose(position,[0,0,0,1]));
    HE.gizmoAxes.forEach(function(axis){
        var n=HE.axisVector(axis,e),q=HE.qfromZ(n),u=HE.rotateVector(q,[1,0,0]),v=HE.rotateVector(q,[0,1,0]);
        var mesh=HE.mode==='move'?'Move':HE.mode==='scale'?'Scale':'Arc';
        if(HE.mode==='rotate'){
            var a=HE.dot(toward,u),b=HE.dot(toward,v);
            if(a*a+b*b<0.005)mesh='Ring';
            else {var angle=Math.atan2(b,a);q=HE.qmul(q,HE.qaxis([0,0,1],angle));u=HE.rotateVector(q,[1,0,0]);v=HE.rotateVector(q,[0,1,0]);}
        }
        g.frame.axes[axis]={normal:n,u:u,v:v,full:mesh==='Ring'};
        var row=g.handles[axis];row.render.mesh=g.meshes[mesh];row.render.scale=[radius,radius,radius];
        Engine.setComponent(row.entity,'transform',{parent:g.parent,position:[0,0,0],rotation:q});Engine.setComponent(row.entity,'render',row.render);
    });
    HE.gizmoPlanes.forEach(function(axes){
        var u=g.frame.axes[axes[0]].normal,v=g.frame.axes[axes[1]].normal,n=HE.cross(u,v);
        g.frame.axes[axes]={normal:n,u:u,v:v};
        var row=g.handles[axes];
        // A plane seen edge-on has no usable area to pick or drag.
        Engine.enabled(row.entity,HE.mode!=='rotate'&&Math.abs(HE.dot(n,toward))>0.12);
        var q=axes==='xy'?[0,0,0,1]:axes==='xz'?HE.qaxis([1,0,0],Math.PI/2):[0.5,0.5,0.5,0.5];
        if(HE.gizmoSpace()==='local')q=HE.qmul([p.rotation.x,p.rotation.y,p.rotation.z,p.rotation.w],q);
        row.render.mesh=g.meshes.Plane;row.render.scale=[radius,radius,radius];
        Engine.setComponent(row.entity,'transform',{parent:g.parent,position:[0,0,0],rotation:q});Engine.setComponent(row.entity,'render',row.render);
    });
    var root=HE.el('gizmo');root.setProperty('display','block');root.setProperty('left',Math.round(point.x)+'px');root.setProperty('top',Math.round(point.y)+'px');
    ['move','rotate','scale'].forEach(function(mode){root.setClass(mode,HE.mode===mode);});
    HE.el('gizmo-mode').setText(({move:'W  MOVE',rotate:'E  ROTATE',scale:'R  SCALE'})[HE.mode]+' / '+HE.gizmoSpace());
};
HE.updateGizmoHover=function(){
    var g=HE.gizmo,p=HE.pointer,r=HE.rect;
    if(!g.visible||HE.drag||HE.navigation||HE.navigationEnded||HE.pickTicket||p.left||HE.modalDoc)return;
    if(p.x<r.x||p.y<r.y||p.x>=r.x+r.width||p.y>=r.y+r.height){HE.gizmoHighlight(null);return;}
    var key=HE.gizmoKey+':'+p.x+':'+p.y;if(g.hoverKey===key)return;g.hoverKey=key;
    HE.pick(p.x,p.y,false,function(entity){HE.gizmoHighlight(HE.gizmoAxis(entity)||null);});
};
HE.rotationAngle=function(r,x,y){
    var direction=HE.mouseRay(x,y),distance=HE.dot(r.offset,r.normal)/HE.dot(direction,r.normal);
    var v=direction.map(function(d,i){return r.eye[i]+d*distance-r.pivot[i];});
    return Math.atan2(HE.dot(v,r.v),HE.dot(v,r.u));
};
HE.planePoint=function(r,x,y){
    var direction=HE.mouseRay(x,y),denominator=HE.dot(direction,r.normal);
    if(Math.abs(denominator)<0.0001)return null;
    var distance=HE.dot(r.offset,r.normal)/denominator;if(distance<=0)return null;
    var delta=direction.map(function(v,i){return r.eye[i]+v*distance-r.pivot[i];});
    return [HE.dot(delta,r.u),HE.dot(delta,r.v)];
};
HE.beginDrag=function(axis,x,y){
    HE.editable();
    var roots=HE.roots(HE.selected).filter(function(e){return Engine.hasComponent(e,'transform');});
    if(!roots.length)return;
    HE.cancelPick();HE.axis=axis;
    var frame=HE.gizmo.frame,axisFrame=frame.axes[axis];
    HE.drag={before:HE.checkpoint(),mode:HE.mode,x:x,y:y,changed:false,pivot:frame.position,radius:frame.radius,
        axes:axis.split(''),vectors:axis.split('').map(function(a){return frame.axes[a].normal;}),rows:roots.map(function(e){
        return {e:e,pose:Engine.position(e),render:Engine.hasComponent(e,'render')?Engine.component(e,'render'):null};
    })};
    HE.gizmoHighlight(axis);
    if(axis.length===2){
        var plane=HE.drag.plane={eye:frame.eye,pivot:frame.position,normal:axisFrame.normal,u:axisFrame.u,v:axisFrame.v};
        plane.offset=plane.pivot.map(function(v,i){return v-plane.eye[i];});
        plane.start=HE.planePoint(plane,x,y);
        if(!plane.start){HE.endDrag(false);return;}
    }
    if(HE.mode==='rotate'){
        var r=HE.drag.rotation={eye:frame.eye,pivot:frame.position,normal:axisFrame.normal,u:axisFrame.u,v:axisFrame.v,angle:0};
        r.offset=r.pivot.map(function(v,i){return v-r.eye[i];});
        // Ray/plane intersection is ill-conditioned edge-on. The visible arc's tangent
        // is then the only on-screen rotation direction; keep it fixed for this drag.
        if(Math.abs(HE.dot(HE.unit(r.offset),r.normal))<0.12){
            var nearest=Infinity;
            for(var i=0;i<=64;i++){
                var a=-Math.PI/2+i*Math.PI/64,c=Math.cos(a),s=Math.sin(a);
                var p=HE.project(r.pivot.map(function(v,k){return v+frame.radius*(r.u[k]*c+r.v[k]*s);}));
                var next=HE.project(r.pivot.map(function(v,k){return v+frame.radius*(r.u[k]*Math.cos(a+0.001)+r.v[k]*Math.sin(a+0.001));}));
                var tx=(next.x-p.x)/0.001,ty=(next.y-p.y)/0.001,dist=(p.x-x)*(p.x-x)+(p.y-y)*(p.y-y);
                if(dist<nearest&&tx*tx+ty*ty>16){nearest=dist;r.tangent={x:tx,y:ty};}
            }
        }else r.last=HE.rotationAngle(r,x,y);
    }
};
HE.dragTo=function(x,y){
    var d=HE.drag;if(!d)return;
    var dx=x-d.x,dy=y-d.y;if(Math.abs(dx)+Math.abs(dy)<2&&!d.changed)return;
    var planeAmounts;
    if(d.plane){
        var point=HE.planePoint(d.plane,x,y);if(!point)return;
        planeAmounts=point.map(function(v,i){return v-d.plane.start[i];});
    }
    d.changed=true;
    if(d.rotation){
        var r=d.rotation;
        if(r.tangent)r.angle=(dx*r.tangent.x+dy*r.tangent.y)/(r.tangent.x*r.tangent.x+r.tangent.y*r.tangent.y);
        else {var angle=HE.rotationAngle(r,x,y),delta=angle-r.last;r.angle+=Math.atan2(Math.sin(delta),Math.cos(delta));r.last=angle;}
        d.angle=r.angle;var step=HE.rotationSnap*Math.PI/180;if(HE.snapEnabled)d.angle=Math.round(d.angle/step)*step;
        HE.el('gizmo-mode').setText('E  ROTATE  '+(d.angle*180/Math.PI).toFixed(1)+' deg');
    }
    d.rows.forEach(function(row){
        var p=row.pose,v=d.vectors[0];
        if(d.mode==='rotate'){
            var q=HE.qaxis(v,d.angle),rotation=HE.qmul(q,[p.rotation.x,p.rotation.y,p.rotation.z,p.rotation.w]);
            var offset=HE.rotateVector(q,[p.x-d.pivot[0],p.y-d.pivot[1],p.z-d.pivot[2]]);
            Engine.transform(row.e,HE.pose(offset.map(function(v,i){return v+d.pivot[i];}),rotation));
            return;
        }
        var origin=HE.project([p.x,p.y,p.z]),b=HE.basis();if(!origin)return;
        var sx=HE.dot(v,b.right),sy=-HE.dot(v,b.up),len=sx*sx+sy*sy;
        var amounts=planeAmounts||[len>0.04?(dx*sx+dy*sy)/(len*origin.scale):(dx-dy)/origin.scale];
        amounts=amounts.map(function(amount){
            if(d.mode==='scale')amount/=d.plane?d.radius:HE.gizmoRadius/origin.scale;
            var step=d.mode==='move'?HE.snap:HE.scaleSnap;
            return HE.snapEnabled?Math.round(amount/step)*step:amount;
        });
        if(d.mode==='move'){
            var position=[p.x,p.y,p.z];
            amounts.forEach(function(amount,i){position=position.map(function(v,k){return v+d.vectors[i][k]*amount;});});
            Engine.transform(row.e,{position:{x:position[0],y:position[1],z:position[2]},rotation:p.rotation});
        }else if(row.render){
            var render=HE.copy(row.render);
            amounts.forEach(function(amount,i){var index=HE.gizmoAxes.indexOf(d.axes[i]);render.scale[index]=Math.max(0.01,render.scale[index]+amount);});
            Engine.setComponent(row.e,'render',render);
        }
    });
};
HE.endDrag=function(cancel){
    var d=HE.drag;if(!d)return;HE.drag=null;HE.gizmoKey=null;HE.gizmoHighlight(null);
    if(!d.changed)return;
    if(cancel){HE.pending={kind:'rollback',selection:d.before.selection,revision:d.before.revision,saveTarget:HE.source};Engine.scene.restore(d.before.snapshot);}
    else HE.commit(d.mode+' '+HE.axis.toUpperCase(),d.before);
};
