# Animation 系统

Animation 是独立的 C++17 库 `afterlight_animation`。上层提交动作意图、期望速度、朝向与可选未来轨迹，得到 parent-local 骨骼姿态、root motion、接触权重和事件。AI4Animation 是 `Solver` 的一个实现；未来 clip graph 和 motion matching 实现同一个接口，不需要继承神经动画类。

```text
Gameplay / JS / Navigation
        │ Input（velocity / facing / action / trajectory）
        ▼
World → Instance → Solver
                   ├─ AI4Animation Controller → ONNX Runtime
                   ├─ future ClipGraphSolver
                   └─ future MotionMatchingSolver
        │ Output（localPose / rootMotion / contacts / events）
        ▼
World：物理接受 root motion → FK → 关节碰撞体 / 不可变骨架与蒙皮快照
```

`Asset` 持有可共享的不可变资源，`createSolver()` 创建每角色独立的播放状态。公共 `AssetManager` 按 `.asset` 头部类型注册加载器并缓存资产，注册发生在 `registerEngineAssets` 组合入口。AnimationController 的内嵌 A4C2 描述通过 AssetRef 引用 OnnxModel 纯头资产，未来后端注册自己的类型即可。World、Instance、Input 和 Output 中没有算法枚举或 neural 专用字段。

## 表现属性与风格 UI

速度、朝向、轨迹及动作请求仍通过每帧 `Input` 提交。风格作为独立的持续属性保存于 `Instance`，由 `Asset::attributes()` 声明，`Context::attributes` 只读提供给 Solver。当前支持有明确选项的枚举属性 `EnumAttribute`，包含稳定 key、显示名、默认值及 value/label 选项；未引入任意键值黑板或尚未使用的数值属性类型。

双足资产声明 `locomotion.style`，默认 `BigSteps`，提供除 Idle 以外的 11 种风格。Gameplay 只提交 `Idle` 或 `Locomotion` 请求。AI4Animation 资产内的动作绑定表将 Idle 映射到静止指导姿态，将 Locomotion 映射到当前风格对应的 guidance；UI、World 和通用 Instance 不包含这张映射。四足资产暂未声明表现属性，仍使用原来的步态动作请求，不会在界面上伪造不支持的 style。

属性只在显式设置时改变；每帧输入、待机、移动、`resetAnimation` 都保留当前值。实例之间独立。低层 `setSolver` 替换的是同一资产契约下的实现，保留属性；挂接新 Asset 会重新采用新声明和默认值。新求解器可将相同语义属性映射到图参数、候选集筛选或匹配代价，但选项与具体资源匹配仍由该资产负责。

选中角色时，左侧动画面板从 `Frame.animationInspection` 的不可变声明和值生成枚举选择器，左右箭头循环切换。UI 点击和滚轮由 ScriptRuntime 在 gameplay 前处理，不会同时触发移动或镜头缩放；键盘移动继续生效。静止时切换会保存选择，移动后观察该风格。面板跟随选中实体，未挂接动画时消失，`--no-hud` 时不拦截输入。绘制与命中区域共用 `engine/ui/AnimationInspector` 布局；小于 620×450 的窗口隐藏面板。

AI4Animation 在下一次 10 Hz 预测时读取所选风格，沿用既有序列混合；UI 不重置播放状态或强制混合不同枚举值。后续求解器自行定义切换时机。

## 坐标和生命周期

单位米，Y 向上，+Z 朝前。`Skeleton` 要求唯一骨骼名和 parent-before-child 顺序，支持多个根。`Pose` 始终是 parent-local；FK 得到 model-space；World 再合成角色根变换。旧 `GameObject.joints` / `animationJoints` 接口中的姿态实际上是 **相对角色根的 model-space**，不是 parent-local；AnimationCollision 在此空间上合成角色根和附属碰撞体偏移。

`World::attachAnimation` 指定骨架、求解器、root motion 策略及骨架原点相对物理物体中心的偏移。胶囊中心在离地 1 米时可用 `{0,-1,0}` 让动画根落地。`updateAnimations(dt)` 在 `ScriptRuntime::tick` 的 gameplay fixedUpdate 之后调用。原生无 JS 调度者直接调用它。

`rootMotion=true` 时 World 通过现有角色 sweep/slide 与站立查询接受位移；神经控制器在下一帧用真实根姿态校正缓存预测，阻挡位移不会累加。`false` 用于 gameplay 已经驱动位移的角色，上层消费姿态而不重复移动角色。此时也会把 gameplay 的真实根姿态反馈给求解器。

`setAnimationSolver` 在相同骨架上替换求解器并保留当前姿态。禁用实体暂停求解，销毁实体释放求解器和碰撞体。瞬移或需要清空接触锁定时调用 `resetAnimation`。`detachAnimation` 释放求解器及关节碰撞体、清空关节姿态并移除之后的骨架快照；改用不同骨架布局前先 detach，然后重新绑定对应关节碰撞体。

输出骨骼通过值复制发布到 `Frame.skeletons[].jointWorld`，蒙皮矩阵发布到 `Frame.skins[].palette`；不可变 `SkinnedMesh` 资源共享。渲染线程不读取求解器、World 或 ONNX session。F2 physics debug 同时显示已挂接骨架的绿色连线与角色物理胶囊。

## 蒙皮和当前场景

`SkinnedMesh` 按骨骼名字绑定到 Animation 骨架，与具体 Solver 无关。World 将当前 joint-world 矩阵乘以资源的 inverse-bind 矩阵，生成快照中的 palette。渲染线程执行 CPU 线性混合蒙皮，将同一份变形顶点用于 G-buffer 和光追几何，并按姿态变化 refit 每个角色的动态 BLAS。网格绑定改变时才重建几何资源。运动向量保存上一次实际渲染的变形位置，因此丢弃仿真快照、重复呈现与窗口恢复不会误用 simulation 的前一帧。

默认 Rain Court 的 Kiln 使用双足 ONNX 控制器与上游人体网格，Ash 使用四足 ONNX 控制器与狗网格。胶囊保留为不可见的运动学碰撞体。Locomotion 和 Companion 用实际移动速度、朝向与动作名驱动统一动画输入，二者均使用 `rootMotion:false`，由 gameplay 负责移动。Companion 定期寻路到主角后方，转向、加速追赶，并在靠近时减速停下；导航和连续扫掠复用 PhysicsScene。

网格由 `export_meshes.py` 从原始 GLB 离线导出成 内嵌 `SKN1` 格式的 `.asset`：骨骼绑定、位置、法线、线性顶点颜色、四个关节索引/权重和三角形索引。原始皮肤骨架中不参与求解的骨骼映射到最近的求解器祖先，并保留绑定姿态修正。狗的 base-color 纹理离线采样为顶点颜色；当前未增加运行时纹理采样或 GPU 蒙皮。资产来源和几何数量见 `Projects/Afterlight/Content/models/characters.asset`。

当前预训练双足资产提供 Idle 及多种移动风格，没有接入专门的蹲姿动画；Ctrl 仍改变移动速度和物理净空，人体网格保持该模型的直立姿态。

## AI4Animation 原生运行链路

上游入口来自 [AI4Animation](https://github.com/sebastianstarke/AI4Animation)，实际 Python 项目是 [facebookresearch/ai4animationpy](https://github.com/facebookresearch/ai4animationpy)，固定提交 `bfb5866681f7ea6dac9984be05181de5955eb48b`。`external/AI4AnimationPy` 是完整原始快照；逐文件 SHA-256 记录在相邻 provenance 文件。训练、优化器、数据集、采集/导入/管理和编辑工具全部保留在 external，CMake 不编译也不执行它们。

| 上游 | 引擎吸收位置 |
| --- | --- |
| Actor 的骨架/FK、RestoreBoneLengths/Alignments | Animation.cpp 的 Skeleton 与刚性姿态操作 |
| IK/FABRIK.py、Locomotion/*/LegIK.py | 通用 solveFabrik、Controller 的接触后处理 |
| Animation/TimeSeries.py、RootModule.Series.Control | Controller 的均匀采样轨迹、速度/转向平滑和未来修正 |
| AI/FeedTensor.py、ReadTensor.py | encodePose、特征拼接与 decodeSequence 的明确 ABI |
| Locomotion/Biped、Quadruped 的 Predict/Animate、Sequence | Controller 与 Sequence：10 Hz 预测、16 帧/0.5 秒序列、采样混合、时间缩放、root lock |
| CxM、CategoricalEncoderDecoder、LinearBlock、FiLM、Codebook、Statistics 的 forward | 导出进 network.onnx / postprocessor.onnx，C++ OnnxModel 执行 |

双足输入为 `positions / forwards / ups / velocities / trajectory XZ positions / directions / velocities / guidance positions`，441 floats。四足去掉姿态 forwards/ups，339 floats。二者接触模型都使用完整的四组姿态特征，加上未来 15 帧四个接触点的位置距离、旋转角（度）和速度距离，分别是 456 / 504 floats。

主网络每帧输出为 `root XYZ-delta / positions / forwards / ups / velocities`，共 `3+12*jointCount` floats。root delta 的 Y 分量是**角度制 yaw**；从第 1 帧开始在预测起始根坐标系中逐项累加，不能把每段位移依次旋转积分。16 帧按上游 30 Hz 解释速度。输出是 `(1,16,279)` 或 `(1,16,327)`；接触模型输出 `(1,64)`。

ONNX 图包含冻结的统计归一化、ELU/线性/FiLM、按 channel 分组的 softmax、CxM estimator、denoiser 与 prior decoder。双足固定 3 次 denoising，四足固定 1 次；sample=False 是上游 demo 的确定性推理模式。导出时移除 dropout 和训练分支。引擎使用 CPU FP32 ONNX Runtime 1.20.1，单 session 单 intra-op 线程，共享 session 但每次调用使用独立输入输出。尚未增加 GPU provider 或多角色批量推理。

与上游的明确适配：输出轴先正交化为刚性 quaternion，姿态混合采用 quaternion slerp；骨长按层级恢复；预测缓存接受物理反馈；角色输入/速度 PID、按键与动作选择归 gameplay，求解器直接接收动作名与目标速度；没有移植 raylib/ECS、可视化 GUI、Authoring 编辑器或训练流程。四足 Dog.glb 缺失 HeadSite，导出器从同目录完整 `Wolf.glb` 的同名父子绑定关系补成虚拟骨骼，原始资产不改。不能从动作 NPZ 直接拷贝这个局部偏移：动作数据与 GLB 的骨骼局部轴不同，会把 muzzle 方向反过来，导致 RestoreBoneAlignments 翻转狗头。足底基准沿用 demo 初始化高度；坡面探测和动态地形脚部贴合不在本次范围。

## 使用

```cpp
AssetManager assets{Project(projectDirectory)};
registerEngineAssets(assets);
auto asset = assets.load<animation::Asset>(AssetPath("/Game/animations/ai4animation/biped/controller"));
world.attachAnimation(id, *asset, true, {0,-1,0});
world.setAnimationAttribute(id, "locomotion.style", "Neutral");
animation::Input input;
input.action = "Locomotion";
input.desiredVelocity = {0,0,1};
world.setAnimationInput(id, input);
// Each native simulation tick, after gameplay:
world.updateAnimations(1.f/60);
```

```javascript
// Project virtual paths. Gameplay-controlled movement uses rootMotion:false.
Engine.animation(id, '/Game/animations/ai4animation/biped/controller', {
    rootMotion: false, rootOffset: {x:0, y:-1, z:0}
});
Engine.skinMesh(id, '/Game/models/biped');
// Persistent setting, independent of per-frame input:
Engine.animationAttribute(id, 'locomotion.style', 'Neutral');
var controls = Engine.animationAttributes(id); // {solver, attributes:[{key,label,type,value,defaultValue,options}]}
Engine.animationInput(id, {action:'Locomotion', velocity:{x:0,y:0,z:1}, facing:{x:0,y:0,z:1}});
// Optional trajectory points: {time, position:{x,y,z}, rotation:{x,y,z,w}, velocity:{x,y,z}}
// Actions and attribute options are asset data; see metadata.json. Unknown values report an error.
Engine.animationReset(id);
Engine.animationDetach(id);
```

原生绑定网格使用 `world.setSkinnedMesh(id, SkinnedMesh::load(path))`；先挂接骨架，再绑定网格。更换同骨架 Solver 不需要重新绑定网格。

## 导出和验证

```powershell
node tools/bootstrap.mjs
python -m pip install -r tools/animation/requirements.txt
python tools/animation/export_onnx.py
python tools/animation/export_meshes.py
python tools/animation/verify_vendor.py
powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Test
```

导出只依赖离线 Python 环境；运行 exe 不需要 Python。导出器直接导入原始模型定义，跳过包的 GUI/ECS 初始化，执行 eval forward，并产生 ONNX 及对应纯头 `.asset`、内嵌 golden/metadata/controller 资产。controller.asset 不含网络权重，其载荷是 `A4C2` 标记的 little-endian 元数据流，字段顺序由 exporter 与 ControllerAsset.cpp 对齐。V2 在原有骨架/指导姿态后增加通用枚举属性声明和后端动作绑定表；旧 V1 资产需重新导出，网络权重不变。

也可导出单独的原始 MLP、Autoencoder、SequentialMLP、CategoricalEncoderDecoder 或 CxM checkpoint：`python tools/animation/export_onnx.py --checkpoint PATH.pt --output PATH.onnx --iterations 3`。此入口导出模型推理图；要用于 locomotion Controller，仍需满足上述特征 ABI 和配套骨架/接触资产。只应对可信 checkpoint 使用 PyTorch 的完整模块 pickle 加载。

测试覆盖父子 FK/空间转换、FABRIK 与骨长、独立求解器替换、root motion/动画碰撞体同步、禁用/销毁、JS 挂接和轨迹输入、不可变骨架快照、序列 ABI/角度与重定位、两套预训练网络的 C++ ONNX/PyTorch 对照，以及双足/四足各 180 帧的移动/转向/阻挡反馈/重置。ONNX 四组不同输入的最大绝对误差约 `3.10e-6`；这证明网络数值一致，**不代表经上述引擎适配后的整条动画轨迹与 Python demo 逐帧完全一致**。

`companion_follow` 额外验证实际关卡中的跟随、绕障、角色间距、两套导入网格和运动后的蒙皮矩阵更新。`animation_attributes` 验证独立求解器消费属性、实例隔离、属性持久性、UI 点击路由、不可变快照、全部风格求解和 Zombie 风格实际抬高手腕。五项 CTest 全部通过。

### 跟随与根运动

人和狗均使用 `rootMotion:true`：Locomotion / Companion 负责寻路、目标速度和动作意图；Solver 同时生成身体动作与根位移/旋转；World 通过碰撞和可行走区域检查应用根运动，下一帧实际根姿态反馈给 Solver。该分工同样适用于未来带 root motion 的 clip 或 motion matching 求解器，不在玩法脚本内绑定神经网络类型。

对应原始四足 `Program.Control` → `RootModule.Series.Control` → `Program.Animate`：先对目标速度/方向平滑并预测 0.5 秒轨迹，每 0.1 秒预测一次动作序列，再每帧采样/混合序列中的根姿态与骨骼。上游不会先把 Actor 转到目标方向，再丢弃网络的根旋转。脚本直接修改狗的 yaw 会绕过预测的转向和脚步，并迫使缓存每帧重定位；原先每 0.45 秒重新寻路因此产生周期性的转向突变。停止时传零 facing，沿用 controller 内的轨迹方向，避免追着残余路径点旋转。

`build/bin/Release/companion_tests.exe captures/dog-turn.csv` 可导出逐帧位置、实际 yaw、求解器根旋转和身体骨骼 yaw。测试同时检查根旋转被实际消费、重新寻路时根与身体转速的连续性，以及已有的跟随/碰撞约束。

双足同样参考原始 `Biped.Program.Control/Animate`：速度与朝向分别作为输入，由 controller 平滑轨迹及每帧混合根运动。Locomotion 在下一帧读取实际位移，供受阻重规划和状态判断使用；到达减速使用 `min(speed, distance * arrivalResponse)`，给网络留出收步时间，不再硬改位移或用脚本限速转 yaw。交互朝向在最后 1.5 米进近时独立输入，角色可以一边移动一边面向物体；站稳后的触发按实际根朝向判断，容差由 `interactionFacingTolerance`（0.2 rad）配置。此容差允许自然落脚后的残余朝向误差，不强制拧正已经着地的脚。`core_gameplay` 覆盖手动转向、停止、导航停靠、交互、动态受阻及逐帧根旋转消费。

### 骨骼姿态诊断

`animation_runtime` 还检查虚拟 muzzle 的绑定方向、待机/小跑时狗头保持向上，以及默认双足步态的双手相对身体摆幅。仅检查有限值、骨长或 GPU validation 无法发现骨骼轴翻转。

`locomotion.json` 的 `idleAction` / `moveAction` 为 `Idle` / `Locomotion`，风格默认值归动画资产。当前默认 `BigSteps` 也是上游 demo 按名字排序后的默认移动指导姿态。此前的 `Neutral` 在 2 m/s 对照中，原始 Python 与 C++ 的双手前后摆幅均约 10–12 cm，近似贴身；切到 BigSteps 后本机同一诊断约 19 cm。这是模型步态选择的差别，不是手骨骼丢失，也没有额外编程摆臂覆盖 ONNX 输出。

可复现的近距离检查（绘图额外需要 matplotlib）：

```powershell
build/bin/Release/animation_tests.exe --dump-poses captures/rig-check BigSteps
python tools/animation/inspect_poses.py captures/rig-check
python tools/animation/reference_runtime.py captures/rig-reference
```

前两条导出包含骨长还原和 IK 的原生姿态，并用实际 SkinnedMesh `.asset` 资源生成正侧面近照。第三条直接执行 external 中原始 Biped `Predict/Animate`、Actor、FABRIK/LegIK 和 PyTorch 模型，固定 Idle→Neutral、2 m/s、60 Hz 输入，用于区分原生移植与模型自身表现；仅跳过 UI 和 scene 初始化。它是离线诊断，不进入游戏运行时。

## 来源许可

上游代码、模型及其移植部分遵循 [CC BY-NC 4.0](../external/AI4AnimationPy/LICENSE)，版权归 Meta Platforms, Inc. and affiliates；不得将本次引入误认为 MIT/商业可自由使用。ONNX Runtime 自身采用 MIT，见 `third_party/onnxruntime/LICENSE`。原始训练资源许可保持原状。

统一资源信封、持久引用、项目路径和场景中的动画属性保存见 [Project / Asset / Scene](projects-assets-scenes.md)。
