# GPU Dynamics

场景组件、输入／输出绑定与十万级模型的组织边界见 [Dynamics 与 ECS](dynamics-ecs.md)。

Dynamics 当前采用 XPBD 求解算法。用户按四层定义模型：自由度、对象、两个形式对象之间的数学关系、绑定关系与具体对象的 pair。关系的残差及自身持久状态更新由用户编写；表达式不能读取求解器乘子、访问 GPU 资源或产生全局副作用。乘子的计算、投影与生命周期完全属于求解器。

## 模块与所有权

源码归属 `engine/physics/dynamics`，保留独立构建模块与 `Engine.dynamics` 接口。Dynamics 不依赖 `PhysicsScene`，可以脱离场景运行。

| 模块 | 拥有的内容 | 依赖 |
| --- | --- | --- |
| `physics/dynamics/model` | 数学表达式、空间、集合、稳定索引、初始数据、不可变快照与变更 | C++ 标准库 |
| `physics/dynamics/compiler` | GLSL 与 Jacobian 生成、SoA 布局、求解阶段、区域分析与融合、Jacobi 关联表、迁移映射 | Model |
| `physics/dynamics/runtime` | GPU 当前状态、执行上下文、提交/完成、状态迁移、范围回读与结果发布 | Compiler、RenderCore |
| `physics/dynamics/adapter` | 脚本定义入口、模型句柄、可靠请求通道、GPU 宿主服务 | 按前端和 GPU 服务分开链接 |

`whimsical_core` 只链接前端 `whimsical_dynamics_adapter`，不因此依赖 GPU Runtime。`Whimsical` 为 `GpuService` 和 Renderer 分别提供执行线程，二者共用应用拥有的 RenderCore。GpuService 通过请求唤醒与自身 GPU fence 推进，不依赖显示刷新。Runtime 本身不创建线程，不依赖 World、Rendering 或脚本。

四个契约是 `ModelSnapshot`、`ChangeSet`、`CompiledPlan` 和 `CompletedState`。同一个不可变计划可以实例化多份独立状态。数值参数存在 Model，当前值、速度、加速度、乘子及用户持久状态存在 Runtime；修改初始值不会悄悄重置当前求解结果。

## 数学定义

`Space` 声明存储维数、切空间维数以及两个纯函数：

```text
retract(q, delta) -> q'
difference(qNew, qOld) -> delta
```

内置 `Space::euclidean(n)` 和四元数 `[w,x,y,z]` 表示的 `Space::rotation()`。后者使用归一化的一阶局部旋转回缩和最短旋转差；它不包含物体、惯性或碰撞语义。自定义空间采用同一个表达式接口。

DSL 中每个关系定义恰好接收两个形式对象。每个对象可以暴露多个具名自由度，因此两个对象仍可表达原来的多端点数学；关系也可以包含多个残差行、参数和用户持久状态。Compiler 将具名自由度依次降低为求解器端点，内部公式的输入顺序统一为：

```text
[各端点存储坐标, 参数, 持久状态, 子步 dt, 子步结束时间]
```

`kind` 描述每行残差 C 的数学条件：`equality`（默认）表示 C = 0，`greaterEqual` 表示 C ≥ 0，`lessEqual` 表示 C ≤ 0。C++ 对应 `RelationKind::Equality`、`GreaterEqual` 和 `LessEqual`。柔顺度仍通过 `compliance` 指定；非零值允许关系发生柔性偏离。

`update` 在每个子步结束时更新一次关系自身的持久状态，与残差使用相同的数学输入，不接收求解器状态。C++ 使用 `finish(residual, kind, update)`。旧的 `domain`、`projection` 和 `multiplier()` 接口已移除；旧脚本的 domain / projection 声明会报错，避免静默改变关系含义。求解器根据关系条件生成内部乘子限制，用户不参与该过程。

共享表达式 DAG 支持常量、输入、四则运算、平方根、三角函数、指数/对数、绝对值、min/max、atan2、比较与选择。向量运算由这些纯运算组成，定义阶段进行公共子表达式复用和无用节点裁剪。反向自动微分生成残差 Jacobian，再通过 `retract` 的 Jacobian 转为切空间梯度。分段操作采用当前分支的导数；未选中的分支不会传播梯度。

每个变量具有完整的对称半正定逆度量矩阵。局部 XPBD 系统使用：

```text
(J W Jᵀ + diag(compliance / h²)) Δlambda
    = -C - diag(compliance / h²) lambda
```

一个关系多次引用同一变量时，先合并该变量的梯度，再形成局部矩阵与修正，保留交叉项。奇异局部系统不会被隐式添加柔顺度：该次修正被跳过，并计入诊断。非有限的残差、修正、空间运算和持久状态输出也会计入诊断。

## C++ 底层构造接口

C++ 保留端点级构造接口供底层调用方使用。DSL 的对象和 pair 存在同一个 Model 中，Compiler 先将连接转换为与求解器无关的 BindingIR，保留集合积、组合和字段映射。着色按需遍历逻辑实例，执行计划确定后才选择 GPU 端点布局；可用仿射索引表达的大集合不物化完整端点表。

```cpp
using namespace whimsical::dynamics;
auto scalar = Space::euclidean(1);
RelationBuilder definition("sum", {scalar, scalar}, 1);
auto sum = definition.finish({definition.endpoint(0)[0]
                           + definition.endpoint(1)[0]
                           - definition.parameter(0)});

Model model;
VariableSet values;
values.name = "values";
values.space = scalar;
values.count = 100000;
values.initial = Field::uniform(values.count, {0});
auto variables = model.variables(values);

auto indices = std::make_shared<std::vector<VariableRef>>(values.count);
for (uint32_t i = 0; i < values.count; ++i)
    (*indices)[i] = {variables, i};
RelationSet relations;
relations.name = "sums";
relations.type = sum;
relations.count = values.count;
relations.endpoints = {indices, indices};
relations.parameters = Field::uniform(values.count, {4});
model.relations(relations);

auto commit = model.commit();
SolverPolicy policy;
policy.substeps = policy.iterations = 1;
auto plan = Compiler().compile(commit.snapshot, policy);
Instance instance(renderCore, plan); // GPU 所属线程
instance.step({1, commit.snapshot.version});
const auto& completed = instance.wait(); // 也可用 poll()
auto tail = instance.read(StateField::Value, variables, 99996, 4);
// tail == {2, 2, 2, 2}
```

字段数据对外采用逐行排列；GPU 内部采用按集合、按分量排列的 SoA。`Field::uniform` 保存统一值，`Field::dense` 和范围修改使用共享分页存储。快照不会复制全部数值数组。

## 规模与调度

实例索引为 32 位；集合内的 `(set, index)` 在追加和计划切换后保持稳定，不复用旧索引。模型实例只有固定的 25 个求解 buffer 角色及少量辅助资源；十万个变量不会变成十万个 ResourceId、表达式或 pass。

`SolverPolicy` 提供：

- `Colored`：同一颜色内没有共享可写端点；颜色不足时编译报错。
- `Jacobi`：关系计算局部贡献，随后按变量的关联表归并。
- `Hybrid`：在颜色预算内着色，超出预算的关系进入 Jacobi；默认预算 32，上限 64。

只读端点不产生写冲突。`readOnly` 是结构承诺，仍可通过速度、加速度或状态命令驱动；逆度量为零不会被编译器自动视为只读。批次按颜色和数学类型形成，不为每个独立连通分量创建一个 pass。

Gather 只覆盖有 Jacobi 贡献的可写变量；动态拓扑按可写变量保留候选范围。Auto 将不同空间中互不依赖的预测、归并、速度恢复批次分别合并，避免仅因增加一个只读空间就增加每轮 dispatch。颜色窗口内部使用工作组范围的内存屏障，其写依赖已由组件划分限制在同一工作组。

执行方式与数值方法分开选择：`SolverPolicy::execution` 默认 `ExecutionMode::Auto`，也可指定 `ExecutionMode::Global` 保留逐阶段 dispatch。JavaScript 对应 `compile(handle, {execution:'auto'})` / `{execution:'global'}`。

Auto 根据静态可写端点连接识别组件，按变量数、关系数、状态容量和函数复杂度选择工作组融合，并把多个小组件打包。区域内的值、前值、速度、history 和乘子放入共享内存，保持颜色顺序和 Jacobi 归并。独占的只读输入归入消费组件，可融合整个 tick；跨组件共享的只读输入由全局预测每子步推进一次，消费它的局部区域在预测后融合该子步的迭代。大型组件保留全局路径；含动态端点的模型目前整体使用全局路径。

`CompiledPlan::statistics` 报告组件数、局部区域数、局部变量 / 关系数、每工作组共享字节数，以及优化前后的每步数值计算 dispatch 数。后者不含动态拓扑构建和上传 / 回读。另有 BindingIR 域数、隐式端点引用数、实际端点存储 word 数、被裁剪的切向导数列数和 CPU 计划编译耗时（不含驱动编译 shader）；脚本可用 `Engine.dynamics.scene.plan(entity)` 查看摘要。具体预算、实现边界和实测结果见 [编译器优化说明](dynamics-compiler-optimization.md)。

关联结构按端点数增长，不创建关系之间的两两冲突图。Jacobi 用端点最大关联度缩放乘子增量与对应修正，避免共享端点的贡献无控制累加；高关联度可能需要更多迭代。它和 Colored 的迭代路径不保证逐位一致。

## 变化与生命周期

数值参数更新：

```cpp
model.patch(FieldKind::Parameters, relationSet, first, rowMajorValues);
instance.apply(model.commit());
```

可修改逆度量、变量启用状态、关系参数、柔顺度及关系启用状态。Runtime 合并重叠范围，只上传变更列，不重建程序或布局。`TickInput::writes` 则修改当前值、速度、加速度和用户持久状态。加速度会保留到下次显式修改；求解乘子在每个子步清零。

追加变量、追加关系或修改静态端点后，提交快照、重新编译，再调用 `install`。Compiler 生成状态迁移映射；Runtime 在完成边界执行 GPU 到 GPU 迁移，保留存活变量的值、速度、加速度，以及端点不变的静态关系持久状态。新增行使用初始值。完整 GLSL 的编译缓存归 RenderCore 设备所有，跨上下文复用。

停用行不会改变索引或释放容量。当前没有物理删除、索引复用或自动压缩；整个模型通过销毁实例释放。需要频繁变化的关系可预留容量，调整启用字段或使用动态端点。

### 动态端点

`RelationSet::dynamicEndpoints = true` 表示固定容量、运行时连接。初始端点仍须有效。该集合采用 Jacobi；`TickInput::endpoints` 可以逐行替换其端点，无需重新编译计划：

```cpp
TickInput input;
input.tick = 2;
input.modelVersion = commit.snapshot.version;
input.endpoints.push_back({relationSet, firstRow, endpointColumns});
instance.step(input);
```

存在动态集合时，计划包含 GPU 计数、分层并行前缀扫描和关联项散布，按当前端点重建全部 Jacobi 关联表。流程每 tick 执行一次，不回读端点或在 CPU 重新着色。GPU 散布顺序可能影响浮点归并的低位结果。

当前端点数据由宿主批量提供；GPU 生成新关系的输入接口与自动扩容尚未提供。提交动态端点的行会重置其持久状态；安装新计划时动态关系使用初始端点和初始持久状态。容量变化仍属于模型拓扑变更。

### 结果消费

`CompletedState` 携带模型身份、版本、tick、时间和诊断。`read` 只回读指定字段的指定行。

`publish()` 显式复制值、速度与持久状态到不可变 GPU 快照。消费者在同一 RenderCore 的自身上下文线程上声明只读 Imported buffer，再使用 `PublishedState::import` 绑定。快照的 `plan` 提供 SoA 偏移和步幅。消费者保留资源所有权到解除或替换导入；即使调用方释放快照，正在使用它的上下文仍保持数据有效。发布有 GPU 复制和显存成本，不会自动在每 tick 产生一个副本。RenderCore 必须晚于所有实例、快照和消费者销毁。

## JavaScript：四层声明语言

`Engine.dynamics` 的建模入口固定为 **自由度 → 对象 → 关系定义 → pair 绑定**。数学空间不是物体类型，批量自由度存储也不会自动成为集合对象。对象声明、成员声明、关系的两个具名字段接口和高层 pair 均保存在 ModelSnapshot / 场景文档中，只有 Compiler 枚举成员组合并生成显式求解端点。

```js
var X = Engine.dynamics, model = X.model(), R3 = X.space(3);

// 1. 自由度：状态及其数学空间。
var q = X.defineDofs(model, {
    name: 'positions', space: R3, count: 100, initial: initialPositions
});

// 2. 对象：显式选择单体或集合。成员引用现有状态，不复制自由度。
var particles = X.defineObject(model, {
    name: 'particles', kind: 'collection', dofs: {position: q}
});
var p0 = X.defineMember(particles, 0);
var p1 = X.defineMember(particles, 1);

// 3. 关系：两个形式对象之间的任意 op 数学。
var separation = X.defineRelation({
    name: 'minimum separation',
    objects: [{position: R3}, {position: R3}],
    parameters: {diameter: 'scalar'}, kind: 'greaterEqual'
}, function(op, a, b, p) {
    var delta = op.vsub(a.position, b.position);
    return {residual: [op.sub(op.dot(delta, delta), op.mul(p.diameter, p.diameter))]};
});

// 4. pair：引用已定义的关系和两个对象。
var contact = X.pair(separation, particles, particles, {diameter: 0.36}, {
    self: 'undirected', includeSelf: false
});
// 若只需要明确连接，可改为：
// var contact = X.pair(separation, p0, p1, {diameter: 0.36});
```

`objects` 中的两个 schema 只声明形式对象需要暴露的字段及空间；不引用真实对象，不选择集合成员。`op` 包含标量 / 向量运算、`parameter(i)`、`state(i)`、`dt`、`time`。`build(op,a,b,p)` 返回 `residual`，以及可选的 `update`；`rows`、`history`、`kind` 与此前的数学含义一致。没有引擎内置的 Distance、Floor、Water 等关系类型。

`parameters` 可以是分量数，此时绑定提供平铺数组、数学使用 `op.parameter(i)`；也可以是具名 schema，如 `{diameter:'scalar', centre:3}`，此时绑定提供 `{diameter:0.36, centre:[0,1,0]}`，数学通过 `p.diameter`、`p.centre` 引用。具名参数当前用于统一值；逐行参数使用分量数及平铺数组。

### 对象及绑定语义

| 两侧对象 | 展开结果与行顺序 |
| --- | --- |
| 单体、单体 | 一个关系实例 |
| 单体、集合 | 按右侧成员顺序应用 |
| 集合、单体 | 按左侧成员顺序应用 |
| 不同集合 | 完整笛卡尔积，左侧成员为外层、右侧为内层 |
| 同一集合，`directed` | 上述顺序；`includeSelf:false` 排除 `i == j` |
| 同一集合，`undirected` | 上述顺序，仅保留 `i < j`；`includeSelf:true` 时保留 `i <= j` |

同一集合必须显式提供 `self:'directed'/'undirected'` 和 `includeSelf:true/false`。单体和自身绑定仍是一个实例。不做按自由度地址的隐式去重；不同对象即便引用相同状态，仍按声明的组合规则处理。无向表示只保留上述一种参数顺序，并不自动对关系数学做对称化。

一个单体可以暴露多个具名自由度，甚至引用其他对象已有的自由度。例如星仪用一个对象暴露三个相位，另一个对象暴露三个载荷高度；一个 pair 保留六个变量、三行残差。单体字段用 `X.dof(q,index)` 选取一个自由度，或者直接引用 `count:1` 的自由度集合。集合各字段的成员数必须一致，且一一对应。`dofs:{}` 的单体可表示没有演化状态的参考框架；关系可以只需要另一侧的自由度及自身 history。

明确连接使用 `X.pairs(relation, [[a,b],[c,d]], parameters, options)`。它只批量提交同一种 pair 声明，各条绑定的展开行依次拼接；不会把两个集合偷偷变成逐行 zip。布料和绳网从集合显式提取成员，再列出相连对象对。`pair` 与 `pairs` 返回可用于 `patch` / history 读写的稳定关系集合索引。

### API 与字段

| 调用 | 含义 |
| --- | --- |
| `space(n)` / `space({kind:'rotation'})` | 数学空间 |
| `space({name,stateSize,tangentSize,retract,difference})` | 自定义空间；公式可用 `expression(inputs)` 构建 |
| `model()` | 创建模型句柄 |
| `defineDofs(model, {space,count,initial,...})` | 声明自由度；可选 `name`、`velocity`、`inverseMetric`、`enabled`、`readOnly`，返回含 `.set` 的自由度句柄 |
| `dof(dofs, index)` | 引用一个已有自由度，用于单体的具名字段 |
| `defineObject(model, {kind,dofs,name?})` | `kind` 必须为 `single` 或 `collection`，返回对象句柄 |
| `defineMember(collection, index)` | 显式声明集合成员为单体对象，保留对原自由度的引用 |
| `defineRelation(definition, build)` | 声明两个形式对象之间的任意数学关系 |
| `pair(relation,a,b,parameters?,options?)` | 绑定关系与两个已声明对象 |
| `pairs(relation,bindings,parameters?,options?)` | 批量声明显式对象对 |
| `describe(model)` | 输出包含四层信息的场景文档 |
| `patch(model,field,set,first,data)` | 修改数值字段，pair 字段索引使用上述展开行顺序 |
| `compile(model,policy?)` | 提交模型，应用数值变更或安装新计划 |
| `step(model,tick,dt,writes?)` / `read(model,field,set,first,count)` / `poll(model)` | 提交求解步、范围回读、消费完成结果 |
| `destroy(model)` | 退役实例 |

`options` 可包含 `name`、`compliance`、`history`、`enabled`，以及同一集合的组合规则。数值字段可用单行数组表达统一值，或提供与完整展开行数相符的逐行数组。字段数组不参与选择成员。当前对象形状固定，增加对象或 pair 属于拓扑变更；没有 DSL 层的端点列替换或动态 pair 接口。C++ 原有动态端点接口仍供底层调用方使用。

旧的 `variables`、`object`、`collection`、`relations`、`appendVariables`、`appendRelations`、`replaceEndpoints` 不再作为脚本建模入口；`defineRelation` 也不再接受 `spaces` 或 `op.endpoints`。状态写入仍使用 `{field,set,first,values}`，其中自由度的 `set` 取自 `defineDofs` 返回的 `.set`。

模型字段名为 `inverseMetric`、`variableEnabled`、`parameters`、`compliance`、`relationEnabled`。运行状态字段名为 `value`、`velocity`、`acceleration`、`history`；乘子只属于求解器。

`compile`、`step`、`read` 使用单请求通道：每个模型最多一个未消费结果；`poll` 返回版本、tick、诊断、`error` 和 `Float32Array values`。处理完成后才能提交下一项操作。场景用法见 [Dynamics 与 ECS](dynamics-ecs.md)。

可运行示例位于 [ConstraintLab](../Projects/ConstraintLab/README.md)：布料、绳索、绳网、穿绳布幕、星仪，以及集合粒子示例。粒子示例只声明三条高层绑定，BindingIR 保留三个域，逻辑上仍有 4,950 个无向粒子组合、500 个粒子—边界组合和 100 个阻力实例。GPU 用 27 个整数描述全部 11,500 个端点引用。当前没有邻域查询、空间剪枝或集合归约；着色、逐关系字段和求解遍历仍随成员组合数增长。

## 重点验证

获准的 `dynamics_tests` 覆盖解析梯度、重复端点、十万关系的 Colored/Jacobi 解、共享变量的 Hybrid 回退、数值更新、GPU 迁移、不可变发布、动态关联表和多行局部求解/持久状态更新。它使用真实 Vulkan 设备和 validation layer，不是性能基准。

```powershell
.\third_party\cmake\bin\cmake.exe --build build --config Release --target dynamics_tests
.\build\bin\Release\dynamics_tests.exe
```
