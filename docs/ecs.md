# ECS 对象与场景架构

实体是身份，能力由组件组成。`World::create()` 只创建 Identity，不分配 Transform、物理体、渲染槽或动画实例。`GameObject`、按形状推导碰撞的生产 `spawn` 配方及 World 内的动画旁表已经删除。World 保留组合入口、实体/地图生命周期、持久身份索引和快照发布；运动、层级、动画与渲染提取分别位于 `engine/ecs/`。

## 存储与边界

`Registry` 使用按类型独立的有序组件池。查询从指定的第一个组件池开始，返回满足组件交集的 Entity ID；不遍历一个带全部可选字段的对象数组。选择有序池是为了保持现有物理/动画的确定性顺序、稳定组件地址，并支持带 unique_ptr 的求解器实例。当前没有引入 archetype 搬迁、任务线程或外部 ECS 依赖；后续可在不改变系统/序列化契约的情况下换成紧凑池。

Entity 为非零 uint32，跨同一个 World 的地图重载保持递增，永不复用；耗尽时明确拒绝创建。存储只包含活实体，没有无限增长的 GameObject 墓碑。BodyHandle 继续使用物理后端的 slot/generation，Render slot 继续由 RenderScene 复用。三种身份互不替代，磁盘只保存持久 ID。

| 组件 | 状态及写入入口 | 必需依赖 |
| --- | --- | --- |
| Identity | 名称、持久 ID；World 创建/销毁 | 无 |
| Transform | local 是权威；world、children 是派生缓存；TransformSystem 写入 | 父级若非零，必须有 Transform |
| Disabled | 本地启用标记；有效状态沿父链计算 | 无 |
| Renderable | 外观、可选不可变静态网格；RenderSystem 管理派生槽位 | Transform |
| Collider | PhysicsScene 中的唯一 BodyHandle；MotionSystem 管理体生命周期 | Transform |
| Interactable | 交互标记；不隐式创建碰撞或渲染 | 无 |
| Animator | 独立 Instance/Solver、输入、资产属性与根绑定 | Transform；rootMotion 还需要胶囊 Collider |
| JointPose | 手工模型空间姿态，或 Animator 输出经 FK 得到的派生缓存 | Transform；有 Animator 时禁止手工覆盖 |
| JointColliders | 关节碰撞能力标记；描述/体由 AnimationCollision 持有 | JointPose |
| Skin | 不可变网格与按骨骼名生成的映射 | Animator、Renderable；不能同时绑定静态网格 |
| ScriptData | 实体级 JSON 对象，脚本读取的是副本，显式写回 | 无 |

碰撞 shape/motion/layer/flags 只在 PhysicsScene 中维护；没有另一份待同步的 Collider 描述。物理体 pose 是 Transform 的空间索引缓存，脚本不能直接写它。动画 Instance 独占 solver/local pose；JointPose 只缓存一次 FK，碰撞附件和渲染提取共同消费。站姿高度改变时 MotionSystem 调整 Animator 根偏移及其派生 JointPose，以保持脚底锚点，随后传播 Transform。

材质与资产绑定、环境灯、当前轨道相机、导航参数属于 SceneResources；选中状态、当前玩家、UI 路径反馈属于 GameplayState。它们是世界级资源，不给每个实体附赠一份。项目自身的车辆速度、圈数等规则仍由项目脚本负责；需要随实体持久化的数据放入 ScriptData。自定义原生运行时数据可通过 `add<T>/edit<T>/remove<T>` 组合；新磁盘组件必须显式扩展 SceneEntity 的编解码与依赖校验，未知格式不会被静默忽略。

## 生命周期与层级

组件由所属系统挂接；Registry 只读接口用于查询，内建组件不能通过 World 通用 add/edit 绕过后端生命周期。删除 Collider 释放该体；删除 Renderable 同时移除依赖它的 Skin，但保留 Animator；删除 Animator 释放 Skin、求解姿态和关节碰撞，保留独立的主碰撞体与 Renderable。删除不存在的 Animator 不会移除手工 JointPose。移除被空间能力使用的 Transform，或被 root motion 使用的 Collider，会报错，调用方需先移除依赖能力。

`destroy(entity)` 按子级优先销毁整棵子树，并清理持久引用、选择、悬停和玩家反馈。`clearScene` 释放活组件，保留 ID 序列、物理/渲染分配器及递增 delta 版本。旧 JS Entity、旧 BodyHandle 无法落到新实体上；未被消费的旧 Frame 继续持有不可变资产与数值快照。

Transform 层级是刚体平移与旋转：`world = parent.world * local`。没有共享的“物体缩放”：RenderComponent 的尺寸和 ColliderShape 的尺寸独立，避免非均匀父级缩放产生无法用刚体表达的剪切/胶囊形变。

- `setLocal` 修改父级空间姿态，`setTransform` 接收世界姿态并反算 local。
- `setParent(child,parent,keepWorld=true)` 默认保留世界姿态；false 保留 local。0 解除父级；自环、环路和无 Transform 父级在写入前被拒绝。
- 父级变动只传播其子树，立即更新世界变换、关联体、关节附件与代理；之后同一 JS tick 内的查询可见新状态。
- 父级禁用不覆盖子级本地 Disabled。恢复父级后，本地禁用的子级仍保持禁用。禁用动画保留最终姿态和网格绑定。
- Maple Circuit 的 16 个车轮使用车身父级；脚本只写轮胎自转/转向的局部姿态。

## 更新依赖

采用显式顺序，而非为当前单线程仿真引入通用任务图：

```text
平台输入 / picking
  → JS fixedUpdate（组件编辑、导航、运动学移动、动画输入）
    → AnimationSystem（按层级深度，父级先于子级）
      → solver.evaluate
      → MotionSystem 接受 root motion → TransformSystem 传播
      → 一次 FK → JointPose → AnimationCollision
    → 待处理地图请求 / JS realm 生命周期 → UI 更新
  → RenderSystem.extract → 不可变 Frame → FrameMailbox
  → 渲染线程资源同步 / 蒙皮 / BLAS / TLAS / 绘制
```

Transform 编辑同步传播，因此物理查询和拾取之前没有隐含的“记得 flush”要求。每个 Animator 的根运动在其子级求解之前提交，顺序不依赖实体创建顺序。`snapshot` 不推进求解器；无 JS 的宿主调用 `World::update(dt)` 即可运行动画阶段。结构编辑发生在脚本阶段/系统入口，渲染线程从不访问 Registry、PhysicsScene、JS heap 或 ONNX 实例。

## 场景与脚本

Map 新写入 version 4，`entities[].components` 只包含实际存在的能力。父级保存持久 ID，Transform 保存 local；world、children、运行时句柄、Skin 骨骼索引、solver 输出不入盘。ScriptData 保存在 `components.data`；场景全局 JSON 继续保存在顶层 data。空实体和空手工 JointPose 也能往返。

加载先校验组件/父级依赖和环路，在临时 World 完成全部资产、属性、骨骼和物理校验后替换活场景；实例化依次建立身份、local、父链，再挂接后端能力。加载失败保持原 World 和 JS realm。版本 3 仅在读取边界转换；版本 1/2 仍不接受。六张现有 Map 和内容生成/渲染验证工具均已迁移，资产及实体持久 ID 不变。

```js
var actor = Engine.create({name:'Sensor', components:{
    transform:{position:[1,2,3]},
    collider:{shape:{type:'box', halfExtents:[.5,.5,.5]}, blocking:false, pickable:true},
    data:{count:0}
}});
Engine.addComponent(actor, 'render', {material:0, scale:[1,1,1]});
Engine.hasComponent(actor, 'collider');
Engine.entities(['transform','render']);
Engine.parent(actor, parentEntity, true);
Engine.localTransform(actor, {position:{x:1,y:0,z:0}, rotation:{w:1}});
var value = Engine.data(actor); value.count++; Engine.setData(actor, value);
Engine.removeComponent(actor, 'render');
Engine.destroy(actor);
Engine.alive(actor); // false
```

组件描述的向量沿用 Map 数组格式；既有 `position/transform/moveBody` 等调用的向量继续使用 `{x,y,z}`。`Engine.spawn` 和混合物理/显示尺寸的 `Engine.pose` 已移除，分别使用 create/组件挂接、transform/renderScale。已有导航、碰撞查询、动画、UI、场景 save/load 入口保留。

## 增量渲染资源

RenderScene 是派生渲染镜像，不是可供业务独立编辑的第二个场景。变换、属性、几何绑定分开记脏；相同值不产生变换/属性事件。geometryChanged 同时记结构槽和递增 topology，跨丢帧仍可检测。禁用/可见性变化不改变 skin/static draw 集合。

渲染线程按 topology 变化同步几何绑定。静态网格按不可变资产共享；蒙皮资源按实体实例独立。增删/替换只创建或释放受影响的 BLAS；顶点/索引范围由可合并空闲区复用，现有网格索引和 BLAS 不移动。共享 GPU buffer 仅容量不足时增长，增长复制数据/更新描述符，不重建其他 BLAS。最后一个静态引用释放资源；蒙皮解绑释放对应动态资源。纹理集合变化也复用仍被引用的图片。

动画姿态变化只蒙皮并 refit 对应实例；重复快照只在需要时收敛一次运动向量。几何变化可能要求 TLAS BUILD，但不意味着重建其引用的全部 BLAS、shader、纹理或代理。GPU 资源仍只在渲染线程、帧 fence 完成之后管理。

## 验证入口

`ecs_lifecycle` 覆盖独立组件组合、组件依赖、失败写入、层级传播/环路、继承启停、创建顺序无关的动画更新、即时站姿锚点、级联销毁、句柄失效、脚本与持久化及几何范围复用。原有玩法/物理/动画/UI 测试已迁移到组件接口。

`python tools/test_ecs_lifecycle.py` 执行真实 Vulkan 生命周期测试，强制启用 validation，验证蒙皮启停/重绑、静态共享/最后引用释放、大网格替换/缓冲增长。预期仅 7 次几何构建、3 次释放、一次初始代理重同步，且 validation 错误为 0。`--smoke` 继续验证窗口操作和地图重载。
