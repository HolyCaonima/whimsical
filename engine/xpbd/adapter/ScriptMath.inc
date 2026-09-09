R"JS(
(function (X) {
    X.expression = function (inputs) {
        var nodes = [], cache = {}, e = {};
        function emit(row) {
            var key = JSON.stringify(row);
            if (cache[key] !== undefined) return { node: cache[key] };
            var index = nodes.length; nodes.push(row); cache[key] = index;
            return { node: index };
        }
        function node(value) { return typeof value === 'number' ? emit(['constant', value]).node : value.node; }
        e.input = function (i) { return emit(['input', i]); };
        e.constant = function (v) { return emit(['constant', v]); };
        ['add','sub','mul','div','min','max','atan2','less'].forEach(function (op) {
            e[op] = function (a,b) { return emit([op,node(a),node(b)]); };
        });
        ['neg','sqrt','sin','cos','exp','log','abs'].forEach(function (op) {
            e[op] = function (a) { return emit([op,node(a)]); };
        });
        e.select = function (c,a,b) { return emit(['select',node(c),node(a),node(b)]); };
        e.vadd = function (a,b) { return a.map(function(v,i){return e.add(v,b[i]);}); };
        e.vsub = function (a,b) { return a.map(function(v,i){return e.sub(v,b[i]);}); };
        e.scale = function (a,b) { return a.map(function(v){return e.mul(v,b);}); };
        e.dot = function (a,b) { var sum=0;for(var i=0;i<a.length;++i)sum=e.add(sum,e.mul(a[i],b[i]));return sum; };
        e.length = function (a) { return e.sqrt(e.dot(a,a)); };
        e.cross = function (a,b) { return [e.sub(e.mul(a[1],b[2]),e.mul(a[2],b[1])),e.sub(e.mul(a[2],b[0]),e.mul(a[0],b[2])),e.sub(e.mul(a[0],b[1]),e.mul(a[1],b[0]))]; };
        e.finish = function (outputs) {
            if (!Array.isArray(outputs)) outputs=[outputs];
            var roots=outputs.map(node), used={}, remap={}, compact=[];
            function visit(i) { if(used[i])return;used[i]=true;var row=nodes[i];if(row[0]!=='input'&&row[0]!=='constant')for(var k=1;k<row.length;++k)visit(row[k]); }
            roots.forEach(visit);
            nodes.forEach(function(row,i){if(!used[i])return;var out=row.slice();if(row[0]!=='input'&&row[0]!=='constant')for(var k=1;k<out.length;++k)out[k]=remap[out[k]];remap[i]=compact.length;compact.push(out);});
            return {inputs:inputs,nodes:compact,outputs:roots.map(function(i){return remap[i];})};
        };
        return e;
    };
    X.defineRelation = function (definition, build) {
        var sizes=definition.spaces.map(function(s){return X.spaceInfo(s).stateSize;}), total=0;
        sizes.forEach(function(n){total+=n;});
        var parameters=definition.parameters||0, history=definition.history||0, rows=definition.rows||1;
        var e=X.expression(total+parameters+history+rows+2), offset=0;
        e.endpoints=sizes.map(function(n){var v=[];for(var i=0;i<n;++i)v.push(e.input(offset++));return v;});
        e.parameter=function(i){return e.input(total+i);};
        e.state=function(i){return e.input(total+parameters+i);};
        e.multiplier=function(i){return e.input(total+parameters+history+i);};
        e.dt=e.input(total+parameters+history+rows);e.time=e.input(total+parameters+history+rows+1);
        var result=build(e), type={};Object.keys(definition).forEach(function(k){type[k]=definition[k];});
        type.residual=e.finish(result.residual);
        if(result.projection)type.projection=e.finish(result.projection);
        if(result.update)type.update=e.finish(result.update);
        return X.relation(type);
    };
})(Engine.xpbd);
)JS"
