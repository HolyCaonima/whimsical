Lab.uiClock=0;
Lab.text=function(id,value){Lab.ui[id].setText(value);};
Lab.notice=function(message){Lab.text('notice',message);Engine.log('ROPE_LAB: '+message);};
function initialize(){
    Lab.definitions();Lab.scene();Lab.document=Engine.ui.loadDocument('/Game/UI/lab.rml').show();Lab.ui={};
    ['viewport','status','population','relation-count','tick','time','gpu','strain','diagnostics','amplitude','stiffness-label','pause','drive','cut','mode','notice','sample-note','equation','meter','footer-state'].forEach(function(id){Lab.ui[id]=Lab.document.getElementById(id);});
    function click(id,fn){Lab.document.getElementById(id).on('click',fn);}
    click('pause',function(){Lab.paused=!Lab.paused;});click('step',function(){Lab.paused=true;Lab.oneStep=true;});
    click('reset',function(){Lab.reset();Lab.notice('恢复两端悬挂的绳子');});
    click('kick',function(){Lab.kick=true;Lab.notice('给绳子一个侧向冲量');});
    click('drive',function(){Lab.drive=!Lab.drive;Lab.notice(Lab.drive?'右端点沿前后方向摆动':'端点停止主动摆动');});
    click('cut',function(){Lab.release();Lab.notice(Lab.cut?'松开右端，观察重力与碰撞':'固定右端到控制位置');});
    click('amplitude-down',function(){Lab.move(0,-0.3);});click('amplitude-up',function(){Lab.move(0,0.3);});
    click('stiffness',function(){Lab.stiffness=(Lab.stiffness+1)%3;Lab.dirty=true;});
    click('mode',function(){Lab.mode=Lab.mode==='hybrid'?'jacobi':'hybrid';Lab.reset();});
    [1,16,64].forEach(function(copies){click('size-'+copies,function(){Lab.copies=copies;Lab.reset();Lab.notice('求解并显示 '+copies+' 条绳子');});});
    click('camera',function(){Lab.camera=Lab.homeCamera();Engine.view.set({camera:Lab.camera});});
    // UI owns keyboard focus after a button click; handle shortcuts in its event stream.
    Lab.held={};
    Lab.document.on('keydown',function(ev){
        var k=ev.parameters.key_identifier;
        if(Lab.held[k])return;Lab.held[k]=true;
        if(k===1)Lab.paused=!Lab.paused;else if(k===29)Lab.reset();
        else if(k===20){Lab.kick=true;Lab.notice('给绳子一个侧向冲量');}
        else if(k===14){Lab.release();Lab.notice(Lab.cut?'松开右端，观察重力与碰撞':'固定右端到控制位置');}
        else if(k===34)Lab.drive=!Lab.drive;else if(k===25&&Lab.paused)Lab.oneStep=true;
        else if(k===90)Lab.move(-0.3,0);else if(k===91)Lab.move(0,0.3);
        else if(k===92)Lab.move(0.3,0);else if(k===93)Lab.move(0,-0.3);
        else return;
        ev.stopPropagation();
    },true);
    Lab.document.on('keyup',function(ev){delete Lab.held[ev.parameters.key_identifier];},true);
    Lab.document.on('blur',function(){Lab.held={};},true);
    Lab.reset();Lab.notice('I 推绳子 · C 松开右端 · 方向键移动端点 · 中键旋转视角');
}
function fixedUpdate(dt,input){Lab.input(input);Lab.pump();Lab.draw(dt);}
function updateUI(dt){
    if(!Lab.document)return;var bounds=Lab.ui.viewport.getBounds(),key=[bounds.x,bounds.y,bounds.width,bounds.height].join(',');
    if(bounds.width>16&&bounds.height>16&&key!==Lab.layout){Lab.layout=key;Engine.view.set({rectangle:{x:Math.round(bounds.x),y:Math.round(bounds.y),width:Math.round(bounds.width),height:Math.round(bounds.height)},camera:Lab.camera});}
    Lab.uiClock+=dt;if(Lab.uiClock<0.1&&dt!==0)return;Lab.uiClock=0;
    Lab.text('status',Lab.error?'ERROR':Lab.stage==='compile'?'COMPILING':Lab.paused?'PAUSED':'GPU LIVE');Lab.ui.status.setClass('warning',!!Lab.error);
    Lab.text('population',String(Lab.total||64).replace(/\B(?=(\d{3})+(?!\d))/g,','));Lab.text('relation-count',String(Lab.relations||255).replace(/\B(?=(\d{3})+(?!\d))/g,','));
    Lab.text('tick',String(Lab.tick));Lab.text('time',Lab.time.toFixed(2)+' s');Lab.text('gpu',Lab.gpu.toFixed(2)+' ms');
    Lab.text('strain',((Lab.strain||0)*100).toFixed(1)+'%');Lab.text('diagnostics',Lab.invalid+' / '+Lab.singular);
    Lab.text('amplitude',Lab.right[1].toFixed(1)+' m');Lab.text('stiffness-label',['柔软','适中','紧致'][Lab.stiffness]);
    Lab.text('pause',Lab.paused?'继续 / Space':'暂停 / Space');Lab.text('drive',Lab.drive?'端点摆动 ON / W':'端点摆动 OFF / W');Lab.text('cut',Lab.cut?'接回右端 / C':'松开右端 / C');
    Lab.text('mode',Lab.mode==='hybrid'?'Hybrid /':'Jacobi /');Lab.text('sample-note',Lab.viewCopies+' 条绳子全部显示 · 每条 64 个求解节点');
    Lab.text('equation','C(q) = ‖qᵢ − qⱼ‖ − L');Lab.ui.meter.setProperty('width',Math.min(100,(Lab.strain||0)*500)+'%');
    Lab.text('footer-state',Lab.stage==='compile'?'正在编译…':Lab.paused?'已暂停，可单步观察':'4 子步 × 12 迭代 · 样本 '+Lab.reads);
    if(Lab.error)Lab.text('notice',Lab.error);
    [1,16,64].forEach(function(n){Lab.document.getElementById('size-'+n).setClass('active',Lab.copies===n);});
    Lab.ui.drive.setClass('active',Lab.drive);Lab.ui.cut.setClass('active',Lab.cut);
}

