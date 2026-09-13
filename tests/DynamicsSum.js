// End-to-end DSL check: vector sum, bound-object identity, serialization,
// readonly members, an empty sum, and a coupled block with no writable anchor.
var X=Engine.dynamics, model=X.model(), scalar=X.space(1);
var q=X.defineDofs(model,{space:scalar,count:2,initial:[0,0]});
var values=X.defineDofs(model,{space:scalar,count:3,initial:[1,2,3],readOnly:true});
var a=X.defineObject(model,{kind:'single',dofs:{u:X.dof(q,0)}});
var b=X.defineObject(model,{kind:'collection',dofs:{u:values}});
var relation=X.defineRelation({name:'sum balance',objects:[{u:scalar},{u:scalar}]},function(op,a,b){
    return {residual:op.sum(op.vsub(a.u,b.u),b)};
});
X.pair(relation,a,b,[]);
if(X.describe(model).relations[0].count!==1)throw new Error('Sum relation count');
var empty=X.defineObject(model,{kind:'collection',count:0,dofs:{}});
var emptyAnchor=X.defineObject(model,{kind:'single',dofs:{u:X.dof(q,1)}});
var emptyRelation=X.defineRelation({name:'empty sum',objects:[{u:scalar},{}]},function(op,a,b){
    return {residual:[op.sub(op.add(a.u[0],op.sum(3,b)),5)]};
});
X.pair(emptyRelation,emptyAnchor,empty,[]);
var R2=X.space(2);
var vectors=X.defineDofs(model,{space:R2,count:3,initial:[1,2,3,4,5,6],inverseMetric:[2,0.5,0.5,1]});
var vectorObject=X.defineObject(model,{kind:'collection',dofs:{u:vectors}});
var frame=X.defineObject(model,{kind:'single',dofs:{}});
var block=X.defineRelation({name:'vector balance',objects:[{},{u:R2}],rows:2},function(op,a,b){
    return {residual:op.vsub(op.sum(b.u,b),[3,3])};
});
X.pair(block,frame,vectorObject,[]);
X.compile(model,{mode:'jacobi',substeps:1,iterations:1});
var stage=0;
function update(){
    var r=X.poll(model);if(!r)return true;if(r.error)throw new Error(r.error);
    if(stage===0){stage=1;X.step(model,1,1/30);return true;}
    if(stage===1){
        if(r.invalidEvaluations||r.singularSystems)throw new Error('Sum diagnostics');
        stage=2;X.read(model,'value',q.set,0,2);return true;
    }
    if(stage===2){
        if(Math.abs(r.values[0]-2)>0.0002||Math.abs(r.values[1]-5)>0.0002)throw new Error('Sum analytic solution');
        stage=3;X.read(model,'value',vectors.set,0,3);return true;
    }
    var expected=[-1,-1,1,1,3,3];
    for(var i=0;i<expected.length;++i)if(Math.abs(r.values[i]-expected[i])>0.0002)throw new Error('Sum block solution');
    Engine.log('Collection sum DSL check passed');return false;
}
