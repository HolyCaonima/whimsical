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
Lab.arrange=function(copies){
    if(Lab.viewCopies===copies)return;
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
            if(direction===1)view.anchor=anchor;
        }
        if(single)for(var i=0;i<Lab.count;++i)view.nodes.push(prop('Rope joint '+i,Lab.initial(i),[0.06,0.06,0.06],'Node',Lab.sphereMesh));
        for(var span=0;span<spans;++span){
            var a=Math.round(span*(Lab.count-1)/spans),b=Math.round((span+1)*(Lab.count-1)/spans);
            view.wires.push({a:(view.first+a)*3,b:(view.first+b)*3,
                id:prop('Rope '+rope+' segment '+span,[0,3,0],[0.06,Lab.rest*(b-a),0.06],'Edge',Lab.rodMesh)});
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
    Lab.readAge+=dt;Lab.drawClock+=dt;if(!Lab.sample||Lab.drawClock<1/30)return;Lab.drawClock=0;
    var t=Math.min(1,Lab.readAge/Lab.readInterval),p=Lab.displayed;t=t*t*(3-2*t);
    for(var i=0;i<p.length;++i)p[i]=Lab.from[i]+(Lab.sample[i]-Lab.from[i])*t;
    Lab.ropeViews.forEach(function(view){
        view.nodes.forEach(function(id,i){var a=(view.first+i)*3;Engine.transform(id,{position:{x:p[a]+view.x,y:p[a+1],z:p[a+2]+view.z}});});
        var end=(view.first+Lab.count-1)*3;
        Engine.visible(view.anchor,!Lab.cut);Engine.transform(view.anchor,{position:{x:p[end]+view.x,y:p[end+1],z:p[end+2]+view.z}});
        view.wires.forEach(function(wire){
            var a=wire.a,b=wire.b,dx=p[b]-p[a],dy=p[b+1]-p[a+1],dz=p[b+2]-p[a+2],length=Math.sqrt(dx*dx+dy*dy+dz*dz);
            var w=Math.sqrt(Math.max(0,(1+dy/length)*0.5));
            var rotation=w>0.00001?{w:w,x:dz/length/(2*w),y:0,z:-dx/length/(2*w)}:{w:0,x:1,y:0,z:0};
            Engine.transform(wire.id,{position:{x:(p[a]+p[b])/2+view.x,y:(p[a+1]+p[b+1])/2,z:(p[a+2]+p[b+2])/2+view.z},rotation:rotation,scale:{x:0.06,y:length,z:0.06}});
        });
    });
    // Error covers every physical segment in every completed rope sample.
    var sum=0,q=Lab.sample;
    for(i=0;i<Lab.total-1;++i)if(i%Lab.count!==Lab.count-1){
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
