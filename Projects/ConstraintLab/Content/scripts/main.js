// Panel frame. The shell renders whatever rows the active experiment declares,
// so adding an experiment never means editing the interface by hand.
Lab.uiClock=0;Lab.rendered='';Lab.held={};
Lab.text=function(id,value){Lab.ui[id].setText(value);};
Lab.notice=function(message){if(message){Lab.text('notice',message);Engine.log('LAB: '+message);}};
Lab.grouped=function(value){return String(value).replace(/\B(?=(\d{3})+(?!\d))/g,',');};
Lab.run=function(handler){
    if(typeof handler==='function'){handler();return;}
    Lab.test.actions.forEach(function(action){if(action.id===handler)Lab.notice(action.click());});
};
Lab.renderPanel=function(){
    var test=Lab.test,document=Lab.document;
    Lab.rendered=test.id;
    Lab.text('eyebrow',test.eyebrow);Lab.text('title',test.title);Lab.text('subtitle',test.subtitle);
    Lab.text('equation',test.equation);Lab.ui.legend.setInnerRML(test.legend);
    Lab.text('size-label',test.sizes.label);
    Lab.ui.sizes.setInnerRML(test.sizes.options.map(function(option,i){
        return '<button id="size-'+i+'">'+option.label+'</button>';}).join(''));
    Lab.ui.actions.setInnerRML(test.actions.map(function(action){
        return '<button id="action-'+action.id+'" class="wide">'+action.text()+'</button>';}).join(''));
    Lab.ui.panel.setInnerRML('<div class="section-title">'+test.panelTitle+'</div>'+test.rows.map(function(row){
        return row.kind==='stepper'
            ? '<div class="control-row"><span>'+row.label+'</span><button id="row-'+row.id+'-down">−</button>'+
              '<b id="row-'+row.id+'">·</b><button id="row-'+row.id+'-up">+</button></div>'
            : '<div class="control-row"><span>'+row.label+'</span><button id="row-'+row.id+'-cycle">'+
              '<b id="row-'+row.id+'">·</b> /</button></div>';}).join(''));
    Lab.tiles=test.sizes.options.map(function(option,i){
        var button=document.getElementById('size-'+i);
        button.on('click',function(){Lab.notice(test.sizes.set(option.value));});
        return {button:button,value:option.value};});
    Lab.switches=test.actions.map(function(action){
        var button=document.getElementById('action-'+action.id);
        button.on('click',function(){Lab.notice(action.click());});
        return {button:button,action:action};});
    Lab.fields=test.rows.map(function(row){
        if(row.kind==='stepper'){
            document.getElementById('row-'+row.id+'-down').on('click',function(){row.step(-1);});
            document.getElementById('row-'+row.id+'-up').on('click',function(){row.step(1);});
        } else document.getElementById('row-'+row.id+'-cycle').on('click',function(){Lab.notice(row.click());});
        return {value:document.getElementById('row-'+row.id),row:row};});
};
function initialize(){
    Lab.definitions();Lab.assets();
    Lab.document=Engine.ui.loadDocument('/Game/UI/lab.rml').show();Lab.ui={};
    ['viewport','status','population','relation-count','tick','time','gpu','strain','diagnostics',
     'stiffness-label','pause','mode','notice','sample-note','equation','meter','footer-state',
     'eyebrow','title','subtitle','legend','size-label','sizes','actions','panel','tabs']
        .forEach(function(id){Lab.ui[id]=Lab.document.getElementById(id);});
    function click(id,fn){Lab.document.getElementById(id).on('click',fn);}
    click('pause',function(){Lab.paused=!Lab.paused;});
    click('step',function(){Lab.paused=true;Lab.oneStep=true;});
    click('reset',function(){Lab.reset();Lab.notice('恢复实验的初始状态');});
    click('kick',function(){Lab.notice(Lab.test.push());});
    click('stiffness',function(){Lab.stiffness=(Lab.stiffness+1)%3;Lab.dirty=true;});
    click('mode',function(){Lab.mode=Lab.mode==='hybrid'?'jacobi':'hybrid';Lab.reset();});
    click('camera',function(){Lab.camera=Lab.test.camera();Engine.view.set({camera:Lab.camera});});
    Lab.ui.tabs.setInnerRML(Lab.order.map(function(id){
        return '<button id="tab-'+id+'">'+Lab.experiments[id].tab+'</button>';}).join(''));
    Lab.tabs=Lab.order.map(function(id){
        var button=Lab.document.getElementById('tab-'+id);
        button.on('click',function(){Lab.select(id);Lab.notice(Lab.experiments[id].title+' · '+Lab.experiments[id].subtitle);});
        return {button:button,id:id};});
    // UI owns keyboard focus after a button click; handle shortcuts in its event stream.
    Lab.document.on('keydown',function(ev){
        var k=ev.parameters.key_identifier;
        if(Lab.held[k]||!Lab.test)return;Lab.held[k]=true;
        if(k===1)Lab.paused=!Lab.paused;
        else if(k===29)Lab.reset();
        else if(k===20)Lab.notice(Lab.test.push());
        else if(k===25&&Lab.paused)Lab.oneStep=true;
        else if(Lab.test.keys[k]!==undefined)Lab.run(Lab.test.keys[k]);
        else return;
        ev.stopPropagation();
    },true);
    Lab.document.on('keyup',function(ev){delete Lab.held[ev.parameters.key_identifier];},true);
    Lab.document.on('blur',function(){Lab.held={};},true);
    Lab.reset();Lab.notice('顶部切换实验 · Space 暂停 · I 施加冲量 · C / W 为实验自己的动作 · 中键旋转视角');
}
function fixedUpdate(dt,input){Lab.input(input);Lab.pump();Lab.draw(dt);}
function updateUI(dt){
    if(!Lab.document)return;
    var bounds=Lab.ui.viewport.getBounds(),key=[bounds.x,bounds.y,bounds.width,bounds.height].join(',');
    if(bounds.width>16&&bounds.height>16&&key!==Lab.layout){Lab.layout=key;
        Engine.view.set({rectangle:{x:Math.round(bounds.x),y:Math.round(bounds.y),width:Math.round(bounds.width),height:Math.round(bounds.height)},camera:Lab.camera});}
    Lab.uiClock+=dt;if(Lab.uiClock<0.1&&dt!==0)return;Lab.uiClock=0;
    if(!Lab.test)return;
    if(Lab.rendered!==Lab.test.id)Lab.renderPanel();
    Lab.text('status',Lab.error?'ERROR':Lab.phase==='compile'?'COMPILING':Lab.paused?'PAUSED':'GPU LIVE');
    Lab.ui.status.setClass('warning',!!Lab.error);
    Lab.text('population',Lab.grouped(Lab.total));Lab.text('relation-count',Lab.grouped(Lab.relations));
    Lab.text('tick',String(Lab.tick));Lab.text('time',Lab.time.toFixed(2)+' s');Lab.text('gpu',Lab.gpu.toFixed(2)+' ms');
    Lab.text('strain',(Lab.strain*100).toFixed(1)+'%');Lab.text('diagnostics',Lab.invalid+' / '+Lab.singular);
    Lab.text('stiffness-label',['柔软','适中','紧致'][Lab.stiffness]);
    Lab.text('pause',Lab.paused?'继续 / Space':'暂停 / Space');
    Lab.text('mode',Lab.mode==='hybrid'?'Hybrid':'Jacobi');
    Lab.text('sample-note',Lab.test.note());
    Lab.ui.meter.setProperty('width',Math.min(100,Lab.strain*500)+'%');
    Lab.text('footer-state',Lab.phase==='compile'?'正在编译…':Lab.paused?'已暂停，可单步观察':'4 子步 × 12 迭代 · 样本 '+Lab.reads);
    if(Lab.error)Lab.text('notice',Lab.error);
    Lab.tabs.forEach(function(tab){tab.button.setClass('active',tab.id===Lab.current);});
    Lab.tiles.forEach(function(tile){tile.button.setClass('active',tile.value===Lab.test.sizes.get());});
    Lab.switches.forEach(function(item){item.button.setText(item.action.text());item.button.setClass('active',item.action.active());});
    Lab.fields.forEach(function(field){field.value.setText(field.row.text());});
}
