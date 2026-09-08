// Run: Afterlight.exe --project Projects/EntityID --width 640 --height 480 --frames 90 --validation
var rt, request, nearBox, farBox, writer, phase = 0, previous;
function expect(ok, message) {
    if (!ok) throw new Error("EntityID example: " + message);
}
function box(name, z, size, material) {
    return Engine.create({name: name, components: {
        transform: {position: [0, 0, z]},
        render: {mesh: Engine.asset('/Engine/Meshes/Box'), scale: size, material: material}
    }});
}
function initialize() {
    var red = Engine.asset('/Game/Materials/Red');
    var blue = Engine.asset('/Game/Materials/Blue');
    Engine.camera(0, 0, 0, 0, 0, 8);
    // The farther box is drawn last: a correct center ID requires depth testing.
    nearBox = box("Near", 3, [1.5, 1.5, 1.5], red);
    farBox = box("Far", 0, [4, 4, 0.5], blue);
    var asset = Engine.asset("/Game/EntityIDs");
    rt = Engine.renderTarget(asset);
    writer = Engine.create({name: "EntityID output", components: {drawEntityID: {target: asset}}});
    expect(Engine.renderTargetInfo(rt).format === "R32Uint", "integer format");
    request = Engine.readPixels(rt); // A ticket, no GPU wait and no per-frame automatic readback.
}
function fixedUpdate() {
    if (!request) return;
    var result = Engine.pollPixels(request);
    if (result === null) return; // Results enter JS only on this simulation thread.
    expect(result.status === "ready", result.status);
    expect(result.data instanceof Uint32Array, "lossless typed pixels");
    var w = result.region.width, h = result.region.height;
    var center = result.data[Math.floor(h / 2) * w + Math.floor(w / 2)];
    if (phase === 0) {
        expect(center === nearBox, "near box must occlude far box");
        expect(result.data[0] === 0, "background must be zero");
        expect(Array.prototype.indexOf.call(result.data, farBox) >= 0, "far box's visible border");
        previous = result;
        Engine.visible(nearBox, false);
        request = Engine.readPixels(rt);
        phase = 1;
    } else if (phase === 1) {
        expect(center === farBox, "hiding near box must reveal far box");
        expect(result.sourceTick !== previous.sourceTick, "new scene snapshot");
        previous = result;
        Engine.removeComponent(writer, "drawEntityID");
        request = Engine.readPixels(rt); // Generic RT read: retained contents need no live writer.
        phase = 2;
    } else if (phase === 2) {
        expect(center === farBox, "retained pixels");
        // A producer may have rendered more frames before its removal reached the renderer.
        previous = result;
        request = Engine.readPixels(rt);
        phase = 3;
    } else {
        expect(center === farBox, "retained pixels");
        expect(result.contentVersion === previous.contentVersion, "retained content version");
        expect(result.renderFrame === previous.renderFrame, "result identifies its producer frame");
        Engine.log("EntityID example PASS near=" + nearBox + " far=" + farBox +
            " background=0 renderFrame=" + result.renderFrame + " sourceTick=" + result.sourceTick +
            " rtVersion=" + result.rtVersion + " contentVersion=" + result.contentVersion);
        Engine.releaseRenderTarget(rt);
        request = null;
    }
}
