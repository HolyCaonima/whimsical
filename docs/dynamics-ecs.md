# Dynamics 与 ECS

`World::dynamics` 在 `World::update` 中管理场景求解。`dynamics` 组件的单位是完整数学模型：一块布、一条绳子或多条共享定义的绳子可以合为一个模型，内部十万级变量和关系仍使用集合、字段页、索引连接和 GPU SoA。ECS 系统不会遍历这些内部变量，也不会为它们创建实体、组件或 GPU 实例。

`dynamicsBinding` 是可选的实体交互／姿态映射，不是模型内部的拓扑表示。显示十万个网格顶点不应创建十万个 Transform 绑定，整体网格应通过 GPU 几何接入。当前场景层实现了 Transform 输入／输出、Render 实例数组输出与显式范围观察，尚未添加布料网格组件。Runtime 已有 `PublishedState::import` 的 GPU 消费契约；后续网格接入应沿该边界扩展，不向数学后端添加布料语义。

建模使用 `defineDofs → defineObject / defineMember → defineRelation → pair / pairs`。`X.describe` 保存对象声明、成员引用、两个形式对象的具名自由度接口和高层绑定；ECS 加载文档后由 Compiler 保留集合域和字段映射，按需遍历逻辑组合，执行计划确定后选择隐式索引或显式端点布局。`dynamicsBinding.variables` 仍是显示 / 输入映射的自由度地址，取 `defineDofs` 返回句柄的 `.set`，不承担数学对象或关系绑定语义。

## 组件与生命周期

`physics/dynamics/scene` 编入 Core，依赖 World 和 Adapter。数学 Model、Compiler 和 GPU Runtime 仍不依赖 ECS。World 与独立脚本模型共用宿主 GPU 服务，但各自拥有通道和实例。该服务与 Renderer 平级，拥有独立执行线程与计算上下文；请求到达时唤醒，GPU fence 完成后通知主线程，不再通过显示帧轮询。主线程也在固定更新之外消费完成通知，固定更新只负责累积模拟时间和提供输入。

组件保存数学定义、初始数据、策略和引用，不保存脚本句柄或 GPU 当前状态。加载时在隔离 World 编译模型，GPU 通道在活动 World 首次更新时接入；删除或替换模型组件会退休旧通道。保存／恢复从初始数据重新开始，不是求解中途快照。数值 `patch` 会反映在保存描述中，瞬时 `write` 不会。

```js
var X = Engine.dynamics;
// builder 由原有 space / defineRelation / variables / relations 接口构建。
var modelId = Engine.content.newId();
var owner = Engine.create({id:modelId, name:'Simulation', components:{
    transform:{},
    dynamics:{
        model:X.describe(builder),
        policy:{mode:'hybrid',substeps:4,iterations:12},
        stepTime:1/60, paused:false,
        // 可选的 CPU 观察范围；无观察和输出绑定时不读回。
        observe:[{set:0,first:0,count:8}]
    }
}});
X.destroy(builder); // 场景实例不依赖构建句柄。

var expression = X.expression(3);
var marker = Engine.create({components:{
    transform:{parent:modelId},
    dynamicsBinding:{
        direction:'output', variables:[{set:0,index:3}], interpolation:0.05,
        mapping:expression.finish([
            expression.input(0),expression.input(1),expression.input(2),
            1,0,0,0, 1,1,1
        ])
    }
}});
```

绑定可通过 `model` 指定模型实体的持久 ID；省略时沿父级查找最近的模型。输出 mapping 输入按 `variables` 顺序拼接各变量的存储坐标，输出为 `[position.xyz, quaternion.wxyz, scale.xyz]`，表示绑定实体的 **local** 姿态。项目可声明点、两点间线段等映射。`interpolation` 为可选的姿态插值时间，单位秒，默认 0；它不对任意数学状态空间直接做线性插值。

输入绑定使用 `direction:'input'`，mapping 输入为同样顺序的十个 local 姿态分量，输出写入 `variables` 中唯一的目标变量。`field` 可为 `value`（默认）、`velocity` 或 `acceleration`，输出宽度须匹配字段。输入在实际求解步提交前采集，适用于固定点、操纵器等少量控制实体；批量输入使用 `scene.write`。输入绑定在其目标上优先于排队的批量 write。

输出绑定通过 TransformSystem 的通用 `claim / release / setDrivenLocal` 契约独占实体 local 姿态写权，普通编辑、运动和 rootMotion 不能同时写它。可以移动其父实体，或删除绑定后直接编辑实体。停用绑定实体停止其输入／输出；停用模型实体停止发出新步骤，已提交步骤仍会完成。显式引用的模型被删除时，绑定保持最后姿态，等待该持久 ID 再次提供模型；删除显示实体不会修改变量或关系。

## 批量输出到 Render 实例

输出绑定的 `target` 默认是 `"transform"`；设为 `"renderInstances"` 后，绑定依赖同一实体的 Render 组件，以其 `instanceCount` 为映射次数。它只保存一份公式与变量流描述，不为成员创建实体或绑定。第 i 个实例读取每个变量流的 `index + i * stride`，按流的声明顺序拼接输入；`stride` 默认 1，设为 0 时所有实例读取同一个变量，也可以用更大的步长选取交错数据。多个流可以把连续两点映射成一组线段，或组合不同变量集的状态。

```js
var e = X.expression(3);
Engine.addComponent(particleEntity, 'dynamicsBinding', {
    model:modelId, direction:'output', target:'renderInstances', interpolation:0.07,
    variables:[{set:positions.set, index:0}],
    mapping:e.finish([e.input(0),e.input(1),e.input(2), 1,0,0,0, 0.05,0.05,0.05])
});
```

此处 `particleEntity` 预先拥有球体网格、材质、数量与等长初始变换数组，见 [Render 实例契约](instanced-rendering.md)。每次完成样本只读取一次各变量范围，再逐实例应用同一映射；输出仍为十个 TRS 分量，解释为相对实体的局部实例姿态。实例数组由一次 RenderSystem 调用发布，排序始终只有一个组件项。每个实例独立保留插值起点和目标，沿用 Transform 输出的平滑插值规则。

实例输出通过 `claimInstances / releaseInstances / setDrivenInstances` 独占实例数组写权，实体自身的 Transform 仍可移动或由 rootMotion 驱动。删除 Render 时级联移除该绑定；删除绑定释放数组写权，并保留最后的姿态。更改受驱动的数量或数组前须移除绑定；材质、可见性等仍可独立修改。

这条路径沿用现有 CPU 样本回读，并非 GPU 状态直接作为实例缓冲。它消除逐实例 ECS、绑定调度与排序项，映射、插值、变换上传和实际 GPU 实例数据仍随实例数量增长。未来直接消费 GPU 状态时可沿已有发布接口扩展，不需加入粒子专用后端。

## 控制与按需同步

```js
var S = Engine.dynamics.scene;
S.control(owner, true);                        // 暂停
S.control(owner, true, true);                  // 排队一个单步
S.write(owner,'acceleration',0,0,acceleration); // 整行 Float32Array 或数组
S.patch(owner,'compliance',0,0,compliance);     // 自动 apply，无需重建实例
var state = S.state(owner);                    // tick/time、sampleTick、ready、error、数值诊断
var plan = S.plan(owner);                      // CPU 编译耗时、域 / 端点存储、导数裁剪、融合及 dispatch 统计
var firstEight = S.values(owner,0,0,8);        // 查询已完成的缓存，不隐式等待 GPU
```

`values` 仅查询已订阅范围的缓存，首个样本前返回初始值。观察范围和启用的输出绑定共同决定每步读回需求：合并重叠／相邻范围，通过 `TickInput::reads` 随求解一起提交，在同一 GPU 执行图的求解之后拷贝，再通过同一个完成通知原子发布 CPU 样本。无需先等待 Step 回复、再发送 Read 请求，也没有额外的 GPU 提交／阻塞等待。独立的显式 `read` 接口仍保留。默认不复制整模型，也不读取关系持久状态或求解器乘子。新增输出绑定若尚无对应样本，等待下一次完成的求解步。

每个模型使用固定 `stepTime`，在途最多一个操作，最多保留一个待执行步的时间量。GPU 或 CPU 观察跟不上时模拟时间落后于墙钟，不累计无限追赶队列。单步请求合并为一个待执行步；暂停时 write 保留到下一步，同字段、同起点、同长度的重复输入覆盖旧请求。重新设置 `dynamics` 组件用于拓扑／策略替换，并从初始状态重新开始。

原有独立 `compile / step / read / poll` 接口仍适用于脱离 ECS 的程序，不与场景组件共同调度同一个实例。

ConstraintLab 用一个模型承载所有绳子，交互端点通过输入绑定进入求解，关节和线段通过项目定义的输出表达式显示。项目不再管理 GPU 轮询或逐帧回写求解姿态。它为应变仪显式观察全部变量，这是小型示例的选择，不是引擎默认行为。
