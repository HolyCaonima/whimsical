# 引擎与 gameplay 约定

对象、组件、状态所有权、层级及迁移契约见 [ECS 架构](ecs.md)。World 作为组合入口持有 Registry 与四个独立系统；ComponentCatalog 统一组件接入、依赖、派生所有权、脚本和持久化，Changes 在提交边界驱动后端同步。

应用与场景的组合由 `RuntimeHost` 管理：常驻应用 realm 与场景 realm 独立，`ScenePersistence` 只装载数据，`ScriptRuntime` 只执行代码，模拟调度和视图由宿主控制。完整接口与保存边界见 [运行宿主与视图](runtime-host.md)。

## 线程所有权

引擎主线程拥有 Win32 窗口消息、Input、World、PhysicsScene、Navigation 查询及宿主/场景 JS heap。simulation 使用 60 Hz 固定步长，长帧最多补进 100 ms。输入按下边沿、滚轮和鼠标 delta 仅由第一个 simulation tick 消费，补帧不会重复点击。

主线程按固定步长睡到下一次 tick 到期，不做轮询；`Window` 在构造时申请 1 ms 定时器精度，避免节拍被系统默认的 15.6 ms 粒度量化。渲染线程结束时会唤醒主线程，关闭延迟不依赖超时。

每次模拟完成，把值拷贝到 `Frame`，包成不可变快照经单槽 `FrameMailbox` 发布。槽位已占用时替换旧快照；主线程不等待 GPU。被替换的快照如果从未被取走，它携带的场景事件会**折叠进新快照**（`SceneDelta::prepend`）：渲染线程靠事件链增量更新，链上缺一环就只能重扫整个场景，因此丢帧只应该丢掉延迟，不该丢掉事件。合并与写入在同一把锁内完成——若消费者能在两者之间取走旧快照，它拿到的 delta 会比自己的镜像更靠前，反而触发一次本可避免的重扫。快照按引用计数移交，跨线程不发生深拷贝，渲染线程也因此能零成本保留上一帧。`acquire` **不清空槽位**：只有在首个快照到达前才阻塞，之后没有新快照时返回 `Repeat`，渲染线程重新呈现最新快照而不是空等下一次 60 Hz tick。渲染帧率由显示器和 GPU 决定，不被仿真频率锁死。渲染线程只访问快照，不读取 World 或 JS 对象。上一次**真正渲染**的快照用于物体与相机运动向量，不能拿上一个 simulation tick 代替。

渲染线程创建、使用和销毁 Vulkan 对象，包括 descriptor、swapchain、BLAS/TLAS、NRD pools 和 UI 几何/纹理资源。一个 GPU frame in flight，fence 完成后才能更新 host-visible buffer 或 descriptor pool。present 完成 semaphore 按 swapchain image 分配。呈现模式由 `--present` 选择，默认 FIFO；`mailbox` 与 `immediate` 用于在没有 vblank 量化的情况下测量真实帧成本，设备不支持时回退 FIFO。

uiCore 在主线程拥有 RmlUi Context、文档、DOM 和输入。EngineUi 只拥有控制台与可选性能统计，不依赖 World；项目 Engine.ui 绑定拥有全部玩法文档、布局和事件。两者复用同一布局/事件系统，显示开关独立。每次发布时记录不可变 UiFrame，完整绘制列表持有几何和纹理的共享引用，跨线程无需传递 RmlUi 或 JS 指针。Vulkan UiRenderer 缓存资源，在 frame fence 后回收失效资源，并以预乘 alpha 绘制到已放置场景视图的窗口目标；不再有 GDI 光栅或整屏 CPU HUD 上传。场景文档及回调跟随场景 realm 重建，常驻应用文档与引擎工具持续存在。详见 [UI Core](ui-core.md)。

关闭路径：主线程停止发布 → mailbox.close 唤醒渲染线程 → join → GPU idle / 资源析构完成 → 销毁窗口。渲染线程异常也会关闭 mailbox，主线程读到 finished 后 join 并报告错误。零尺寸窗口不执行 GPU 帧，恢复／resize 时等待 GPU 并重新创建屏幕资源和 NRD 历史。

## 常驻 RenderScene 与 proxy

运行参数由主线程拥有的 `ConsoleRegistry` 管理，`EngineSettings` 注册引擎变量并把实际生效值装入已有 `Frame`。渲染线程不直接访问可变注册表；控制台编辑器发布展示数据，由主线程 EngineUi 更新 RML 文档。变量、命令、配置优先级及重启语义见 [CVar 与控制台](console.md)。CVar 的 cfg 是进程运行配置，独立于内容资产。

`RenderScene` 是常驻的、按槽位寻址的可绘制物描述，`World` 之外的渲染状态只经它流转。只有 `Renderable` 组件持有派生的 `slot` 槽位；槽位在对象销毁后进入自由列表待复用，单张运行场景内 `proxies_` 只增不减（整张地图替换会接收新场景的槽表），因此**槽位在所属 proxy 生命周期内稳定，销毁后可以复用**，可以直接当作 GPU instance buffer、TLAS instance 和 `gl_InstanceIndex` 的下标，三者天然对齐，无需任何映射表。

proxy 拆成两半，因为它们的变化频率相差一到两个数量级：`ProxyTransform`（位置、朝向、缩放）几乎每帧都动，`ProxyAttributes`（网格、材质、可见性、可交互）很少动。这个划分对应到 `GpuInstance` 的内存布局上——前 128 字节是 `model` 与 `previousModel`，后 16 字节是 `info`——所以变换更新是一条**只写前 128 字节的快速路径**，不碰属性、不重建 proxy、不需要改动 shader。

`RenderSystem` 的每一处渲染可见改动都收口到 `publishTransform` / `publishAttributes` 两个函数，`RenderScene` 逐槽做值比较后才记脏，因此"设置成原值"不会产生事件。`publish()` 交出一个 `SceneDelta`：结构变化、移动、属性三张槽位表，外加 `base`／`revision` 构成的链，消费者镜像正好落在 `base` 上才能应用增量，否则重扫。

`topology` 是单调计数而非每帧布尔标志。它只在槽位数增长或某个槽位改绑到不同几何体时递增——这是加速结构增量 refit 唯一吸收不了的变化。用计数是因为标志会随它所在的那个快照一起被丢弃，消费者会毫不知情地继续 refit 一个拓扑已经变了的结构。渲染器据此决定 TLAS 走 `UPDATE` 还是 `BUILD`，并且只在容量真正不够时才按 64 的块重新分配显存。

容量增长**不**触发重扫：槽位只能经 `create` 出现，而 `create` 必定记为结构事件，所以一条完整的事件链已经涵盖了镜像没见过的每一个槽位；把已经见过的槽位一并重写，会让"生成一个物体"退化成"重建整个场景"。

运动向量由渲染器侧的 CPU 影子 `shadowModel` 维持。重扫改变的是"重写哪些槽位"，不是"忘记它们在哪"——影子里仍然保存着每个槽位上一次实际渲染的变换，所以跳过快照不会毁掉运动向量；只有镜像从未写过的槽位、或历史被显式重置时才清零运动。上一帧动过、这一帧停下的槽位会被 `settleMotion` 收敛，否则它会持续上报旧运动，在降噪器后面拖出尾迹。

`--stress N` 生成一个 N 个额外物体、但每 tick 变化物体数固定为 8 的场景，`--full-upload` 强制退回逐帧全量重写。两者配合可以在同一个二进制里量出"代价随变化量还是随场景规模增长"，见 [验证](verification.md)。

## 渲染系统的职责切分

渲染侧按"渲染功能 / 图编译 / GPU 执行"三条职责拆开，每个文件只回答一个问题：

| 文件 | 职责 | 知道 Vulkan |
| --- | --- | --- |
| `render/graph/Registry` | 逻辑资源声明：格式、尺寸规则、所有权、shader 视图；binding 号的唯一来源 | 否 |
| `render/graph/ResourcePool` | 物理实现：显存、descriptor set、history 奇偶、transient 共享 | 是 |
| `render/graph/RenderGraph` | 图编译与执行：依赖、裁剪、校验、生命期、barrier 推导、pass 分发 | 是 |
| `render/RenderResources` | 本管线声明了哪些资源（按功能分组） | 否 |
| `render/GpuScene` | GPU 上的场景：几何、加速结构、instance 镜像、材质、纹理 | 是 |
| `render/RenderPipeline` | pipeline 对象，以及本帧由哪些 pass 组成、各自读写什么 | 是 |
| `render/Renderer` | 帧循环、swapchain、统计 | 是 |

这条切分线的判据是**尺寸由什么决定**：`GpuScene` 里的东西按内容大小分配（实例数、网格数、灯数），所以它自己拥有并 `import` 给图；图拥有的都是按屏幕尺寸分配的。这也是为什么 `Lifetime::External` 的资源图只绑定不同步——它们的写入发生在渲染线程的 CPU 侧，早于命令录制。

接入一个渲染功能只需要两处：在 `RenderResources` 里 `declare` 资源，在 `RenderPipeline::build` 里声明这个 pass 读写什么。binding 号、descriptor layout、GLSL 声明、显存分配、barrier 和裁剪都由此推导，没有中央资源表要改，也没有 barrier 要手补。声明与实际录制的一致性由图自己校验：pass body 只能通过 `PassContext` 取用声明过的资源，消费本帧没人产出的内容会在编译期报错。详见 [RT 渲染接入约定](rendering.md#资源依赖驱动的-rendergraph)。

## 坐标和资产

- 单位米，Y 向上，角色 +Z 朝前；GLM 列主序矩阵，列向量。
- Box 是中心原点的单位立方体；Capsule 半径 0.4 m，高 2 m，默认中心离地 1 m。
- 材质 `color` 和 `emission` 使用线性空间；roughness 是线性 perceptual roughness，metallic ∈ [0,1]。
- Project 根目录包含 `.project` 与 `Content/`，Project 只负责启动配置，ContentMounts/AssetManager 可独立挂载多个 Content。调用方访问任意挂载名，每个 Content 的 `/Game/...` 依赖只在自身解析，加载和保存禁止跨来源引用。网格、控制器、脚本、配置和 Map 都使用 `.asset`；payload 存储方式与类型独立，资产文件和 UI 共用底层路径映射。详见 [Project / Asset / Scene](projects-assets-scenes.md)。
- Map 内嵌对象、材质、灯、相机、导航及持久玩法描述；共享配置是 Data 资产。World 从 Map 重建临时 Entity、Render slot、BodyHandle；它们与持久 Asset/Object ID 分离。

## JS 生命周期和绑定

先加载 Map，再按 `.project` 顺序载入公共 Script 资产（3C、通用玩法、bootstrap），最后加载 Map 的专属脚本。`initialize()` 绑定已创建对象的玩法；`fixedUpdate(dt, input)` 执行控制、移动、同伴跟随、镜头，随后按层级顺序更新 Animator。脚本使用 ES5 语法，Duktape 2.7 不是 Node.js，没有 DOM / npm runtime。

| Binding | 作用 |
| --- | --- |
| `Engine.material(r,g,b,roughness,er,eg,eb,metallic)` | 返回材质索引 |
| `Engine.create({name,components})` / `addComponent` / `setComponent` / `removeComponent` | 创建实体或组合能力；不隐式创建物理或显示资源 |
| `Engine.light(x,y,z,radius,r,g,b,intensity)` | 创建 Transform + LightComponent 实体，返回实体 ID |
| `Engine.position(id)` / `transform(id,pose)` / `localTransform(id,pose)` | 读取世界姿态／写世界姿态／写局部姿态 |
| `Engine.parent(id,parent,keepWorld)` / `renderScale(id,scale)` | 设置刚体层级／独立修改显示尺寸 |
| `Engine.move(id,dx,dz)` | 执行碰撞扫掠和滑移，返回实际位置 |
| `Engine.findPath(id,target)` | 用角色物理胶囊尺寸、实际地面与净空执行 A* 和路径平滑 |
| `Engine.setPlayer(id)` / `select(id)` | 指定控制角色与选择状态 |
| `Engine.camera(x,y,z,yaw,pitch,distance)` | 提交相机状态 |
| `Engine.showPath(points)` / `status(state,message)` | 提交指令反馈 |
| `Engine.solid(id,bool)` / `setMaterial(id,index)` | 玩法引起的碰撞／外观变化 |
| `Engine.lightIntensity(id,value)` | 修改光源组件强度 |
| `Engine.readJson(path)` | 经 AssetManager 从虚拟路径读取 Data 资产 |
| `Engine.animation(id,path,options)` / `animationInput(id,input)` | 绑定动画资源并提交动作意图 |
| `Engine.skinMesh(id,path)` | 按骨骼名字将蒙皮网格绑定到已挂接动画的实体 |
| `Engine.animationAttributes(id)` | 查询该角色资产声明的属性选项和实例当前值 |
| `Engine.animationAttribute(id,key,value)` | 设置持续的表现属性，不覆盖每帧运动意图 |

JS 接收输入副本，runtime 拒绝在非 owner 线程 tick。绑定检查实体／材质索引，脚本异常通过受保护调用转成引擎错误。

## 3C 与玩法扩展

动画底层通过独立 `afterlight_animation` 库提供统一 Asset/Instance/Solver 协议。AnimationSystem 在 gameplay tick 后求解，接受物理 root motion、将 FK 结果发布到关节碰撞体及不可变骨架快照。AI4Animation/ONNX 是一个具体后端；clip graph 和 motion matching 未来接入同级 Solver，无需修改上层输入/输出。详见 [Animation 架构、资产与接口](animation.md)。

Controller 是单角色命令入口；它不包含关卡内容。Locomotion 先更新物理站姿，再处理命令、路径、速度、转角，并向动画系统提交实际速度和动作意图。直接 WASD 覆盖点击命令；不可通行目标被拒绝；遇到动态阻挡时重新查询路径。Navigation 只查询 PhysicsScene，使用角色真实胶囊尺寸检查地面与净空，避免对角切角并验证平滑路径。实际位移使用连续扫掠和滑移；Companion 复用这些入口跟随主角。参见 [Physics Scene](physics-scene.md) 的所有权、动画接口和实现边界。

交互通过 `Interactions.register(id,name,approach,action)` 注册。控制器先寻路到 approach，再转向物体，最后执行 action；Map 资产定义摆放，场景脚本定义 action 并通过持久角色引用绑定。增加新关卡时保留 3C 和通用 interactions，添加 Map 和专属 Script 资产。未来支持多个可控角色时，把 Locomotion 实例化为每实体状态，并在 Controller 维护一个当前角色 ID；渲染线程和资产目录不需要改变。

CameraRig 使用平滑跟随、平滑 zoom、俯仰／距离钳制、自由平移和显式重新跟随。边缘平移需要 Alt，避免单角色交互时误触发。当前尚没有复杂的相机遮挡消隐或体积碰撞。

Scene Save/Load、Object Path、缓存生命周期、JS 持久状态接口与版本约定见 [Project / Asset / Scene](projects-assets-scenes.md)。
