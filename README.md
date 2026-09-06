# Afterlight — 单角色 RTS 3C / Vulkan RT 引擎基础

这是一个可运行的 Windows x64 原生工程。C++17 引擎，Duktape JavaScript gameplay，Vulkan 1.3 渲染；主线程执行平台事件、60 Hz simulation 和 JS，独立渲染线程持有全部 GPU 资源。

当前关卡是 **The Rain Court**：用 PBR box 搭建庭院、建筑和障碍。主角 Kiln 为双足人，四足狗 Ash 会寻路跟随；两者使用 AI4Animation 的原始蒙皮模型和 ONNX 动画求解器。

UI 由独立 **uiCore / RmlUi** 驱动：项目 JS 通过 `Engine.ui` 操作文档、DOM 和事件，控制台与项目角色面板共用 RmlUi 布局和 Vulkan UI 后端。接口、线程边界、示例及当前后端能力见 [UI Core](docs/ui-core.md)。

## 直接运行

新增 **蜜芽庭院 / Honeybud Court**：双击 `RunHoneybud.cmd` 体验通过 Blender MCP 制作的暖色庭院。包含可复用静态模型、布艺/木纹/PBR 纹理、陶瓷喷泉与原有角色导航；源文件、重建流程和资产管线见 [美术资产说明](docs/honeybud-art-pipeline.md)。

双击根目录 `Run.cmd`。当前机器的可执行文件是 `build/bin/Release/Afterlight.exe`。

按 **~ 或 F10** 打开游戏内控制台：支持 CVar 查询/修改、类型与范围校验、多关键词模糊补全、上下键选择候选、命令历史、配置保存。可试 `r.Exposure 1.4`、`r.Hud 0`、`t.TimeScale 0`、`help`。控制台打开时接管游戏输入，关闭 HUD 后仍可使用。变量、启动覆盖及开发接口见 [CVar 与控制台](docs/console.md)。

输入 `profileGPU` 抓取下一帧的 GPU 阶段耗时，包含 RenderGraph、加速结构、NRD 子阶段和拷贝；结果输出到控制台及 `captures/gpu-profile.json` / `.txt`。启动时也可传 `--profile-gpu`。见 [GPU 分阶段计时](docs/gpu-profiling.md)。

输入 `profileCPU` 抓取 Game / Render 线程的 CPU 阶段耗时，包含独立标注的等待阶段，以及 inclusive / self 读数；`profileCPU last` 查看最近报告，结果保存在 `captures/cpu-profile.json` / `.txt`。启动参数为 `--profile-cpu`。见 [CPU 分阶段计时](docs/cpu-profiling.md)。

标题栏显示实时 **FPS**、**Frame ms**（包含线程与呈现等待的整帧间隔）、**CPU (Render) ms**（场景准备、CPU 蒙皮、HUD、命令录制与提交，不含常规帧同步/呈现等待）、**GPU ms**（Vulkan timestamp），约每半秒更新。启动及重建窗口尺寸后的统计预热显示 `-- FPS`。默认使用 FIFO 垂直同步，因此通常不超过 60 FPS；用 `r.Stats 1` 开启独立性能面板（默认关闭），显示当前呈现模式与 VSync 状态；`r.Hud` 只控制项目游戏界面。渲染帧率不再被 60 Hz 仿真锁死——渲染线程在没有新快照时重新呈现最新快照，用 `--present immediate` 或 `--present mailbox` 即可跑出显示器刷新率以上的真实帧成本。累计帧数只保留在日志和截图报告中。

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

庭院左侧的 **Power console** 会切换青色灯光和建筑门的开合／碰撞；前方的 **Supply cache** 可激活。悬停高亮、角色轮廓、目标环和小地图路径提供指令反馈。

选中 Kiln 后，左侧 **ANIMATION** 面板的左右箭头可切换 11 种移动风格。先点击地面让角色行走，再切换比较；停下会保留选择。风格由动画资产声明，作为实例属性保存，与每帧速度／动作输入分离。

## 从源码构建

需要 Windows x64、Visual Studio 2019 C++ desktop workload / Windows SDK、Node.js 22+。本机已完成 Release 构建。支持 Vulkan 1.3、ray query 和 acceleration structure 的 GPU 是运行要求；没有将光追自动替换成传统光照。

```powershell
node tools/bootstrap.mjs
powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Test
.\Run.cmd
```

依赖全部位于 `third_party/`，不需要系统安装 Vulkan SDK。bootstrap 使用固定版本和 `tools/dependencies.lock.json` 中的 SHA-256 校验下载。CMake、GLSL 编译器、DXC 也随工程依赖下载。

```powershell
# 原生窗口自动运行、GPU readback 截图、正常退出
.\build\bin\Release\Afterlight.exe --frames 90 --capture

# 点击移动、镜头、resize、最小化恢复、取消命令、运行中重载 Map
.\build\bin\Release\Afterlight.exe --smoke --width 960 --height 600

# 1280×800 默认；也可指定尺寸、调试视图和无 HUD 截图
.\build\bin\Release\Afterlight.exe --view 5 --no-hud --frames 60 --capture

# 解除 vsync 量化，测量真实的每帧成本
.\build\bin\Release\Afterlight.exe --present immediate --frames 360 --capture

# 放大场景但保持每 tick 变化量恒定；加 --full-upload 退回逐帧全量重写以作对照
.\build\bin\Release\Afterlight.exe --present immediate --frames 900 --capture --stress 900
```

截图写入 `captures/frame.bmp`；GPU 测量和 validation 状态写入 `captures/render-report.json`。`--capture` 未指定帧数时默认 90 帧。报告中的 `sceneSlotWrites` 与 `sceneSlotWritesIfRebuilt` 是同一场景下增量上传与全量重建的实际写入次数。

Release 构建默认**关闭** validation（每帧约 1 ms CPU，足以把贴着预算的帧推过一次 vblank），Debug 构建默认开启。用 `--validation` 显式开启；`--smoke` 作为正确性关卡始终开启。`--present fifo|mailbox|immediate` 选择呈现模式，默认 `fifo`。

## 目录约定

```text
engine/
  assets/                 Project、虚拟路径、资产注册表、统一 .asset 和类型加载
  scene/                  Map 描述、Scene Save/Load 与持久 Object 身份
  core/                   世界、实体、材质、相机、常驻 RenderScene、不可变帧快照和 mailbox
  platform/               Win32 窗口、输入、焦点／尺寸事件
  navigation/             膨胀障碍栅格 A*、路径平滑、连续碰撞移动
  animation/              统一骨架/姿态/求解器框架、AI4Animation 原生运行时和 ONNX 推理
  scripting/              Duktape runtime 和 C++ ↔ JS binding
  uiCore/                 RmlUi Context、输入、文档与不可变 UI 绘制快照
  ui/                     RML 引擎调试工具：控制台和可选性能统计
  Content/UI/             引擎 RML / RCSS
  render/                 Vulkan、几何、BLAS/TLAS、pass graph、NRD、HUD
    shaders/              G-buffer / DI / GI / reuse / resolve / composite
Projects/Afterlight/       可整体搬迁的独立项目
  .project                项目 ID、默认 Map、公共脚本加载顺序
  Content/                /Game 虚拟路径根
    Maps/                 RainCourt.asset：对象、材质、灯、相机、导航和玩法状态
    animations/           控制器、配置、ONNX 纯头资产与独立模型
    models/               内嵌蒙皮网格和模型描述
    materials/            PBR 材质配置资产
    physics/              碰撞配置资产
    scripts/              3C、通用玩法、Map 初始化脚本，均为内嵌 Script 资产
    UI/                   项目 HUD 的 RML / RCSS：角色、小地图、动画选择器和提示，由项目 JS 驱动
tests/                    原生核心、独立物理场景与真实 JS 接口测试
tools/                    固定依赖下载、构建、验证脚本
docs/                     架构、渲染约定、第三方来源、验证记录
third_party/              可重新获取的外部依赖，不放自有 gameplay
external/AI4AnimationPy/   原样保存的上游源码、训练/数据工具及原始模型
build/                    CMake 产物与 SPIR-V，不放源资源
captures/                 实际 GPU 截图和运行报告
```

## 已实现的渲染路径

```text
程序生成 box/capsule + 导入 StaticMesh → 共享静态 BLAS
动画骨架快照 → CPU 蒙皮 → 动态 BLAS refit
每帧实体姿态 → TLAS update
G-buffer 光栅化
  ├─ albedo/metallic、normal/roughness、world position/entity ID
  └─ current→previous 屏幕运动向量、线性 viewZ、emission
DI reservoir 初始采样 + BRDF 二次光线生成 GI samples + GGX specular ray
时域重投影 / 空间重采样 / GI Jacobian 与二次表面重新验证
最终光追可见性与 diffuse/specular radiance resolve
NRD RELAX_DIFFUSE_SPECULAR（原生 Vulkan dispatch）
材质重调制 → tone map → 选择反馈 / HUD → swapchain
```

DI 是自写的 ReSTIR DI 风格实现：8 个初始灯样本、时域历史和 4 个空间邻居，最终阴影使用硬件 ray query。GI 从 G-buffer 主表面的 BRDF secondary rays 出发，保存二次命中的位置、法线、实体 ID 和 radiance，再做时空重采样和重连接。没有 shadow map、SSAO、SSGI、lightmap 或 probe GI 替代这条路径。

NRD 4.17.3 实际参与 GPU 计算，使用 RELAX 的原生 SPIR-V、资源池、每 pass 常量、sampled/storage bindings 和两个 sampler；不是仅有接口占位。

## 当前实现边界

这是可继续开发的第一版基础，不是大型游戏的最终渲染器。DI/GI 的算法结构参考 ReSTIR，自写实现没有接入 NVIDIA RTXDI SDK，也不声称达到 MegaLights 的算法、规模或质量。当前 GI 是 **一次二次表面命中上的漫反射重采样**；镜面间接光使用独立 GGX VNDF 路径和 NRD，尚无 ReSTIR PT、多跳路径重采样或无偏 MIS。历史长度与 GI Jacobian 有限幅，属于有偏实时方案。

灯光是具有球形位置扰动的解析灯，用于柔和光追阴影；发光材质支持二次光线命中，但还没有 emissive mesh light importance sampling。基础 mesh 共享静态 BLAS，蒙皮角色各自持有动态 BLAS 并随姿态 refit；TLAS 在只有变换变化时走 `UPDATE`，在拓扑变化（槽位增长或改绑几何体）时重建。当前采用一帧 GPU in flight 和保守 pass barrier，还没有 async compute、自动资源别名、流式场景或 GPU-driven indirect draw。

渲染场景是常驻的：`RenderScene` 给每个物体一个稳定槽位，`World` 的改动以增删改事件发布，渲染器只上传脏槽位，变换变化还有一条只写实例前 128 字节的快速路径。因此每帧代价随**变化量**而不是场景规模增长——场景放大 19.75 倍时帧时间增加 2.6%，实测见[验证记录](docs/verification.md)。注意这在当前 48 个物体的关卡上不会让帧变快，收益体现在规模上。

一帧在飞是**实测后的选择**而非遗留：在 `--present immediate` 下，整帧时间中的非 GPU 部分为 0.42–0.52 ms（1280×800 至 2560×1440），即 CPU 录制与 GPU 执行重叠最多只能省下这个量。而多帧在飞需要把 TLAS、descriptor set 和全部 host-visible 上传缓冲按帧复制——在带时域历史与 reservoir 复用的 ReSTIR 管线里，这个同步风险显著大于 0.5 ms 的收益。等 CPU 侧成本重新变得显著时再做。

3C 包含加速、刹车、原地转向、行走、跑步、蹲行速度与物理净空、交互。独立 Physics Scene 存储 box/capsule、查询层与生命周期；角色移动、root motion、蹲起净空和 0.25 m 庭院导航共用该场景的查询。动画关节附属碰撞体支持原生及 JS 更新。当前为查询／运动学后端，未包含受力积分、刚体堆叠和 ragdoll；楼梯、跳跃／翻越、坡面、多层导航、动画图混合和多角色控制仍需扩展。当前双足模型尚未接入蹲姿动画。

Animation 底层支持可替换 Solver、骨骼 FK、root motion/物理反馈、接触和 FABRIK。双足/四足 AI4Animation CxM 通过 ONNX Runtime 原生推理接入，与未来 clip graph、motion matching 处于同一层级。默认关卡已使用双足人和跟随狗，蒙皮网格同时参与光栅与光追。详细入口、模型导出、验证和上游非商业许可见 [Animation](docs/animation.md)。

项目路径、资产格式、对象身份、保存与加载入口见 [Project / Asset / Scene](docs/projects-assets-scenes.md)。支持 `--project <目录或.project>` 与 `--map /Game/Maps/RainCourt`；默认运行 Afterlight 项目的默认 Map。

更多实现约定见 [架构](docs/architecture.md)、[Physics Scene](docs/physics-scene.md)、[渲染说明](docs/rendering.md)、[验证记录](docs/verification.md)。

排查闪烁时可运行 `Afterlight.exe --audit NAME --view 1 --capture`：固定场景，预热 64 帧后统计连续帧；加 `--audit-motion` 使用固定的镜头旋转轨迹。结果在 `captures/NAME/`。使用控制台 `r.DebugView 0..7` 设置诊断视图，1 为 ALBEDO；DIRECT RT (RAW) / INDIRECT RT (RAW) 是降噪前的光照信号。
