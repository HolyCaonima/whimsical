/* Runs inside the actual Duktape host with the unchanged Vulkan engine. */
HE.smokeStart=function(){HE.smoke={phase:0,wait:0};};
HE.smokeTick=function(dt,input){
    var s=HE.smoke;if(!s)return;
    if(s.done){if(!s.logged&&++s.wait>15){s.logged=true;
        if(HE.el('entity-name').getBounds().width<100)throw Error('Inspector layout collapsed');
        if(HE.el('asset-1').getBounds().x<=HE.el('asset-0').getBounds().x)throw Error('Content cards must form a row');
        Engine.log('HumanEditor layout PASS: inspector width and Content card grid');
    }return;}
    function check(ok,message){if(!ok)throw Error('HumanEditor smoke: '+message);}
    function find(name){return HE.entities().filter(function(e){return Engine.entity(e).name===name;})[0];}
    if(HE.pending)return;
    if(s.phase===0){
        check(HE.source.path.indexOf('/Target/')===0,'mounted scene');
        s.cube=find('Cube');s.id=Engine.entity(s.cube).id;s.original=HE.copy(Engine.component(s.cube,'transform'));
        HE.camera={target:[0,0.65,0],yaw:0,pitch:0,distance:8,fov:0.62};HE.applyCamera();s.phase=1;return;
    }
    if(s.phase===1){
        if(++s.wait<12)return;
        var info=Engine.renderTargetInfo(HE.rt);if(!info.width)return;
        HE.pick(HE.rect.x+HE.rect.width/2,HE.rect.y+HE.rect.height/2,false);s.phase=2;return;
    }
    if(s.phase===2){
        if(HE.pickTicket)return;
        check(HE.lastPick&&HE.lastPick.entity===s.cube,'DrawEntityID selects the visible cube');
        check(HE.selected[0]===s.cube,'GPU selection reaches inspector');
        HE.command('Smoke edit',function(){Engine.rename(s.cube,'Edited cube');var t=Engine.component(s.cube,'transform');t.position[0]=1.25;Engine.setComponent(s.cube,'transform',t);Engine.addComponent(s.cube,'data',{answer:42});});
        s.child=Engine.create({name:'Child',components:{transform:{position:[0,1,0],parent:s.id},data:{child:true}}});
        HE.select(s.cube);HE.duplicate();
        check(HE.selected.length===2,'duplicate includes children');s.copyIds=HE.ids();
        check(Engine.component(HE.selected[1],'transform').parent===s.copyIds[0],'duplicate remaps hierarchy');
        HE.remove();check(!Engine.findEntity(s.copyIds[0]),'delete subtree');
        HE.history(false);s.phase=3;return;
    }
    if(s.phase===3){
        check(!!Engine.findEntity(s.copyIds[0])&&!!Engine.findEntity(s.copyIds[1]),'undo deletion restores subtree');
        HE.history(true);s.phase=4;return;
    }
    if(s.phase===4){
        check(!Engine.findEntity(s.copyIds[0]),'redo deletes copied subtree');
        s.cube=Engine.findEntity(s.id);HE.select(s.cube);
        HE.setMode('move');HE.beginDrag('x',500,350);HE.dragTo(580,350);HE.endDrag(false);
        check(Engine.position(s.cube).x!==1.25,'axis drag moves selected actor');
        HE.history(false);s.phase=5;return;
    }
    if(s.phase===5){
        s.cube=Engine.findEntity(s.id);check(Math.abs(Engine.position(s.cube).x-1.25)<0.001,'drag is a single undo transaction');
        HE.save('/Target/Maps/SmokeSave');
        var saved=Engine.content.load('/Target/Maps/SmokeSave');
        check(saved.payload.entities.every(function(e){return !e.components.drawEntityID;}),'transient tools excluded from disk');
        check(saved.payload.entities.some(function(e){return e.id===s.id&&e.components.data.answer===42;}),'component saved with persistent identity');
        HE.open('/Target/Maps/SmokeSave');s.phase=6;return;
    }
    if(s.phase===6){
        s.cube=Engine.findEntity(s.id);check(Engine.entity(s.cube).name==='Edited cube','saved level roundtrip');
        HE.select(s.cube);HE.command('Before Save As undo',function(){Engine.rename(s.cube,'After save');});
        HE.save('/Target/Maps/SaveAs');HE.history(false);s.phase=7;return;
    }
    if(s.phase===7){
        check(HE.source.path==='/Target/Maps/SaveAs','undo keeps Save As destination');
        s.beforePlay=Engine.scene.capture();HE.play();s.phase=8;s.wait=0;return;
    }
    if(s.phase===8){
        if(++s.wait<5)return;
        check(Engine.simulation.state().running,'Play starts target program');
        check(Engine.scene.resources().data.playStarted===true,'target script executes');
        Engine.simulation.pause(true);s.phase=9;return;
    }
    if(s.phase===9){check(Engine.simulation.state().paused,'Pause');HE.stop();s.phase=10;return;}
    if(s.phase===10){
        check(!Engine.simulation.state().running,'Stop');
        check(JSON.stringify(Engine.scene.capture())===JSON.stringify(s.beforePlay),'Stop restores authored state exactly');
        check(HE.selected.length===1&&Engine.entity(HE.selected[0]).id===s.id,'Stop restores selection');
        s.rollback=JSON.stringify(Engine.scene.capture());
        try{HE.command('Rejected edit',function(){Engine.rename(HE.selected[0],'Should roll back');Engine.addComponent(HE.selected[0],'noSuchComponent',{});});}catch(error){check(String(error).indexOf('noSuchComponent')>=0,'component error exposed');}
        s.phase=11;return;
    }
    if(s.phase===11){
        check(JSON.stringify(Engine.scene.capture())===s.rollback,'failed compound command rolls back');
        var inputForUi={width:input.width,height:input.height,x:HE.rect.x+200,y:HE.rect.y+200,focused:true,pointerCaptured:true,keyboardCaptured:true};
        var yaw=HE.camera.yaw;
        HE.pointerDown({parameters:{button:1,mouse_x:inputForUi.x,mouse_y:inputForUi.y}});
        HE.pointerMove({parameters:{mouse_x:inputForUi.x+30,mouse_y:inputForUi.y+10}});
        HE.input(dt,inputForUi);check(HE.camera.yaw!==yaw,'UI-owned orbit ignores consumed gameplay delta');
        var before=HE.copy(HE.camera.target);
        HE.keyEvent({parameters:{key_identifier:34,ctrl_key:0,shift_key:0}},true);
        HE.input(dt,inputForUi);HE.keyEvent({parameters:{key_identifier:34,ctrl_key:0,shift_key:0}},false);
        check(JSON.stringify(before)!==JSON.stringify(HE.camera.target),'UI-owned WASD fly');HE.navigation=null;
        var distance=HE.camera.distance;
        HE.pointerWheel({parameters:{wheel_delta_y:-1},stopPropagation:function(){}});HE.input(dt,inputForUi);
        check(HE.camera.distance<distance,'UI wheel zoom');
        HE.keyEvent({parameters:{key_identifier:16,ctrl_key:0,shift_key:0}},true);HE.input(dt,inputForUi);
        check(HE.mode==='rotate','UI keyboard shortcut');HE.keyEvent({parameters:{key_identifier:16,ctrl_key:0,shift_key:0}},false);
        HE.setMode('move');HE.scan();
        HE.focus();
        HE.log('SMOKE PASS: GPU ID, ECS, hierarchy, history, transforms, save/load, Play/Pause/Stop, rollback, UI camera and keyboard input');
        s.done=true;s.wait=0;
    }
};
