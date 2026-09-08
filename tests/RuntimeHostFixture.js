// Framework exercise only. Both Content directories are generated under build/ by
// RuntimeHostTests; no application/editor project or example Content is modified.
var hostTicks = 0, hostChanges = 0, uiReads = 0;
var document, writer, rt, ticket = null, phase = 0;
function expect(value, reason) { if (!value) throw Error(reason); }
function initialize() {
    var settings = Engine.readJson('/Game/settings');
    expect(settings.owner === 'host', 'host initialization origin');
    Engine.content.mount('/Target', settings.target);
    document = Engine.ui.createDocument(
        '<rml><head><style>body{width:100%;height:100%;pointer-events:none;font-family:LatoLatin;}' +
        '#bar{position:absolute;left:0;top:0;width:100%;height:36px;background-color:#252525;color:#eeeeee;}' +
        'button{pointer-events:auto;width:140px;height:30px;}</style></head>' +
        '<body id="host-ui"><div id="bar"><button id="origin">Host Content</button> Scene / View contract</div></body></rml>',
        '/Game/UI/host.rml').show();
    document.getElementById('origin').on('click', function () {
        expect(Engine.readJson('/Game/settings').owner === 'host', 'host UI callback origin');
        uiReads++;
    });
    Engine.view.set({rectangle:{x:160,y:80,width:320,height:240},
        camera:{target:[0,0,0],yaw:0,pitch:0,distance:8,fov:0.62}});
    Engine.scene.load('/Target/Maps/Test');
}
function sceneChanged() {
    hostChanges++;
    expect(Engine.readJson('/Game/settings').owner === 'host', 'host keeps Content after scene replacement');
    if (ticket) Engine.cancelPixels(ticket);
    ticket = null;
    var asset = Engine.asset('/Game/Picking');
    rt = Engine.renderTarget(asset);
    writer = Engine.create({name:'Transient output',persistent:false,components:{drawEntityID:{target:asset}}});
}
function fixedUpdate(dt, input) {
    hostTicks++;
    if (!rt || phase === 2) return;
    if (!ticket) {
        var r = Engine.view.get().rectangle;
        var pixel = Engine.view.pixel(r.x+r.width/2, r.y+r.height/2, input.width, input.height);
        if (pixel) ticket = Engine.readPixels(rt, {x:pixel.x,y:pixel.y,width:1,height:1});
        return;
    }
    var result = Engine.pollPixels(ticket);
    if (result === null) return;
    ticket = null;
    expect(result.status === 'ready', 'GPU result: ' + result.status);
    expect(result.data[0] === Engine.findEntity('11111111111111111111111111111111'), 'ID image selects the nearer entity');
    if (phase === 0) {
        expect(result.width === 320 && result.height === 240, 'view-sized ID image');
        Engine.view.set({rectangle:{x:120,y:60,width:400,height:300}});
        phase = 1;
    } else {
        expect(result.width === 400 && result.height === 300, 'view resize reaches ID output');
        Engine.log('RuntimeHost GPU PASS: cross-Content scene, persistent UI, offset view, ID picking and view resize');
        document.getElementById('bar').setText('PASS - host UI / target scene / EntityID / resized view');
        phase = 2;
    }
}
