/* HumanEditor owns commands and history. Engine owns the component contracts. ES5 for Duktape. */
var HE = {
    selected: [], undoStack: [], redoStack: [], revision: 0, savedRevision: 0, serial: 0,
    pending: null, source: null, mount: '/Target', assets: [], clipboard: null,
    mode: 'move', axis: 'x', space: 'world', snap: 0.25, snapEnabled: true,
    messages: [], targetPath: '', publicScripts: [], programs: {}
};
HE.copy = function (v) { return JSON.parse(JSON.stringify(v)); };
HE.escape = function (s) { return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;'); };
HE.log = function (s) {
    HE.messages.push(String(s));
    if (HE.messages.length > 80) HE.messages.shift();
    Engine.log('HumanEditor: ' + s);
    if (HE.doc) { HE.el('status').setText(s); HE.el('log-text').setText(HE.messages.join('\n')); }
};
HE.guard = function (fn) { return function () { try { return fn.apply(null, arguments); } catch (e) { HE.log(e.message || e); } }; };
HE.editable = function () {
    if (HE.pending || Engine.simulation.state().running) throw Error('Stop simulation before editing the scene.');
};
HE.entities = function () { return Engine.entities([]).filter(function (e) { return Engine.entity(e).effectivePersistent; }); };
HE.ids = function () { return HE.selected.filter(Engine.alive).map(function (e) { return Engine.entity(e).id; }); };
HE.syncOutlines = function () {
    Engine.view.outlines(Engine.simulation.state().running ? [] : HE.selected.filter(Engine.alive).map(function(e) {
        return {entity:e, color:[0.55,1,0.86,1]};
    }));
};
HE.select = function (e, additive) {
    HE.cancelPick();
    if (!additive) HE.selected = [];
    if (e && Engine.alive(e) && Engine.entity(e).effectivePersistent) {
        var i = HE.selected.indexOf(e);
        if (i >= 0) HE.selected.splice(i, 1); else HE.selected.push(e);
    }
    HE.syncOutlines();
    HE.paintSelection(); HE.refreshDetails();
};
HE.checkpoint = function () { return {snapshot: Engine.scene.capture(), selection: HE.ids(), revision: HE.revision}; };
HE.commit = function (label, before) {
    HE.undoStack.push({label: label, state: before});
    if (HE.undoStack.length > 50) HE.undoStack.shift();
    HE.redoStack = []; HE.revision = ++HE.serial;
    HE.refreshTree(); HE.refreshDetails(); HE.refreshStatus(); HE.log(label);
};
HE.command = function (label, action) {
    HE.editable();
    var before = HE.checkpoint();
    try { action(); } catch (error) {
        // Multi-entity commands are one transaction from the user's point of view.
        HE.pending = {kind:'rollback', selection:before.selection, revision:before.revision,saveTarget:HE.source};
        Engine.scene.restore(before.snapshot);
        throw error;
    }
    HE.commit(label, before);
};
HE.history = function (redo) {
    HE.editable();
    var from = redo ? HE.redoStack : HE.undoStack, to = redo ? HE.undoStack : HE.redoStack;
    if (!from.length) return;
    var entry = from[from.length - 1];
    HE.pending = {kind:'history', selection:entry.state.selection, revision:entry.state.revision,saveTarget:HE.source,
        from:from, to:to, entry:entry, current:HE.checkpoint()};
    var snapshot=HE.copy(entry.state.snapshot);snapshot.source=Engine.scene.info().source;
    Engine.scene.restore(snapshot);
};
HE.dirty = function () { return HE.revision !== HE.savedRevision; };
HE.unsaved = function (action) {
    HE.editable();
    if (!HE.dirty()) return action();
    HE.modal('Unsaved changes', '<p>Save the current level before continuing?</p>', [
        {label:'Save and continue', run:function () { if(HE.source){HE.save();action();}else HE.saveAs(action); }},
        {label:'Discard changes', run:action}, {label:'Cancel'}
    ]);
};
HE.open = function (ref) {
    HE.unsaved(function () { HE.pending = {kind:'open'}; Engine.scene.load(ref); });
};
HE.mountContent = function (path, scripts) {
    HE.editable();
        // Reuse the source lease when reopening a project; other sources keep their identities.
        function key(p){return p.replace(/\\/g,'/').toLowerCase();}
        var mounted=Engine.content.mounts().filter(function(source){return key(source.root)===key(path);})[0];
        if(!mounted)mounted=Engine.content.mount('/Content'+(++HE.mountSerial),path,true);
        var alias=mounted.mount;
        HE.mount = alias; HE.targetPath = path; HE.publicScripts = (scripts||[]).map(function(p){return p.replace('/Game/',alias+'/');});
        HE.programs[mounted.source]=HE.publicScripts;
        HE.assets = Engine.content.browse(alias); HE.folder = alias;
        HE.refreshAssets(); HE.log('Mounted ' + path + '. Double-click a Map to open it.');
};
HE.mountSerial = 0;
HE.openProject = function (project) {
    HE.unsaved(function () {
        HE.mountContent(project.content, project.scripts);
        HE.projectPath=project.path;HE.projectName=project.name;
        if(project.startupMap){
            HE.pending={kind:'open'};
            Engine.scene.load(project.startupMap.replace('/Game/',HE.mount+'/'));
        }else{
            var snapshot=Engine.scene.capture(),document=snapshot.document;
            snapshot.source=null;document.entities=[];document.materials=[];document.materialAssets=[];
            document.scripts=[];document.references={};document.data={};
            HE.pending={kind:'new'};Engine.scene.restore(snapshot);
        }
        HE.refreshStatus();
    });
};
HE.save = function (path) {
    HE.editable();
    var target = path || HE.source;
    if (!target) throw Error('Choose Save As for an untitled level.');
    HE.source = Engine.scene.save(target, HE.levelName());
    HE.savedRevision = HE.revision; HE.refreshStatus(); HE.log('Saved ' + HE.source.path);
};
HE.levelName = function () { return HE.source ? HE.source.path.split('/').pop() : 'Untitled'; };
HE.saveAs = function (after) {
    HE.prompt('Save level as', 'Mounted asset path (without .asset)', HE.mount + '/Maps/NewLevel', function (path) { HE.save(path); HE.scan(); if(typeof after==='function')after(); });
};
HE.newLevel = function () {
    HE.unsaved(function () {
        // Preserve the target Content's material library so new objects stay source-local.
        var snap = Engine.scene.capture();
        snap.document.entities = []; snap.document.references = {};
        snap.document.scripts = []; snap.document.data = {};
        HE.pending = {kind:'new'}; Engine.scene.restore(snap);
    });
};
HE.roots = function (selection) {
    var ids = selection.map(function (e) { return Engine.entity(e).id; });
    return selection.filter(function (e) {
        var c = Engine.entity(e).components.transform;
        while (c && c.parent) {
            if (ids.indexOf(c.parent) >= 0) return false;
            var p = Engine.findEntity(c.parent); c = p ? Engine.entity(p).components.transform : null;
        }
        return true;
    });
};
HE.subtree = function (roots) {
    var result = [], all = HE.entities().map(Engine.entity), ids = roots.map(function(e){return Engine.entity(e).id;});
    function visit(id) {
        var row = all.filter(function(r){return r.id === id;})[0];
        if (!row) return;
        result.push(row);
        all.forEach(function(r){if(r.components.transform && r.components.transform.parent === id) visit(r.id);});
    }
    ids.forEach(visit); return result;
};
HE.copySelection = function () {
    HE.clipboard = HE.copy(HE.subtree(HE.roots(HE.selected)));
    HE.log('Copied ' + HE.clipboard.length + ' entities');
};
HE.paste = function () {
    if (!HE.clipboard || !HE.clipboard.length) return;
    HE.command('Paste entities', function () {
        var mapping = {}, selected = [];
        HE.clipboard.forEach(function (row) { mapping[row.id] = Engine.content.newId(); });
        HE.clipboard.forEach(function (row) {
            var c = HE.copy(row.components), t = c.transform;
            if (t) {
                if (mapping[t.parent]) t.parent = mapping[t.parent];
                else t.position[0] += 1;
            }
            selected.push(Engine.create({id:mapping[row.id], name:row.name + ' Copy', enabled:row.enabled, components:c}));
        });
        HE.selected = selected; HE.syncOutlines();
    });
};
HE.duplicate = function () { HE.copySelection(); HE.paste(); };
HE.remove = function () {
    if (!HE.selected.length) return;
    HE.command('Delete entities', function () {
        HE.subtree(HE.roots(HE.selected)).reverse().forEach(function(r){Engine.destroy(r.entity);});
        HE.selected = []; HE.syncOutlines();
    });
};
HE.add = function (kind, asset) {
    HE.command('Add ' + kind, function () {
        var c = {transform:{position:HE.copy(HE.camera.target)}};
        if (kind === 'Box' || kind === 'Capsule' || kind === 'StaticMesh') {
            c.render = {shape:kind === 'Capsule' ? 'capsule' : 'box', scale:[1,1,1], material:0};
            if (asset) c.render.mesh = asset;
        } else if (kind !== 'Entity') {
            c.light = {type:kind.toLowerCase(), color:[1,0.88,0.72], intensity:kind === 'Directional' ? 2 : 500};
            if (kind === 'Rect') { c.light.width = 2; c.light.height = 1; }
            if (kind === 'CapsuleLight') c.light = {type:'capsule', color:[1,1,1], intensity:500,radius:0.2,length:1};
        }
        HE.selected = [Engine.create({name:kind, components:c})]; HE.syncOutlines();
    });
};
HE.setParent = function (parent) {
    HE.command('Change parent', function () { HE.roots(HE.selected).forEach(function(e){Engine.parent(e,parent,true);}); });
};
HE.toggleEnabled = function () {
    if (!HE.selected.length) return;
    var on = !Engine.entity(HE.selected[0]).enabled;
    HE.command(on ? 'Enable entities' : 'Disable entities',function(){HE.selected.forEach(function(e){Engine.enabled(e,on);});});
};
HE.play = function () {
    if (HE.pending) return;
    if (Engine.simulation.state().running) return;
    HE.playSelection = HE.ids();
    HE.playSaveTarget = HE.source;
    Engine.view.outlines([]);
    Engine.view.set({camera:null});
    var source=Engine.scene.info().source;
    Engine.simulation.play({scripts:source?(HE.programs[source.source]||[]):HE.publicScripts}); HE.log('Play — Stop restores the authored scene');
};
HE.stop = function () { if (Engine.simulation.state().running) Engine.simulation.stop(); };
HE.scan = function () { Engine.content.scan(HE.mount); HE.assets = Engine.content.browse(HE.mount); HE.refreshAssets(); };
