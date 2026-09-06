# RT 渲染接入约定

## 每帧数据

`Globals` 和 GPU struct 有静态尺寸检查，GLSL 使用相同 std140/std430 布局。几何和材质只经过同一份数据源，raster 与 ray query 不维护两套不同场景。每个 TLAS instance 的 custom index 指向 GPU instance；instance 中保存绝对 index-buffer 起始偏移、material index、entity ID 和上次渲染的 model matrix。

`GpuVertex` 包含位置、法线、线性顶点颜色和上一次实际渲染的位置，共 64 字节。蒙皮资源按骨骼名字绑定，World 只发布不可变 palette；渲染线程执行 CPU 蒙皮并上传变形顶点。蒙皮顶点已处于世界空间，对应 instance 的 model 为单位矩阵。G-buffer 与 ray query 都将插值后的顶点颜色乘以材质颜色，并访问同一份变形几何。重复呈现同一仿真快照时，上一位置收敛到当前位置，避免重复报告形变运动。

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

DI reservoir 保存球形位置扰动的解析灯样本、归一化 W、代表样本数 M 和选中样本 target。初始灯索引均匀采样，以 diffuse + specular BRDF luminance 做 RIS target。时域用 motion 重投影，并核对 entity ID、法线和之前 clip depth；空间阶段只读 initial reservoir，不在同一次 dispatch 内读写邻居的最终结果。最终选中灯重新执行当前 TLAS visibility。

GI initial sample 从当前 G-buffer 表面发射余弦半球 BRDF 光线。命中时保存二次位置、法线、实体 ID、该点的一次 next-event lighting 和 PDF；未命中时保存环境方向。GI temporal/spatial 重连接包含固体角 Jacobian，并追踪验证候选二次表面仍然存在、法线相容和可见。不可见候选以零权重计入 represented sample count，避免简单丢弃造成额外亮度偏差。

当前方案使用受限历史与 Jacobian 支持域，相关样本与有限邻居存在偏差；还没有 production ReSTIR 的完整 bias correction / pairwise MIS。它是可运行、可测量和可替换的第一版算法。环境由实际 secondary ray miss 求值，不使用屏幕空间遮蔽或环境探针。

镜面间接光为独立的 GGX VNDF ray。二次表面的 shading 当前为 diffuse NEE 加 emission；这是一次 GI bounce 的基础，不等同多跳 glossy path tracing。

## NRD

固定 NRD 4.17.3，`RELAX_DIFFUSE_SPECULAR`。`NrdDenoiser` 通过 `CreateInstance/GetInstanceDesc` 建立 Vulkan pipeline、permanent/transient textures、sampler、descriptor layout 和常量区。每帧 `SetCommonSettings/GetComputeDispatches` 获取真实 dispatch 列表，逐个绑定资源并执行。当前选择对应 15 条原生 Vulkan pipelines；完整 NRD 构建产生 159 个 shader variants。

RELAX 输入直接是 RGB radiance + 真实 hit distance，不用 REBLUR 的归一化 hit-distance 编码。Diffuse 在 denoise 前不乘 base color / (1-metallic)，合成时再乘；specular 保留完整 BRDF 信号，合成直接相加。当前没有采纳 NRD 可选的 specular material demodulation，所以未来引入高频金属材质时需要一起扩展 material factor 契约。

Resize、首帧、大相机跳变、灯光／材质变化会清理历史。NRD 帧号按实际 GPU render 递增；timeDelta 使用实际渲染帧间隔，GPU timestamp 只用来报告 pass 时间。

## 同步与资源生命周期

`RenderGraph` 是明确顺序的 pass graph，记录每 pass 的 debug marker 和 memory barrier。纹理 layout 由对应 pass 进行显式 transition；读写 reservoir 的 dispatch 之间有内存可见性边界。基础几何共享静态 BLAS，蒙皮角色各自持有支持 update 的 BLAS 和持续复用的对齐 scratch。每次新姿态先更新顶点、refit 动态 BLAS，经 barrier 后更新 TLAS，再进入光追查询。蒙皮绑定列表发生变化时重建几何和 BLAS，并清理历史。

TLAS 只在拓扑变化时重建：`SceneDelta::topology` 是单调计数，槽位数增长或某个槽位改绑到不同几何体时递增；蒙皮资源重建也会使 TLAS 失效，其余情况走 `UPDATE`。TLAS 存储与 scratch 按 64 的块增长，只在容量真正不够时重新分配，避免姿态变化引发显存反复分配。

GPU fence 完成后再更新 CPU-visible 常量／实例／灯光数据；NRD descriptor pool 每帧重置前也已完成同一 fence。历史 image 和 reservoir 在当前帧末尾保存。Resize 等待 device idle，再重建 swapchain、screen-size images、reservoir 和 NRD pool。关闭时先 idle，再按依赖关系析构。

当前 pipeline 的选择以正确性为先：保守 barrier、一帧 GPU in flight、host-visible geometry upload、逐实体 draw。实例数据不再逐帧全量写入：槽位由 `RenderScene` 稳定分配，渲染器按 `SceneDelta` 只写脏槽位，变换变化只触及 `GpuInstance` 前 128 字节的 `model`／`previousModel`，属性变化只触及末 16 字节的 `info`。资源上限是 1024 个 instance、256 个 material 和 256 个 light，超限明确报错。要做大型场景，下一步应增加 GPU allocator、staging 上传、chunk/streaming、draw batching、light importance distribution 和细粒度 graph dependency。

## 参考来源

- [Khronos：Acceleration Structures](https://docs.vulkan.org/tutorial/latest/courses/18_Ray_tracing/02_Acceleration_structures.html)
- [Khronos：Ray Queries](https://docs.vulkan.org/samples/latest/samples/extensions/ray_queries/README.html)
- [NVIDIA RTXDI integration model](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/Integration.md)
- [NVIDIA ReSTIR GI integration](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/RestirGI.md)
- [NRD 源码和集成说明](https://github.com/NVIDIA-RTX/NRD)

RTXDI 是设计参考，工程并未依赖或声称集成该 SDK。NRD 则是实际编译、链接和执行的依赖。
