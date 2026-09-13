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
        e.sum = function (value,axis) {
            return Array.isArray(value)?value.map(function(v){return e.sum(v,axis);}):emit(['sum',node(value),axis]);
        };
        e.vadd = function (a,b) { return a.map(function(v,i){return e.add(v,b[i]);}); };
        e.vsub = function (a,b) { return a.map(function(v,i){return e.sub(v,b[i]);}); };
        e.scale = function (a,b) { return a.map(function(v){return e.mul(v,b);}); };
        e.dot = function (a,b) { var sum=0;for(var i=0;i<a.length;++i)sum=e.add(sum,e.mul(a[i],b[i]));return sum; };
        e.length = function (a) { return e.sqrt(e.dot(a,a)); };
        e.cross = function (a,b) { return [e.sub(e.mul(a[1],b[2]),e.mul(a[2],b[1])),e.sub(e.mul(a[2],b[0]),e.mul(a[0],b[2])),e.sub(e.mul(a[0],b[1]),e.mul(a[1],b[0]))]; };
        e.finish = function (outputs) {
            if (!Array.isArray(outputs)) outputs=[outputs];
            var roots=outputs.map(node), used={}, remap={}, compact=[];
            function visit(i) { if(used[i])return;used[i]=true;var row=nodes[i];if(row[0]!=='input'&&row[0]!=='constant')for(var k=1;k<(row[0]==='sum'?2:row.length);++k)visit(row[k]); }
            roots.forEach(visit);
            nodes.forEach(function(row,i){if(!used[i])return;var out=row.slice();if(row[0]!=='input'&&row[0]!=='constant')for(var k=1;k<(row[0]==='sum'?2:out.length);++k)out[k]=remap[out[k]];remap[i]=compact.length;compact.push(out);});
            return {inputs:inputs,nodes:compact,outputs:roots.map(function(i){return remap[i];})};
        };
        return e;
    };
    var nativeDofs=X._dofs, nativeObject=X._object, nativeMember=X._member,
        nativeRelation=X._relation, nativePairs=X._pairs;
    delete X._dofs;delete X._object;delete X._member;delete X._relation;delete X._pairs;
    X.defineDofs = function (model, definition) {
        return Object.freeze({model:model,set:nativeDofs(model,definition.space,definition),
            space:definition.space,count:definition.count});
    };
    X.dof = function (dofs, index) {
        return Object.freeze({model:dofs.model,set:dofs.set,index:index});
    };
    X.defineObject = function (model, definition) {
        var kind=definition.kind, fields=[], count=kind==='single'?1:definition.count;
        if(kind!=='single'&&kind!=='collection')throw new Error('Object kind must be single or collection');
        Object.keys(definition.dofs).forEach(function(name){
            var ref=definition.dofs[name], source;
            if(ref.model!==model)throw new Error('Object DOFs belong to another model');
            if(kind==='single') {
                if(ref.index===undefined&&ref.count!==1)throw new Error('Select one DOF for a single object');
                source={kind:'object',set:ref.set,index:ref.index===undefined?0:ref.index};
            } else {
                if(ref.index!==undefined)throw new Error('Collection DOFs require a DOF set');
                if(count===undefined)count=ref.count;
                if(count!==ref.count)throw new Error('Collection DOF fields have different counts');
                source={kind:'collection',set:ref.set,first:0,count:count,stride:1};
            }
            fields.push({name:name,source:source});
        });
        if(count===undefined)throw new Error('A collection with no DOF fields requires count');
        var id=nativeObject(model,{name:definition.name||'object',kind:kind,count:count,dofs:fields});
        return Object.freeze({model:model,id:id,kind:kind,count:count});
    };
    X.defineMember = function (collection, index) {
        return Object.freeze({model:collection.model,id:nativeMember(collection.model,collection.id,index),kind:'single',count:1});
    };
    X.defineRelation = function (definition, build) {
        if(!Array.isArray(definition.objects)||definition.objects.length!==2)
            throw new Error('A relation declares exactly two formal object schemas');
        var spaces=[], names=definition.objects.map(function(schema){
            return Object.keys(schema).map(function(name){spaces.push(schema[name]);return name;});
        });
        var sizes=spaces.map(function(s){return X.spaceInfo(s).stateSize;}), total=0;
        sizes.forEach(function(n){total+=n;});
        var parameterSchema=definition.parameters||0, parameterNames=typeof parameterSchema==='number'?null:Object.keys(parameterSchema);
        var widths=parameterNames?parameterNames.map(function(name){return parameterSchema[name]==='scalar'?1:parameterSchema[name];}):[];
        var parameters=parameterNames?widths.reduce(function(a,b){return a+b;},0):parameterSchema, history=definition.history||0;
        var e=X.expression(total+parameters+history+2), offset=0, slot=0;
        var objects=names.map(function(fields){var object={};fields.forEach(function(name){
            var v=[],n=sizes[slot++];for(var i=0;i<n;++i)v.push(e.input(offset++));object[name]=v;
        });return object;});
        var sum=e.sum;
        e.sum=function(value,object){
            var axis=objects.indexOf(object);
            if(axis<0)throw new Error('Sum requires a formal object from this relation');
            return Array.isArray(value)?value.map(function(v){return sum(v,axis);}):sum(value,axis);
        };
        e.parameter=function(i){if(i<0||i>=parameters)throw new Error('Parameter index out of range');return e.input(total+i);};
        e.state=function(i){if(i<0||i>=history)throw new Error('History index out of range');return e.input(total+parameters+i);};
        e.dt=e.input(total+parameters+history);e.time=e.input(total+parameters+history+1);
        var p={},parameterOffset=0;
        if(parameterNames)parameterNames.forEach(function(name,k){var v=[];for(var i=0;i<widths[k];++i)v.push(e.parameter(parameterOffset++));p[name]=widths[k]===1?v[0]:v;});
        var result=build(e,objects[0],objects[1],p), type={};
        Object.keys(definition).forEach(function(k){type[k]=definition[k];});
        type.spaces=spaces;type.objects=names;type.parameters=parameters;
        type.residual=e.finish(result.residual);
        if(result.projection!==undefined)throw new Error('Relations cannot define solver projections');
        if(result.update)type.update=e.finish(result.update);
        return Object.freeze({id:nativeRelation(type),parameterNames:parameterNames,widths:widths});
    };
    function bind(relation, bindings, parameters, options) {
        if(!bindings.length)throw new Error('At least one explicit pair is required');
        var model=bindings[0][0].model, def={};
        Object.keys(options||{}).forEach(function(k){def[k]=options[k];});
        def.pairs=bindings.map(function(pair){
            if(pair.length!==2||pair[0].model!==model||pair[1].model!==model)
                throw new Error('A pair binds two objects from the same model');
            var a=pair[0],b=pair[1],binding={a:a.id,b:b.id};
            if(a.id===b.id&&a.kind==='collection') {
                if(def.self===undefined||typeof def.includeSelf!=='boolean')
                    throw new Error('Self collection pairs require self: directed/undirected and includeSelf: true/false');
                binding.self=def.self;binding.includeSelf=def.includeSelf;
            } else if(def.self!==undefined||def.includeSelf!==undefined)
                throw new Error('Self rules apply only to the same collection object');
            return binding;
        });
        if(relation.parameterNames) {
            def.parameters=[];
            relation.parameterNames.forEach(function(name,k){
                var value=parameters[name];
                if(relation.widths[k]===1)def.parameters.push(value);
                else for(var i=0;i<relation.widths[k];++i)def.parameters.push(value[i]);
            });
        } else def.parameters=parameters;
        return nativePairs(model,relation.id,def);
    }
    X.pair = function (relation, a, b, parameters, options) {
        return bind(relation,[[a,b]],parameters,options);
    };
    X.pairs = function (relation, bindings, parameters, options) {
        return bind(relation,bindings,parameters,options);
    };
})(Engine.dynamics);
)JS"
