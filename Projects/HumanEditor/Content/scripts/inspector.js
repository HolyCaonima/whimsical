/* Project-owned presentation of component values; Engine remains the authority on their contracts. */
HE.detailDrafts={};
HE.detailGroups={};
HE.isAssetRef=function(value){return value&&typeof value==='object'&&typeof value.id==='string'&&typeof value.path==='string';};
HE.assetChoices=function(type){
    return HE.assets.concat(Engine.content.browse('/Engine')).map(function(ref){return Engine.content.describe(ref);}).filter(function(asset){return asset.header.type===type;})
        .sort(function(a,b){return a.header.name.localeCompare(b.header.name)||a.ref.path.localeCompare(b.ref.path);});
};
HE.defaultMaterial=function(){
    var visuals=HE.entities().filter(function(e){return Engine.hasComponent(e,'render');});
    if(visuals.length)return Engine.component(visuals[0],'render').material;
    var choices=HE.assetChoices('Material');
    if(!choices.length)throw Error('Create a Material asset in this project before adding geometry.');
    return choices[0].ref;
};
HE.assetMatchScore=function(text,query){
    text=text.toLowerCase();
    var at=text.indexOf(query);
    if(at>=0)return at+(text===query?0:10);
    var previous=-1,score=40;
    for(var i=0;i<query.length;i++){
        var next=text.indexOf(query[i],previous+1);
        if(next<0)return -1;
        score+=next-previous-1;previous=next;
    }
    return score;
};
HE.pickDetailAsset=function(f){
    HE.closeContext();HE.cancelPick();HE.keys={};HE.pressed={};HE.navigation=null;
    var current=Engine.content.describe(f.raw),choices=HE.assetChoices(current.header.type);
    if(!choices.some(function(a){return a.ref.id===current.ref.id&&a.ref.source===current.ref.source;}))choices.unshift(current);
    var anchor=HE.el(f.id).getBounds(),width=Math.min(380,HE.width-16),below=HE.height-anchor.y-anchor.height-12,above=anchor.y-12;
    var rows=Math.min(6,Math.max(1,choices.length),Math.max(1,Math.floor((Math.max(below,above)-78)/42)));
    var height=rows*42+78,left=Math.max(8,Math.min(anchor.x+anchor.width-width,HE.width-width-8));
    var top=below>=height?anchor.y+anchor.height+4:Math.max(8,anchor.y-height-4);
    var html='<rml><head><link type="text/rcss" href="editor.rcss"/></head><body id="context-overlay"><div id="detail-asset-picker" style="left:'+left+'px;top:'+top+'px;width:'+width+'px;">'+
        '<input id="asset-picker-search" type="text" placeholder="Search '+HE.escape(current.header.type.toLowerCase())+' name or path..."/>'+
        '<div id="asset-picker-results" style="height:'+(rows*42)+'px;"/><div id="asset-picker-count"/></div></body></rml>';
    var doc=Engine.ui.createDocument(html,'/Game/UI/asset-picker.rml').show(true);HE.contextDoc=doc;
    var search=doc.getElementById('asset-picker-search'),results=doc.getElementById('asset-picker-results'),matches=[],active=0,first=0;
    function close(){HE.closeContext();HE.el(f.id).focus();}
    function choose(index){f.raw=matches[index].ref;HE.paintDetailField(f);HE.updateDetailDraft(f.component);close();}
    function paint(){
        var visible=matches.slice(first,first+rows);
        results.setInnerRML(visible.length?visible.map(function(a,i){
            var selected=a.ref.id===current.ref.id&&a.ref.source===current.ref.source;
            return '<button class="asset-picker-item '+(first+i===active?'active ':'')+(selected?'current':'')+'" id="asset-choice-'+i+'" title="'+HE.escape(a.ref.path)+'">'+
                '<span class="asset-picker-name">'+HE.escape(a.header.name)+(selected?'  *':'')+'</span><span class="asset-picker-path">'+HE.escape(a.ref.path)+'</span></button>';
        }).join(''):'<div class="asset-picker-empty">No matching assets</div>');
        visible.forEach(function(a,i){doc.getElementById('asset-choice-'+i).on('click',HE.guard(function(){choose(first+i);}));});
        doc.getElementById('asset-picker-count').setText((matches.length?(first+1)+'–'+(first+visible.length)+' / '+matches.length:'0 results')+'   ·   Scroll / ↑ ↓   Enter to choose');
    }
    function filter(){
        var query=search.getValue().toLowerCase().replace(/^\s+|\s+$/g,''),terms=query?query.split(/\s+/):[];
        matches=choices.map(function(a){
            var score=0;
            for(var i=0;i<terms.length;i++){
                var name=HE.assetMatchScore(a.header.name,terms[i]),path=HE.assetMatchScore(a.ref.path,terms[i]);
                if(name<0&&path<0)return null;
                score+=name>=0?name:path+100;
            }
            return {asset:a,score:score};
        }).filter(function(a){return a!==null;}).sort(function(a,b){return a.score-b.score||a.asset.header.name.localeCompare(b.asset.header.name)||a.asset.ref.path.localeCompare(b.asset.ref.path);})
            .map(function(a){return a.asset;});
        active=0;
        if(!query)matches.forEach(function(a,i){if(a.ref.id===current.ref.id&&a.ref.source===current.ref.source)active=i;});
        first=Math.max(0,Math.min(active,matches.length-rows));paint();
    }
    doc.getElementById('detail-asset-picker').on('mousedown',function(ev){ev.stopPropagation();});
    doc.on('mousedown',HE.closeContext);
    doc.on('keydown',HE.guard(function(ev){
        var key=ev.parameters.key_identifier;
        if(key===81){ev.stopPropagation();close();}
        else if(key===72){ev.stopPropagation();if(matches.length)choose(active);}
        else if(key===91||key===93){
            ev.stopPropagation();if(!matches.length)return;
            active=Math.max(0,Math.min(matches.length-1,active+(key===91?-1:1)));
            first=Math.max(0,Math.min(first,active));if(active>=first+rows)first=active-rows+1;
            paint();search.focus();
        }
    }),true);
    results.on('mousescroll',function(ev){
        ev.stopPropagation();var delta=ev.parameters.wheel_delta_y;
        if(!delta)return;
        first=Math.max(0,Math.min(Math.max(0,matches.length-rows),first+(delta>0?1:-1)));
        active=Math.max(first,Math.min(active,first+rows-1));paint();
    });
    search.on('change',HE.guard(filter));filter();search.focus();
};
HE.detailLabel=function(key){return key.replace(/([a-z0-9])([A-Z])/g,'$1 $2').replace(/_/g,' ').replace(/^./,function(c){return c.toUpperCase();});};
HE.detailOrder=function(value,component){
    var order={transform:['position','rotation','parent'],render:['mesh','material','scale','offset','visible','castShadow','animationScale'],light:['type','color','intensity'],collider:['enabled','shape']}[component]||[];
    return Object.keys(value).sort(function(a,b){var ai=order.indexOf(a),bi=order.indexOf(b);return (ai<0?99:ai)-(bi<0?99:bi)||a.localeCompare(b);});
};
HE.detailNumber=function(value){return value%1===0?String(value):String(Number(value.toPrecision(7)));};
HE.detailRaw=function(value){return HE.isAssetRef(value)?HE.copy(value):Array.isArray(value)?value.map(HE.detailNumber):typeof value==='boolean'?value:typeof value==='number'?HE.detailNumber(value):String(value);};
HE.detailChanged=function(f){return JSON.stringify(f.raw)!==JSON.stringify(HE.detailRaw(f.value));};
HE.paintDetailField=function(f){
    if(f.asset){var asset=Engine.content.describe(f.raw);HE.el(f.id+'-name').setText(asset.header.name);HE.el(f.id).setAttribute('title',asset.ref.path+' — Click to choose '+asset.header.type);}
    else if(Array.isArray(f.raw))f.raw.forEach(function(v,i){HE.el(f.id+'-'+i).setValue(v);});
    else if(typeof f.raw==='boolean'){HE.el(f.id).setClass('on',f.raw);HE.el(f.id+'-state').setText(f.raw?'On':'Off');}
    else HE.el(f.id).setValue(f.raw);
    HE.el(f.id+'-row').setClass('modified',HE.detailChanged(f));
};
HE.updateDetailField=function(f){
    if(Array.isArray(f.value))f.raw=f.value.map(function(v,i){return HE.el(f.id+'-'+i).getValue();});
    else if(typeof f.value!=='boolean')f.raw=HE.el(f.id).getValue();
    HE.el(f.id+'-row').setClass('modified',HE.detailChanged(f));
    HE.updateDetailDraft(f.component);
};
HE.updateDetailDraft=function(name){
    var section=HE.detailSections[name],inputs={},changed=false;
    section.fields.forEach(function(f){if(HE.detailChanged(f)){inputs[JSON.stringify(f.path)]=f.raw;changed=true;}});
    if(changed)HE.detailDrafts[section.draftKey]={baseline:section.baseline,inputs:inputs};
    else delete HE.detailDrafts[section.draftKey];
    HE.el('actions-'+name).setProperty('display',changed?'flex':'none');
    HE.el('section-'+name).setClass('modified',changed);
    HE.el('json-'+name)[changed?'setAttribute':'removeAttribute']('disabled','disabled');
    HE.el('json-'+name).setAttribute('title',changed?'Apply or revert field changes before editing JSON':'Edit complete component JSON');
    HE.el('error-'+name).setText('');
};
HE.applyDetailComponent=function(name){
    var section=HE.detailSections[name],value=HE.copy(Engine.component(HE.detailEntity,name));
    try{
        section.fields.forEach(function(f){
            if(!HE.detailChanged(f))return;
            // Formatting must not round untouched axes when another axis is edited.
            var parsed=Array.isArray(f.value)?f.raw.map(function(raw,i){return raw===HE.detailNumber(f.value[i])?f.value[i]:HE.number(raw);}):typeof f.value==='number'?HE.number(f.raw):f.raw;
            if(!f.path.length)value=parsed;
            else{var target=value;f.path.slice(0,-1).forEach(function(k){target=target[k];});target[f.path[f.path.length-1]]=parsed;}
        });
        if(!HE.detailDrafts[section.draftKey])return;
        HE.command('Edit '+name,function(){Engine.setComponent(HE.detailEntity,name,value);});
        delete HE.detailDrafts[section.draftKey];
    }catch(error){var message=error.message||String(error);HE.el('error-'+name).setText(message);HE.log(message);}
};
HE.revertDetailComponent=function(name){
    HE.detailSections[name].fields.forEach(function(f){f.raw=HE.detailRaw(f.value);HE.paintDetailField(f);});
    HE.updateDetailDraft(name);
};
HE.refreshDetails=function(){
    if(!HE.doc)return;
    HE.detailRunning=Engine.simulation.state().running;
    HE.detailFields=[];HE.detailSections={};
    HE.detailGroupRows=[];
    HE.el('selection-count').setText(HE.selected.length?HE.selected.length+' selected':'');
    if(!HE.selected.length){HE.el('inspector').setInnerRML('<div class="empty">No actor selected<br/>Choose an actor in the viewport<br/>or World Outliner.</div>');return;}
    var e=HE.selected[HE.selected.length-1];if(!Engine.alive(e))return;
    HE.detailEntity=e;
    var row=Engine.entity(e),fields=HE.detailFields,groups=[],locked=HE.detailRunning;
    var html='<div class="inspect-pad"><div class="actor-summary"><div class="actor-name-row"><input id="entity-name" type="text"/><button id="rename-entity" class="subtle">Rename</button></div>';
    html+='<div class="actor-meta"><span>Actor</span><span title="'+HE.escape(row.id)+'">ID '+e+'</span></div>';
    if(HE.selected.length>1)html+='<div class="inspect-notice">Editing '+HE.escape(row.name)+' only. '+HE.selected.length+' actors selected.</div>';
    if(locked)html+='<div class="inspect-notice">Stop simulation to edit properties.</div>';
    html+='<div class="actor-actions"><button id="entity-enabled" class="bool-toggle '+(row.enabled?'on':'')+'"><span class="toggle-mark"/><span>'+ (row.enabled?'Enabled':'Disabled')+'</span></button><button id="add-component" class="subtle">+ Component</button></div></div>';
    var priority=['transform','render','light','collider'];
    Object.keys(row.components).sort(function(a,b){var ai=priority.indexOf(a),bi=priority.indexOf(b);return (ai<0?99:ai)-(bi<0?99:bi)||a.localeCompare(b);}).forEach(function(name){
        var value=row.components[name],draftKey=JSON.stringify([row.id,name]),baseline=JSON.stringify(value),draft=HE.detailDrafts[draftKey];
        if(draft&&draft.baseline!==baseline){delete HE.detailDrafts[draftKey];draft=null;}
        var section=HE.detailSections[name]={fields:[],draftKey:draftKey,baseline:baseline};
        html+='<div class="component-section" id="section-'+name+'"><div class="component-title"><button id="collapse-'+name+'" class="component-toggle"/><button id="json-'+name+'" class="component-json-button" title="Edit complete component JSON">JSON</button><button id="remove-'+name+'" class="component-remove" title="Remove component">Remove</button></div><div id="component-body-'+name+'">';
        function property(v,path,ancestors){
            var key=path.length?String(path[path.length-1]):'Value',label=HE.detailLabel(key),id='field-'+fields.length;
            var vector=Array.isArray(v)&&v.length>=2&&v.length<=4&&v.every(function(n){return typeof n==='number';});
            var asset=HE.isAssetRef(v);
            if(v&&typeof v==='object'&&!Array.isArray(v)&&!asset&&Object.keys(v).length){
                var groupId='detail-group-'+groups.length,groupKey=JSON.stringify([name,path]);
                groups.push({id:groupId,key:groupKey,component:name,path:path,ancestors:ancestors});
                html+='<div class="property-group" id="'+groupId+'"><button class="property-group-toggle" id="'+groupId+'-toggle"/><div id="'+groupId+'-body">';
                HE.detailOrder(v,path.length?'':name).forEach(function(k){property(v[k],path.concat(k),ancestors.concat(groupId));});
                html+='</div></div>';return;
            }
            var parent=name==='transform'&&key==='parent',complex=v===null||typeof v==='object'&&!vector&&!asset;
            var f={id:id,component:name,key:path.join('.'),label:label,path:path,value:v,ancestors:ancestors,asset:asset,readonly:parent||complex};
            fields.push(f);
            if(!f.readonly){f.raw=draft&&Object.prototype.hasOwnProperty.call(draft.inputs,JSON.stringify(path))?draft.inputs[JSON.stringify(path)]:HE.detailRaw(v);section.fields.push(f);}
            html+='<div class="field" id="'+id+'-row"><span class="field-label" title="'+HE.escape(f.key)+'">'+HE.escape(label)+(name==='transform'&&key==='rotation'?'<span class="field-unit">Quaternion</span>':'')+'</span><div class="field-control">';
            if(parent){var p=v?Engine.findEntity(v):0;html+='<button id="'+id+'" class="parent-link">'+HE.escape(p?Engine.entity(p).name:'None')+'</button>';}
            else if(asset)html+='<button id="'+id+'" class="asset-reference"><span id="'+id+'-name"/><span class="asset-choose">...</span></button>';
            else if(complex)html+='<span class="field-summary">'+(Array.isArray(v)?v.length+' items':v===null?'None':'Empty object')+' <span class="field-unit">Edit in JSON</span></span>';
            else if(vector){
                var axes=/color/i.test(key)?['R','G','B','A']:['X','Y','Z','W'];
                html+='<div class="vector">';v.forEach(function(n,i){html+='<div class="vector-axis axis-'+i+'"><span>'+axes[i]+'</span><input type="text" id="'+id+'-'+i+'" title="'+HE.escape(label)+' '+axes[i]+'"/></div>';});html+='</div>';
            }else if(typeof v==='boolean')html+='<button id="'+id+'" class="bool-toggle"><span class="toggle-mark"/><span id="'+id+'-state"/></button>';
            else html+='<input type="text" class="'+(typeof v==='number'?'number':'')+'" id="'+id+'"/>';
            html+='</div></div>';
        }
        if(value&&typeof value==='object'&&!Array.isArray(value))HE.detailOrder(value,name).forEach(function(key){property(value[key],[key],[]);});
        else property(value,[],[]);
        if(!fields.some(function(f){return f.component===name;}))html+='<div class="component-empty">No properties. Use JSON to add data.</div>';
        html+='<div class="component-error" id="error-'+name+'"/><div class="component-actions" id="actions-'+name+'"><span>Unapplied changes</span><button id="revert-'+name+'">Revert</button><button id="apply-'+name+'" class="primary">Apply</button></div></div></div>';
    });
    if(row.derived.length)html+='<div class="derived">Derived · Read only<br/>'+HE.escape(row.derived.map(HE.detailLabel).join(', '))+'</div>';
    html+='<div id="details-empty" class="empty">No matching properties.</div></div>';
    HE.el('inspector').setInnerRML(html);HE.detailGroupRows=groups;HE.el('entity-name').setValue(row.name);HE.sizeInspector();
    HE.el('inspector').querySelectorAll('input[type="text"]').forEach(function(input){
        input.on('click',function(){input.select();});
    });
    HE.bind('rename-entity',function(){var name=HE.el('entity-name').getValue();if(name!==row.name)HE.command('Rename actor',function(){Engine.rename(e,name);});});
    HE.bind('entity-name',function(ev){if(ev.parameters.key_identifier===72){var name=HE.el('entity-name').getValue();if(name!==row.name)HE.command('Rename actor',function(){Engine.rename(e,name);});}else if(ev.parameters.key_identifier===81)HE.el('entity-name').setValue(row.name);},'keydown');
    HE.bind('entity-enabled',function(){HE.command(row.enabled?'Disable actor':'Enable actor',function(){Engine.enabled(e,!row.enabled);});});
    HE.bind('add-component',function(){HE.addComponentDialog(e);});
    Object.keys(row.components).forEach(function(name){
        HE.bind('collapse-'+name,function(){HE.componentCollapsed[name]=!HE.componentCollapsed[name];HE.filterDetails();});
        HE.bind('remove-'+name,function(){HE.command('Remove '+name,function(){Engine.removeComponent(e,name);});delete HE.detailDrafts[JSON.stringify([row.id,name])];});
        HE.bind('json-'+name,function(){HE.jsonDialog(HE.detailLabel(name)+' · JSON',Engine.component(e,name),function(value){HE.command('Edit '+name,function(){Engine.setComponent(e,name,value);});});});
        HE.bind('apply-'+name,function(){HE.applyDetailComponent(name);});
        HE.bind('revert-'+name,function(){HE.revertDetailComponent(name);});
        HE.updateDetailDraft(name);
    });
    groups.forEach(function(g){HE.bind(g.id+'-toggle',function(){HE.detailGroups[g.key]=!HE.detailGroups[g.key];HE.filterDetails();});});
    fields.forEach(function(f){
        if(f.readonly){if(f.component==='transform'&&f.key==='parent')HE.bind(f.id,function(){HE.parentDialog(e);});return;}
        HE.paintDetailField(f);
        if(f.asset)HE.bind(f.id,function(){HE.pickDetailAsset(f);});
        else if(typeof f.value==='boolean')HE.bind(f.id,function(){f.raw=!f.raw;HE.paintDetailField(f);HE.updateDetailDraft(f.component);});
        else (Array.isArray(f.value)?f.value.map(function(v,i){return f.id+'-'+i;}):[f.id]).forEach(function(id){
            HE.bind(id,function(){HE.updateDetailField(f);},'change');
            HE.bind(id,function(ev){
                if(ev.parameters.key_identifier===72){HE.updateDetailField(f);HE.applyDetailComponent(f.component);ev.stopPropagation();}
                else if(ev.parameters.key_identifier===81){f.raw=HE.detailRaw(f.value);HE.paintDetailField(f);HE.updateDetailDraft(f.component);ev.stopPropagation();}
            },'keydown');
        });
    });
    if(locked)HE.el('inspector').querySelectorAll('input,button').forEach(function(el){if(!el.hasClass('component-toggle')&&!el.hasClass('property-group-toggle'))el.setAttribute('disabled','disabled');});
    HE.filterDetails();
};
HE.filterDetails=function(){
    var search=HE.el('details-search').getValue().toLowerCase().replace(/^\s+|\s+$/g,''),hits=0;
    function matches(text){return text.toLowerCase().indexOf(search)>=0;}
    HE.el('inspector').querySelectorAll('.component-section').forEach(function(section){
        var name=section.getAttribute('id').substring(8),all=!search||matches(name)||matches(HE.detailLabel(name));
        var fields=HE.detailFields.filter(function(f){return f.component===name;});
        fields.forEach(function(f){f.hit=all||matches(f.key)||matches(HE.detailLabel(f.key));HE.el(f.id+'-row').setProperty('display',f.hit?'flex':'none');});
        var hit=all||fields.some(function(f){return f.hit;});if(hit)hits++;
        section.setProperty('display',hit?'block':'none');
        HE.el('component-body-'+name).setProperty('display',search||!HE.componentCollapsed[name]?'block':'none');
        HE.el('collapse-'+name).setText((!search&&HE.componentCollapsed[name]?'+  ':'−  ')+HE.detailLabel(name));
    });
    (HE.detailGroupRows||[]).forEach(function(g){
        var hit=HE.detailFields.some(function(f){return f.hit&&f.ancestors.indexOf(g.id)>=0;}),closed=!search&&HE.detailGroups[g.key];
        HE.el(g.id).setProperty('display',hit?'block':'none');
        HE.el(g.id+'-body').setProperty('display',closed?'none':'block');
        HE.el(g.id+'-toggle').setText((closed?'+  ':'−  ')+HE.detailLabel(String(g.path[g.path.length-1])));
    });
    var empty=HE.el('details-empty');if(empty)empty.setProperty('display',hits?'none':'block');
};
HE.sizeInspector=function(){
    var pad=HE.el('inspector').querySelector('.inspect-pad');if(!pad)return;
    // RmlUi scroll contents need a definite width; all child rows use flex layout.
    pad.setProperty('width',((HE.detailsWidth||340)-16)+'px');
};
