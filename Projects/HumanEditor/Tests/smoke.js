/* Runs inside the actual Duktape host with the unchanged Vulkan engine. */
HE.smokeStart=function(){HE.smoke={phase:0,wait:0};};
HE.smokeTick=function(dt,input){
    var s=HE.smoke;if(!s)return;
    if(s.done){s.wait++;if(!s.logged&&s.wait>15){s.logged=true;
        if(HE.el('entity-name').getBounds().width<100)throw Error('Inspector layout collapsed');
        if(HE.el('asset-1').getBounds().x<=HE.el('asset-0').getBounds().x)throw Error('Content cards must form a row');
        if(HE.el('place-search').getBounds().width<150)throw Error('Palette search collapsed');
        if(HE.el('category-1').getBounds().x<=HE.el('category-0').getBounds().x)throw Error('Palette categories must form a row');
        if(HE.el('section-render').getBounds().height>45)throw Error('Collapsed component still occupies field space');
        Engine.log('HumanEditor layout PASS: inspector, Content grid, palette search, categories and component collapse');
        HE.browseFolder(HE.mount);HE.layout.bottom=200;HE.resize(input.width,input.height,true);
    }
    if(s.wait===28){
        if(HE.el('asset-folder-1').getBounds().x<=HE.el('asset-folder-0').getBounds().x)throw Error('Folder grid collapses when scrolling');
        HE.browser.list=true;HE.refreshAssets();
    }
    if(s.wait===40){
        var first=HE.el('asset-folder-0').getBounds(),second=HE.el('asset-folder-1').getBounds();
        if(first.width<500||second.y<=first.y||second.x!==first.x)throw Error('Browser list layout is not a full-width column');
        Engine.log('HumanEditor browser PASS: nested folders, history, search, asset open, keyboard focus, scrolling grid and list view');
    }
    if(s.wait===50){
        HE.camera.yaw=0.65;HE.camera.pitch=0.45;HE.applyCamera();HE.setMode('rotate');s.gizmoIndex=0;s.gizmoNext=70;
        s.gizmoCases=[['rotate','x'],['rotate','y'],['rotate','z'],['move','xy'],['move','xz'],['move','yz'],['scale','xy'],['scale','xz'],['scale','yz']];
    }
    if(s.gizmoCases&&s.gizmoIndex<s.gizmoCases.length&&s.wait>=s.gizmoNext&&!HE.pending){
        if(!s.gizmoCheck){
            var mode=s.gizmoCases[s.gizmoIndex][0],axis=s.gizmoCases[s.gizmoIndex][1],frame=HE.gizmo.frame,plane=frame.axes[axis];
            function planePoint(u,v){return HE.project(frame.position.map(function(p,i){return p+frame.radius*(plane.u[i]*u+plane.v[i]*v);}));}
            var start=mode==='rotate'?planePoint(Math.cos(0.2),Math.sin(0.2)):planePoint(0.35,0.35);
            s.gizmoCheck={axis:axis,mode:mode,frame:frame,end:mode==='rotate'?planePoint(Math.cos(0.7),Math.sin(0.7)):planePoint(0.66,0.54),
                before:HE.copy(Engine.position(HE.selected[0])),scale:HE.copy(Engine.component(HE.selected[0],'render').scale)};
            HE.pointerDown({parameters:{button:0,mouse_x:start.x,mouse_y:start.y}});
        }else if(s.gizmoCheck.undo){
            if(JSON.stringify(Engine.position(HE.selected[0]))!==JSON.stringify(s.gizmoCheck.before))throw Error('Gizmo undo did not restore the actor');
            if(JSON.stringify(Engine.component(HE.selected[0],'render').scale)!==JSON.stringify(s.gizmoCheck.scale))throw Error('Gizmo undo did not restore scale');
            s.gizmoIndex++;s.gizmoCheck=null;s.gizmoNext=s.wait+15;
            if(s.gizmoIndex===s.gizmoCases.length){
                if(Engine.scene.capture().document.entities.some(function(e){return e.name.indexOf('Gizmo ')===0||e.name==='Transform gizmo';}))throw Error('Gizmo entity leaked into scene capture');
                Engine.log('HumanEditor gizmo PASS: GPU picks and drags rotation axes plus XY/XZ/YZ move and local scale planes; snapping, unchanged third axis, undo, transient capture');
            }else HE.setMode(s.gizmoCases[s.gizmoIndex][0]);
        }else if(!HE.pickTicket){
            var probe=s.gizmoCheck;
            if(!HE.drag||HE.axis!==probe.axis||HE.lastPick.entity!==HE.gizmo.handles[probe.axis].entity)throw Error('GPU pick did not start the '+probe.axis+' mesh handle');
            HE.pointerMove({parameters:{mouse_x:probe.end.x,mouse_y:probe.end.y}});
            if(probe.mode==='rotate'){
                if(Math.abs(HE.drag.angle-Math.PI/6)>0.001)throw Error('Mesh ring rotation did not follow the pointer');
            }else{
                var before=probe.before,actual=Engine.position(HE.selected[0]),scale=Engine.component(HE.selected[0],'render').scale;
                var expectedPosition=[before.x,before.y,before.z],expectedScale=probe.scale.slice();
                probe.axis.split('').forEach(function(a,i){
                    if(probe.mode==='move'){
                        var amount=Math.round([0.31,0.19][i]*probe.frame.radius/HE.snap)*HE.snap;
                        expectedPosition=expectedPosition.map(function(v,k){return v+probe.frame.axes[a].normal[k]*amount;});
                    }else expectedScale[HE.gizmoAxes.indexOf(a)]+=[0.3,0.2][i];
                });
                [actual.x,actual.y,actual.z].forEach(function(v,i){if(Math.abs(v-expectedPosition[i])>0.001||Math.abs(scale[i]-expectedScale[i])>0.001)throw Error(probe.mode+' '+probe.axis+' plane changed the wrong dimensions');});
            }
            HE.pointerUp({parameters:{button:0,mouse_x:probe.end.x,mouse_y:probe.end.y}});
            probe.undo=true;HE.history(false);
        }
    }
    return;}
    function check(ok,message){if(!ok)throw Error('HumanEditor smoke: '+message);}
    function find(name){return HE.entities().filter(function(e){return Engine.entity(e).name===name;})[0];}
    if(HE.pending)return;
    if(s.phase===0){
        check(HE.source.path.indexOf(HE.mount+'/')===0,'mounted scene');
        s.cube=find('Cube');s.id=Engine.entity(s.cube).id;s.original=HE.copy(Engine.component(s.cube,'transform'));
        HE.camera={target:[0,0.65,0],yaw:0,pitch:0,distance:8,fov:0.62};HE.applyCamera();s.phase=1;return;
    }
    if(s.phase===1){
        if(++s.wait<12)return;
        var info=Engine.renderTargetInfo(HE.rt);if(!info.width)return;
        var rightClick={parameters:{button:1,mouse_x:HE.rect.x+HE.rect.width/2,mouse_y:HE.rect.y+HE.rect.height/2}};
        HE.pointerDown(rightClick);HE.pointerUp(rightClick);
        var openContext=HE.pickCallback;HE.pickCallback=function(entity){openContext(entity);s.contextOpened=!!HE.contextDoc&&HE.contextDoc.querySelectorAll('button').length===8;};
        s.phase=2;return;
    }
    if(s.phase===2){
        if(HE.pickTicket)return;
        check(HE.lastPick&&HE.lastPick.entity===s.cube,'DrawEntityID selects the visible cube');
        check(HE.selected[0]===s.cube,'GPU selection reaches inspector');
        check(s.contextOpened,'right click opens actor commands after GPU picking');
        HE.closeContext();
        HE.command('Smoke edit',function(){Engine.rename(s.cube,'Edited cube');var t=Engine.component(s.cube,'transform');t.position[0]=1.25;Engine.setComponent(s.cube,'transform',t);Engine.addComponent(s.cube,'data',{answer:42});});
        s.child=Engine.create({name:'Child',components:{transform:{position:[0,1,0],parent:s.id},data:{child:true}}});
        HE.select(s.cube);HE.duplicate();
        check(HE.selected.length===2,'duplicate includes children');s.copyIds=HE.ids();
        HE.actorContext(HE.selected[0],600,300);check(JSON.stringify(HE.ids())===JSON.stringify(s.copyIds),'right click preserves multi-selection');HE.closeContext();
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
        check(!HE.el('tool-move')&&!HE.el('tool-rotate')&&!HE.el('tool-scale'),'transform toolbar buttons removed');
        HE.setMode('rotate');
        var frame=HE.gizmo.frame,plane=frame.axes.z;
        var start=HE.project(frame.position.map(function(v,i){return v+frame.radius*plane.u[i];}));
        var end=HE.project(frame.position.map(function(v,i){return v+frame.radius*plane.v[i];}));
        HE.beginDrag('z',start.x,start.y);
        HE.setMode('scale');check(HE.mode==='rotate','active drag keeps its transform mode');
        HE.dragTo(end.x,end.y);HE.endDrag(false);
        check(Math.abs(Engine.position(s.cube).rotation.z-Math.sin(Math.PI/4))<0.001,'rotation follows the projected ring and snaps to 90 degrees');
        HE.setMode('scale');var scale=Engine.component(s.cube,'render').scale[0];
        check(HE.space==='world'&&Math.abs(HE.gizmo.frame.axes.x.normal[1]-1)<0.001,'scale gizmo uses the rotated local X axis despite World mode');
        HE.beginDrag('x',500,350);HE.dragTo(500,350-HE.gizmoRadius*0.3);HE.endDrag(false);
        check(Math.abs(Engine.component(s.cube,'render').scale[0]-scale-0.3)<0.001,'scale follows its projected axis and snapping');
        HE.setMode('move');HE.beginDrag('x',500,350);HE.dragTo(580,350);HE.endDrag(false);
        check(Engine.position(s.cube).x!==1.25,'axis drag moves selected actor');
        HE.history(false);s.phase=5;return;
    }
    if(s.phase===5){
        s.cube=Engine.findEntity(s.id);check(Math.abs(Engine.position(s.cube).x-1.25)<0.001,'drag is a single undo transaction');
        HE.save(HE.mount+'/Maps/SmokeSave');
        var saved=Engine.content.load(HE.mount+'/Maps/SmokeSave');
        check(saved.payload.entities.every(function(e){return !e.components.drawEntityID;}),'transient tools excluded from disk');
        check(saved.payload.entities.some(function(e){return e.id===s.id&&e.components.data.answer===42;}),'component saved with persistent identity');
        HE.open(HE.mount+'/Maps/SmokeSave');s.phase=6;return;
    }
    if(s.phase===6){
        s.cube=Engine.findEntity(s.id);check(Engine.entity(s.cube).name==='Edited cube','saved level roundtrip');
        HE.select(s.cube);HE.command('Before Save As undo',function(){Engine.rename(s.cube,'After save');});
        HE.save(HE.mount+'/Maps/SaveAs');HE.history(false);s.phase=7;return;
    }
    if(s.phase===7){
        check(HE.source.path===HE.mount+'/Maps/SaveAs','undo keeps Save As destination');
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
        check(JSON.stringify(before)!==JSON.stringify(HE.camera.target),'UI-owned WASD fly');
        HE.pointerUp({parameters:{button:1,mouse_x:inputForUi.x+30,mouse_y:inputForUi.y+10}});
        check(!HE.pickTicket&&!HE.contextDoc,'right drag does not open a context menu');
        var distance=HE.camera.distance;
        HE.pointerWheel({parameters:{wheel_delta_y:-1},stopPropagation:function(){}});HE.input(dt,inputForUi);
        check(HE.camera.distance<distance,'UI wheel zoom');
        [[16,'rotate'],[29,'scale'],[34,'move']].forEach(function(shortcut){
            HE.keyEvent({parameters:{key_identifier:shortcut[0],ctrl_key:0,shift_key:0}},true);HE.input(dt,inputForUi);
            check(HE.mode===shortcut[1]&&HE.el('gizmo').hasClass(shortcut[1]),'keyboard shortcut updates the visible '+shortcut[1]+' gizmo');
            HE.keyEvent({parameters:{key_identifier:shortcut[0],ctrl_key:0,shift_key:0}},false);
        });
        HE.setMode('move');HE.scan();
        var undoCount=HE.undoStack.length,rect=HE.copy(HE.rect);
        HE.toggleViewport();check(HE.rect.width>rect.width,'maximize expands viewport');
        HE.toggleViewport();check(JSON.stringify(HE.rect)===JSON.stringify(rect),'restore preserves panel layout');
        HE.toggleContent();check(HE.rect.height>rect.height,'drawer frees viewport space');HE.toggleContent();
        HE.panelDrag='right';HE.movePanel(input.width-390,300);HE.panelDrag=null;
        check(HE.detailsWidth===390,'panel divider resizes details');
        HE.layout.right=360;HE.resize(input.width,input.height,true);
        check(HE.undoStack.length===undoCount,'workspace changes do not enter scene history');
        HE.browseFolder(HE.mount+'/Maps');HE.el('asset-filter').setValue('Script');HE.refreshAssets();
        check(!HE.browser.items.some(function(item){return item.kind==='asset';}),'type filter excludes maps');
        HE.el('asset-filter').setValue('All');HE.browseFolder(HE.mount);
        check(!HE.browser.items.some(function(item){return item.parent;}),'mount root has no parent entry');
        check(HE.el('assets').querySelectorAll('.folder-card').length>=2,'root shows child folders');
        var folderItem=HE.browser.items.filter(function(item){return item.path===HE.mount+'/Props';})[0];
        HE.selectBrowserItem(folderItem);check(HE.folder===HE.mount,'single click selects without entering');
        HE.keyEvent({parameters:{key_identifier:99,ctrl_key:0,shift_key:0}},true);HE.input(dt,inputForUi);
        HE.keyEvent({parameters:{key_identifier:99,ctrl_key:0,shift_key:0}},false);
        check(Engine.alive(HE.selected[0])&&HE.undoStack.length===undoCount,'asset focus does not delete scene selection');
        HE.openBrowserItem(folderItem);check(HE.folder===HE.mount+'/Props','folder open enters directory');
        check(HE.browser.items[0].parent&&HE.browser.items[0].path===HE.mount,'parent entry comes first');
        HE.selectBrowserItem(HE.browser.items[0]);
        HE.browserKey({parameters:{key_identifier:72},stopPropagation:function(){}});
        check(HE.folder===HE.mount,'Enter on parent returns to root');HE.browserTravel(-1);
        HE.openBrowserItem(HE.browser.items.filter(function(item){return !item.parent;})[0]);check(HE.folder===HE.mount+'/Props/Architecture','nested folder open');
        HE.browserTravel(-1);check(HE.folder===HE.mount+'/Props','Back');
        HE.browserTravel(1);check(HE.folder===HE.mount+'/Props/Architecture','Forward');
        HE.selectBrowserItem(HE.browser.items.filter(function(item){return item.kind==='asset';})[0]);HE.openBrowserItem(HE.browser.selected);
        check(!!HE.modalDoc&&!!HE.modalDoc.getElementById('asset-editor'),'asset open reaches its editor');
        HE.modalDoc.close();HE.modalDoc=null;
        HE.el('asset-search').setValue('no-match');HE.el('asset-filter').setValue('Map');HE.el('asset-sort').setValue('desc');HE.refreshAssets();
        check(HE.browser.items.length===1&&HE.browser.items[0].parent,'parent survives search, category and descending sort');
        HE.el('asset-sort').setValue('asc');
        HE.browseFolder(HE.mount);HE.el('asset-filter').setValue('Map');HE.refreshAssets();
        check(HE.browser.items.some(function(item){return item.kind==='folder'&&item.name==='Props';}),'category filter keeps folder navigation');
        HE.el('asset-filter').setValue('All');HE.el('asset-search').setValue('Architecture');HE.refreshAssets();
        check(HE.browser.items.some(function(item){return item.kind==='folder'&&item.name==='Architecture';}),'search finds nested folders');
        HE.browseFolder(HE.mount+'/Maps');HE.browserTravel(-1);
        check(HE.el('asset-search').getValue()==='Architecture','Back restores search state');
        HE.browseFolder(HE.mount);
        HE.el('asset-search').setValue('Workbench');HE.refreshAssets();
        check(HE.el('assets').querySelectorAll('.asset').length===1,'search includes descendants');
        HE.browseFolder(HE.mount+'/Maps');
        HE.paletteCategory='Lights';HE.refreshPalette();
        check(HE.el('palette').querySelectorAll('button').length===5,'light placement category');
        HE.paletteCategory='Basic';HE.refreshPalette();
        HE.componentCollapsed.render=true;HE.filterDetails();
        HE.focus();
        HE.log('SMOKE PASS: GPU ID, ECS, history, save/load, Play/Pause/Stop, UI input, layout, assets, palette and actor context menu');
        s.done=true;s.wait=0;
    }
};
