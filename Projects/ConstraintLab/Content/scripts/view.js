// Each solver instance has a local frame; the gallery places those frames in a grid.
// Every rope reads its own GPU result, never a copy of the first rope's positions.
Lab.homeCamera=function(){
    var side=Math.sqrt(Lab.viewCopies||1);
    return side===1?{target:[0,2.7,0],yaw:0.28,pitch:0.23,distance:14,fov:0.65}:
        {target:[0,1.8,0],yaw:0.08,pitch:0.95,distance:side*14,fov:0.75};
};
Lab.camera=Lab.homeCamera();Lab.drawClock=0;Lab.layout='';Lab.entities=[];Lab.ropeViews=[];Lab.viewCopies=0;
Lab.mesh=function(name,p,s,material,mesh){
    var id=Engine.create({name:name,persistent:false,components:{transform:{position:p,scale:s},render:{mesh:mesh||Lab.boxMesh,material:Lab.materials[material]}}});
    Lab.entities.push(id);return id;
};
Lab.scene=function(){
    Lab.boxMesh=Engine.asset('/Engine/Meshes/Box');Lab.rodMesh=Engine.asset('/Game/Models/Rod');Lab.sphereMesh=Engine.asset('/Game/Models/Sphere');Lab.standMesh=Engine.asset('/Game/Models/Stand');Lab.materials={};
    ['Node','Edge','Anchor','Floor','Grid','Frame','Obstacle'].forEach(function(n){Lab.materials[n]=Engine.asset('/Game/Materials/'+n);});
};
Lab.bindPoint=function(id,index,ox,oz,size){
    var e=Lab.X.expression(3);
    Engine.addComponent(id,'dynamicsBinding',{model:Lab.modelId,direction:'output',interpolation:0.07,variables:[{set:Lab.variables,index:index}],
        mapping:e.finish([e.add(e.input(0),ox),e.input(1),e.add(e.input(2),oz),1,0,0,0,size,size,size])});
};
Lab.bindSpan=function(id,a,b,ox,oz){
    var e=Lab.X.expression(6),p=[e.input(0),e.input(1),e.input(2)],q=[e.input(3),e.input(4),e.input(5)],d=e.vsub(q,p);
    var length=e.max(e.length(d),0.000001),w=e.sqrt(e.max(0,e.mul(e.add(1,e.div(d[1],length)),0.5)));
    var safe=e.max(w,0.00001),flip=e.less(w,0.00001),mid=e.scale(e.vadd(p,q),0.5);
    Engine.addComponent(id,'dynamicsBinding',{model:Lab.modelId,direction:'output',interpolation:0.07,variables:[{set:Lab.variables,index:a},{set:Lab.variables,index:b}],
        mapping:e.finish([e.add(mid[0],ox),mid[1],e.add(mid[2],oz),e.select(flip,0,w),
            e.select(flip,1,e.div(d[2],e.mul(length,e.mul(2,safe)))),0,e.select(flip,0,e.neg(e.div(d[0],e.mul(length,e.mul(2,safe))))),0.06,length,0.06])});
};
Lab.arrange=function(copies){
    if(Lab.viewCopies===copies){Lab.ropeViews.forEach(function(view){Engine.enabled(view.controller,true);Engine.visible(view.anchor,true);});return;}
    Lab.entities.forEach(function(id){Engine.destroy(id);});Lab.entities=[];Lab.ropeViews=[];
    Lab.viewCopies=copies;
    var side=Math.sqrt(copies),single=copies===1;
    // Distant gallery ropes use fewer visual spans, while all 64 solver nodes remain active.
    var spans=single?63:12;
    for(var rope=0;rope<copies;++rope){
        var ox=(rope%side-(side-1)/2)*10,oz=(Math.floor(rope/side)-(side-1)/2)*8;
        var view={x:ox,z:oz,first:rope*Lab.count,nodes:[],wires:[]};Lab.ropeViews.push(view);
        function prop(name,p,s,material,mesh){return Lab.mesh(name,[p[0]+ox,p[1],p[2]+oz],s,material,mesh);}
        if(single)prop('Platform '+rope,[0,-0.18,0],[9,0.35,6.5],'Floor');
        else prop('Stand '+rope,[0,0,0],[1,1,1],'Floor',Lab.standMesh);
        if(single){
            for(var k=-4;k<=4;++k)prop('Floor line',[k,0,0],[0.012,0.012,6.5],'Grid');
            for(k=-3;k<=3;++k)prop('Floor line',[0,0,k],[9,0.012,0.012],'Grid');
        }
        prop('Collision sphere '+rope,[0,1.8,0],[1.1,1.1,1.1],'Obstacle',Lab.sphereMesh);
        if(single)prop('Sphere pedestal',[0,0.35,0],[0.8,0.7,0.8],'Frame');
        for(var direction=-1;direction<=1;direction+=2){
            if(single)prop('Support foot',[direction*3.5,0.1,0],[0.7,0.2,0.7],'Frame');
            if(single)prop('Support pole',[direction*3.5,2.5,0.22],[0.045,5,0.045],'Frame');
            if(!single&&direction===-1)continue;
            var anchor=prop('Attachment',[direction*3.5,5,0],[0.13,0.13,0.13],'Anchor',Lab.sphereMesh);
            if(direction===1){view.anchor=anchor;Lab.bindPoint(anchor,view.first+Lab.count-1,ox,oz,0.13);}
        }
        var input=Lab.X.expression(10);
        view.controller=Engine.create({name:'Attachment input',persistent:false,components:{transform:{position:Lab.right},
            dynamicsBinding:{model:Lab.modelId,direction:'input',variables:[{set:Lab.variables,index:view.first+Lab.count-1}],
                mapping:input.finish([input.input(0),input.input(1),input.input(2)])}}});
        Lab.entities.push(view.controller);
        if(single)for(var i=0;i<Lab.count;++i){var node=prop('Rope joint '+i,Lab.initial(i),[0.06,0.06,0.06],'Node',Lab.sphereMesh);
            view.nodes.push(node);Lab.bindPoint(node,view.first+i,ox,oz,0.06);}
        for(var span=0;span<spans;++span){
            var a=Math.round(span*(Lab.count-1)/spans),b=Math.round((span+1)*(Lab.count-1)/spans);
            view.wires.push({a:(view.first+a)*3,b:(view.first+b)*3,
                id:prop('Rope '+rope+' segment '+span,[0,3,0],[0.06,Lab.rest*(b-a),0.06],'Edge',Lab.rodMesh)});
            Lab.bindSpan(view.wires[view.wires.length-1].id,view.first+a,view.first+b,ox,oz);
        }
    }
    var lightScale=single?1:side*0.9;
    Lab.entities.push(Engine.light(0,9*lightScale,lightScale,0.4*lightScale,0.82,0.92,1,90*lightScale*lightScale));
    Lab.entities.push(Engine.light(-7*lightScale,5*lightScale,-4*lightScale,0.3*lightScale,0.3,0.75,1,55*lightScale*lightScale));
    Lab.entities.push(Engine.light(6*lightScale,6*lightScale,6*lightScale,0.3*lightScale,1,0.65,0.34,55*lightScale*lightScale));
    Lab.camera=Lab.homeCamera();Engine.view.set({camera:Lab.camera});
    Engine.log('ROPE_LAB scene: '+copies+' visible ropes; '+copies*spans+' rendered spans');
};
Lab.draw=function(dt){
    Lab.drawClock+=dt;if(!Lab.sample||Lab.drawClock<0.1)return;Lab.drawClock=0;
    // Error covers every physical segment in every completed rope sample.
    var sum=0,q=Lab.sample;
    for(var i=0;i<Lab.total-1;++i)if(i%Lab.count!==Lab.count-1){
        var a=i*3,dx=q[a+3]-q[a],dy=q[a+4]-q[a+1],dz=q[a+5]-q[a+2];
        sum+=Math.abs(Math.sqrt(dx*dx+dy*dy+dz*dz)-Lab.rest)/Lab.rest;
    }
    Lab.strain=sum/(Lab.viewCopies*(Lab.count-1));
};
Lab.input=function(input){
    if(!input.pointerCaptured){var moved=false;
        if(input.middle){Lab.camera.yaw-=input.dx*0.006;Lab.camera.pitch=Math.max(-0.1,Math.min(1.35,Lab.camera.pitch+input.dy*0.005));moved=true;}
        if(input.wheel){var maxDistance=Math.min(120,Lab.homeCamera().distance*2);
            Lab.camera.distance=Math.max(8,Math.min(maxDistance,Lab.camera.distance*Math.pow(0.9,input.wheel)));moved=true;}
        if(moved)Engine.view.set({camera:Lab.camera});}
};
