# 引擎与 gameplay 约定

## 线程所有权

引擎主线程拥有 Win32 窗口消息、Input、World、PhysicsScene、Navigation 查询和唯一 JS heap。simulation 使用 60 Hz 固定步长，长帧最多补进 100 ms。输入按下边沿、滚轮和鼠标 delta 仅由第一个 simulation tick 消费，补帧不会重复点击。

每次模拟完成，把值拷贝到 `Frame`，经单槽 `FrameMailbox` 发布。槽位已占用时替换旧快照；主线程不等待 GPU。渲染线程读到快照后只访问自己的副本，不读取 World 或 JS 对象。上一次**真正渲染**的快照用于物体与相机运动向量，不能拿上一个 simulation tick 代替。

渲染线程创建、使用和销毁 Vulkan 对象，包括 descriptor、swapchain、BLAS/TLAS、NRD pools 和 HUD 的离屏 GDI 位图。一个 GPU frame in flight，fence 完成后才能更新 host-visible buffer 或 descriptor pool。present 完成 semaphore 按 swapchain image 分配。

关闭路径：主线程停止发布 → mailbox.close 唤醒渲染线程 → join → GPU idle / 资源析构完成 → 销毁窗口。渲染线程异常也会关闭 mailbox，主线程读到 finished 后 join 并报告错误。零尺寸窗口不执行 GPU 帧，恢复／resize 时等待 GPU 并重新创建屏幕资源和 NRD 历史。

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
