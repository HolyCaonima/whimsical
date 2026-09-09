# GPU XPBD

系统只定义数学状态空间、变量集合、关系定义和端点连接。关系的残差、乘子投影及持久状态更新由用户编写；表达式不能访问 GPU 资源或产生全局副作用。

## 模块与所有权

| 模块 | 拥有的内容 | 依赖 |
| --- | --- | --- |
| `xpbd/model` | 数学表达式、空间、集合、稳定索引、初始数据、不可变快照与变更 | C++ 标准库 |
| `xpbd/compiler` | Jacobian 生成、SoA 布局、求解阶段、区域分析与融合、Jacobi 关联表、迁移映射 | Model |
| `xpbd/runtime` | GPU 当前状态、执行上下文、提交/完成、状态迁移、范围回读与结果发布 | Compiler、RenderCore |
| `xpbd/adapter` | 脚本定义入口、模型句柄、可靠请求通道、GPU 宿主服务 | 按前端和 GPU 服务分开链接 |

`whimsical_core` 只链接前端 `whimsical_xpbd_adapter`，不因此依赖 GPU Runtime。`Whimsical` 在现有 GPU 线程创建 `GpuService`，与 Renderer 共用应用拥有的 RenderCore。Runtime 不创建线程，不依赖 World、Rendering 或脚本。

四个契约是 `ModelSnapshot`、`ChangeSet`、`CompiledPlan` 和 `CompletedState`。同一个不可变计划可以实例化多份独立状态。数值参数存在 Model，当前值、速度、加速度、乘子及用户持久状态存在 Runtime；修改初始值不会悄悄重置当前求解结果。

## 数学定义

`Space` 声明存储维数、切空间维数以及两个纯函数：

```text
retract(q, delta) -> q'
difference(qNew, qOld) -> delta
```

内置 `Space::euclidean(n)` 和四元数 `[w,x,y,z]` 表示的 `Space::rotation()`。后者使用归一化的一阶局部旋转回缩和最短旋转差；它不包含物体、惯性或碰撞语义。自定义空间采用同一个表达式接口。

关系可以包含多个端点、多个残差行、参数和用户持久状态。所有关系阶段的输入顺序统一为：

```text
[各端点存储坐标, 参数, 持久状态, 候选乘子, 子步 dt, 子步结束时间]
```

残差不能读取候选乘子。`projection` 将候选乘子投影到用户定义的集合，`update` 在每个子步结束时更新一次持久状态。默认关系为等式；还支持乘子非负、非正和自定义投影。

共享表达式 DAG 支持常量、输入、四则运算、平方根、三角函数、指数/对数、绝对值、min/max、atan2、比较与选择。向量运算由这些纯运算组成，定义阶段进行公共子表达式复用和无用节点裁剪。反向自动微分生成残差 Jacobian，再通过 `retract` 的 Jacobian 转为切空间梯度。分段操作采用当前分支的导数；未选中的分支不会传播梯度。

每个变量具有完整的对称半正定逆度量矩阵。局部 XPBD 系统使用：

```text
(J W Jᵀ + diag(compliance / h²)) Δlambda
    = -C - diag(compliance / h²) lambda
```

一个关系多次引用同一变量时，先合并该变量的梯度，再形成局部矩阵与修正，保留交叉项。奇异局部系统不会被隐式添加柔顺度：该次修正被跳过，并计入诊断。非有限的残差、修正、空间运算和持久状态输出也会计入诊断。

## C++ 使用

```cpp
using namespace whimsical::xpbd;
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

执行方式与数值方法分开选择：`SolverPolicy::execution` 默认 `ExecutionMode::Auto`，也可指定 `ExecutionMode::Global` 保留逐阶段 dispatch。JavaScript 对应 `compile(handle, {execution:'auto'})` / `{execution:'global'}`。

Auto 根据静态端点连接识别封闭组件，按变量数、关系数、状态容量和函数复杂度选择工作组融合，并把多个小组件打包。区域内的值、前值、速度、history 和乘子放入共享内存，保持原有颜色顺序、Jacobi 归并及子步 / 迭代循环。共享只读端点也会连接组件，因为预测仍可能推进只读变量。大型组件保留全局路径；含动态端点的模型目前整体使用全局路径。

`CompiledPlan::statistics` 报告组件数、局部区域数、局部变量 / 关系数、每工作组共享字节数，以及优化前后的每步数值计算 dispatch 数。后者不含动态拓扑构建和上传 / 回读。具体预算、实现边界和实测结果见 [编译器优化说明](xpbd-compiler-optimization.md)。

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

`publish()` 显式复制值、速度与持久状态到不可变 GPU 快照。消费者在同一 RenderCore/GPU 线程上声明只读 Imported buffer，再使用 `PublishedState::import` 绑定。快照的 `plan` 提供 SoA 偏移和步幅。消费者保留资源所有权到解除或替换导入；即使调用方释放快照，正在使用它的上下文仍保持数据有效。发布有 GPU 复制和显存成本，不会自动在每 tick 产生一个副本。RenderCore 必须晚于所有实例、快照和消费者销毁。

## JavaScript

引擎宿主中的脚本使用 `Engine.xpbd`，句柄属于创建它的 realm。关系通过 `defineRelation(definition, build)` 构建一次；批量实例数据使用普通数组、`Float32Array` 和 `Uint32Array`，无需每行创建对象。

主要接口：

| 调用 | 含义 |
| --- | --- |
| `space(n)` / `space({kind:'rotation'})` | 定义数学空间 |
| `space({name,stateSize,tangentSize,retract,difference})` | 自定义空间；公式可用 `expression(inputs)` 构建 |
| `defineRelation(definition, build)` | 自定义关系、投影、持久状态更新 |
| `model()` | 创建模型句柄 |
| `variables(model, space, fields)` | 添加变量集合；包含 `count`、`initial`，可选 `velocity`、`inverseMetric`、`enabled`、`readOnly` |
| `relations(model, type, fields)` | 添加关系集合；包含 `endpoints`，可选 `parameters`、`compliance`、`history`、`enabled`、`dynamicEndpoints` |
| `patch(model, field, set, first, data)` | 修改数值模型字段 |
| `compile(model, policy?)` | 提交模型；根据变更应用参数或安装新计划 |
| `step(model, tick, dt, writes?, endpoints?)` | 提交一个求解步 |
| `read(model, field, set, first, count)` | 请求范围回读 |
| `poll(model)` | 消费完成结果；未完成返回 `null` |
| `appendVariables` / `appendRelations` / `replaceEndpoints` | 修改模型拓扑，之后调用 `compile` |
| `destroy(model)` | 退役所属 GPU 实例 |

创建集合时，一个字段可用单行数组表达统一值，也可提供完整逐行数据。追加接口要求完整的新增数据。每个端点列为 `{set, indices}`；跨集合端点用 `{sets, indices}`，两个数组逐项对应。状态写入格式为 `{field, set, first, values}`；动态连接写入为 `{set, first, endpoints}`。

数值模型字段名为 `inverseMetric`、`variableEnabled`、`parameters`、`compliance`、`relationEnabled`。运行状态字段名为 `value`、`velocity`、`acceleration`、`history`、`multiplier`，其中 `multiplier` 只可回读。

`compile`、`step`、`read` 是可靠的单请求通道：每个模型最多一个未消费结果，再次提交会报告 Busy。`poll` 返回版本、tick、诊断、`error` 和 `Float32Array values`。脚本应先处理错误、消费结果，再提交下一项操作；宿主不会丢弃或合并 tick。Model 编辑可先积累，但新结构必须安装后才能用于求解。场景 realm 销毁时自动退役其全部模型。

完整示例见 [batched-scalar.js](../examples/xpbd/batched-scalar.js)，可脱离 World 和 Rendering 运行：

```powershell
.\third_party\cmake\bin\cmake.exe --build build --config Release --target xpbd_run
.\build\bin\Release\xpbd_run.exe examples/xpbd/batched-scalar.js
```

独立宿主持续调用脚本的 `update()`，返回 `false` 时结束。示例创建十万个变量和关系，只回读最后四个值。

## 重点验证

获准的 `xpbd_tests` 覆盖解析梯度、重复端点、十万关系的 Colored/Jacobi 解、共享变量的 Hybrid 回退、数值更新、GPU 迁移、不可变发布、动态关联表和多行局部求解/持久状态更新。它使用真实 Vulkan 设备和 validation layer，不是性能基准。

```powershell
.\third_party\cmake\bin\cmake.exe --build build --config Release --target xpbd_tests
.\build\bin\Release\xpbd_tests.exe
```
