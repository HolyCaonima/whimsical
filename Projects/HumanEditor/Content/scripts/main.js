HE.openContentDialog=function(){
    var body='<p>Choose a project preset, or enter any Content directory.</p><select id="content-preset"><option value="custom">Custom Content...</option>';
    HE.settings.presets.forEach(function(p,i){body+='<option value="'+i+'">'+HE.escape(p.name)+'</option>';});
    body+='</select><input id="content-path" type="text"/><p>Public scene scripts for Play (JSON array of paths relative to the mounted Content). Map scripts are loaded automatically.</p><textarea id="public-scripts" style="height:110px;"/>';
    HE.modal('Open project Content',body,[{label:'Mount Content',run:function(d){var path=d.getElementById('content-path').getValue(),scripts=JSON.parse(d.getElementById('public-scripts').getValue());if(!Array.isArray(scripts))throw Error('Scripts must be an array.');HE.mountContent(path,scripts);}},{label:'Cancel'}],function(d){
        d.getElementById('content-path').setValue(HE.targetPath);
        d.getElementById('public-scripts').setValue('[]');
        d.getElementById('content-preset').on('change',function(){var n=d.getElementById('content-preset').getValue();if(n==='custom')return;var p=HE.settings.presets[Number(n)];d.getElementById('content-path').setValue(p.content);d.getElementById('public-scripts').setValue(JSON.stringify(p.scripts,null,2));});
    });
};
HE.parentDialog=function(){
    if(!HE.selected.length)return;
    var rows=HE.entities().map(Engine.entity).filter(function(r){return r.components.transform&&HE.selected.indexOf(r.entity)<0;});
    var html='<p>Reparent the selected roots, preserving their world pose.</p><select id="parent-choice"><option value="0">World (no parent)</option>';
    rows.forEach(function(r){html+='<option value="'+r.entity+'">'+HE.escape(r.name)+' ['+r.entity+']</option>';});
    HE.modal('Set parent',html+'</select>',[{label:'Apply',run:function(d){HE.setParent(Number(d.getElementById('parent-choice').getValue()));}},{label:'Cancel'}]);
};
HE.help=function(){HE.modal('HumanEditor controls',
    '<p>Click visible geometry to select using the GPU Entity ID image. Ctrl+click adds or removes actors. Select lights and non-rendering entities in the Outliner.</p>'+
    '<p>W / E / R switches the gizmo at the selected actor: move arrows, rotation rings, or scale boxes. Drag the red X, green Y or blue Z axis; drag along a ring to rotate. The small XY / XZ / YZ squares move or scale two axes together. World / Local changes move and rotation axis orientation. Scale always uses local axes and changes render dimensions; collider dimensions remain independent.</p>'+
    '<p>Right click an actor or Outliner row for actor commands. Right drag: orbit, with WASD and Q/E for camera travel. Middle mouse: pan. Wheel: zoom. F: focus.</p>'+
    '<p>Drag panel dividers to resize the workspace. Maximize / Restore expands the viewport. Ctrl+Space toggles the Content Drawer. Viewport Settings adjusts snapping increments and camera speed; Window resets the layout.</p>'+
    '<p>Ctrl+S save; Ctrl+Z / Y undo / redo; Ctrl+D duplicate subtree; Ctrl+C / V copy / paste; Delete removes selected subtrees. Escape cancels a transform drag.</p>'+
    '<p>Open Content mounts an existing project. Double-click a Map to open it, a StaticMesh to place it, a Material to assign it, or a text / JSON asset to edit it. Save As must stay in a Content containing all scene dependencies.</p>'+
    '<p>Play launches the target scripts. Pause suspends simulation. Stop discards play changes and restores the authored level. Asset file writes are separate from scene undo history.</p>',[{label:'Close'}]);};
function initialize(){
    HE.settings=Engine.readJson('/Game/Settings');
    HE.doc=Engine.ui.loadDocument('/Game/UI/editor.rml').show();
    var bindings={
        'open-content':HE.openContentDialog,'new-level':HE.newLevel,'save':function(){if(HE.source)HE.save();else HE.saveAs();},'save-as':HE.saveAs,
        'undo':function(){HE.history(false);},'redo':function(){HE.history(true);},
        'play':HE.play,'stop':HE.stop,'pause':function(){var s=Engine.simulation.state();if(s.running)Engine.simulation.pause(!s.paused);},
        'world-settings':HE.worldSettings,'help':HE.help,'rescan':HE.scan,'asset-new':HE.newAsset,
        'asset-up':function(){HE.browseFolder(HE.parentFolder(HE.folder));},
        'asset-back':function(){HE.browserTravel(-1);},'asset-forward':function(){HE.browserTravel(1);},
        'asset-view':function(){HE.browser.list=!HE.browser.list;HE.refreshAssets();},
        'show-log':function(){HE.showLog(true);},'hide-log':function(){HE.showLog(false);},
        'content-drawer':HE.toggleContent,'maximize-view':HE.toggleViewport,'view-options':HE.viewOptions,
        'camera-reset':function(){HE.camera=HE.copy(Engine.scene.resources().camera);HE.applyCamera();},
        'space':function(){HE.space=HE.space==='world'?'local':'world';HE.el('space').setText(HE.space==='world'?'World':'Local');},
        'snap':function(){HE.snapEnabled=!HE.snapEnabled;HE.el('snap').setClass('active',HE.snapEnabled);},
        'file-menu':function(){HE.modal('File','<p>Level and Content operations</p>',[{label:'Open Content',run:HE.openContentDialog},{label:'New level',run:HE.newLevel},{label:'Save',run:bindings.save},{label:'Save As',run:HE.saveAs},{label:'Close'}]);},
        'edit-menu':function(){HE.modal('Edit','<p>Scene command history and selection</p>',[{label:'Undo',run:function(){HE.history(false);}},{label:'Redo',run:function(){HE.history(true);}},{label:'Duplicate',run:HE.duplicate},{label:'Delete',run:HE.remove},{label:'Close'}]);},
        'window-menu':function(){HE.modal('Window','<p>Drag the panel dividers to resize your workspace. Ctrl+Space toggles the Content Drawer.</p>',[{label:'Content Browser',run:bindings['hide-log']},{label:'Output Log',run:bindings['show-log']},{label:'Maximize / Restore viewport',run:HE.toggleViewport},{label:'Reset layout',run:function(){HE.layout={left:250,right:360,bottom:270,details:0.43,content:true,maximized:false};HE.resize(HE.width,HE.height,true);}},{label:'Close'}]);}
    };
    Object.keys(bindings).forEach(function(id){HE.bind(id,bindings[id]);});
    HE.bind('tree-search',HE.refreshTree,'change');HE.bind('asset-search',HE.refreshAssets,'change');
    HE.bind('place-search',HE.refreshPalette,'change');HE.bind('details-search',HE.filterDetails,'change');HE.bind('asset-filter',HE.refreshAssets,'change');
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
    HE.createTools();HE.applyCamera();HE.refreshTree();HE.refreshDetails();HE.refreshStatus();
    if(HE.settings.content){
        var mounted=Engine.content.mount('/Target',HE.settings.content,true);HE.targetPath=HE.settings.content;HE.mount='/Target';HE.folder='/Target';
        HE.publicScripts=(HE.settings.scripts||[]).map(function(p){return p.replace('/Game/','/Target/');});
        HE.programs[mounted.source]=HE.publicScripts;
        HE.assets=Engine.content.browse('/Target');HE.refreshAssets();
        if(HE.settings.map)HE.open(HE.settings.map.replace('/Game/','/Target/'));
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
        HE.undoStack=[];HE.redoStack=[];HE.revision=++HE.serial;HE.savedRevision=p.kind==='new'?-1:HE.revision;
        if(p.kind==='new')HE.source=null;
        HE.camera=HE.copy(Engine.scene.resources().camera);
    }
    HE.playSelection=null;
    HE.createTools();Engine.select(HE.selected.length?HE.selected[HE.selected.length-1]:0);
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
    var key=[s.running,s.paused,HE.undoStack.length,HE.redoStack.length].join(':');if(HE.uiState===key)return;HE.uiState=key;
    HE.el('pause').setText(s.paused?'Resume':'Pause');
    HE.el('play').setClass('active',s.running);
    [['play',s.running],['pause',!s.running],['stop',!s.running],['undo',s.running||!HE.undoStack.length],['redo',s.running||!HE.redoStack.length]].forEach(function(pair){var el=HE.el(pair[0]);if(pair[1])el.setAttribute('disabled','disabled');else el.removeAttribute('disabled');});
}
