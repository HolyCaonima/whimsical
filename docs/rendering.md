# RT 渲染接入约定

Shader → Material 的资产、共享表面契约、source linking 与容量约定见 [Shader / Material](shader-materials.md)。

## 每帧数据

`Globals` 和 GPU struct 有静态尺寸检查，GLSL 使用相同 std140/std430 布局。几何和材质只经过同一份数据源，raster 与 ray query 不维护两套不同场景。每个 TLAS instance 的 custom index 指向 GPU instance；instance 中保存绝对 index-buffer 起始偏移、material index、entity ID 和上次渲染的 model matrix。

`GpuVertex` 包含位置、法线、线性顶点颜色、上一次实际渲染的位置、UV 和切线，共 96 字节。静态网格保留局部空间顶点，重复实例共享静态 BLAS；蒙皮资源按骨骼名字绑定，World 发布不可变 palette，渲染线程执行 CPU 蒙皮并上传世界空间顶点，对应 instance 的 model 为单位矩阵。G-buffer 与 ray query 使用同一份几何、顶点色和 PBR 纹理。重复呈现同一仿真快照时，上一位置收敛到当前位置，避免重复报告形变运动。静态网格与纹理路径见 [Honeybud 美术管线](honeybud-art-pipeline.md)。

| G-buffer | Vulkan 格式 | 含义 |
| --- | --- | --- |
| albedo | RGBA16F | RGB 线性 base color，A metallic |
| normal | RGBA16F | signed best-fit world normal，A 线性 roughness，NRD encoding 4 |
| position | RGBA32F | XYZ world position，A 稳定 entity ID；0 为背景 |
| motion | RGBA16F | XY = previousUV − currentUV；Z = previous clip W，仅供自有历史验证 |
| viewZ | R32F | 正线性 view depth；背景 10000，大于 NRD denoising range |
| emission | RGBA16F | 发光材质 radiance |
| depth | D32F | 光栅深度测试，clear=1 |

NRD 只消费 motion XY，`motionVectorScale.z=0`。GLM 用于 Vulkan raster 的 projection 做了 Y 翻转；提交给 NRD 的 projection 撤销这个翻转，让 NRD 内部的纹理 UV 重建和当前 G-buffer 对齐。矩阵全部为未抖动矩阵。当前没有 TAA jitter。

## DI / GI

DI 直接编译固定版本的 NVIDIA RTXDI-Library（`f12037fa8e97ebc08e9e3edfd2de528ed1772a4b`），通过 `rtxdi_bridge.glsl` 的 RAB 接口接入当前 G-buffer、灯表、材质和 TLAS。Reservoir streaming、packing、时域／空间复用及 ray-traced MIS-like bias correction 使用 SDK 源码。初始 8 个候选来自 CPU 构建的 90% power / 10% uniform 离散灯分布；UV 量化后求 target，以五类物理光源共享采样器计算 Le/pdfOmega，见 [物理光源](physical-lights.md)。初始可见性剔除保留 M，最终选中灯每帧重新查询当前 TLAS，不缓存旧阴影。

时域启用 permutation sampling、最多 20 帧历史、深度／法线匹配；空间阶段读取完整的 temporal 输出，使用 4 个邻居，短历史区域增至 8 个。时域与空间是独立 dispatch，避免邻居读写竞争。灯槽在实体身份数组不变时保持索引对应，上一帧灯参数单独保留，参数动画不会全屏 reset；灯实体增删、启停和同数量替换会重启历史。

DI buffer 使用 SDK 的 16×16 block-linear 地址与 24 字节 packed reservoir，共四层：previous final、initial/temporal/replay、spatial/final、previous replay/initial probe。梯度 pass 在 initial sampling 覆写前读取 previous replay。四层共 96 字节／对齐像素，与旧 DI 三个 32 字节 buffer 相同（尺寸向 16 对齐会产生少量 padding）。shader 按角色寻址、`diLayer()` 每帧把角色映射到轮转后的物理层，因此帧末不需要复制 final 和 replay。

具体集成范围、梯度与验证见 [RTXDI 集成](rtxdi-integration.md)。
GI initial sample 从当前 G-buffer 表面发射余弦半球 BRDF 光线。命中时保存二次位置、法线、实体 ID、该点的一次 next-event lighting 和 PDF；未命中时保存环境方向。GI temporal/spatial 重连接包含固体角 Jacobian，并追踪验证候选二次表面仍然存在、法线相容和可见。不可见候选以零权重计入 represented sample count，避免简单丢弃造成额外亮度偏差。

GI 仍使用原有的受限历史与 Jacobian 支持域，相关样本与有限邻居存在偏差；本次仅替换 DI，不把它称为 SDK ReSTIR GI。环境由实际 secondary ray miss 求值，不使用屏幕空间遮蔽或环境探针。

镜面间接光为独立的 GGX VNDF ray。二次表面的 shading 为 diffuse + GGX NEE 加 emission；这是一次 GI bounce 的基础，不等同多跳 glossy path tracing。

## NRD

固定 NRD 4.17.3，`RELAX_DIFFUSE_SPECULAR`。`NrdDenoiser` 通过 `CreateInstance/GetInstanceDesc` 建立 Vulkan pipeline、permanent/transient textures、sampler、descriptor layout 和常量区。每帧 `SetCommonSettings/GetComputeDispatches` 获取真实 dispatch 列表，逐个绑定资源并执行。当前选择对应 15 条原生 Vulkan pipelines；完整 NRD 构建产生 159 个 shader variants。

DI + GI 合并后交给同一个 `RELAX_DIFFUSE_SPECULAR`，不分别运行两套 NRD。RELAX 输入直接是 RGB 光照信号 + 世界单位 hit distance，不用 REBLUR 的归一化 hit-distance 编码。Diffuse 在 denoise 前不乘 base color / (1-metallic)，合成时再乘。Specular 采用 RTXDI 的 F0 解调基线：resolve 除以 `max(mix(0.04, albedo, metallic), 0.01)`，合成和 RAW 视图乘回同一个 `specularMaterialFactor`；Direct/Indirect RAW 调试信号保留原物理光照值。

Diffuse hit distance 保留当前像素 initial cosine ray 的一跳距离，由 `lighting.comp` 暂存在 `rawDiffuse.a`，resolve 保留它而不使用重采样后偏向高亮样本的 GI reservoir 距离。Specular 沿用独立 GGX ray 的一跳距离，不替换成选中灯的距离。该契约无需新增 ray、纹理或 pass。

RELAX 默认大半径预滤波会跨越同一平面上的投射阴影，其后 A-trous 无法恢复已经损失的边缘。参考 RTX Remix/RTXGI，当前 diffuse/specular prepass 半径为 0/20，`PhiLuminance` 为 0.5/0.35，diffuse fast history 为 4；保留 24 帧主历史、5 次 A-trous 和 anti-firefly。当前分别采样两个 lobe，没有 probabilistic lobe split，因此不需要开启 diffuse prepass 或 hit-distance reconstruction 来填补抽样空洞。这是一套针对合并信号的配置，不直接套用 Remix 的两套 DI/GI 配置。调查依据和验证见 [NRD 接入调查](nrd-integration-audit.md)。

Resize、首帧、大相机跳变、灯数量／材质变化会清理历史。NRD 帧号按实际 GPU render 递增；timeDelta 使用实际渲染帧间隔，GPU timestamp 只用来报告 pass 时间。

## 资源依赖驱动的 RenderGraph

渲染系统由三层组成，每层只回答一个问题：

| 模块 | 回答的问题 | 是否知道 Vulkan |
| --- | --- | --- |
| `graph/Registry` | 这个资源**是什么**（格式、尺寸规则、所有权、shader 视图） | 否 |
| `graph/ResourcePool` | 它这一帧**落在哪块显存和哪个 descriptor 上** | 是 |
| `graph/RenderGraph` | pass 之间**有什么依赖**，因此需要什么 barrier、什么可以裁掉 | 是 |
| `RenderResources` | 本管线声明了哪些资源 | 否 |
| `RenderPipeline` | 本帧由哪些 pass 组成、各自读写什么 | 是 |

一个 pass 只声明意图，不写 barrier：

```cpp
graph.add("RTXDI Spatial Resampling")
    .read(r_.gbuffer.surface(), Access::ComputeRead)
    .read(r_.di.neighbours, Access::ComputeRead)
    .readWrite(r_.di.reservoirs, Access::ComputeReadWrite)
    .dispatch(compute_[DiSpatial]);
```

`Access` 是唯一的意图词汇表，`accessInfo()` 是把意图翻译成 stage/access/layout 三元组的唯一位置。因此**接入一个新功能不需要改中央资源表，也不需要手写 barrier**：在 `RenderResources` 里 `declare` 资源，在 `RenderPipeline` 里声明读写，binding 号、descriptor layout、GLSL 声明、显存分配和同步全部由此推导。

### 所有权

`Lifetime` 是资源契约里唯一说明"谁拥有、内容活多久"的地方，图编译完全按它决策：

| Lifetime | 含义 | 图的行为 |
| --- | --- | --- |
| `Transient` | 只在首次写入到最后一次读取之间有意义 | 可被裁剪；生命期不重叠时共享显存 |
| `Persistent` | 图拥有，内容原地带入下一帧 | 不裁剪、不共享、跨帧排序 |
| `History` | 图拥有的一对，`previous(id)` 命名上一帧写的那半 | 每帧翻转，两套 descriptor set 各绑一种奇偶 |
| `Imported` | 外部分配、图负责同步（swapchain、TLAS） | 只同步不分配 |
| `External` | 外部分配且外部同步（顶点、实例、灯表） | 只绑定，不参与 barrier |

`History` 是替代"帧末复制"的机制。`ResourcePool` 为它分配两个物理槽位，`flip()` 翻转奇偶，`previous` 视图指向上一帧写入的那半，两半的 descriptor 在两套 set 里各写一次。G-buffer 的 albedo/normal/position/viewZ 和 GI reservoir 都走这条路，帧末不再有拷贝。

DI 的四层 reservoir 不是 `History`，因为四层里两层跨帧、两层帧内复用同一块地址空间。它改成**角色轮转**：shader 说角色，`diLayer(role) = role ^ g.renderSettings.w`，`w` 每帧在 0 和 2 之间翻转，等价于原来的 `final→previous final`、`replay→previous replay` 两次拷贝。

### 图编译

`compile()` 做三件事，全部从声明推导：

1. **裁剪**：反向遍历，一个 pass 只有在写了跨帧资源、写了存活 pass 要读的东西，或显式标了 `sideEffect()` 时才存活。因此 capture、audit、history 初始化这些条件功能不出现时不花任何代价，也不需要 `if` 包住半个管线。
2. **显存复用**：从存活 pass 算出每个 transient 的活跃区间，区间不重叠且存储签名相同的就共享一块分配。`storageSignature` 只看尺寸、格式和 buffer 字节数——所有彩色 image 都带全部 color usage，所以同格式的 image 是一个可互换的存储类，角色不再切分池子。
3. **实现**：只有存储形状真正变化的槽位才重新分配，所以某一帧多出一个 pass 不会连带丢掉 history 和 persistent 的内容。

`synchronise()` 按物理槽位记录 `AccessState`（上一次写的 stage/access、之后的读 stage、已经 flush 过的部分），逐 pass 合并出这一批 barrier。状态存在 pool 里并跨越帧边界，所以第 N 帧的首次访问会自动对第 N-1 帧的末次访问排序——ping-pong 的 history 和共享显存的 transient 都靠这一条成立，没有额外规则。

### 边界

NRD、加速结构、UI、回读和呈现都是普通 pass，用同一套 `Access` 词汇表描述：

- **NRD**：图把它的输入输出声明成 `ComputeRead`/`ComputeWrite` 并保证进入时已在 `GENERAL`；`NrdDenoiser` 只 transition 自己 pool 里的纹理。
- **加速结构**：`BuildRead`/`BuildWrite`/`TraceRead`。BLAS refit 写 `scene.tlas`、TLAS build 也写它，光追 pass 读它，refit → build → trace 的顺序因此是推导出来的而不是写死的。**分配不在录制里**：结构对象必须在本帧 descriptor 写入之前存在，所以 `reserveTlas` 在图装配阶段决定并分配，`recordTlas` 只发命令。
- **UI**：`UiRenderer::draw` 只画，attachment、layout 和 `vkCmdBeginRendering` 由声明了 `color(hud, black())` 的 pass 提供。
- **回读**：`TransferRead` 源图 + `TransferWrite` 目标 buffer，加 `sideEffect()`（它的产物在 fence 之后才被 CPU 读，图看不见消费者）。audit 信号按**名字**解析成资源，`RenderAudit` 不再持有 binding 表。
- **呈现**：swapchain 是 `Imported`，最后一个 pass 声明 `Access::Present`。

### shader binding

binding 号只存在于 `Registry` 里。构建期的 `shader_bindings` 工具从同一个 registry 生成 `graph.shared.glsl`／`graph.compute.glsl`／`graph.rtxdi.glsl`，shader 用 `#include "generated/..."` 取用，离线 glslang 和运行期 `ShaderCompiler` 指向同一个输出目录。descriptor set layout 和 GLSL 声明因此是同一句话的两种形式，不可能对不上。没有 shader 视图的资源（capture、screenshot、audit）在一次运行里存在、另一次不存在，也不会挪动任何 binding 号。

### 几何与加速结构

基础几何共享静态 BLAS，蒙皮角色各自持有支持 update 的 BLAS 和持续复用的对齐 scratch。每次新姿态先更新顶点、refit 动态 BLAS，再更新 TLAS，然后进入光追查询——这三步的顺序由上面的资源依赖给出。蒙皮绑定列表发生变化时重建几何和 BLAS，并清理历史。

TLAS 只在拓扑变化时重建：`SceneDelta::topology` 是单调计数，槽位数增长或某个槽位改绑到不同几何体时递增；蒙皮资源重建也会使 TLAS 失效，其余情况走 `UPDATE`。TLAS 存储与 scratch 按 64 的块增长，只在容量真正不够时重新分配，避免姿态变化引发显存反复分配。

GPU fence 完成后再更新 CPU-visible 常量／实例／灯光数据；NRD descriptor pool 每帧重置前也已完成同一 fence。历史内容由 `History` 的奇偶翻转和 DI 的角色轮转带入下一帧，帧末没有拷贝 pass。Resize 只等待 device idle 并重建 swapchain 与 NRD pool；屏幕尺寸资源由下一次 `compile()` 按新的存储签名重新实现。关闭时先 idle，再按依赖关系析构。

### 按实测收益做的取舍

显存复用、多帧在途和并行调度这三项，图已经具备做出决定所需的全部信息，实测结论是当前管线只有第一项值得留着，且收益很小：

- **显存复用**：机制正常工作，但 1100×700 下只省 3.08 MB / 362 MB（0.84%），且只发生在存在 capture 图的那一帧。原因是结构性的：ReSTIR + NRD 管线里几乎每个屏幕尺寸资源要么是跨帧历史，要么正好被下一个 pass 消费而后继资源同时在产生，活跃区间天然重叠。这套复用是生命期分析的免费副产品（约 40 行），保留；但不值得再投入 suballocator 或显存堆。
- **多帧在途**：帧时间 4.11 ms 中 GPU 占 3.35 ms，可回收上限是 0.76 ms，代价是把 TLAS、descriptor set 和全部 host-visible 上传缓冲按帧复制。结论与[之前那次测量](verification.md)一致：不做。区别在于现在这个决定是可撤销的——`ResourcePool` 已经按奇偶管理物理槽位，扩成 N 帧是同一套机制。
- **并行调度**：17 个 pass 的依赖图基本是一条链，唯一与主链无关的分支是 UI overlay（一个小的 raster pass）。单队列上已经没有可暴露的并发，二队列 + semaphore 的复杂度换不回可测的时间。

实例数据不再逐帧全量写入：槽位由 `RenderScene` 稳定分配，渲染器按 `SceneDelta` 只写脏槽位，变换变化只触及 `GpuInstance` 前 128 字节的 `model`／`previousModel`，属性变化只触及末 16 字节的 `info`。资源上限是 1024 个 instance、256 个 material 和 256 个 light，超限明确报错。要做大型场景，下一步应增加 GPU allocator、staging 上传、chunk/streaming、draw batching 和面向大量灯的 ReGIR 分布。

## 参考来源

- [Khronos：Acceleration Structures](https://docs.vulkan.org/tutorial/latest/courses/18_Ray_tracing/02_Acceleration_structures.html)
- [Khronos：Ray Queries](https://docs.vulkan.org/samples/latest/samples/extensions/ray_queries/README.html)
- [NVIDIA RTXDI integration model](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/Integration.md)
- [NVIDIA ReSTIR GI integration](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/RestirGI.md)
- [NRD 源码和集成说明](https://github.com/NVIDIA-RTX/NRD)

RTXDI DI shader SDK 与 NRD 都是实际构建和执行的固定版本依赖。RTXDI 的 GLSL 补丁只处理 bool／uint 语法兼容，不改重采样算法。
