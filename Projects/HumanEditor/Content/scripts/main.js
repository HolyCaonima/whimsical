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
    '<p>W / E / R: move, rotate, scale. Drag the red X, green Y or blue Z handle at the selected actor. World / Local changes axis orientation. Scale changes render dimensions; collider dimensions remain independent.</p>'+
    '<p>Right mouse: orbit, with WASD and Q/E for camera travel. Middle mouse: pan. Wheel: zoom. F: focus.</p>'+
    '<p>Ctrl+S save; Ctrl+Z / Y undo / redo; Ctrl+D duplicate subtree; Ctrl+C / V copy / paste; Delete removes selected subtrees. Escape cancels a transform drag.</p>'+
    '<p>Open Content mounts an existing project. Double-click a Map to open it, a StaticMesh to place it, a Material to assign it, or a text / JSON asset to edit it. Save As must stay in a Content containing all scene dependencies.</p>'+
    '<p>Play launches the target scripts. Pause suspends simulation. Stop discards play changes and restores the authored level. Asset file writes are separate from scene undo history.</p>',[{label:'Close'}]);};
function initialize(){
    HE.settings=Engine.readJson('/Game/Settings');
    HE.doc=Engine.ui.loadDocument('/Game/UI/editor.rml').show();
    var bindings={
        'open-content':HE.openContentDialog,'new-level':HE.newLevel,'save':function(){if(HE.source)HE.save();else HE.saveAs();},'save-as':HE.saveAs,
        'undo':function(){HE.history(false);},'redo':function(){HE.history(true);},'duplicate':HE.duplicate,'delete':HE.remove,
        'focus':HE.focus,'enable':HE.toggleEnabled,'parent':HE.parentDialog,'unparent':function(){HE.setParent(0);},
        'play':HE.play,'stop':HE.stop,'pause':function(){var s=Engine.simulation.state();if(s.running)Engine.simulation.pause(!s.paused);},
        'world-settings':HE.worldSettings,'help':HE.help,'rescan':HE.scan,'asset-new':HE.newAsset,
        'asset-up':function(){HE.folder=HE.folder.substring(0,HE.folder.lastIndexOf('/'))||HE.mount;HE.refreshAssets();},
        'show-log':function(){HE.el('log-panel').setProperty('display','block');},'hide-log':function(){HE.el('log-panel').setProperty('display','none');},
        'camera-reset':function(){HE.camera=HE.copy(Engine.scene.resources().camera);HE.applyCamera();},
        'space':function(){HE.space=HE.space==='world'?'local':'world';HE.el('space').setText(HE.space==='world'?'World coordinates':'Local coordinates');},
        'snap':function(){HE.snapEnabled=!HE.snapEnabled;HE.el('snap').setText(HE.snapEnabled?'Snap: 0.25 m / 10 deg':'Snap: off');},
        'file-menu':function(){HE.modal('File','<p>Level and Content operations</p>',[{label:'Open Content',run:HE.openContentDialog},{label:'New level',run:HE.newLevel},{label:'Save',run:function(){HE.save();}},{label:'Save As',run:HE.saveAs},{label:'Close'}]);},
        'edit-menu':function(){HE.modal('Edit','<p>Scene command history and selection</p>',[{label:'Undo',run:function(){HE.history(false);}},{label:'Redo',run:function(){HE.history(true);}},{label:'Duplicate',run:HE.duplicate},{label:'Delete',run:HE.remove},{label:'Close'}]);},
        'window-menu':function(){HE.modal('Window','<p>Editor panels</p>',[{label:'Content Browser',run:bindings['hide-log']},{label:'Output Log',run:bindings['show-log']},{label:'World Settings',run:HE.worldSettings},{label:'Close'}]);}
    };
    Object.keys(bindings).forEach(function(id){HE.bind(id,bindings[id]);});
    ['move','rotate','scale'].forEach(function(mode){HE.bind('tool-'+mode,function(){HE.setMode(mode);});});
    HE.bind('tree-search',HE.refreshTree,'change');HE.bind('asset-search',HE.refreshAssets,'change');
    var palette=['Entity','Box','Capsule','Point','Spot','Directional','Rect','CapsuleLight'];
    HE.el('palette').setInnerRML(palette.map(function(k,i){return '<button id="place-'+i+'" class="wide">'+(i<3?'◇  ':'*  ')+k+'</button>';}).join(''));
    palette.forEach(function(k,i){HE.bind('place-'+i,function(){HE.add(k);});});
    HE.bind('viewport',HE.pointerDown,'mousedown');
    HE.bind('viewport',HE.pointerWheel,'mousescroll');
    HE.doc.on('mousemove',HE.pointerMove,true);
    HE.doc.on('keydown',function(ev){HE.keyEvent(ev,true);},true);
    HE.doc.on('keyup',function(ev){HE.keyEvent(ev,false);},true);
    HE.doc.on('focus',function(ev){var id=ev.target.getAttribute('id')||'';HE.editingText=/^(field-|entity-name|tree-search|asset-search|log-text)/.test(id);},true);
    HE.doc.on('mouseup',HE.guard(function(ev){HE.navigationEnded=HE.navigation;HE.navigation=null;if(HE.drag)HE.dragTo(ev.parameters.mouse_x,ev.parameters.mouse_y);HE.endDrag(false);}),true);
    ['x','y','z'].forEach(function(axis){HE.bind('axis-'+axis,function(ev){if(ev.parameters.button!==0)return;ev.stopPropagation();HE.beginDrag(axis,ev.parameters.mouse_x,ev.parameters.mouse_y);},'mousedown');});
    HE.source=Engine.scene.info().source;HE.camera=HE.copy(Engine.scene.resources().camera);
    HE.createTools();HE.applyCamera();HE.refreshTree();HE.refreshDetails();HE.refreshStatus();
    if(HE.settings.content){
        var mounted=Engine.content.mount('/Target',HE.settings.content,true);HE.targetPath=HE.settings.content;HE.mount='/Target';HE.folder='/Target/Maps';
        HE.publicScripts=(HE.settings.scripts||[]).map(function(p){return p.replace('/Game/','/Target/');});
        HE.programs[mounted.source]=HE.publicScripts;
        HE.assets=Engine.content.browse('/Target');HE.refreshAssets();
        if(HE.settings.map)HE.open(HE.settings.map.replace('/Game/','/Target/'));
    }else{HE.mount='/Game';HE.folder='/Game';HE.assets=Engine.content.browse('/Game');HE.refreshAssets();}
    HE.log('Ready — open a level, select an actor, and start editing.');
    if(HE.settings.smoke&&typeof HE.smokeStart==='function')HE.smokeStart();
}
function sceneChanged(){
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
