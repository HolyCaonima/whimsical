# 引擎与 gameplay 约定

## 线程所有权

引擎主线程拥有 Win32 窗口消息、Input、World、PhysicsScene、Navigation 查询和唯一 JS heap。simulation 使用 60 Hz 固定步长，长帧最多补进 100 ms。输入按下边沿、滚轮和鼠标 delta 仅由第一个 simulation tick 消费，补帧不会重复点击。

主线程按固定步长睡到下一次 tick 到期，不做轮询；`Window` 在构造时申请 1 ms 定时器精度，避免节拍被系统默认的 15.6 ms 粒度量化。渲染线程结束时会唤醒主线程，关闭延迟不依赖超时。

每次模拟完成，把值拷贝到 `Frame`，包成不可变快照经单槽 `FrameMailbox` 发布。槽位已占用时替换旧快照；主线程不等待 GPU。被替换的快照如果从未被取走，它携带的场景事件会**折叠进新快照**（`SceneDelta::prepend`）：渲染线程靠事件链增量更新，链上缺一环就只能重扫整个场景，因此丢帧只应该丢掉延迟，不该丢掉事件。合并与写入在同一把锁内完成——若消费者能在两者之间取走旧快照，它拿到的 delta 会比自己的镜像更靠前，反而触发一次本可避免的重扫。快照按引用计数移交，跨线程不发生深拷贝，渲染线程也因此能零成本保留上一帧。`acquire` **不清空槽位**：只有在首个快照到达前才阻塞，之后没有新快照时返回 `Repeat`，渲染线程重新呈现最新快照而不是空等下一次 60 Hz tick。渲染帧率由显示器和 GPU 决定，不被仿真频率锁死。渲染线程只访问快照，不读取 World 或 JS 对象。上一次**真正渲染**的快照用于物体与相机运动向量，不能拿上一个 simulation tick 代替。

渲染线程创建、使用和销毁 Vulkan 对象，包括 descriptor、swapchain、BLAS/TLAS、NRD pools 和 HUD 的离屏 GDI 位图。一个 GPU frame in flight，fence 完成后才能更新 host-visible buffer 或 descriptor pool。present 完成 semaphore 按 swapchain image 分配。呈现模式由 `--present` 选择，默认 FIFO；`mailbox` 与 `immediate` 用于在没有 vblank 量化的情况下测量真实帧成本，设备不支持时回退 FIFO。

HUD 是与 swapchain 同分辨率的 CPU 光栅目标，因此**不能**每帧重画：`DebugHud::draw` 对其显示的全部内容取签名，只在内容变化时重绘并回报是否需要上传，渲染线程据此跳过整屏 buffer→image 拷贝。稳定状态下重绘频率约 2 Hz，由统计刷新间隔驱动；`--physics-debug` 的线框跟随实时姿态，该模式下每帧重绘。写入映射缓冲必须使用 32 位整字：该内存在独显上是写合并的，逐字节写会瓦解写合并。

关闭路径：主线程停止发布 → mailbox.close 唤醒渲染线程 → join → GPU idle / 资源析构完成 → 销毁窗口。渲染线程异常也会关闭 mailbox，主线程读到 finished 后 join 并报告错误。零尺寸窗口不执行 GPU 帧，恢复／resize 时等待 GPU 并重新创建屏幕资源和 NRD 历史。

## 持久化 RenderScene 与 proxy

`RenderScene` 是常驻的、按槽位寻址的可绘制物描述，`World` 之外的渲染状态只经它流转。每个 `GameObject` 持有一个 `proxy` 槽位；槽位在对象销毁后进入自由列表待复用，`proxies_` 本身只增不减，因此**槽位索引在整个进程生命周期内稳定**，可以直接当作 GPU instance buffer、TLAS instance 和 `gl_InstanceIndex` 的下标，三者天然对齐，无需任何映射表。

proxy 拆成两半，因为它们的变化频率相差一到两个数量级：`ProxyTransform`（位置、朝向、缩放）几乎每帧都动，`ProxyAttributes`（网格、材质、可见性、可交互）很少动。这个划分对应到 `GpuInstance` 的内存布局上——前 128 字节是 `model` 与 `previousModel`，后 16 字节是 `info`——所以变换更新是一条**只写前 128 字节的快速路径**，不碰属性、不重建 proxy、不需要改动 shader。

`World` 的每一处渲染可见改动都收口到 `publishTransform` / `publishAttributes` 两个函数，`RenderScene` 逐槽做值比较后才记脏，因此"设置成原值"不会产生事件。`publish()` 交出一个 `SceneDelta`：结构变化、移动、属性三张槽位表，外加 `base`／`revision` 构成的链，消费者镜像正好落在 `base` 上才能应用增量，否则重扫。

`topology` 是单调计数而非每帧布尔标志。它只在槽位数增长或某个槽位改绑到不同几何体时递增——这是加速结构增量 refit 唯一吸收不了的变化。用计数是因为标志会随它所在的那个快照一起被丢弃，消费者会毫不知情地继续 refit 一个拓扑已经变了的结构。渲染器据此决定 TLAS 走 `UPDATE` 还是 `BUILD`，并且只在容量真正不够时才按 64 的块重新分配显存。

容量增长**不**触发重扫：槽位只能经 `create` 出现，而 `create` 必定记为结构事件，所以一条完整的事件链已经涵盖了镜像没见过的每一个槽位；把已经见过的槽位一并重写，会让"生成一个物体"退化成"重建整个场景"。

运动向量由渲染器侧的 CPU 影子 `shadowModel` 维持。重扫改变的是"重写哪些槽位"，不是"忘记它们在哪"——影子里仍然保存着每个槽位上一次实际渲染的变换，所以跳过快照不会毁掉运动向量；只有镜像从未写过的槽位、或历史被显式重置时才清零运动。上一帧动过、这一帧停下的槽位会被 `settleMotion` 收敛，否则它会持续上报旧运动，在降噪器后面拖出尾迹。

`--stress N` 生成一个 N 个额外物体、但每 tick 变化物体数固定为 8 的场景，`--full-upload` 强制退回逐帧全量重写。两者配合可以在同一个二进制里量出"代价随变化量还是随场景规模增长"，见 [验证](verification.md)。

## 坐标和资产

- 单位米，Y 向上，角色 +Z 朝前；GLM 列主序矩阵，列向量。
- Box 是中心原点的单位立方体；Capsule 半径 0.4 m，高 2 m，默认中心离地 1 m。
- 材质 `color` 和 `emission` 使用线性空间；roughness 是线性 perceptual roughness，metallic ∈ [0,1]。
- 基础模型 manifest 描述当前 C++ 生成器，尚未做通用模型 importer。动画 manifest 中的速度和状态机参数由 JS 读取。
- material、camera/spawn、locomotion 和 physics profile JSON 在启动时读取；修改后重新运行即可。关卡边界与 navigationCellSize 已用于物理地面查询和导航网格配置。lighting 字段仍是记录用途，GPU 对应参数仍在 GLSL 中。

## JS 生命周期和绑定

脚本以明确顺序载入：`3c/locomotion`、`3c/camera`、`3c/controller`、`gameplay/interactions`、`levels/rain_court`、`bootstrap`。`initialize()` 创建世界；`fixedUpdate(dt, input)` 执行控制、移动、镜头。脚本使用 ES5 语法，Duktape 2.7 不是 Node.js，没有 DOM / npm runtime。

| Binding | 作用 |
| --- | --- |
| `Engine.material(r,g,b,roughness,er,eg,eb,metallic)` | 返回材质索引 |
| `Engine.spawn(name,capsule,x,y,z,sx,sy,sz,material,solid,interactable)` | 创建实体，返回稳定非零 ID |
| `Engine.light(x,y,z,radius,r,g,b,intensity)` | 创建球形位置扰动的解析灯 |
| `Engine.position(id)` / `pose(id,x,y,z,yaw,renderHeight)` | 读取／设置对象与物理刚体姿态；末参数为显示 Y 尺寸 |
| `Engine.move(id,dx,dz)` | 执行碰撞扫掠和滑移，返回实际位置 |
| `Engine.findPath(id,target)` | 用角色物理胶囊尺寸、实际地面与净空执行 A* 和路径平滑 |
| `Engine.setPlayer(id)` / `select(id)` | 指定控制角色与选择状态 |
| `Engine.camera(x,y,z,yaw,pitch,distance)` | 提交相机状态 |
| `Engine.showPath(points)` / `status(state,message)` | 提交指令反馈 |
| `Engine.solid(id,bool)` / `setMaterial(id,index)` | 玩法引起的碰撞／外观变化 |
| `Engine.lightIntensity(index,value)` | 动态灯强度 |
| `Engine.readJson(path)` | 从 `game/assets` 内读取 JSON |

JS 接收输入副本，runtime 拒绝在非 owner 线程 tick。绑定检查实体／材质索引，脚本异常通过受保护调用转成引擎错误。

## 3C 与玩法扩展

Controller 是单角色命令入口；它不包含关卡内容。Locomotion 先更新物理站姿，再处理命令、路径、速度、转角和表现。直接 WASD 覆盖点击命令；不可通行目标被拒绝；遇到动态阻挡时重新查询路径。Navigation 只查询 PhysicsScene，使用角色真实胶囊尺寸检查地面与净空，避免对角切角并验证平滑路径。实际位移使用连续扫掠和滑移，视觉 bob 独立于物理姿态。参见 [Physics Scene](physics-scene.md) 的所有权、动画接口和实现边界。

交互通过 `Interactions.register(id,name,approach,action)` 注册。控制器先寻路到 approach，再转向物体，最后执行 action；场景脚本只定义摆放和 action 内容。增加新关卡时保留 3C 和通用 interactions，添加独立 level script / data。未来支持多个可控角色时，把 Locomotion 实例化为每实体状态，并在 Controller 维护一个当前角色 ID；渲染线程和资产目录不需要改变。

CameraRig 使用平滑跟随、平滑 zoom、俯仰／距离钳制、自由平移和显式重新跟随。边缘平移需要 Alt，避免单角色交互时误触发。当前尚没有复杂的相机遮挡消隐或体积碰撞。
