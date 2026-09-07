# ECS 组件契约

实体只提供身份；能力由独立组件组合。World 管理实体、场景资源、系统调度和快照发布。组件接入不再依赖 World 的类型分支、SceneEntity 的固定可选字段或 ScriptRuntime 的组件名单。

## 一处注册，贯通全部入口

`ComponentCatalog` 是进程内的组件契约表，在启动阶段注册，运行阶段只读。每份契约声明：

- 稳定名称、运行时类型、描述类型及存在查询。
- 描述的编解码、值校验和组合校验。
- 必需依赖，以及依赖被移除时拒绝或级联移除的策略。
- 由该组件拥有的派生组件，例如 Animator 拥有 JointPose。
- 只读准备、安装、捕获与释放动作。

`SceneEntity.components` 是按契约名称索引的描述集合，内容是描述值，不保存运行时句柄。脚本创建、组件组接入、查询、移除和 Map 读写使用同一张表。未知组件报错；未注册的原生普通数据仍可作为瞬态组件使用。Identity 和 Disabled 属于实体生命周期元数据，分别对应 Map 的 id/name 与 enabled，不走普通组件挂接接口。

普通数据组件用 `dataComponent<T>` 注册即可，不需要编辑 World、场景结构或脚本分支：

```cpp
#include "ecs/DataComponent.h"
struct Energy { int value; };
auto type = dataComponent<Energy>("energy",
    [](const Json& j) { return Energy{int(j.at("value").number())}; },
    [](const Energy& v) { return Json{{"value", v.value}}; },
    [](const Energy& v) {
        if (v.value < 0) throw std::invalid_argument("Negative energy");
    });
type.dependencies = {{"transform"}};
componentCatalog().add(std::move(type));
```

这个类型可经 `World::add/edit/remove<Energy>` 使用，也可经脚本及 Map 使用。原生 edit 先编辑副本，校验成功后替换并通知；失败不改变原值。原生 add 同样检查注册依赖。依赖为条件性的后端能力应由所属系统编辑，不通过普通数据 edit 改变其结构关系。

带后端资源的组件通过 `component<Runtime, Description>` 定义契约。`prepare(const World&, ...)` 解析资产、创建待安装资源，只能读取世界；返回 `PreparedComponent`。安装和释放时，目录提供 `ComponentAccess`，允许访问组件存储及后端，不需要为扩展类型增加 World 的友元或类型分支。运行时类型声明 `using Ownership = SystemComponent`，阻止普通 add/edit 绕开其生命周期。组件保存资源句柄，不保存临时准备 World 的地址。

## 所有权与派生关系

| 组件 | 权威状态 / 派生状态 | 依赖 |
| --- | --- | --- |
| Transform | local、parent / world、children | 父级必须有 Transform |
| Renderable | 外观、不可变静态网格 / render slot | Transform |
| Collider | PhysicsScene 独占形状、运动分类、层、查询标志；组件持有唯一 BodyHandle | Transform |
| Animator | Instance/Solver、输入、属性、根偏移 / 求解输出 | Transform |
| RootMotionBinding | 根运动消费策略、碰撞 mask、是否保持动画锚点 | Animator；碰撞移动策略还依赖 Collider |
| JointPose | 手工模型空间姿态及可选骨架布局；有 Animator 时是其拥有的 FK 结果 | Transform |
| JointColliders | 绑定描述和物理体由 AnimationCollision 持有 | JointPose |
| Skin | 不可变网格 / 根据 JointPose 布局解析的关节映射 | JointPose、Renderable |
| Interactable | 交互标记 | 无 |
| ScriptData | 实体级 JSON 对象 | 无 |

Transform 的 world 是空间事实；物理体 pose、关节附件 pose 和 render proxy 是派生镜像。PhysicsScene 中的形状与查询标志不在 ECS 再保存一份。Animator 独占 solver/local pose，JointPose 缓存一次 FK，碰撞附件和蒙皮共用这个结果。手工 JointPose 可携带骨架名称及父子布局，直接驱动 Skin，不必创建 Animator。

Skin 与静态 mesh 共用当前后端的一个 render binding，因此二者不能同时挂到一个 Renderable；该限制在绑定入口校验，不要求所有渲染实体拥有 Animator。蒙皮资源与动画求解器仍分离。

RootMotionBinding 可选 `transform`、`kinematic` 或 `grounded`。前者直接合成刚体位移/旋转，不需要物理体；kinematic 接受任意受支持的碰撞形状及完整旋转；grounded 使用现有直立胶囊和地面可站立查询。Animator 自身不再要求胶囊，也不隐式选择移动方式。没有 RootMotionBinding 时只求解姿态，游戏代码可独立移动实体。场景明确为人和狗选择 grounded。

`preserveAnchor` 是显式策略。角色高度变化时 MotionSystem 发布中心位移，AnimationSystem 仅为选择该策略的实体补偿根偏移和关节缓存。MotionSystem 不再直接改写 Animator 或 JointPose。

材质、灯光、相机、导航参数和场景数据属于 SceneResources；选择、玩家与 UI 反馈属于 GameplayState；车辆规则继续由项目脚本拥有。

## 接入、移除与提交

组件组接入分为三个步骤：校验现有组件和依赖；按依赖拓扑顺序准备全部资源；在一个变更批次内安装。字典顺序不决定安装顺序。准备失败不修改已有实体；安装失败时，目录逆序释放本组已完成的安装，保留原有能力。单个安装器必须在发布自己的组件前完成构造，或清理自身尚未完成的后端动作。释放动作不得抛异常。

移除先计算完整计划，确认不会留下悬空依赖，然后执行。规则均来自目录：移除被使用的 Transform 会拒绝；移除 Renderable 会级联 Skin；移除 Animator 会释放其拥有的 JointPose，以及依赖它的 Skin、JointColliders、RootMotionBinding，保留独立 Collider 与 Renderable。直接移除 solver 拥有的 JointPose 会拒绝。不存在的 Animator 不拥有任何东西，因此不会删除手工姿态。空 JointColliders 也保留组件存在性。

Registry 在组件实际增删时发布类型通知。普通数据编辑发布同样的类型通知；系统修改空间、姿态或外观时发布明确的派生失效通知。`Changes` 按类型和实体合并这些通知，只记录失效，不复制状态：

```text
TransformSystem：local / parent → 整个子树的 world
    → WorldPoseChanged
        → MotionSystem：主物理体 pose
        → AnimationSystem：关节附件 pose
        → RenderSystem：proxy transform
启停 → EffectiveEnabledChanged → 物理、附件、render attributes
JointPoseChanged → 关节附件
渲染外观变更 → transform / attributes / geometry 各自的通知
```

常规系统调用和脚本调用返回前完成提交，同一 JS tick 的后续查询可见更新。原生调用者可显式使用 `auto batch = world.changes()` 合并多次写入，再调用 `batch.commit()`；批次中的 Registry 是编辑状态，禁止对外做后端查询或帧提取。这个批次只合并通知，不承诺回滚任意原生代码的写入；组件组接入的回滚由目录负责。异常路径若留下未提交的原生编辑，需由调用者处理并 `commitChanges()`，不会默默返回过期的后端结果。

订阅者读取最终权威状态、更新自己拥有的派生资源，不在通知中发起新的实体结构编辑。通知无资源句柄；释放几何这类需要旧绑定的生命周期动作在目录释放步骤中直接处理，避免实体已删除后丢失失效事件。新增普通数据或后端可用 `World::onChange<T>` 消费结构及数据变化；删除时订阅者必须按组件是否仍存在处理解绑。

## 层级与调度

Transform 层级保持刚体平移与旋转：`world = parent.world * local`。渲染尺寸和碰撞尺寸独立，不用共享缩放给刚体后端制造剪切或胶囊形变。

`setParent(child,parent,keepWorld=true)` 默认保持世界姿态，false 保持 local，0 解除父级。环路与缺失父级在写入前拒绝。父级修改只传播子树；禁用继承不覆盖子级本地 Disabled。Maple Circuit 的车轮仍通过局部姿态跟随车身。

更新顺序保持明确的单线程阶段：脚本 fixedUpdate → 按层级父先子后求解 Animator → RootMotionBinding 消费根运动并提交变换 → FK / JointPose 提交 → 地图请求及 JS realm/UI → RenderSystem.extract → 不可变 Frame / FrameMailbox → 渲染线程。

snapshot 不推进求解器。渲染线程不访问 Registry、PhysicsScene、JS heap 或 ONNX Instance；它仍独占 GPU 资源及 fence 后的创建、更新、释放。静态资产共享、按实例蒙皮、几何空闲区复用和局部 BLAS 更新沿用既有实现。

## Map v5 与场景替换

Map v5 保存实际存在的组件描述。父级使用持久 ID，Transform 保存 local；world、children、slot、BodyHandle、骨骼映射和 solver 输出不入盘。手工 joints 可以是姿态数组，或 `{poses, layout}`；求解器拥有的 joints 不捕获为手工组件。`Engine.component` 返回可保存的描述副本，读取派生组件会明确报错。

场景加载先建立身份，按父先子后实例化实体，再由目录决定实体内部的组件顺序。所有资产与后端准备在隔离 World 中完成。验证成功后释放旧场景、一次转移 Registry 和后端绑定及资源，不重复创建求解器。系统和订阅关系留在所属 World，最终发布新组件通知。加载失败保留原世界和 JS realm。

实体 ID 和物理句柄 generation 序列跨地图延续，旧引用不能落到新对象上。RenderScene 的 revision/topology 跨替换单调推进；整张地图替换允许一次完整同步，局部组件变更仍只影响相关资源。未消费的旧 Frame 继续持有不可变资产和数值快照。

v3/v4 只在读取边界迁移：Animator 的 rootMotion 布尔值被转换为独立 grounded 组件，并显式开启原有锚点策略。六张现有地图与生成工具已迁移为 v5，资产和实体持久 ID 不变。

```js
var e = Engine.create({name:'Sensor', components:{
    transform:{position:[1,2,3]},
    data:{count:0}
}});
Engine.addComponents(e, {
    render:{material:0,scale:[1,1,1]},
    collider:{shape:{type:'box',halfExtents:[.5,.5,.5]},pickable:true},
    interactable:{}
});
var description = Engine.component(e,'render'); // 副本
Engine.entities(['transform','render']);
Engine.removeComponent(e,'render');
Engine.destroy(e);
```

组件描述沿用 Map 的数组向量；既有 transform/moveBody 等命令仍使用 `{x,y,z}`。`Engine.animation` 是 Animator 接入的便捷入口，不再接收 rootMotion 开关；用 `Engine.addComponent(e,'rootMotion',{mode:'grounded',preserveAnchor:true})` 明确接入消费策略。

## 关键验证

`ecs_lifecycle` 在原有生命周期覆盖上验证：注册新组件后脚本/原生/持久化一致；失败 draft 与组件组回滚；依赖和派生资源移除；批次提交只同步最终姿态；场景只准备一次且旧句柄失效；手工姿态蒙皮；无碰撞和盒体的根运动。`project_assets_scene` 与 `animation_runtime` 验证既有场景、脚本和求解链路。GPU 生命周期诊断入口仍为 `tools/test_ecs_lifecycle.py`。
