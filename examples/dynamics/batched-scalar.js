// One broadcast object and two paired collections define 100,000 relation instances.
// No endpoint index arrays, World or rendering dependency.
var X = Engine.dynamics;
var scalar = X.space(1);
var sum = X.defineRelation({
    name: 'sum', spaces: [scalar, scalar, scalar], parameters: 1
}, function (e) {
    return { residual: [e.sub(e.add(e.add(e.endpoints[0][0], e.endpoints[1][0]), e.endpoints[2][0]), e.parameter(0))] };
});
var model = X.model();
var anchor = X.variables(model, scalar, { count: 1, initial: [0], readOnly: true });
var variables = X.variables(model, scalar, { count: 100000, initial: [0] });
var offsets = X.variables(model, scalar, { count: 100000, initial: [2], readOnly: true });
var relations = X.relations(model, sum, {
    endpoints: [X.object(anchor, 0), X.collection(variables), X.collection(offsets)],
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
