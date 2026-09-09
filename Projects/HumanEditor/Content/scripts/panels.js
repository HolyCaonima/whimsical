HE.el = function(id){return HE.doc.getElementById(id);};
HE.bind = function(id,fn,type){HE.el(id).on(type||'click',HE.guard(fn));};
HE.closeMenu=function(){if(!HE.activeMenu)return;HE.el(HE.activeMenu).setClass('active',false);HE.activeMenu=null;HE.el('main-menu').setProperty('display','none');HE.el('menu-shield').setProperty('display','none');};
HE.closeContext=function(){HE.closeMenu();if(HE.contextDoc){HE.contextDoc.close();HE.contextDoc=null;}};
HE.openMenu=function(id){
    HE.closeContext();HE.cancelPick();HE.keys={};HE.pressed={};HE.navigation=null;
    var actions=HE.menus[id](),anchor=HE.el(id).getBounds(),menu=HE.el('main-menu');
    HE.activeMenu=id;HE.menuActions=actions;HE.menuIndex=-1;HE.el(id).setClass('active',true);
    menu.setInnerRML(actions.map(function(a,i){return (a.separator?'<div class="menu-separator"/>':'')+'<button id="menu-item-'+i+'"'+(a.enabled===false?' disabled="disabled"':'')+'>'+HE.escape(a.label)+'<span>'+(a.key||'')+'</span></button>';}).join(''));
    var bar=HE.el('menubar').getBounds();
    menu.setProperty('left',Math.max(0,Math.min(anchor.x,HE.width-264))+'px').setProperty('top',(bar.y+bar.height)+'px').setProperty('display','block');
    HE.el('menu-shield').setProperty('display','block');
    actions.forEach(function(a,i){HE.bind('menu-item-'+i,function(){HE.closeMenu();a.run();});HE.bind('menu-item-'+i,function(){HE.menuIndex=i;},'focus');});
    HE.el(id).focus();
};
HE.menuKey=function(ev){
    var k=ev.parameters.key_identifier,actions=HE.menuActions;
    if(k===81){var id=HE.activeMenu;HE.closeMenu();HE.el(id).focus();}
    else if(k===90||k===92){var next=(HE.menuIds.indexOf(HE.activeMenu)+(k===90?3:1))%4;HE.openMenu(HE.menuIds[next]);}
    else if(k===91||k===93){
        var index=HE.menuIndex;
        for(var i=0;i<actions.length;i++){index=(index+(k===91?-1:1)+actions.length)%actions.length;if(actions[index].enabled!==false)break;}
        if(actions[index].enabled!==false){HE.menuIndex=index;HE.el('menu-item-'+index).focus();}
    }else if(k===72&&HE.menuIndex>=0){var action=actions[HE.menuIndex];HE.closeMenu();HE.guard(action.run)();}
    ev.stopPropagation();
};
HE.actorContext=function(entity,x,y){
    if(Engine.simulation.state().running||HE.pending)return;
    HE.closeContext();HE.cancelPick();
    if(!entity||HE.selected.indexOf(entity)<0)HE.select(entity);
    HE.navigation=null;HE.navigationEnded=null;HE.rightGesture=null;HE.keys={};HE.pressed={};
    var selected=HE.selected.length>0;
    var actions=[
        {label:'Focus selected',key:'F',run:HE.focus,enabled:selected},
        {label:'Copy',key:'Ctrl+C',run:HE.copySelection,enabled:selected},
        {label:'Paste',key:'Ctrl+V',run:HE.paste,enabled:!!(HE.clipboard&&HE.clipboard.length)},
        {label:'Duplicate',key:'Ctrl+D',run:HE.duplicate,enabled:selected},
        {label:'Delete',key:'Del',run:HE.remove,enabled:selected},
        {label:'Toggle enabled',run:HE.toggleEnabled,enabled:selected,separator:true},
        {label:'Set parent...',run:HE.parentDialog,enabled:selected},
        {label:'Detach from parent',run:function(){HE.setParent(0);},enabled:selected}
    ];
    var left=Math.max(4,Math.min(x,HE.width-256)),top=Math.max(4,Math.min(y,HE.height-258));
    var html='<rml><head><link type="text/rcss" href="editor.rcss"/></head><body id="context-overlay"><div id="actor-menu" style="left:'+left+'px;top:'+top+'px;">';
    actions.forEach(function(a,i){if(a.separator)html+='<div class="context-separator"/>';html+='<button id="context-'+i+'"'+(a.enabled?'':' disabled="disabled"')+'>'+a.label+'<span>'+(a.key||'')+'</span></button>';});
    var doc=Engine.ui.createDocument(html+'</div></body></rml>','/Game/UI/context.rml').show(true);HE.contextDoc=doc;
    doc.getElementById('actor-menu').on('mousedown',function(ev){ev.stopPropagation();});
    doc.on('mousedown',HE.closeContext);
    doc.on('keydown',function(ev){if(ev.parameters.key_identifier===81)HE.closeContext();});
    actions.forEach(function(a,i){doc.getElementById('context-'+i).on('click',HE.guard(function(){if(!a.enabled)return;HE.closeContext();a.run();}));});
};
HE.collapsed = {};
HE.componentCollapsed={};
HE.paletteCategory='Basic';
HE.paletteItems=[
    {kind:'Entity',label:'Empty Actor',category:'Basic',icon:'◇'},
    {kind:'Box',label:'Cube',category:'Shapes',icon:'□'},
    {kind:'Capsule',label:'Capsule',category:'Shapes',icon:'◊'},
    {kind:'Point',label:'Point Light',category:'Lights',icon:'*'},
    {kind:'Spot',label:'Spot Light',category:'Lights',icon:'*'},
    {kind:'Directional',label:'Directional Light',category:'Lights',icon:'*'},
    {kind:'Rect',label:'Rect Light',category:'Lights',icon:'*'},
    {kind:'CapsuleLight',label:'Capsule Light',category:'Lights',icon:'*'}
];
HE.refreshPalette=function(){
    var search=HE.el('place-search').getValue().toLowerCase();
    var categories=['Basic','Shapes','Lights'];
    HE.el('place-categories').setInnerRML(categories.map(function(c,i){return '<button id="category-'+i+'" class="subtle '+(HE.paletteCategory===c?'active':'')+'">'+c+'</button>';}).join(''));
    categories.forEach(function(c,i){HE.bind('category-'+i,function(){HE.paletteCategory=c;HE.el('place-search').setValue('');HE.refreshPalette();});});
    var rows=HE.paletteItems.filter(function(p){return search?(p.label+' '+p.kind).toLowerCase().indexOf(search)>=0:HE.paletteCategory==='Basic'?['Entity','Box','Point'].indexOf(p.kind)>=0:p.category===HE.paletteCategory;});
    HE.el('palette').setInnerRML(rows.length?rows.map(function(p,i){return '<button id="place-'+i+'" class="wide"><span class="place-icon">'+p.icon+'</span>'+p.label+'</button>';}).join(''):'<div class="empty">No matching actors.</div>');
    rows.forEach(function(p,i){HE.bind('place-'+i,function(){HE.add(p.kind);});});
    HE.sizePalette();
};
HE.sizePalette=function(){
    var width=HE.layout.left-30;
    HE.el('place').querySelector('.pad').setProperty('width',width+'px');
    HE.el('palette').setProperty('width',width+'px');
    HE.el('place-search').setProperty('width',(width-14)+'px');
    HE.el('place-categories').setProperty('width',width+'px');
    HE.el('place-categories').querySelectorAll('button').forEach(function(el){el.setProperty('width','49px');});
    HE.el('place').querySelectorAll('.wide').forEach(function(el){el.setProperty('width',(width-18)+'px');});
};
HE.viewOptions=function(){
    var options=[['position-snap','Position snap (meters)',[0.1,0.25,0.5,1,5],HE.snap],['rotation-snap','Rotation snap (degrees)',[5,10,15,30,45,90],HE.rotationSnap],['scale-snap','Scale snap',[0.01,0.1,0.25,0.5,1],HE.scaleSnap],['camera-speed','Camera speed',[0.25,0.5,1,2,4],HE.cameraSpeed]];
    HE.modal('Viewport settings',options.map(function(o){return '<p>'+o[1]+'</p><select id="'+o[0]+'">'+o[2].map(function(v){return '<option value="'+v+'">'+v+'</option>';}).join('')+'</select>';}).join(''),[{label:'Apply',run:function(d){HE.snap=Number(d.getElementById('position-snap').getValue());HE.rotationSnap=Number(d.getElementById('rotation-snap').getValue());HE.scaleSnap=Number(d.getElementById('scale-snap').getValue());HE.cameraSpeed=Number(d.getElementById('camera-speed').getValue());}},{label:'Cancel'}],function(d){options.forEach(function(o){d.getElementById(o[0]).setValue(String(o[3]));});});
};
HE.paintSelection = function(){
    if(!HE.doc)return;
    HE.el('tree').querySelectorAll('.tree-row').forEach(function(el){var e=Number(el.getAttribute('id').replace('entity-',''));el.setClass('selected',HE.selected.indexOf(e)>=0);});
    HE.el('actor-count').setText((HE.treeShown||0)+' / '+(HE.treeCount||0)+' actors  |  '+HE.selected.length+' selected');
};
HE.refreshStatus = function(){
    if(!HE.doc)return;
    HE.el('level-label').setText(HE.levelName());
    HE.el('dirty-dot').setClass('on',HE.dirty());
    HE.el('save-state').setText(HE.dirty()?'Unsaved changes':'All saved');
    HE.el('project-label').setText(HE.contentName());
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
        el.on('mouseup',HE.guard(function(ev){if(ev.parameters.button!==1)return;ev.stopPropagation();HE.actorContext(r.entity,ev.parameters.mouse_x,ev.parameters.mouse_y);}));
        HE.doc.getElementById('fold-'+r.entity).on('click',HE.guard(function(ev){ev.stopPropagation();HE.collapsed[r.id]=!HE.collapsed[r.id];HE.refreshTree();}));
    });
    HE.treeCount=rows.length;HE.treeShown=HE.el('tree').querySelectorAll('.tree-row').length;HE.sizeTree();HE.paintSelection();
};
HE.sizeTree=function(){
    // Scroll contents need a definite width in RmlUi, just like inspector fields.
    HE.el('tree').querySelectorAll('.tree-row').forEach(function(el){el.setProperty('width',((HE.detailsWidth||HE.layout.right)-18)+'px');});
};
HE.number=function(s){if(!String(s).replace(/\s/g,'').length||!isFinite(Number(s)))throw Error('Enter a finite number.');return Number(s);};
HE.modal=function(title,body,buttons,setup){
    HE.closeContext();
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
    var defaults={transform:{position:[0,0,0],scale:[1,1,1]},render:{},collider:{shape:{type:'box',halfExtents:[0.5,0.5,0.5]}},light:{type:'point',color:[1,1,1],intensity:500},data:{},interactable:{},rootMotion:{mode:'transform'},joints:[],jointColliders:[]};
    HE.modal('Add component',html,[{label:'Add',run:function(d){var name=d.getElementById('component-type').getValue(),value=JSON.parse(d.getElementById('component-value').getValue());HE.command('Add '+name,function(){Engine.addComponent(e,name,value);});}},{label:'Cancel'}],function(d){
        function fill(){var name=d.getElementById('component-type').getValue(),value=HE.copy(defaults[name]||{});if(name==='render'){value.material=HE.defaultMaterial();value.mesh=Engine.asset('/Engine/Meshes/Box');}d.getElementById('component-value').setValue(JSON.stringify(value,null,2));}
        d.getElementById('component-type').on('change',fill);fill();
    });
};
HE.worldSettings=function(){HE.jsonDialog('World settings',Engine.scene.resources(),function(value){HE.command('Edit world settings',function(){Engine.scene.resources(value);});});};
