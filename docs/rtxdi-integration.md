# RTXDI DI 集成

DI 的 reservoir 算法直接使用 NVIDIA RTXDI-Library，而不是继续维护一份近似复刻。固定提交为 `f12037fa8e97ebc08e9e3edfd2de528ed1772a4b`，归档 SHA-256 为 `f91de92d7c27f9824915ee5fcf62b0da664a6eb294edb659056ff3c462823f75`。`tools/bootstrap.mjs` 下载依赖并应用 `tools/patches/rtxdi-glsl.patch`；补丁仅把 HLSL 隐式 bool/uint 转换改成 GLSL 可接受的写法，C++/HLSL 参数布局不变。SDK 原始许可证保留在依赖目录。

## SDK 与引擎的边界

- SDK：24 字节 packed reservoir、16×16 block-linear 寻址、RIS streaming/finalization、temporal permutation、历史匹配、空间邻居复用、ray-traced MIS-like bias correction。
- RAB bridge：当前／历史 G-buffer、稳定灯槽与上一帧灯参数、解析灯采样、BRDF target、同一套材质透明度／背面规则和当前 TLAS 可见性。
- 初始候选：8 个离散灯样本，CPU 构建 90% power / 10% uniform 混合 CDF，GPU binary search，PDF 随分布存储。初始 RIS 完成后 M=1，与 SDK initial sampling 的约定一致。
- temporal/spatial 分为独立 dispatch：temporal 最多 20 帧，空间 4 个邻居，短历史区域 8 个；开启 ray-traced bias correction，关闭旧可见性 shortcut，最终选中灯重新查当前 TLAS。

当前引擎仍只有球形位置扰动的解析局部灯。没有新增 emissive triangle、环境灯 importance sampling、ReGIR、checkerboard、BRDF/light MIS 或 ReSTIR PT；这些需要相应灯光／采样架构支持。GI 继续使用原实现。SDK 空间算法包含 pairwise MIS 分支，本接入选择 ray-traced 分支。

## 动态阴影与单套 NRD

静止地面的 motion vector 不能描述移动遮挡物投下的阴影。只依赖深度／法线重投影，会同时留下 DI reservoir 和 NRD 光照历史。尤其只使用当前 TLAS 做 temporal bias correction 时，旧阴影区域的 reservoir M 会拖慢重新见光后的亮度恢复。

每帧先在宽、高各 1/3 的网格中，从上一帧 replay reservoir 选择一个灯样本，用相同灯 ID、UV、权重、表面及视角，在当前 TLAS 上重算可见性与光照。重算值转换为 FP16 后再与历史 luminance 比较，避免量化误差产生假梯度。黑色样本也保留在 replay 中；否则“遮挡移开”没有可供回放的灯样本。梯度分母使用合并 DI/GI 能量，避免 DI 只占少量贡献时把 GI 历史全部清掉。

梯度在低分辨率执行深度／法线引导的空间滤波，再双线性放大并转为 diffuse/specular confidence。采用短暂的非线性恢复滤波，新变化立即降低 confidence，随后恢复，避免一次变化之后 reservoir 尚未稳定就重新使用长历史。confidence 同时控制 SDK `maxHistoryLength` 和 NRD 的 `IN_DIFF_CONFIDENCE`／`IN_SPEC_CONFIDENCE`。

NRD 4.17 在 previous UV 采样 confidence，所以当前输出 confidence 位于上一帧的像素坐标。resolve 把它重投影到当前表面坐标后保存，供下一帧恢复滤波使用。该纹理分阶段读写，没有邻居读写竞争。

灯参数变化时，GI 的旧 secondary radiance 不能像 DI 灯样本那样重新求值，因此丢弃 GI reservoir 历史。此前零功率灯没有可回放样本，其参数变化直接降低对应表面的 confidence。

仍只创建、执行一套 `RELAX_DIFFUSE_SPECULAR`，输入为合并后的 DI/GI。新增的是一条低分辨率回放与滤波路径，不是第二套 NRD。

## 资源与近似

DI 四层 packed reservoir 为 96 字节／对齐像素；旧实现三个 32 字节 reservoir 也是 96 字节／像素。另增历史 albedo/viewZ、paired luminance、两个 R16 confidence、一个 RG16 恢复历史以及两张低分辨率梯度纹理。回放每个 3×3 区块最多增加一条 visibility ray；空间 bias correction 也增加邻居可见性查询。性能需以关闭 audit/validation 的 GPU profile 衡量，不能把读回审计的 FPS 当运行性能。

没有保留上一帧 TLAS：SDK temporal visibility 使用当前 TLAS 是近似，梯度驱动的局部历史缩短用于控制动态偏差，不声称动态场景严格无偏。回放在旧表面世界位置上进行，移动或变形的接收表面可能保守地降低 confidence；NRD 仍使用几何运动重投影。灯槽假定数量不变时索引稳定；若以后支持灯重排，应提供稳定 light ID 的前后帧映射。

## 验证入口

```powershell
node tools/bootstrap.mjs
tools/build.ps1 -Test
python tools/test_nrd_shadows.py
python tools/test_rtxdi_motion.py
python tools/test_rtxdi_lights.py
python tools/test_shader_surfaces.py
```

`r.DIHistoryConfidence false` 用于关闭 DI/NRD 置信度反馈做 A/B；默认开启，不归档到用户设置。`--audit-occluder SLOT` 在第 64 个 GPU 帧后将指定 proxy 沿 X 移动 2 世界单位，保留历史而非 reset，用于可重复的遮挡阶跃测试。该诊断必须配合 `--audit NAME`。

### 动态遮挡实测

RTX 3080，640×400，固定点光源、静止地面、移动方盒。第 64 帧移动方盒，测之后 8 帧的平均线性信号；与新位置预热 64 帧后的 raw diffuse 参考比较。只统计两次姿态均为地面且阴影状态改变的 10,538 个像素。

| 指标 | 关闭反馈 | 开启反馈 |
| --- | ---: | ---: |
| 阴影变化区域 NRD 相对误差 | 27.83% | 7.38% |
| 新覆盖阴影区域误差 | 12.44% | 8.01% |
| 遮挡移开区域误差 | 46.91% | 6.60% |

开启反馈后，遮挡移开区域的 raw DI 亮度达到稳定参考的 98.64%。这证明同时修复了 reservoir 恢复滞后和 NRD 历史拖影；残余误差仍存在，不等同所有运动场景已无拖影。结果位于 `build/rtxdi-motion-0351000b/motion-metrics.json`，四组原始审计位于 `captures/rtxdi-motion-0351000b-*`。各次验证层错误与非有限信号计数均为 0。

![遮挡变化后前八帧的线性光照对照](../captures/rtxdi-shadow-history.png)

图中每行使用相同显示范围；下排右侧是稳定姿态的 raw diffuse 平均值，用作 NRD 输出的参考。

### 其他 GPU 回归

- `captures/nrd-shadow-0fff289a`：192 帧静态点光阴影，边缘误差 0.812%，地面 raw/NRD 时域 RMS 为 0.022999/0.001011；保留降噪效果与阴影边界。
- `captures/rtxdi-lights-65d72816-many`：643×403、256 个非等功率 RGB 点光源，与逐灯解析求和比较，总 RGB 能量误差 1.328%，32×32 区域平均误差 3.086%。Raw 逐像素误差为 14.236%，反映相关有限采样噪声，未把它当成降噪结果。尺寸同时覆盖非 16 与非 3 对齐的 dispatch／reservoir 边界。
- `captures/rtxdi-lights-65d72816-zero`：零灯时 direct radiance 精确为 0，GI／NRD 信号无 NaN/Inf。
- `build/shader-surfaces-22cf3e38`：透明裁剪的表面与完全不存在的表面，在 albedo、direct、display 三个信号的最大差异均为 0；实际 foliage 仍改变遮挡和光照。3 个 Shader 对应 3 个 raster + 6 个 linked compute 编译。
- 以上 GPU 审计均开启 Vulkan validation，错误为 0。Release 构建、13/13 CTest 和全部六个材质链接 compute pass 的 SPIR-V 编译通过。全新 SDK 归档应用 GLSL 补丁后的内容与当前依赖一致，bootstrap 重复执行也通过。
- 最终 160 帧 smoke 覆盖相机运动、resize、最小化恢复、地图重载；验证错误为 0。日志 `build/rtxdi-final-smoke.log`，报告 `captures/rtxdi-final-smoke-report.json`。FIFO/validation 下最后统计窗口约 60.2 FPS、GPU 4.78 ms；这不是同场景、同姿态的前后性能基准。`rtxdi-startup-profile.json` 捕获的是首帧资源初始化，不用于推断稳态 pass 成本。

## 官方参考

- [RTXDI-Library 固定版本](https://github.com/NVIDIA-RTX/RTXDI-Library/tree/f12037fa8e97ebc08e9e3edfd2de528ed1772a4b)
- [SDK TemporalResampling](https://github.com/NVIDIA-RTX/RTXDI-Library/blob/f12037fa8e97ebc08e9e3edfd2de528ed1772a4b/Include/Rtxdi/DI/TemporalResampling.hlsli)
- [RTXDI FullSample ComputeGradients](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Samples/FullSample/Shaders/DenoisingPasses/ComputeGradients.hlsl)
- [RTXDI FullSample ConfidencePass](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Samples/FullSample/Shaders/DenoisingPasses/ConfidencePass.hlsl)
- [NRD 4.17.3](https://github.com/NVIDIA-RTX/NRD/tree/v4.17.3)
