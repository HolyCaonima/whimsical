// The host supplies roots.a and roots.b: plain Content directories, with no .project.
function check(ok, message) { if (!ok) throw Error(message); }
function rejects(action) {
    var rejected = false;
    try { action(); } catch (error) { rejected = true; }
    check(rejected, 'expected a Content boundary error');
}
var content = Engine.content;
content.mount('/A', roots.a);
content.mount('/B', roots.b);
var a = content.load('/A/value');
var b = content.load('/B/value');
check(a.ref.id === b.ref.id && a.ref.source !== b.ref.source, 'independent ID namespaces');
check(a.payload.value === 1 && b.payload.value === 2, 'same path reads its own Content');
check(content.browse('/A').length === content.browse('/B').length, 'browse both sources');
var created = content.save('/A/new', {
    id: content.newId(), type:'Data', name:'New data', version:1,
    storage:'external', source:'raw/new.json', metadata:{}
}, {value:3});
check(content.load(created).payload.value === 3, 'create an external JSON asset');
var link = content.load('/B/link');
check(link.payload.target.source === b.ref.source, 'internal /Game resolves in B');
b.payload.value = 20;
content.save(b.ref, b.header, b.payload);
check(content.load('/A/value').payload.value === 1, 'writes stay in B');
check(content.load('/B/value').payload.value === 20, 'write visible after cache invalidation');
rejects(function () { content.save(link.ref, link.header, {target: a.ref}); });
rejects(function () { content.save(link.ref, link.header, {target: {id:a.ref.id, path:'/A/value'}}); });
var blob = content.load('/B/blob');
content.save(blob.ref, blob.header, 'changed external bytes');
check(content.load('/B/script').payload.indexOf('executed') >= 0, 'Script loads as text');
content.load('/B/map');
check(typeof executed === 'undefined', 'content access never executes scripts or starts a project');
content.unmount('/B');
rejects(function () { content.load(b.ref); });
content.mount('/B', roots.b);
rejects(function () { content.load(b.ref); });
rejects(function () { content.save(b.ref, b.header, b.payload); });
check(content.load('/B/value').payload.value === 20, 'remount reads persisted data');
check(content.load('/B/link').payload.target.source !== link.payload.target.source, 'dependencies bind to new mount');
Engine.log('Content mounts example PASS');
