// One mathematical definition, 100,000 instances. No World or rendering dependency.
var X = Engine.dynamics;
var scalar = X.space(1);
var sum = X.defineRelation({
    name: 'sum', spaces: [scalar, scalar], parameters: 1
}, function (e) {
    return { residual: [e.sub(e.add(e.endpoints[0][0], e.endpoints[1][0]), e.parameter(0))] };
});
var model = X.model();
var variables = X.variables(model, scalar, { count: 100000, initial: [0] });
var indices = new Uint32Array(100000);
for (var i = 0; i < indices.length; ++i) indices[i] = i;
var relations = X.relations(model, sum, {
    endpoints: [{ set: variables, indices: indices }, { set: variables, indices: indices }],
    parameters: [4], compliance: [0]
});
X.compile(model, { substeps: 1, iterations: 1, mode: 'hybrid' });
var stage = 0;
function update() {
    var result = X.poll(model);
    if (!result) return true;
    if (result.error) throw new Error(result.error);
    if (stage === 0) {
        X.step(model, 1, 1 / 60);
    } else if (stage === 1) {
        Engine.log('Completed tick ' + result.tick + ', invalid=' + result.invalidEvaluations + ', singular=' + result.singularSystems);
        X.read(model, 'value', variables, 99996, 4);
    } else {
        Engine.log('Last four scalar values: ' + Array.prototype.join.call(result.values, ', '));
        X.destroy(model);
        return false;
    }
    ++stage;
    return true;
}
