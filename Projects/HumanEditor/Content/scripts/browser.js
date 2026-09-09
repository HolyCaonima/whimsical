/* Content navigation and asset actions. ES5 for the project host. */
HE.folder='';HE.assetSelected=null;
HE.browser={mount:null,source:null,dirs:{},expanded:{},history:[],cursor:-1,selected:null,items:[],list:false};
HE.assetKind=function(path){var p=path.toLowerCase();return p.indexOf('/maps/')>=0?'Map':p.indexOf('/materials/')>=0?'Material':p.indexOf('/scripts/')>=0?'Script':p.indexOf('/models/')>=0?'Mesh':p.indexOf('/textures/')>=0?'Texture':'Asset';};
HE.parentFolder=function(path){return path.substring(0,path.lastIndexOf('/'))||HE.mount;};
HE.folderGlyph=function(){return '<span class="folder-glyph"><span class="folder-tab"/><span class="folder-front"/></span>';};
HE.browserIndex=function(){
    var b=HE.browser;
    if(b.mount!==HE.mount){
        b.mount=HE.mount;b.source=null;b.expanded={};b.expanded[HE.mount]=true;b.history=[];b.cursor=-1;b.selected=null;
        HE.el('asset-search').setValue('');HE.el('asset-filter').setValue('All');HE.el('folder-search').setValue('');
    }
    if(b.source===HE.assets)return;
    b.source=HE.assets;b.dirs={};b.dirs[HE.mount]={path:HE.mount,children:[],assets:0};
    HE.assets.forEach(function(ref){
        var parts=ref.path.split('/');
        for(var i=2;i<parts.length;i++){var p=parts.slice(0,i).join('/');if(!b.dirs[p])b.dirs[p]={path:p,children:[],assets:0};b.dirs[p].assets++;}
    });
    Object.keys(b.dirs).sort().forEach(function(p){if(p!==HE.mount)b.dirs[HE.parentFolder(p)].children.push(p);});
};
HE.browserState=function(){return {path:HE.folder,search:HE.el('asset-search').getValue(),filter:HE.el('asset-filter').getValue()};};
HE.browseFolder=function(path){
    var b=HE.browser;HE.browserIndex();
    if(!b.dirs[path])return;
    if(b.cursor>=0)b.history[b.cursor]=HE.browserState();
    HE.folder=path;HE.el('asset-search').setValue('');b.selected=null;HE.assetSelected=null;
    if(b.cursor<0||b.history[b.cursor].path!==path){b.history=b.history.slice(0,b.cursor+1);b.history.push(HE.browserState());b.cursor++;}
    HE.revealFolder(path);HE.refreshAssets();HE.el('assets').focus();HE.browserFocused=true;HE.editingText=false;
};
HE.browserTravel=function(delta){
    var b=HE.browser,next=b.cursor+delta;if(next<0||next>=b.history.length)return;
    b.history[b.cursor]=HE.browserState();b.cursor=next;var state=b.history[next];
    HE.folder=state.path;HE.el('asset-search').setValue(state.search);HE.el('asset-filter').setValue(state.filter);
    b.selected=null;HE.assetSelected=null;HE.revealFolder(HE.folder);HE.refreshAssets();HE.el('assets').focus();HE.browserFocused=true;HE.editingText=false;
};
HE.revealFolder=function(path){var b=HE.browser;while(path!==HE.mount){path=HE.parentFolder(path);b.expanded[path]=true;}};
HE.refreshFolders=function(){
    var b=HE.browser,query=HE.el('folder-search').getValue().toLowerCase(),visible=[],matches={};
    if(query)Object.keys(b.dirs).forEach(function(p){if(p.substring(HE.mount.length).toLowerCase().indexOf(query)<0)return;matches[p]=true;while(p!==HE.mount){p=HE.parentFolder(p);matches[p]=true;}});
    function visit(p,depth){if(query&&!matches[p])return;visible.push({path:p,depth:depth});if(query||b.expanded[p])b.dirs[p].children.forEach(function(c){visit(c,depth+1);});}
    visit(HE.mount,0);
    HE.el('folders').setInnerRML(visible.map(function(row,i){var p=row.path,kids=b.dirs[p].children.length;return '<div id="folder-'+i+'" class="folder '+(HE.folder===p?'active':'')+'" style="padding-left:'+(6+row.depth*13)+'px;"><span id="folder-fold-'+i+'" class="folder-fold">'+(kids?(query||b.expanded[p]?'-':'+'):'')+'</span>'+HE.folderGlyph()+'<span class="folder-label">'+HE.escape(p===HE.mount?'Content':p.split('/').pop())+'</span></div>';}).join('')||'<div class="empty">No matching folders.</div>');
    visible.forEach(function(row,i){
        HE.bind('folder-'+i,function(){HE.browseFolder(row.path);});
        HE.bind('folder-fold-'+i,function(ev){ev.stopPropagation();b.expanded[row.path]=!b.expanded[row.path];HE.refreshFolders();});
    });
};
HE.selectBrowserItem=function(item){
    HE.browser.selected=item;HE.assetSelected=item.kind==='folder'?null:item.ref;
    HE.browser.items.forEach(function(row){HE.el(row.element).setClass('selected',row.path===item.path&&row.kind===item.kind);});
    HE.el('asset-location').setText(item.path.replace(HE.mount,'Content'));
    HE.el(item.element).focus().scrollIntoView('nearest');
};
HE.openBrowserItem=function(item){if(item.kind==='folder')HE.browseFolder(item.path);else HE.openAsset(item.ref);};
HE.browserKey=function(ev){
    var p=ev.parameters,k=p.key_identifier;
    // Asset focus must not send Delete / Ctrl+D to the scene selection.
    if(k===72&&!HE.editingText&&HE.browser.selected){HE.openBrowserItem(HE.browser.selected);ev.stopPropagation();}
    if(k===69&&!HE.editingText){HE.browseFolder(HE.parentFolder(HE.folder));ev.stopPropagation();}
    if(k===1&&p.ctrl_key){HE.toggleContent();ev.stopPropagation();}
    if(k===81){HE.browser.selected=null;HE.el('asset-search').setValue('');HE.refreshAssets();ev.stopPropagation();}
};
HE.refreshAssets=function(){
    if(!HE.doc)return;HE.browserIndex();
    var b=HE.browser;
    while(!b.dirs[HE.folder]&&HE.folder!==HE.mount)HE.folder=HE.parentFolder(HE.folder);
    if(b.cursor<0){b.history.push(HE.browserState());b.cursor=0;}
    HE.refreshFolders();
    var folder=HE.folder,search=HE.el('asset-search').getValue().toLowerCase(),filter=HE.el('asset-filter').getValue()||'All';
    var descending=HE.el('asset-sort').getValue()==='desc',items=[];
    Object.keys(b.dirs).forEach(function(p){
        if(p===folder||p.indexOf(folder+'/')!==0)return;
        var relative=p.substring(folder.length+1);
        if(search?relative.toLowerCase().indexOf(search)<0:HE.parentFolder(p)!==folder)return;
        items.push({kind:'folder',path:p,name:p.split('/').pop(),type:'Folder'});
    });
    HE.assets.forEach(function(ref){
        if(ref.path.indexOf(folder+'/')!==0)return;
        var relative=ref.path.substring(folder.length+1),type=HE.assetKind(ref.path);
        if(!search&&filter==='All'&&relative.indexOf('/')>=0)return;
        if(search&&relative.toLowerCase().indexOf(search)<0)return;
        if(filter!=='All'&&type!==filter)return;
        items.push({kind:'asset',path:ref.path,ref:ref,name:ref.path.split('/').pop(),type:type});
    });
    items.sort(function(a,c){if(a.kind!==c.kind)return a.kind==='folder'?-1:1;return (descending?-1:1)*a.name.toLowerCase().localeCompare(c.name.toLowerCase())||a.path.localeCompare(c.path);});
    var folders=0,assets=0;
    items.forEach(function(item){item.element=item.kind==='folder'?'asset-folder-'+folders++:'asset-'+assets++;});
    // Navigation stays first regardless of filtering/sort, and is not a real directory entry.
    if(folder!==HE.mount)items.unshift({kind:'folder',parent:true,path:HE.parentFolder(folder),name:'..',type:'Parent folder',element:'asset-parent'});
    b.items=items;
    if(b.selected){b.selected=items.filter(function(item){return item.kind===b.selected.kind&&item.path===b.selected.path;})[0]||null;HE.assetSelected=b.selected&&b.selected.ref||null;}
    HE.el('assets').setClass('list-view',b.list);
    HE.el('asset-grid').setInnerRML(items.map(function(item){
        var icon=item.kind==='folder'?HE.folderGlyph():item.type==='Map'?'△':item.type==='Material'?'●':item.type==='Script'?'JS':'◇';
        return '<div id="'+item.element+'" class="asset '+(item.kind==='folder'?'folder-card ':'')+(b.selected&&b.selected.path===item.path?'selected':'')+'"><div class="asset-icon type-'+(item.kind==='folder'?'Folder':item.type)+'">'+icon+'</div><div class="asset-name">'+HE.escape(item.name)+'</div><div class="asset-type">'+item.type+'</div><div class="asset-path">'+HE.escape((item.parent?item.path:HE.parentFolder(item.path)).replace(HE.mount,'Content'))+'</div></div> ';
    }).join('')+(folders+assets===0?'<div class="empty">'+(search||filter!=='All'?'No matching items. Clear the search or choose All categories.':'This folder contains no indexed assets.')+'</div>':''));
    items.forEach(function(item){HE.bind(item.element,function(){HE.selectBrowserItem(item);});HE.bind(item.element,function(){HE.openBrowserItem(item);},'dblclick');});
    var parts=folder.split('/'),crumbs=[];for(var n=2;n<=parts.length;n++)crumbs.push(parts.slice(0,n).join('/'));
    HE.el('breadcrumb').setInnerRML(crumbs.map(function(p,i){return (i?' / ':'')+'<button id="crumb-'+i+'" class="subtle">'+HE.escape(i?p.split('/').pop():'Content')+'</button>';}).join(''));
    crumbs.forEach(function(p,i){HE.bind('crumb-'+i,function(){HE.browseFolder(p);});});
    HE.el('asset-count').setText(folders+' folders / '+assets+' assets'+(search||filter!=='All'?' (including subfolders)':''));
    HE.el('asset-location').setText((b.selected?b.selected.path:folder).replace(HE.mount,'Content'));
    HE.el('asset-tiles').setClass('active',!b.list);HE.el('asset-list').setClass('active',b.list);
    [['asset-up',folder===HE.mount],['asset-back',b.cursor<=0],['asset-forward',b.cursor>=b.history.length-1]].forEach(function(pair){HE.enable(HE.el(pair[0]),!pair[1]);});
    HE.sizeBrowser();
};
HE.sizeBrowser=function(){
    var width=Math.max(1,(HE.width||1440)-(HE.detailsWidth||360)-254);
    HE.el('asset-area').setProperty('width',width+'px').setClass('compact',width<600);
    HE.el('assets').setProperty('width',width+'px');
    HE.el('asset-grid').setProperty('width',Math.max(1,width-18)+'px');
    HE.el('asset-controls').setProperty('width',width+'px');
    HE.el('assets').querySelectorAll('.asset').forEach(function(el){el.setProperty('width',HE.browser.list?(width-18)+'px':'112px');});
};
HE.openAsset=function(ref){
    var asset=Engine.content.load(ref);
    if(asset.header.type==='Map'){HE.open(ref);return;}
    if(asset.header.type==='StaticMesh'){HE.add('StaticMesh',ref);return;}
    if(asset.header.type==='Material'){
        HE.modal(asset.header.name,'<p>Apply this material to the selected mesh actors, or edit its asset data.</p>',[
            {label:'Apply to selection',run:function(){HE.command('Assign material',function(){HE.selected.forEach(function(e){if(Engine.hasComponent(e,'render'))Engine.setMaterial(e,ref);});});}},
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
    HE.modal('Create asset','<p>Path without .asset</p><input id="new-path" type="text"/><select id="new-type"><option value="Data">Data (JSON)</option><option value="Material">Material</option><option value="Script">Script (JavaScript)</option></select><textarea id="new-payload"/>',[
        {label:'Create',run:function(d){HE.editable();var type=d.getElementById('new-type').getValue(),path=d.getElementById('new-path').getValue(),payload=d.getElementById('new-payload').getValue();Engine.content.save(path,{id:Engine.content.newId(),type:type,name:path.split('/').pop(),version:1,storage:'embedded',metadata:{}},type==='Script'?payload:JSON.parse(payload));HE.scan();}}, {label:'Cancel'}
    ],function(d){
        d.getElementById('new-path').setValue(HE.folder+'/NewAsset');d.getElementById('new-payload').setValue('{}');
        d.getElementById('new-type').on('change',HE.guard(function(){
            var type=d.getElementById('new-type').getValue(),payload=type==='Script'?'':'{}';
            if(type==='Material'){
                var shaders=HE.assetChoices('Shader');
                payload=JSON.stringify({shader:shaders.length?shaders[0].ref:null,properties:{},textures:{}},null,2);
            }
            d.getElementById('new-payload').setValue(payload);
        }));
    });
};
