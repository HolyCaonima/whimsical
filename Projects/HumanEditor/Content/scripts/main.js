HE.openProjectDialog=function(){
    var project=null;
    var body='<p class="project-intro">Choose a .project file to open its workspace.</p>'+
        '<div class="project-file-label">PROJECT FILE</div><div class="project-file-row"><input id="project-file" class="disabled" type="text" disabled="disabled"/><button id="project-browse">Browse...</button></div>'+
        '<div id="project-preview"><div class="project-badge">P</div><div class="project-summary"><div id="project-name">No project selected</div><div id="project-map">Browse to a project to get started.</div></div></div>'+
        '<p class="project-note">The startup level opens in edit mode. Use Play when you are ready to run it.</p>';
    HE.modal('Open Project',body,[{label:'Cancel'},{label:'Open Project',run:function(){HE.openProject(project);}}],function(d){
        d.getElementById('dialog').setClass('project-dialog',true);
        var open=d.getElementById('dialog-1');open.setClass('primary',true);HE.enable(open,false);
        function read(path){
            project=null;HE.enable(open,false);d.getElementById('project-file').setValue(path);
            d.getElementById('project-name').setText('No project selected');d.getElementById('project-map').setText('Choose a valid .project file.');
            d.getElementById('dialog-error').setText('');
            try{
                project=Engine.project.read(path);
                d.getElementById('project-name').setText(project.name);
                d.getElementById('project-map').setText(project.startupMap?'Startup level: '+project.startupMap.replace('/Game/','Content/'):'No startup level — opens an empty workspace.');
                HE.enable(open,true);
            }catch(e){d.getElementById('dialog-error').setText(e.message||e);}
        }
        d.getElementById('project-browse').on('click',function(){
            try{
                var path=Engine.files.openDialog({title:'Open Project',initialDirectory:HE.projectPath?HE.projectPath.replace(/[/\\][^/\\]+$/,''):'',filters:[{name:'Whimsical project (.project)',pattern:'.project'}]});
                if(path)read(path);
            }catch(e){d.getElementById('dialog-error').setText(e.message||e);}
        });
        if(HE.projectPath)read(HE.projectPath);
    });
};
HE.parentDialog=function(entity){
    var selection=entity?[entity]:HE.selected;
    if(!selection.length)return;
    var rows=HE.entities().map(Engine.entity).filter(function(r){return r.components.transform&&selection.indexOf(r.entity)<0;});
    var html='<p>'+(entity?'Reparent '+HE.escape(Engine.entity(entity).name):'Reparent the selected roots')+', preserving world pose.</p><select id="parent-choice"><option value="0">World (no parent)</option>';
    rows.forEach(function(r){html+='<option value="'+r.entity+'">'+HE.escape(r.name)+' ['+r.entity+']</option>';});
    HE.modal('Set parent',html+'</select>',[{label:'Apply',run:function(d){var parent=Number(d.getElementById('parent-choice').getValue());if(entity)HE.command('Change parent',function(){Engine.parent(entity,parent,true);});else HE.setParent(parent);}},{label:'Cancel'}],function(d){if(selection.length===1){var parent=Engine.component(selection[0],'transform').parent;d.getElementById('parent-choice').setValue(String(parent?Engine.findEntity(parent):0));}});
};
HE.help=function(){HE.modal('HumanEditor controls',
    '<p>Click visible geometry to select using the GPU Entity ID image. Ctrl+click adds or removes actors. Select lights and non-rendering entities in the Outliner.</p>'+
    '<p>W / E / R switches the gizmo at the selected actor: move arrows, rotation rings, or scale boxes. Drag the red X, green Y or blue Z axis; drag along a ring to rotate. The small XY / XZ / YZ squares move or scale two axes together. World / Local changes move and rotation axis orientation. Scale always uses local axes and changes render dimensions; collider dimensions remain independent.</p>'+
    '<p>Right click an actor or Outliner row for actor commands. Right drag: orbit, with WASD and Q/E for camera travel. Middle mouse: pan. Wheel: zoom. F: focus.</p>'+
    '<p>Drag panel dividers to resize the workspace. Maximize / Restore expands the viewport. Ctrl+Space toggles the Content Drawer. Grid, rotation and scale snap, plus camera speed, live on the viewport toolbar. Window resets the layout.</p>'+
    '<p>Ctrl+S save; Ctrl+Z / Y undo / redo; Ctrl+D duplicate subtree; Ctrl+C / V copy / paste; Delete removes selected subtrees. Escape cancels a transform drag.</p>'+
    '<p>Open Project mounts an existing project. Double-click a Map to open it, a StaticMesh to place it, a Material to assign it, or a text / JSON asset to edit it. Save As must stay in a Content containing all scene dependencies.</p>'+
    '<p>Details asset fields show the asset name; click the field to choose another asset, or the folder button beside it to reveal that asset in the Content Browser. The folder button is dimmed for assets outside this project Content, such as /Engine meshes.</p>'+
    '<p>Play launches the target scripts. Pause suspends simulation. Stop discards play changes and restores the authored level. Asset file writes are separate from scene undo history.</p>',[{label:'Close'}]);};
function initialize(){
    HE.settings=Engine.readJson('/Game/Settings');
    HE.doc=Engine.ui.loadDocument('/Game/UI/editor.rml').show();
    var bindings={
        'open-content':HE.openProjectDialog,'new-level':HE.newLevel,'save':function(){if(HE.source)HE.save();else HE.saveAs();},'save-as':HE.saveAs,
        'undo':function(){HE.history(false);},'redo':function(){HE.history(true);},
        'play':HE.play,'stop':HE.stop,'pause':function(){var s=Engine.simulation.state();if(s.running)Engine.simulation.pause(!s.paused);},
        'world-settings':HE.worldSettings,'rescan':HE.scan,'asset-new':HE.newAsset,
        'asset-up':function(){HE.browseFolder(HE.parentFolder(HE.folder));},
        'asset-back':function(){HE.browserTravel(-1);},'asset-forward':function(){HE.browserTravel(1);},
        'asset-tiles':function(){HE.browser.list=false;HE.refreshAssets();},
        'asset-list':function(){HE.browser.list=true;HE.refreshAssets();},
        'show-log':function(){HE.showLog(true);},'hide-log':function(){HE.showLog(false);},
        'content-drawer':HE.toggleContent,'maximize-view':HE.toggleViewport,
        'camera-reset':function(){HE.camera=HE.copy(Engine.scene.resources().camera);HE.applyCamera();},
        'space':function(){HE.space=HE.space==='world'?'local':'world';HE.paintViewbar();},
        'snap-move':function(){HE.toggleSnap('snapEnabled');},
        'snap-rotate':function(){HE.toggleSnap('rotationSnapEnabled');},
        'snap-scale':function(){HE.toggleSnap('scaleSnapEnabled');},
    };
    Object.keys(bindings).forEach(function(id){HE.bind(id,bindings[id]);});
    HE.menuIds=['file-menu','edit-menu','window-menu','help'];
    HE.menus={
        'file-menu':function(){return [
            {label:'Open Project...',run:HE.openProjectDialog},{label:'New level',run:HE.newLevel},
            {label:'Save',key:'Ctrl+S',run:bindings.save,separator:true},{label:'Save As...',run:HE.saveAs}
        ];},
        'edit-menu':function(){var editable=!Engine.simulation.state().running,selected=editable&&HE.selected.length>0;return [
            {label:'Undo',key:'Ctrl+Z',run:bindings.undo,enabled:editable&&HE.undoStack.length>0},
            {label:'Redo',key:'Ctrl+Y',run:bindings.redo,enabled:editable&&HE.redoStack.length>0},
            {label:'Copy',key:'Ctrl+C',run:HE.copySelection,enabled:selected,separator:true},
            {label:'Paste',key:'Ctrl+V',run:HE.paste,enabled:editable&&!!(HE.clipboard&&HE.clipboard.length)},
            {label:'Duplicate',key:'Ctrl+D',run:HE.duplicate,enabled:selected},
            {label:'Delete',key:'Del',run:HE.remove,enabled:selected}
        ];},
        'window-menu':function(){return [
            {label:'Content Browser',run:bindings['hide-log']},{label:'Output Log',run:bindings['show-log']},
            {label:HE.layout.content?'Hide Content Drawer':'Show Content Drawer',key:'Ctrl+Space',run:HE.toggleContent},
            {label:HE.layout.maximized?'Restore viewport':'Maximize viewport',run:HE.toggleViewport,separator:true},
            {label:'Reset layout',run:function(){HE.layout={left:250,right:360,bottom:270,details:0.43,content:true,maximized:false};HE.resize(HE.width,HE.height,true);}}
        ];},
        'help':function(){return [{label:'Controls and shortcuts',run:HE.help}];},
        'snap-move-value':function(){return HE.incrementMenu('snap','snapEnabled',[0.1,0.25,0.5,1,5],' m');},
        'snap-rotate-value':function(){return HE.incrementMenu('rotationSnap','rotationSnapEnabled',[5,10,15,30,45,90],'\u00b0');},
        'snap-scale-value':function(){return HE.incrementMenu('scaleSnap','scaleSnapEnabled',[0.01,0.1,0.25,0.5,1],'');},
        'camera-speed':function(){return HE.incrementMenu('cameraSpeed',null,[0.25,0.5,1,2,4],'x');}
    };
    HE.menuIds.forEach(function(id){
        HE.bind(id,function(){if(HE.activeMenu===id)HE.closeMenu();else HE.openMenu(id);});
        HE.bind(id,function(){if(HE.activeMenu&&HE.activeMenu!==id)HE.openMenu(id);},'mouseover');
    });
    ['snap-move-value','snap-rotate-value','snap-scale-value','camera-speed'].forEach(function(id){
        HE.bind(id,function(){if(HE.activeMenu===id)HE.closeMenu();else HE.openMenu(id);});
    });
    HE.bind('menu-shield',HE.closeMenu,'mousedown');
    HE.bind('tree-search',HE.refreshTree,'change');HE.bind('asset-search',HE.refreshAssets,'change');
    HE.bind('place-search',HE.refreshPalette,'change');HE.bind('details-search',HE.filterDetails,'change');HE.bind('asset-filter',HE.refreshAssets,'change');
    HE.bind('details-search',function(){HE.el('details-search').select();});
    HE.bind('folder-search',HE.refreshFolders,'change');HE.bind('asset-sort',HE.refreshAssets,'change');
    HE.doc.on('mousedown',function(){HE.browserFocused=false;},true);
    HE.el('content').on('mousedown',function(){HE.browserFocused=true;},true);
    HE.el('content').on('focus',function(){HE.browserFocused=true;},true);
    HE.el('content').on('keydown',HE.guard(HE.browserKey),true);
    HE.refreshPalette();
    ['left','right','bottom','details'].forEach(function(edge){HE.bind('split-'+edge,function(ev){if(ev.parameters.button!==0)return;ev.stopPropagation();HE.panelDrag=edge;HE.navigation=null;HE.cancelPick();},'mousedown');});
    HE.bind('viewport',HE.pointerDown,'mousedown');
    HE.bind('viewport',HE.pointerWheel,'mousescroll');
    HE.doc.on('mousemove',HE.pointerMove,true);
    HE.doc.on('keydown',function(ev){HE.keyEvent(ev,true);},true);
    HE.doc.on('keyup',function(ev){HE.keyEvent(ev,false);},true);
    HE.doc.on('focus',function(ev){HE.browserFocused=false;var id=ev.target.getAttribute('id')||'';HE.editingText=/^(field-|entity-name|tree-search|asset-search|folder-search|place-search|details-search|log-text|asset-filter|asset-sort)/.test(id);},true);
    HE.doc.on('mouseup',HE.guard(HE.pointerUp),true);
    HE.source=Engine.scene.info().source;HE.camera=HE.copy(Engine.scene.resources().camera);
    HE.createTools();HE.applyCamera();HE.refreshTree();HE.refreshDetails();HE.refreshStatus();HE.paintViewbar();
    if(HE.settings.project){
        HE.openProject(Engine.project.read(HE.settings.project));
    }else{HE.mount='/Game';HE.folder='/Game';HE.assets=Engine.content.browse('/Game');HE.refreshAssets();}
    HE.log('Ready — open a level, select an actor, and start editing.');
    if(HE.settings.smoke&&typeof HE.smokeStart==='function')HE.smokeStart();
}
function sceneChanged(){
    HE.closeContext();
    var p=HE.pending;HE.pending=null;HE.drag=null;HE.navigation=null;
    HE.source=p&&(p.kind==='history'||p.kind==='rollback')?p.saveTarget:HE.playSelection?HE.playSaveTarget:Engine.scene.info().source;
    var ids=p&&p.selection?p.selection:HE.playSelection||[];
    HE.selected=ids.map(Engine.findEntity).filter(function(e){return !!e;});
    if(p&&p.kind==='history'){
        p.from.pop();p.to.push({label:p.entry.label,state:p.current});HE.revision=p.revision;HE.log('Restored '+p.entry.label);
    }else if(p&&p.kind==='rollback')HE.revision=p.revision;
    else if(p&&(p.kind==='open'||p.kind==='new')){
        HE.detailDrafts={};
        HE.undoStack=[];HE.redoStack=[];HE.revision=++HE.serial;HE.savedRevision=p.kind==='new'?-1:HE.revision;
        if(p.kind==='new')HE.source=null;
        HE.camera=HE.copy(Engine.scene.resources().camera);
    }
    HE.playSelection=null;
    HE.createTools();HE.syncOutlines();
    HE.applyCamera();HE.refreshTree();HE.refreshDetails();HE.refreshStatus();
}
function fixedUpdate(dt,input){
    HE.guard(HE.input)(dt,input);
    if(HE.pending){HE.pending.ticks=(HE.pending.ticks||0)+1;if(HE.pending.ticks>2){HE.pending=null;HE.log('Scene operation did not complete. See the engine console for the load error.');}}
    if(HE.settings.smoke&&typeof HE.smokeTick==='function')HE.smokeTick(dt,input);
}
var uiElapsed=0;
function updateUI(dt){
    if(!HE.doc)return;uiElapsed+=dt;if(uiElapsed<0.2)return;uiElapsed=0;
    var s=Engine.simulation.state();
    if(HE.detailRunning!==s.running)HE.refreshDetails();
    var key=[s.running,s.paused,HE.undoStack.length,HE.redoStack.length].join(':');if(HE.uiState===key)return;HE.uiState=key;
    HE.el('pause').setText(s.paused?'Resume':'Pause');
    HE.el('play').setClass('active',s.running);
    [['play',s.running],['pause',!s.running],['stop',!s.running],['undo',s.running||!HE.undoStack.length],['redo',s.running||!HE.redoStack.length]].forEach(function(pair){HE.enable(HE.el(pair[0]),!pair[1]);});
}
