# Whimsical 引擎开发指南

本页集中说明引擎架构、实现边界、开发命令与历史测量。项目介绍、截图和快速开始见 [README](README.md)。专题文档继续保存在 `docs/`，测量结果只适用于各自记录的场景与配置。

## 开发环境与运行诊断

需要 Windows x64、Visual Studio 2019 C++ desktop workload / Windows SDK、Node.js 22+。支持 Vulkan 1.3、ray query 和 acceleration structure 的 GPU 是运行要求；没有将光追自动替换成传统光照。

```powershell
node tools/bootstrap.mjs
powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Test
.\Run.cmd
```

依赖全部位于 `third_party/`，不需要系统安装 Vulkan SDK。bootstrap 使用固定版本和 `tools/dependencies.lock.json` 中的 SHA-256 校验下载。CMake、GLSL 编译器、DXC 也随工程依赖下载，来源见 [固定依赖](docs/third-party.md)。`-Test` 运行 CMake 当前注册的 3 组 CTest：`xpbd_solver`、`console_system` 与 `content_mounts`。历史文档可能引用清理前的 ECS、玩法、物理、动画、Shader 与渲染审计测试数量，应按对应记录的日期与命令理解。

表面 Shader 由随附 glslang 在运行时按 pass 编译，因此当前开发运行时需要源码树在位；离线 cook 与持久二进制缓存是后续工作。

```powershell
# 原生窗口自动运行、GPU readback 截图、正常退出
.\build\bin\Release\Whimsical.exe --frames 90 --capture

# 点击移动、镜头、resize、最小化恢复、取消命令、运行中重载 Map
.\build\bin\Release\Whimsical.exe --smoke --width 960 --height 600

# 按呈现帧驱动的 ECS 生命周期回归：启停、蒙皮解绑重绑、几何增删改
.\build\bin\Release\Whimsical.exe --ecs-smoke --width 960 --height 600

# 1280×800 默认；也可指定尺寸、调试视图和无 HUD 截图
.\build\bin\Release\Whimsical.exe --view 5 --no-hud --frames 60 --capture

# 解除 vsync 量化，测量真实的每帧成本；启动即覆盖 CVar
.\build\bin\Release\Whimsical.exe --present immediate --frames 360 --capture --cvar "r.Exposure=1.4"

# 放大场景但保持每 tick 变化量恒定；加 --full-upload 退回逐帧全量重写以作对照
.\build\bin\Release\Whimsical.exe --present immediate --frames 900 --capture --stress 900
```

截图写入 `captures/frame.bmp`；GPU 测量和 validation 状态写入 `captures/render-report.json`。`--capture` 未指定帧数时默认 90 帧。报告中的 `sceneSlotWrites` 与 `sceneSlotWritesIfRebuilt` 是同一场景下增量上传与全量重建的实际写入次数。

Release 构建默认**关闭** validation（每帧约 1 ms CPU，足以把贴着预算的帧推过一次 vblank），Debug 构建默认开启。用 `--validation` 显式开启；`--smoke`、`--ecs-smoke`、`--console-smoke` 作为正确性关卡始终开启。`--present fifo|mailbox|immediate` 选择呈现模式，默认 `fifo`。完整参数见 `--help`。

## 控制台与性能采样

按 **~ 或 F10** 打开游戏内控制台：支持 CVar 查询/修改、类型与范围校验、多关键词模糊补全、上下键选择候选、命令历史、配置保存。可试 `r.Exposure 1.4`、`r.Hud 0`、`r.DebugView 1`、`t.TimeScale 0`、`help`。控制台打开时接管游戏输入，关闭 HUD 后仍可使用。变量、启动覆盖及开发接口见 [CVar 与控制台](docs/console.md)。

输入 `profileGPU` 采集随后约一秒内同一 RenderCore 的 GPU 提交，包含渲染、XPBD、RenderGraph、加速结构、NRD 子阶段和拷贝；结果输出到控制台及 `captures/gpu-profile.json` / `.txt`。启动时也可传 `--profile-gpu`。见 [GPU 分阶段计时](docs/gpu-profiling.md)。

输入 `profileCPU` 抓取 Game / Render 线程的 CPU 阶段耗时，包含独立标注的等待阶段，以及 inclusive / self 读数；`profileCPU last` 查看最近报告，结果保存在 `captures/cpu-profile.json` / `.txt`。启动参数为 `--profile-cpu`。见 [CPU 分阶段计时](docs/cpu-profiling.md)。

标题栏显示实时 **FPS**、**Frame ms**（包含线程与呈现等待的整帧间隔）、**CPU (Render) ms**（场景准备、CPU 蒙皮、HUD、命令录制与提交，不含常规帧同步/呈现等待）、**GPU ms**（Vulkan timestamp），约每半秒更新。启动及重建窗口尺寸后的统计预热显示 `-- FPS`。默认使用 FIFO 垂直同步，因此通常不超过 60 FPS；用 `r.Stats 1` 开启独立性能面板（默认关闭），显示当前呈现模式与 VSync 状态；`r.Hud` 只控制项目游戏界面。渲染帧率不被 60 Hz 仿真锁死——渲染线程在没有新快照时重新呈现最新快照，用 `--present immediate` 或 `--present mailbox` 即可跑出显示器刷新率以上的真实帧成本。

## 目录约定

```text
engine/
  assets/                 Project、虚拟路径、资产注册表、统一 .asset 和类型加载
  scene/                  Map 描述、Scene Save/Load 与持久 Object 身份
  ecs/                    Registry、组件契约目录、变更提交、层级/运动/动画/渲染系统
  core/                   组合入口 World、CVar、常驻 RenderScene、不可变帧快照和 mailbox
  platform/               Win32 窗口、输入、焦点／尺寸事件
  navigation/             膨胀障碍栅格 A*、路径平滑、连续碰撞移动
  physics/                独占形状与句柄、动态 AABB Tree、运动学扫掠／滑移
  animation/              统一骨架/姿态/求解器框架、AI4Animation 原生运行时和 ONNX 推理
  scripting/              Duktape runtime 和 C++ ↔ JS binding
  uiCore/                 RmlUi Context、输入、文档与不可变 UI 绘制快照
  ui/                     RML 引擎调试工具：控制台和可选性能统计
  debug/                  物理线框、ECS 生命周期 smoke 等诊断入口
  Content/UI/             引擎 RML / RCSS
  renderCore/             GPU 资源、RenderGraph、Vulkan 执行、同步与计时
  xpbd/                   数学模型、求解编译器、GPU 运行时与脚本适配
  render/                 几何、BLAS/TLAS、光照、NRD、运行时 Shader 编译与材质绑定
    shaders/              surface 契约、G-buffer / DI / GI / reuse / resolve / composite
Projects/                 独立项目；编辑器可显式挂载其他项目的 Content
  Afterlight/             双足与四足神经动画示例（Rain Court / Honeybud Court）
    .project              项目 ID、默认 Map、公共脚本加载顺序
    Config/               进程运行配置（CVar cfg）
    Content/              /Game 虚拟路径根
      Maps/               RainCourt / HoneybudCourt：实体组件、材质、灯、相机、导航和玩法状态
      shaders/            Standard / Paving / Foliage 表面资产与 GLSL 载荷
      materials/          引用 Shader 的可复用材质资产
      textures/           PBR 纹理资产
      models/             内嵌蒙皮／静态网格和模型描述
      animations/         控制器、配置、ONNX 纯头资产与独立模型
      physics/            碰撞配置资产
      scripts/            3C、通用玩法、UI、Map 初始化脚本，均为 Script 资产
      UI/                 项目 HUD 的 RML / RCSS，由项目 JS 驱动
    SourceArt/            Blender 源文件、manifest 与面数审计
  MapleCircuit/           赛车项目：赛道数据、车辆／AI／比赛 JS、天空 Shader、生成器与项目测试
  HumanEditor/            场景编辑器、资产浏览、变换工具与 Play/Pause/Stop
  EntityID/               整数渲染目标与异步像素回读示例
  ConstraintLab/          通用 GPU XPBD 的动态编译与运行示例
tests/                    XPBD、控制台与 Content 挂载的原生验证
tools/                    固定依赖下载、构建、迁移与 GPU 诊断脚本
docs/                     架构、渲染约定、第三方来源、验证记录
third_party/              可重新获取的外部依赖，不放自有 gameplay
external/AI4AnimationPy/  原样保存的上游源码、训练/数据工具及原始模型
build/                    CMake 产物与 SPIR-V，不放源资源
captures/                 实际 GPU 截图和运行报告
```

## 引擎结构

**ECS 与组件契约。** 实体只提供身份，能力由组件组合。`ComponentCatalog` 是进程内唯一的组件契约表：一处注册即贯通原生 API、JS 脚本、Map 读写和依赖校验，新增普通数据组件不需要改 World、场景结构或脚本分支。系统只拥有自己的派生状态——Transform 的 world 是空间事实，物理体 pose、关节附件、render proxy 都是镜像；`Changes` 在提交边界合并失效通知并驱动后端同步。组件组接入按依赖拓扑准备资源，失败逆序回滚。父子变换通过 Transform 层传播，Maple Circuit 的车轮只提交局部旋转；TRS 契约见 [变换说明](docs/transform-trs.md)。Map 写 `version: 7`，保存实际存在的组件描述与父级持久 ID。详见 [ECS 架构](docs/ecs.md)。

**Shader → Material。** `ShaderAsset` 拥有 GLSL 表面函数体、有序属性／纹理 schema、material model 和 render state；`MaterialAsset` 拥有解析后的取值和纹理引用。`engine/render/shaders/surface.glsl` 的 `EvaluateSurface` 是公共 ABI，同一份表面代码同时供 G-buffer 光栅化和六个 ray query pass 使用，通过 source linking 生成 dispatch 表，不需要 SBT 或 callable shader。当前只支持 `opaque` 与 `masked`，透明混合在资产加载时明确拒绝。详见 [Shader / Material](docs/shader-materials.md)。

**Project / Asset / Scene。** `Project` 描述启动配置，`ContentMounts` / `AssetManager` 统一挂载和管理多个独立 Content，加载与保存的依赖必须在各自来源内闭合。JS 可通过 `Engine.content` 挂载、浏览、加载和保存。所有资产使用统一的 `ALAS1` 头 + 载荷格式，载荷可内嵌或外置。资产 ID 在所属 Content 内是权威身份，移动资产后旧引用仍按 ID 解析；卸载后旧引用失效，同名重挂不会重新绑定。对象路径为 `/Game/Maps/RainCourt:<ObjectID>`；Entity ID、render slot 和物理句柄是运行时句柄，不入盘。支持 `--project <目录或.project>` 与 `--map /Game/Maps/RainCourt`。详见 [Project / Asset / Scene](docs/projects-assets-scenes.md)。

**物理与动画。** 独立主线程 `PhysicsScene` 存储 box/capsule 形状、查询层、动态 AABB Tree 与生命周期；角色移动、root motion、蹲起净空和庭院导航共用同一份查询，Maple Circuit 的车辆位移与撞击法线也来自它。动画层提供可替换 Solver、骨骼 FK、root motion／物理反馈、接触和 FABRIK；根运动消费策略是独立组件，可选直接变换、任意形状 kinematic 或直立胶囊 grounded。见 [Physics Scene](docs/physics-scene.md) 与 [Animation](docs/animation.md)。

**UI。** UI 由独立 **uiCore / RmlUi** 驱动：项目 JS 通过 `Engine.ui` 操作文档、DOM 和事件，控制台与项目面板共用 RmlUi 布局和 Vulkan UI 后端，绘制列表作为不可变快照跨线程传递。见 [UI Core](docs/ui-core.md)。

**应用宿主与场景。** `RuntimeHost` 分开管理常驻应用脚本、场景程序、模拟推进和独立视图。项目 JS 可跨 Content 装载场景数据而保留自己的 UI，查询/编辑 ECS 和场景资源，显式 Play/Pause/Stop，并通过临时对象标记隔离工具与地图的保存范围。视图颜色和 DrawEntityID 共用相机投影，UI 在窗口尺寸下合成。配置、API 与验证入口见 [运行宿主与视图](docs/runtime-host.md)。

线程所有权、帧快照 mailbox、常驻 RenderScene 的槽位契约和 JS 绑定表见 [架构](docs/architecture.md)。

## 已实现的渲染路径

```text
程序生成 box/capsule + 导入 StaticMesh → 共享静态 BLAS
动画骨架快照 → CPU 蒙皮 → 动态 BLAS refit
每帧实体姿态 → TLAS update
G-buffer 光栅化 → 生成的 Shader 表面求值
  ├─ albedo/metallic、normal/roughness、world position/entity ID
  └─ current→previous 屏幕运动向量、线性 viewZ、emission
RTXDI reservoir 初始采样 + BRDF 二次光线生成 GI samples + GGX specular ray
时域重投影 / 空间重采样 / GI Jacobian 与二次表面重新验证
最终光追可见性与 diffuse/specular radiance resolve
NRD RELAX_DIFFUSE_SPECULAR（原生 Vulkan dispatch）
材质重调制 → tone map → 选择反馈 / HUD → swapchain
```

DI 接入固定版本 RTXDI-Library：8 个 power/uniform RIS 候选、SDK 时域复用、4/8 个空间邻居和 ray-traced bias correction，最终阴影使用当前 TLAS ray query。同样本可见性梯度反馈给一套合并 DI/GI 的 NRD。GI 从 G-buffer 主表面的 BRDF secondary rays 出发，保存二次命中的位置、法线、实体 ID 和 radiance，再做时空重采样和重连接。masked 表面在 GI、镜面和阴影可见性中使用同一套 opacity 与 cull 规则。没有 shadow map、SSAO、SSGI、lightmap 或 probe GI 替代这条路径。

NRD 4.17.3 实际参与 GPU 计算，使用 RELAX 的原生 SPIR-V、资源池、每 pass 常量、sampled/storage bindings 和两个 sampler；不是仅有接口占位。

五类光源示例：`Whimsical.exe --map /Game/Maps/PhysicalLights`。Space 切换恒功率发光几何动画；组件接口、物理公式和验证命令见 [物理光源](docs/physical-lights.md)。

## 当前实现边界

这是可继续开发的第一版基础，不是大型游戏的最终渲染器。DI 使用 NVIDIA RTXDI SDK，GI 保留自写实现；集成范围和当前近似见 [RTXDI 集成](docs/rtxdi-integration.md)。当前 GI 是 **一次二次表面命中上的辐亮度重采样**；镜面间接光使用独立 GGX VNDF 路径和 NRD，尚无 ReSTIR PT、多跳路径重采样或 GI 的完整 MIS。历史长度与 GI Jacobian 有限幅，属于有偏实时方案。

灯光支持 Directional、Spot、Point、Rect、Capsule，共享物理单位、发光几何、能量归一化及采样 PDF，详见 [物理光源](docs/physical-lights.md)。发光材质支持二次光线命中，但还没有 emissive mesh light importance sampling。基础 mesh 共享静态 BLAS，蒙皮角色各自持有动态 BLAS 并随姿态 refit；TLAS 在只有变换变化时走 `UPDATE`，在拓扑变化（槽位增长或改绑几何体）时重建。当前采用一帧 GPU in flight 和保守 pass barrier，还没有 async compute、自动资源别名、流式场景或 GPU-driven indirect draw。资源上限为 1024 个 instance、256 个 material、256 盏灯和 64 张纹理，超限明确报错——Honeybud Court 的 667 个对象已经占掉实例上限的六成。

**没有 3D 透明混合**：表面只有 opaque 和 masked，Maple Circuit 的漂移烟团因此使用不透明网格加缩放消散。透明材质模式、深度策略、排序或 OIT 与合成阶段是引擎侧待补的契约，不应由项目脚本绕过。引擎也**没有音频系统**。

渲染场景是常驻的：`RenderScene` 给物体分配稳定槽位，ECS 以增删改事件发布变化，渲染器上传脏槽位；变换变化还有只写实例前 128 字节的快速路径。场景同步与上传成本跟随变化量，绘制和光追成本仍受场景规模影响。同步架构见 [RenderCore](docs/render-core.md) 与 [RenderGraph](docs/render-graph.md)。

Afterlight 项目中的 3C 包含加速、刹车、原地转向、行走、跑步、蹲行速度与物理净空、交互。`PhysicsScene` 当前为查询／运动学后端，未提供刚体堆叠和 ragdoll；独立 GPU XPBD 模块支持通过数学状态与关系定义求解，不会自动为场景实体生成动力学模型。楼梯、跳跃／翻越、坡面、多层导航、动画图混合和多角色控制仍需扩展。当前双足模型尚未接入蹲姿动画。Maple Circuit 的车辆是平面街机模型，没有悬挂或动力学。

ECS 侧当前是单 World、单已加载 Map，尚未引入 streaming、同一 Map 多实例或跨 Map 活实体解析。Map v3/v4 只在读取边界迁移，v1/v2 明确拒绝。

排查闪烁时可运行 `Whimsical.exe --audit NAME --view 1 --capture`：固定场景，预热 64 帧后统计连续帧；加 `--audit-motion` 使用固定的镜头旋转轨迹。结果在 `captures/NAME/`。使用控制台 `r.DebugView 0..7` 设置诊断视图，1 为 ALBEDO；DIRECT RT (RAW) / INDIRECT RT (RAW) 是降噪前的光照信号。资源和同步边界见 [RenderCore](docs/render-core.md)。

## GPU XPBD 与编译器

GPU XPBD 分为 Model、Compiler、Runtime 和 Adapter。项目定义数学空间、变量、残差、乘子投影与 history；编译器负责 SoA、着色、Jacobi 关联结构及执行映射，运行时通过 RenderCore 提交计算。

默认 Auto 执行模式识别静态封闭组件，按容量打包到工作组，在共享内存内保留子步与迭代循环。大型组件、动态端点模型或超预算程序保留全局路径。`execution:'global'` 可显式选择全局执行；这与 Colored / Jacobi / Hybrid 数值方法独立。

2026-09-09，同一 Release 构建、RTX 3080、ConstraintLab 64 变量 / 255 关系、4 子步 × 12 迭代的分别采样中，每步计算 dispatch 从 400 降为 1，计算阶段区间总和从 2.493 ms 降为 0.840 ms。此数据不代表整帧或所有模型的加速比。现有 XPBD 验证通过，没有为该次优化新增测试。

接口与生命周期见 [GPU XPBD](docs/xpbd.md)，预算、同步语义、测量口径与尚未实现的优化见 [编译器优化说明](docs/xpbd-compiler-optimization.md)。

## 项目实践与记录

- **Afterlight**：Kiln 与 Ash 分别使用 AI4Animation 双足 / 四足预训练网络，通过 ONNX Runtime 生成动画姿态与根运动，并结合物理反馈；双足角色支持 11 种移动风格。实现、模型来源与许可见 [动画文档](docs/animation.md)。Rain Court / Honeybud Court 提供展示场景，Honeybud 的 Blender 源文件、纹理与面数预算见 [美术资产管线](docs/honeybud-art-pipeline.md)。
- **Maple Circuit**：车辆、AI、比赛规则与 HUD 由项目 JS 实现；初版制作没有修改引擎。复刻评估与当时的功能缺口见 [能力核对](docs/maple-circuit-capability-audit.md)，项目自己的运行记录保留在 [项目说明](Projects/MapleCircuit/README.md)。
- **HumanEditor**：基于通用 Content、ECS、RuntimeHost、RenderTarget 与 RmlUi 接口。编辑器挂载目标项目的数据，Play 时才运行目标场景脚本，详见 [项目说明](Projects/HumanEditor/README.md)。
- **EntityID**：检查遮挡、背景 ID、对象隐藏和输出组件移除后的内容保留，详见 [回读示例](Projects/EntityID/README.md)。
- **ConstraintLab**：展示从数学关系动态编译 GPU 求解执行图、运行时更新与异步结果消费；当前使用项目表达式定义的绳索与碰撞作为交互示例，详见 [项目说明](Projects/ConstraintLab/README.md)。

历史性能数字与验证结果按专题保存，不作为 README 中的运行承诺。`captures/` 是本地运行输出，默认不进入版本管理。

### 原 README 中的历史性能记录

以下保留原 README 的测量结论与取舍背景，本次文档整理未重跑这些性能基准：

- **增量场景上传**：曾在变化量保持恒定的实验中将场景放大 19.75 倍，帧时间增加 2.6%。原记录指出小型 Rain Court 的收益不明显，Honeybud 与 Maple Circuit 当时尚未完成与全量上传的逐项对照。
- **一帧在飞**：原记录在 `--present immediate`、1280×800 至 2560×1440 下测得非 GPU 部分约 0.42–0.52 ms，因此当时选择保持一帧在飞，避免复制 TLAS、descriptor set 和上传缓冲引入的时域同步复杂度。此取舍应在新的 CPU / GPU 负载下重新测量。

## README 截图记录

2026-09-09 使用当前 Release 可执行文件逐个启动项目，通过 `--capture` 的 GPU 回读保存运行画面，再将 BMP 无损转换为 PNG，放在 `docs/screenshots/`。原生运行均正常退出。没有修改项目场景、引擎代码或新增测试。

| 图片 | 项目 / 地图 | 采集方式 |
| --- | --- | --- |
| `afterlight-rain-court.png` | Afterlight / RainCourt | 1440×900，120 帧 |
| `afterlight-honeybud-court.png` | Afterlight / HoneybudCourt | 1440×900，120 帧 |
| `maple-circuit.png` | MapleCircuit / DriftPreview | 1440×900，240 帧，项目自带的冻结漂移演示 |
| `human-editor.png` | HumanEditor / 默认工作台 | 1600×1000，120 帧，默认加载 Afterlight / RainCourt |
| `constraint-lab.png` | ConstraintLab / 默认单绳 | 1440×900，180 帧 |
| `entity-id.png` | EntityID / 初始化场景 | 1440×900，90 帧，`t.TimeScale=0` 保留前后遮挡画面 |

截图时通过命令行设置 `r.Hud=1`、`r.Stats=0`、`r.DebugView=0`；除 EntityID 初始化画面外使用 `t.TimeScale=1`。EntityID 也以正常时间推进单独运行，日志出现 `EntityID example PASS`。详细采集日志在本地 `captures/readme-*.log`，该目录不纳入版本管理。

## 专题文档

| 文档 | 内容 |
| --- | --- |
| [架构](docs/architecture.md) | 线程所有权、帧 mailbox、RenderScene 槽位契约、JS 绑定表、3C 扩展 |
| [ECS 架构](docs/ecs.md) | 组件契约、所有权与派生关系、接入／修改／移除／提交、Map v7 |
| [Project / Asset / Scene](docs/projects-assets-scenes.md) | 内容根、虚拟路径、`.asset` 格式、对象身份、Save/Load |
| [Shader / Material](docs/shader-materials.md) | 表面 ABI、source linking、schema 与容量、持久化迁移 |
| [渲染说明](docs/rendering.md) | 每帧数据、G-buffer 格式、DI/GI、NRD、同步与资源生命周期 |
| [RTXDI 集成](docs/rtxdi-integration.md) | SDK 接入范围、梯度反馈与当前近似 |
| [NRD 接入调查](docs/nrd-integration-audit.md) | RELAX 参数选择依据与验证 |
| [Physics Scene](docs/physics-scene.md) | 形状与句柄所有权、扫掠／滑移、BVH 粗筛、JS 接口 |
| [Animation](docs/animation.md) | 骨架/姿态/求解器协议、AI4Animation 接入、导出与上游许可 |
| [UI Core](docs/ui-core.md) | RmlUi Context、UI 快照、`Engine.ui` 接口与后端能力 |
| [CVar 与控制台](docs/console.md) | 变量、命令、配置优先级与重启语义 |
| [GPU 分阶段计时](docs/gpu-profiling.md) / [CPU 分阶段计时](docs/cpu-profiling.md) | 逐阶段耗时抓取与报告格式 |
| [美术资产管线](docs/honeybud-art-pipeline.md) | Honeybud 的 Blender 源文件、纹理与面数预算 |
| [Maple Circuit 能力核对](docs/maple-circuit-capability-audit.md) | 用现有引擎接口复刻赛车视频的结论与缺口 |
| [固定依赖](docs/third-party.md) | `third_party/` 的版本、用途与许可 |
| [运行宿主与视图](docs/runtime-host.md) | 常驻脚本、场景加载与独立视图、Play/Pause/Stop |
| [RenderTarget](docs/render-targets.md) | 离屏输出、整数 ID、异步像素读取 |
| [GPU XPBD](docs/xpbd.md) | 数学接口、模型变化、GPU 状态与结果消费 |
| [XPBD 编译器优化](docs/xpbd-compiler-optimization.md) | 区域分析、共享状态融合、预算与实测 |
| [RenderCore](docs/render-core.md) / [RenderGraph](docs/render-graph.md) | GPU 资源、计算图、访问声明与同步 |
| [变换 TRS](docs/transform-trs.md) | 局部与世界变换、父子层级和缩放 |
| [材质渲染策略](docs/material-render-policy.md) | 材质域、深度层和光追可见性 |
