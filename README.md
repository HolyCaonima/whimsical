# Afterlight — C++ / JavaScript / Vulkan 光追引擎

这是一个可运行的 Windows x64 原生工程。C++17 引擎，Duktape JavaScript gameplay，Vulkan 1.3 ray query 渲染；主线程执行平台事件、60 Hz simulation 和 JS，独立渲染线程持有全部 GPU 资源。

引擎不为某一种玩法定制：对象与场景是 ECS，能力按需组合；表面外观是资产化的 Shader → Material；内容按 Project 独立组织，通过 `/Game` 虚拟路径访问。仓库内有两个项目、三张可直接运行的地图。

## 三个可直接运行的场景

| 入口 | 场景 | 内容 |
| --- | --- | --- |
| `Run.cmd` | **Rain Court**（默认） | 48 个对象、10 个材质、8 盏灯的 PBR box 庭院。双足 Kiln 与四足 Ash 使用 AI4Animation 的原始蒙皮模型和 ONNX 动画求解器，狗会寻路跟随 |
| `RunHoneybud.cmd` | **Honeybud Court** | 667 个对象的暖色庭院，经 Blender MCP 制作：21 类静态资产、8 组 PBR 纹理、陶瓷喷泉，沿用同一套角色、导航与相机 |
| `RunMapleCircuit.cmd` | **Maple Circuit** | 独立赛车项目：423 个对象、458 m 闭环赛道、四车三圈比赛，车辆、AI、比赛规则、漂移特效和 HUD 全部由项目 JS 实现 |

Maple Circuit 是对引擎边界的一次实测：它没有改动 `engine/`、构建配置或另一个项目，只使用 Project 资产、Map、`Engine.*` 绑定和 RmlUi。运行方式、操作和验证记录见 [Maple Circuit](Projects/MapleCircuit/README.md)，复刻过程中发现的缺口（3D 透明混合、无音频系统）见 [能力核对](docs/maple-circuit-capability-audit.md)。Honeybud 的源文件、面数预算和重建流程见 [美术资产管线](docs/honeybud-art-pipeline.md)。

## 直接运行

双击根目录对应的 `.cmd`。当前机器的可执行文件是 `build/bin/Release/Afterlight.exe`，也可以直接传参：

```powershell
.\build\bin\Release\Afterlight.exe --map /Game/Maps/HoneybudCourt
.\build\bin\Release\Afterlight.exe --project Projects/MapleCircuit --width 1280 --height 800
```

按 **~ 或 F10** 打开游戏内控制台：支持 CVar 查询/修改、类型与范围校验、多关键词模糊补全、上下键选择候选、命令历史、配置保存。可试 `r.Exposure 1.4`、`r.Hud 0`、`r.DebugView 1`、`t.TimeScale 0`、`help`。控制台打开时接管游戏输入，关闭 HUD 后仍可使用。变量、启动覆盖及开发接口见 [CVar 与控制台](docs/console.md)。

输入 `profileGPU` 抓取下一帧的 GPU 阶段耗时，包含 RenderGraph、加速结构、NRD 子阶段和拷贝；结果输出到控制台及 `captures/gpu-profile.json` / `.txt`。启动时也可传 `--profile-gpu`。见 [GPU 分阶段计时](docs/gpu-profiling.md)。

输入 `profileCPU` 抓取 Game / Render 线程的 CPU 阶段耗时，包含独立标注的等待阶段，以及 inclusive / self 读数；`profileCPU last` 查看最近报告，结果保存在 `captures/cpu-profile.json` / `.txt`。启动参数为 `--profile-cpu`。见 [CPU 分阶段计时](docs/cpu-profiling.md)。

标题栏显示实时 **FPS**、**Frame ms**（包含线程与呈现等待的整帧间隔）、**CPU (Render) ms**（场景准备、CPU 蒙皮、HUD、命令录制与提交，不含常规帧同步/呈现等待）、**GPU ms**（Vulkan timestamp），约每半秒更新。启动及重建窗口尺寸后的统计预热显示 `-- FPS`。默认使用 FIFO 垂直同步，因此通常不超过 60 FPS；用 `r.Stats 1` 开启独立性能面板（默认关闭），显示当前呈现模式与 VSync 状态；`r.Hud` 只控制项目游戏界面。渲染帧率不被 60 Hz 仿真锁死——渲染线程在没有新快照时重新呈现最新快照，用 `--present immediate` 或 `--present mailbox` 即可跑出显示器刷新率以上的真实帧成本。

### Afterlight 操作（Rain Court / Honeybud Court）

| 操作 | 行为 |
| --- | --- |
| 左键点击角色 | 选择角色；初始默认选中 Kiln |
| 左键或右键点击地面 | 寻路移动；新命令替换旧命令 |
| 点击交互物体 | 寻路到接近点、转身面对目标、执行交互 |
| WASD | 相机方向相对的直接移动，取消路径命令 |
| Shift / Ctrl | 跑步 / 蹲行 |
| E | 接近并使用附近交互物体 |
| Esc | 停止移动／取消交互命令 |
| Tab | 切换选中状态 |
| 中键拖动 | 旋转镜头、调整俯仰 |
| 滚轮 | 平滑缩放 |
| Q / R | 旋转镜头 |
| 方向键；Alt + 屏幕边缘 | 平移镜头，脱离自动跟随 |
| F 或 Space | 恢复平滑跟随 |
| F2 | 显示／隐藏 Physics Scene 碰撞体线框；也可用 `--physics-debug` 启动 |
| ~ / F10 | 打开／关闭控制台；Esc 关闭，上下键选择候选（空输入或无候选时浏览历史），Enter 执行；也支持 Tab 补全 |
| Alt+F4 或窗口关闭 | 正常结束两条线程并释放资源 |

Rain Court 左侧的 **Power console** 会切换青色灯光和建筑门的开合／碰撞；前方的 **Supply cache** 可激活。悬停高亮、角色轮廓、目标环和小地图路径提供指令反馈。

选中 Kiln 后，左侧 **ANIMATION** 面板的左右箭头可切换 11 种移动风格。先点击地面让角色行走，再切换比较；停下会保留选择。风格由动画资产声明，作为实例属性保存，与每帧速度／动作输入分离。

Maple Circuit 使用 W/S 加速刹车、A/D 转向、Space 漂移、R 回正、V 交给 AI，完整列表见[项目说明](Projects/MapleCircuit/README.md)。

## 从源码构建

需要 Windows x64、Visual Studio 2019 C++ desktop workload / Windows SDK、Node.js 22+。本机已完成 Release 构建。支持 Vulkan 1.3、ray query 和 acceleration structure 的 GPU 是运行要求；没有将光追自动替换成传统光照。

```powershell
node tools/bootstrap.mjs
powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Test
.\Run.cmd
```

依赖全部位于 `third_party/`，不需要系统安装 Vulkan SDK。bootstrap 使用固定版本和 `tools/dependencies.lock.json` 中的 SHA-256 校验下载。CMake、GLSL 编译器、DXC 也随工程依赖下载，来源见 [固定依赖](docs/third-party.md)。`-Test` 运行 15 组 CTest：ECS 生命周期、资产／场景、玩法、物理、动画、Shader 编译、UI、控制台、性能计时与渲染审计。

表面 Shader 由随附 glslang 在运行时按 pass 编译，因此当前开发运行时需要源码树在位；离线 cook 与持久二进制缓存是后续工作。

```powershell
# 原生窗口自动运行、GPU readback 截图、正常退出
.\build\bin\Release\Afterlight.exe --frames 90 --capture

# 点击移动、镜头、resize、最小化恢复、取消命令、运行中重载 Map
.\build\bin\Release\Afterlight.exe --smoke --width 960 --height 600

# 按呈现帧驱动的 ECS 生命周期回归：启停、蒙皮解绑重绑、几何增删改
.\build\bin\Release\Afterlight.exe --ecs-smoke --width 960 --height 600

# 1280×800 默认；也可指定尺寸、调试视图和无 HUD 截图
.\build\bin\Release\Afterlight.exe --view 5 --no-hud --frames 60 --capture

# 解除 vsync 量化，测量真实的每帧成本；启动即覆盖 CVar
.\build\bin\Release\Afterlight.exe --present immediate --frames 360 --capture --cvar "r.Exposure=1.4"

# 放大场景但保持每 tick 变化量恒定；加 --full-upload 退回逐帧全量重写以作对照
.\build\bin\Release\Afterlight.exe --present immediate --frames 900 --capture --stress 900
```

截图写入 `captures/frame.bmp`；GPU 测量和 validation 状态写入 `captures/render-report.json`。`--capture` 未指定帧数时默认 90 帧。报告中的 `sceneSlotWrites` 与 `sceneSlotWritesIfRebuilt` 是同一场景下增量上传与全量重建的实际写入次数。

Release 构建默认**关闭** validation（每帧约 1 ms CPU，足以把贴着预算的帧推过一次 vblank），Debug 构建默认开启。用 `--validation` 显式开启；`--smoke`、`--ecs-smoke`、`--console-smoke` 作为正确性关卡始终开启。`--present fifo|mailbox|immediate` 选择呈现模式，默认 `fifo`。完整参数见 `--help`。

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
  render/                 Vulkan、几何、BLAS/TLAS、pass graph、NRD、运行时 Shader 编译与材质绑定
    shaders/              surface 契约、G-buffer / DI / GI / reuse / resolve / composite
Projects/                 可整体搬迁的独立项目，互不引用
  Afterlight/
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
tests/                    原生核心、ECS、物理、Shader 与真实 JS 接口测试
tools/                    固定依赖下载、构建、迁移与 GPU 诊断脚本
docs/                     架构、渲染约定、第三方来源、验证记录
third_party/              可重新获取的外部依赖，不放自有 gameplay
external/AI4AnimationPy/  原样保存的上游源码、训练/数据工具及原始模型
build/                    CMake 产物与 SPIR-V，不放源资源
captures/                 实际 GPU 截图和运行报告
```

## 引擎结构

**ECS 与组件契约。** 实体只提供身份，能力由组件组合。`ComponentCatalog` 是进程内唯一的组件契约表：一处注册即贯通原生 API、JS 脚本、Map 读写和依赖校验，新增普通数据组件不需要改 World、场景结构或脚本分支。系统只拥有自己的派生状态——Transform 的 world 是空间事实，物理体 pose、关节附件、render proxy 都是镜像；`Changes` 在提交边界合并失效通知并驱动后端同步。组件组接入按依赖拓扑准备资源，失败逆序回滚。父子变换保持刚体平移与旋转，Maple Circuit 的车轮只提交局部旋转。Map 写 `version: 5`，保存实际存在的组件描述与父级持久 ID。详见 [ECS 架构](docs/ecs.md)。

**Shader → Material。** `ShaderAsset` 拥有 GLSL 表面函数体、有序属性／纹理 schema、material model 和 render state；`MaterialAsset` 拥有解析后的取值和纹理引用。`engine/render/shaders/surface.glsl` 的 `EvaluateSurface` 是公共 ABI，同一份表面代码同时供 G-buffer 光栅化和六个 ray query pass 使用，通过 source linking 生成 dispatch 表，不需要 SBT 或 callable shader。当前只支持 `opaque` 与 `masked`，透明混合在资产加载时明确拒绝。详见 [Shader / Material](docs/shader-materials.md)。

**Project / Asset / Scene。** `Project` 是独立内容根，`AssetManager` 是唯一注册与加载入口，所有资产使用统一的 `ALAS1` 头 + 载荷格式，载荷可内嵌或外置。资产 ID 是权威身份，移动资产后旧引用仍按 ID 解析。对象路径为 `/Game/Maps/RainCourt:<ObjectID>`；Entity ID、render slot 和物理句柄是运行时句柄，不入盘。支持 `--project <目录或.project>` 与 `--map /Game/Maps/RainCourt`。详见 [Project / Asset / Scene](docs/projects-assets-scenes.md)。

**物理与动画。** 独立主线程 `PhysicsScene` 存储 box/capsule 形状、查询层、动态 AABB Tree 与生命周期；角色移动、root motion、蹲起净空和庭院导航共用同一份查询，Maple Circuit 的车辆位移与撞击法线也来自它。动画层提供可替换 Solver、骨骼 FK、root motion／物理反馈、接触和 FABRIK；根运动消费策略是独立组件，可选直接变换、任意形状 kinematic 或直立胶囊 grounded。见 [Physics Scene](docs/physics-scene.md) 与 [Animation](docs/animation.md)。

**UI。** UI 由独立 **uiCore / RmlUi** 驱动：项目 JS 通过 `Engine.ui` 操作文档、DOM 和事件，控制台与项目面板共用 RmlUi 布局和 Vulkan UI 后端，绘制列表作为不可变快照跨线程传递。见 [UI Core](docs/ui-core.md)。

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

## 当前实现边界

这是可继续开发的第一版基础，不是大型游戏的最终渲染器。DI 使用 NVIDIA RTXDI SDK，GI 保留自写实现；集成范围和当前近似见 [RTXDI 集成](docs/rtxdi-integration.md)。当前 GI 是 **一次二次表面命中上的漫反射重采样**；镜面间接光使用独立 GGX VNDF 路径和 NRD，尚无 ReSTIR PT、多跳路径重采样或 GI 的完整 MIS。历史长度与 GI Jacobian 有限幅，属于有偏实时方案。

灯光是具有球形位置扰动的解析灯，用于柔和光追阴影；发光材质支持二次光线命中，但还没有 emissive mesh light importance sampling。基础 mesh 共享静态 BLAS，蒙皮角色各自持有动态 BLAS 并随姿态 refit；TLAS 在只有变换变化时走 `UPDATE`，在拓扑变化（槽位增长或改绑几何体）时重建。当前采用一帧 GPU in flight 和保守 pass barrier，还没有 async compute、自动资源别名、流式场景或 GPU-driven indirect draw。资源上限为 1024 个 instance、256 个 material、256 盏灯和 64 张纹理，超限明确报错——Honeybud Court 的 667 个对象已经占掉实例上限的六成。

**没有 3D 透明混合**：表面只有 opaque 和 masked，Maple Circuit 的漂移烟团因此使用不透明网格加缩放消散。透明材质模式、深度策略、排序或 OIT 与合成阶段是引擎侧待补的契约，不应由项目脚本绕过。引擎也**没有音频系统**。

渲染场景是常驻的：`RenderScene` 给每个物体一个稳定槽位，ECS 系统的改动以增删改事件发布，渲染器只上传脏槽位，变换变化还有一条只写实例前 128 字节的快速路径。因此每帧代价随**变化量**而不是场景规模增长——场景放大 19.75 倍时帧时间增加 2.6%，实测见[验证记录](docs/verification.md)。这条路径在 48 个物体的 Rain Court 上不会让帧变快，收益体现在规模上；Honeybud Court 与 Maple Circuit 是目前实际跑在它上面的百级／千级对象场景，但两者尚未做与全量上传的逐项对照测量。

一帧在飞是**实测后的选择**而非遗留：在 `--present immediate` 下，整帧时间中的非 GPU 部分为 0.42–0.52 ms（1280×800 至 2560×1440），即 CPU 录制与 GPU 执行重叠最多只能省下这个量。而多帧在飞需要把 TLAS、descriptor set 和全部 host-visible 上传缓冲按帧复制——在带时域历史与 reservoir 复用的 ReSTIR 管线里，这个同步风险显著大于 0.5 ms 的收益。等 CPU 侧成本重新变得显著时再做。

3C 包含加速、刹车、原地转向、行走、跑步、蹲行速度与物理净空、交互。物理层当前为查询／运动学后端，未包含受力积分、刚体堆叠和 ragdoll；楼梯、跳跃／翻越、坡面、多层导航、动画图混合和多角色控制仍需扩展。当前双足模型尚未接入蹲姿动画。Maple Circuit 的车辆是平面街机模型，没有悬挂或动力学。

ECS 侧当前是单 World、单已加载 Map，尚未引入 streaming、同一 Map 多实例或跨 Map 活实体解析。Map v3/v4 只在读取边界迁移，v1/v2 明确拒绝。

排查闪烁时可运行 `Afterlight.exe --audit NAME --view 1 --capture`：固定场景，预热 64 帧后统计连续帧；加 `--audit-motion` 使用固定的镜头旋转轨迹。结果在 `captures/NAME/`。使用控制台 `r.DebugView 0..7` 设置诊断视图，1 为 ALBEDO；DIRECT RT (RAW) / INDIRECT RT (RAW) 是降噪前的光照信号。线程边界与开销见 [Render audit](docs/render-audit.md)。

## 文档

| 文档 | 内容 |
| --- | --- |
| [架构](docs/architecture.md) | 线程所有权、帧 mailbox、RenderScene 槽位契约、JS 绑定表、3C 扩展 |
| [ECS 架构](docs/ecs.md) | 组件契约、所有权与派生关系、接入／移除／提交、Map v5 |
| [Project / Asset / Scene](docs/projects-assets-scenes.md) | 内容根、虚拟路径、`.asset` 格式、对象身份、Save/Load |
| [Shader / Material](docs/shader-materials.md) | 表面 ABI、source linking、schema 与容量、持久化迁移 |
| [渲染说明](docs/rendering.md) | 每帧数据、G-buffer 格式、DI/GI、NRD、同步与资源生命周期 |
| [RTXDI 集成](docs/rtxdi-integration.md) | SDK 接入范围、梯度反馈与当前近似 |
| [NRD 接入调查](docs/nrd-integration-audit.md) | RELAX 参数选择依据与验证 |
| [Render audit](docs/render-audit.md) | 连续帧读回诊断的线程边界与实测开销 |
| [Physics Scene](docs/physics-scene.md) | 形状与句柄所有权、扫掠／滑移、BVH 粗筛、JS 接口 |
| [Animation](docs/animation.md) | 骨架/姿态/求解器协议、AI4Animation 接入、导出与上游许可 |
| [UI Core](docs/ui-core.md) | RmlUi Context、UI 快照、`Engine.ui` 接口与后端能力 |
| [CVar 与控制台](docs/console.md) | 变量、命令、配置优先级与重启语义 |
| [GPU 分阶段计时](docs/gpu-profiling.md) / [CPU 分阶段计时](docs/cpu-profiling.md) | 逐阶段耗时抓取与报告格式 |
| [美术资产管线](docs/honeybud-art-pipeline.md) | Honeybud 的 Blender 源文件、纹理与面数预算 |
| [Maple Circuit 能力核对](docs/maple-circuit-capability-audit.md) | 用现有引擎接口复刻赛车视频的结论与缺口 |
| [验证记录](docs/verification.md) | 构建、CTest、GPU smoke、性能与确定性的实测数据 |
| [固定依赖](docs/third-party.md) | `third_party/` 的版本、用途与许可 |
