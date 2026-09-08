HE.el = function(id){return HE.doc.getElementById(id);};
HE.bind = function(id,fn,type){HE.el(id).on(type||'click',HE.guard(fn));};
HE.collapsed = {};
HE.paintSelection = function(){
    if(!HE.doc)return;
    HE.el('tree').querySelectorAll('.tree-row').forEach(function(el){var e=Number(el.getAttribute('id').replace('entity-',''));el.setClass('selected',HE.selected.indexOf(e)>=0);});
    HE.el('actor-count').setText((HE.treeCount||0)+' actors  |  '+HE.selected.length+' selected');
};
HE.refreshStatus = function(){
    if(!HE.doc)return;
    HE.el('level-label').setText(HE.levelName()+(HE.dirty()?' *':''));
    HE.el('save-state').setText(HE.dirty()?'Unsaved changes':'All saved');
    HE.el('project-label').setText(HE.targetPath||'HUMANEDITOR');
};
HE.refreshTree = function(){
    if(!HE.doc)return;
    var rows=HE.entities().map(Engine.entity),search=HE.el('tree-search').getValue().toLowerCase(),html='',byId={},children={};
    rows.forEach(function(r){byId[r.id]=r;var p=r.components.transform?r.components.transform.parent:'';(children[p]||(children[p]=[])).push(r);});
    function row(r,depth){
        var kids=children[r.id]||[],type=r.components.light?'Light':r.components.render?(r.components.render.mesh?'StaticMesh':'Mesh'):'Entity';
        var hit=!search||r.name.toLowerCase().indexOf(search)>=0;
        if(hit)html+='<div id="entity-'+r.entity+'" class="tree-row '+(HE.selected.indexOf(r.entity)>=0?'selected ':'')+(!r.enabled?'disabled ':'')+(r.entity%2?'odd':'')+'" style="padding-left:'+(6+depth*13)+'px"><span class="tree-type">'+type+'</span><span class="fold" id="fold-'+r.entity+'">'+(kids.length?(HE.collapsed[r.id]?'+':'-'):'')+'</span><span class="tree-icon">'+(type==='Light'?'*':'◇')+'</span>'+HE.escape(r.name)+'</div>';
        if(search||!HE.collapsed[r.id])kids.forEach(function(c){row(c,search?0:depth+1);});
    }
    rows.filter(function(r){var p=r.components.transform?r.components.transform.parent:'';return !p||!byId[p];}).forEach(function(r){row(r,0);});
    HE.el('tree').setInnerRML(html);
    rows.forEach(function(r){var el=HE.doc.getElementById('entity-'+r.entity);if(!el)return;
        el.on('click',HE.guard(function(ev){HE.select(r.entity,!!ev.parameters.ctrl_key);}));
        el.on('dblclick',HE.guard(function(){HE.select(r.entity);HE.focus();}));
        HE.doc.getElementById('fold-'+r.entity).on('click',HE.guard(function(ev){ev.stopPropagation();HE.collapsed[r.id]=!HE.collapsed[r.id];HE.refreshTree();}));
    });
    HE.treeCount=rows.length;HE.paintSelection();
};
HE.refreshDetails = function(){
    if(!HE.doc)return;
    HE.el('selection-count').setText(HE.selected.length?HE.selected.length+' selected':'');
    if(!HE.selected.length){HE.el('inspector').setInnerRML('<div class="empty">Select an actor to view details.<br/>Click a surface in the viewport<br/>or choose an actor in the Outliner.</div>');return;}
    var e=HE.selected[HE.selected.length-1];if(!Engine.alive(e))return;
    var row=Engine.entity(e),html='<div class="inspect-pad"><input id="entity-name" class="inspect-name" type="text"/><button id="rename-entity" class="subtle">Rename</button><div class="id-label">Object '+row.id+'</div><div class="id-label">Entity '+e+(HE.selected.length>1?' — inspecting the active selection':'')+'</div><button id="entity-enabled" class="subtle">'+(row.enabled?'Enabled':'Disabled')+'</button><button id="add-component" class="subtle">+ Component</button>',fields=[];
    var priority=['transform','render','light','collider'];
    var componentNames=Object.keys(row.components).sort(function(a,b){var ai=priority.indexOf(a),bi=priority.indexOf(b);return (ai<0?99:ai)-(bi<0?99:bi)||a.localeCompare(b);});
    componentNames.forEach(function(name){
        var value=row.components[name];html+='<div class="component-title">'+HE.escape(name)+'<button id="remove-'+name+'">Remove</button></div>';
        if(value&&typeof value==='object'&&!Array.isArray(value))Object.keys(value).forEach(function(key){
            var v=value[key],id='field-'+fields.length;
            if(v!==null&&(typeof v!=='object'||(Array.isArray(v)&&v.length<=4&&v.every(function(n){return typeof n==='number';})))&&key!=='parent'){
                fields.push({id:id,component:name,key:key,value:v});
                html+='<div class="field"><span class="field-label">'+HE.escape(key)+'</span>';
                if(Array.isArray(v)){html+='<span class="vector">';v.forEach(function(n,i){html+='<input type="text" id="'+id+'-'+i+'" style="width:'+(v.length===4?'19%':'25%')+';"/>';});html+='</span>';}
                else if(typeof v==='boolean')html+='<select id="'+id+'"><option value="true">true</option><option value="false">false</option></select>';
                else html+='<input type="text" id="'+id+'"/>';
                html+='</div>';
            }
        });
        html+='<button id="apply-'+name+'" class="subtle">Apply fields</button><button id="json-'+name+'" class="subtle">Complete JSON...</button>';
    });
    if(row.derived.length)html+='<div class="derived">Derived (read only): '+HE.escape(row.derived.join(', '))+'</div>';
    html+='</div>';HE.el('inspector').setInnerRML(html);HE.el('entity-name').setValue(row.name);
    HE.sizeInspector();
    HE.bind('rename-entity',function(){var name=HE.el('entity-name').getValue();HE.command('Rename actor',function(){Engine.rename(e,name);});});
    HE.bind('entity-enabled',HE.toggleEnabled);HE.bind('add-component',function(){HE.addComponentDialog(e);});
    Object.keys(row.components).forEach(function(name){
        HE.bind('remove-'+name,function(){HE.command('Remove '+name,function(){Engine.removeComponent(e,name);});});
        HE.bind('json-'+name,function(){HE.jsonDialog(name,Engine.component(e,name),function(value){HE.command('Edit '+name,function(){Engine.setComponent(e,name,value);});});});
        HE.bind('apply-'+name,function(){
            var c=Engine.component(e,name);
            fields.filter(function(f){return f.component===name;}).forEach(function(f){
                if(Array.isArray(f.value))c[f.key]=f.value.map(function(v,i){return HE.number(HE.el(f.id+'-'+i).getValue());});
                else if(typeof f.value==='number')c[f.key]=HE.number(HE.el(f.id).getValue());
                else if(typeof f.value==='boolean')c[f.key]=HE.el(f.id).getValue()==='true';
                else c[f.key]=HE.el(f.id).getValue();
            });
            HE.command('Edit '+name,function(){Engine.setComponent(e,name,c);});
        });
    });
    fields.forEach(function(f){
        if(Array.isArray(f.value))f.value.forEach(function(v,i){HE.el(f.id+'-'+i).setValue(String(Math.round(v*10000)/10000));});
        else HE.el(f.id).setValue(String(f.value));
    });
};
HE.sizeInspector=function(){
    var pad=HE.el('inspector').querySelector('.inspect-pad');if(!pad)return;
    var width=(HE.detailsWidth||340)-26;
    pad.setProperty('width',width+'px');
    HE.el('inspector').querySelectorAll('.field').forEach(function(el){el.setProperty('width',width+'px');});
};
HE.number=function(s){if(!String(s).replace(/\s/g,'').length||!isFinite(Number(s)))throw Error('Enter a finite number.');return Number(s);};
HE.modal=function(title,body,buttons,setup){
    if(HE.modalDoc)HE.modalDoc.close();
    var html='<rml><head><link type="text/rcss" href="editor.rcss"/><style>body{pointer-events:auto;background-color:#00000088;}#dialog{position:absolute;left:50%;top:14%;margin-left:-340px;width:640px;padding:20px;background-color:#262626;border:1px #555555;}h2{font-size:19px;margin:0 0 16px;color:#eeeeee;}p{margin:10px 0;line-height:21px;}textarea{height:330px;width:97%;}input{width:97%;margin:8px 0;}#dialog-actions{margin-top:16px;}#dialog-error{color:#ebaa85;margin-top:8px;}</style></head><body><div id="dialog"><h2>'+HE.escape(title)+'</h2>'+body+'<div id="dialog-error"/><div id="dialog-actions">';
    buttons.forEach(function(b,i){html+='<button id="dialog-'+i+'">'+HE.escape(b.label)+'</button>';});
    var doc=Engine.ui.createDocument(html+'</div></div></body></rml>','/Game/UI/dialog.rml').show(true);HE.modalDoc=doc;
    buttons.forEach(function(b,i){doc.getElementById('dialog-'+i).on('click',function(){
        try{if(b.run)b.run(doc);if(HE.modalDoc===doc){doc.close();HE.modalDoc=null;}}catch(e){doc.getElementById('dialog-error').setText(e.message||e);HE.log(e.message||e);}
    });});
    if(setup)setup(doc);
};
HE.prompt=function(title,label,value,callback){HE.modal(title,'<p>'+HE.escape(label)+'</p><input id="answer" type="text"/>',[{label:'Apply',run:function(d){callback(d.getElementById('answer').getValue());}},{label:'Cancel'}],function(d){d.getElementById('answer').setValue(value);d.getElementById('answer').focus();});};
HE.jsonDialog=function(title,value,callback){HE.modal(title,'<p>Edit the complete component or resource description.</p><textarea id="json-editor"/>',[{label:'Apply',run:function(d){callback(JSON.parse(d.getElementById('json-editor').getValue()));}},{label:'Cancel'}],function(d){d.getElementById('json-editor').setValue(JSON.stringify(value,null,2));});};
HE.addComponentDialog=function(e){
    var types=Engine.componentTypes(e),html='<p>Dependencies are validated by the engine component catalog.</p><select id="component-type">';
    types.forEach(function(t){if(!Engine.hasComponent(e,t.name))html+='<option value="'+t.name+'">'+t.name+(t.dependencies.length?' (requires '+t.dependencies.map(function(d){return d.name;}).join(', ')+')':'')+'</option>';});
    html+='</select><textarea id="component-value"/>';
    var defaults={transform:{position:[0,0,0]},render:{shape:'box',material:0,scale:[1,1,1]},collider:{shape:{type:'box',halfExtents:[0.5,0.5,0.5]}},light:{type:'point',color:[1,1,1],intensity:500},data:{},interactable:{},rootMotion:{mode:'transform'},joints:[],jointColliders:[]};
    HE.modal('Add component',html,[{label:'Add',run:function(d){var name=d.getElementById('component-type').getValue(),value=JSON.parse(d.getElementById('component-value').getValue());HE.command('Add '+name,function(){Engine.addComponent(e,name,value);});}},{label:'Cancel'}],function(d){
        function fill(){var name=d.getElementById('component-type').getValue();d.getElementById('component-value').setValue(JSON.stringify(defaults[name]||{},null,2));}
        d.getElementById('component-type').on('change',fill);fill();
    });
};
HE.folder='';HE.assetSelected=null;
HE.assetKind=function(path){var p=path.toLowerCase();return p.indexOf('/maps/')>=0?'Map':p.indexOf('/materials/')>=0?'Material':p.indexOf('/scripts/')>=0?'Script':p.indexOf('/models/')>=0?'Mesh':p.indexOf('/textures/')>=0?'Texture':'Asset';};
HE.refreshAssets=function(){
    if(!HE.doc)return;
    var folder=HE.folder||HE.mount,dirs={},search=HE.el('asset-search').getValue().toLowerCase(),html='';
    HE.assets.forEach(function(ref){var parts=ref.path.split('/');for(var i=2;i<parts.length;i++)dirs[parts.slice(0,i).join('/')]=true;});
    dirs[HE.mount]=true;
    HE.el('folders').setInnerRML(Object.keys(dirs).sort().map(function(p,i){return '<div id="folder-'+i+'" class="folder '+(folder===p?'active':'')+'">'+HE.escape(p.replace(HE.mount,'Content'))+'</div>';}).join(''));
    Object.keys(dirs).sort().forEach(function(p,i){HE.bind('folder-'+i,function(){HE.folder=p;HE.refreshAssets();});});
    var refs=HE.assets.filter(function(r){return r.path.indexOf(folder+'/')===0&&(!search||r.path.toLowerCase().indexOf(search)>=0);});
    refs.forEach(function(ref,i){var type=HE.assetKind(ref.path);html+='<div id="asset-'+i+'" class="asset"><div class="asset-icon">'+(type==='Map'?'△':type==='Material'?'●':type==='Script'?'JS':'◇')+'</div><div class="asset-name">'+HE.escape(ref.path.split('/').pop())+'</div><div class="asset-type">'+type+'</div></div> ';});
    HE.el('assets').setInnerRML(html);HE.el('breadcrumb').setText(folder+'   /   '+refs.length+' assets');
    refs.forEach(function(ref,i){HE.bind('asset-'+i,function(){HE.assetSelected=ref;HE.log(ref.path+' — double-click to open');});HE.bind('asset-'+i,function(){HE.openAsset(ref);},'dblclick');});
};
HE.openAsset=function(ref){
    var asset=Engine.content.load(ref);
    if(asset.header.type==='Map'){HE.open(ref);return;}
    if(asset.header.type==='StaticMesh'){HE.add('StaticMesh',ref);return;}
    if(asset.header.type==='Material'){
        HE.modal(asset.header.name,'<p>Apply this material to the selected mesh actors, or edit its asset data.</p>',[
            {label:'Apply to selection',run:function(){HE.command('Assign material',function(){var index=Engine.scene.addMaterial(ref);HE.selected.forEach(function(e){if(Engine.hasComponent(e,'render'))Engine.setMaterial(e,index);});});}},
            {label:'Edit asset',run:function(){HE.editAsset(asset);}},{label:'Close'}]);return;
    }
    HE.editAsset(asset);
};
HE.editAsset=function(asset){
    if(asset.encoding==='raw'){HE.modal(asset.header.name,'<p>Binary '+HE.escape(asset.header.type)+' asset. Import using the existing project asset pipeline.</p>',[{label:'Close'}]);return;}
    HE.modal('Asset: '+asset.header.name,'<p>'+HE.escape(asset.ref.path)+' — '+HE.escape(asset.header.type)+'</p><textarea id="asset-editor"/>',[
        {label:'Save asset',run:function(d){HE.editable();var text=d.getElementById('asset-editor').getValue();Engine.content.save(asset.ref,asset.header,asset.encoding==='json'?JSON.parse(text):text);HE.log('Saved asset '+asset.ref.path);}}, {label:'Cancel'}
    ],function(d){d.getElementById('asset-editor').setValue(asset.encoding==='json'?JSON.stringify(asset.payload,null,2):asset.payload);});
};
HE.newAsset=function(){
    HE.modal('Create asset','<p>Path without .asset</p><input id="new-path" type="text"/><select id="new-type"><option value="Data">Data (JSON)</option><option value="Script">Script (JavaScript)</option></select><textarea id="new-payload"/>',[
        {label:'Create',run:function(d){HE.editable();var type=d.getElementById('new-type').getValue(),path=d.getElementById('new-path').getValue(),payload=d.getElementById('new-payload').getValue();Engine.content.save(path,{id:Engine.content.newId(),type:type,name:path.split('/').pop(),version:1,storage:'embedded',metadata:{}},type==='Data'?JSON.parse(payload):payload);HE.scan();}}, {label:'Cancel'}
    ],function(d){d.getElementById('new-path').setValue(HE.folder+'/NewAsset');d.getElementById('new-payload').setValue('{}');});
};
HE.worldSettings=function(){HE.jsonDialog('World settings',Engine.scene.resources(),function(value){HE.command('Edit world settings',function(){Engine.scene.resources(value);});});};
