# NRD 接入与阴影边缘调查（2026-09-06）

结论：阴影过度平滑可以复现，主要来自当前合并 DI/GI 信号与 RELAX 默认空间滤波参数不匹配。同时修正了 diffuse hit-distance 来源和缺失的 specular 材质解调。保持一个 `RELAX_DIFFUSE_SPECULAR`、一次 NRD dispatch 列表，不拆成两套 DI/GI 降噪器。

## 根因与修改

`NrdDenoiser.cpp` 原先只覆盖主历史长度、A-trous 次数和 anti-firefly，其余沿用 NRD 4.17.3 默认值。Diffuse prepass 半径为 30、specular 为 50，diffuse/specular `PhiLuminance` 为 2/1。

NRD 的 `RELAX_PrePass.cs.hlsl` 中，diffuse 预滤波根据平面距离、法线、材质和 hit distance 判断邻居，没有亮度边缘停止项。投射到同一平面上的阴影两侧具有相同法线和连续深度，因此容易混色；之后 A-trous 再做亮度保护也无法还原已经丢失的边缘。`RELAX_Atrous.cs.hlsl` 的亮度权重依赖 `PhiLuminance * sqrt(variance)`，较大的 Phi 对亮度变化容忍更大。

| 配置 | 原值 | 当前值 |
| --- | ---: | ---: |
| diffuse prepass 半径 | 30 | 0 |
| specular prepass 半径 | 50 | 20 |
| diffuse PhiLuminance | 2 | 0.5 |
| specular PhiLuminance | 1 | 0.35 |
| diffuse fast history | 6 | 4 |
| diffuse/specular 主历史 | 24/24 | 24/24 |
| A-trous 次数 | 5 | 5 |
| anti-firefly | 开 | 开 |

另有两处输入契约调整：

- 原 `resolve.comp` 直接使用最终 GI reservoir 选中样本的距离。该分布受到亮度选择和重连接影响，不再是当前像素的 cosine-weighted 一跳距离。现在 `lighting.comp` 将原始 diffuse ray 距离存到已有 `rawDiffuse.a`，resolve 保留它。NRD 对 ReSTIR hit distance 的要求见下方官方说明。关闭 diffuse prepass 后，RELAX 不再靠 diffuse hit distance 驱动空间滤波，所以这一修正不是本轮锐化效果的独立证明。
- Specular 原来携带完整材质反射率直接降噪，材质变化会混入光照信号。现在采用 RTXDI 的 F0 解调基线：输入除以 `max(F0, 0.01)`，输出和 RAW 视图乘回相同因子。因子由同一个 GLSL helper 计算。仍使用独立 specular ray 的距离，不把选中灯的距离用于反射重投影。这是 F0 基线，并非完整的 view-dependent environment-BRDF 解调。

没有发现 motion XY 正负号、UV/像素单位、normal/roughness 编码、viewZ、矩阵布局、Vulkan projection Y、资源绑定或 descriptor 生命周期方面的明显错误：motion 是 `previousUV-currentUV`，NRD 默认 scale 为 `(1,1,0)`；motion Z 保存的 previous clip W 不被 NRD 当作深度差；编译缓存确认 normal encoding=4、roughness encoding=1；pool 重置前会等待 frame fence。

## 参考实现

参考源码下载到 `build/nrd-references/`，以下链接固定到调查时读取的 commit。

- [RTX Remix RELAX presets](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/e876135b37295dc203ccdb7b20a8089629588201/src/dxvk/rtx_render/rtx_nrd_settings.cpp)：直接光配置关闭 diffuse prepass、diffuse Phi 为 0.4，并保留 5 次 A-trous；其 GI 配置更强，不能整套照搬到本项目的合并信号。
- [RTXGI Pathtracer NRD 配置](https://github.com/NVIDIA-RTX/RTXGI/blob/10b5770b8eaddfc1faab82b65f799ac6f47dcc44/Samples/Pathtracer/NrdConfig.cpp)：RELAX 的 prepass 为 0/20、Phi 为 0.5/0.35、diffuse fast history 为 4。本轮借用这些设置，没有照搬它的 60 帧历史或针对 probabilistic split 的重建配置。RTXGI 此处指官方 Pathtracer 示例，不把 DDGI probe filtering 当作屏幕空间 NRD。
- [RTXDI 光照输出与解调](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Samples/FullSample/Shaders/LightingPasses/ShadingHelpers.hlsli) 和 [合成](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Samples/FullSample/Shaders/CompositingPass.hlsl)：明确分离光照与 specular F0，并在合成时恢复。
- [NRD 输入约定](https://github.com/NVIDIA-RTX/NRD/blob/master/README.md#noisy-inputs) 和 [DI/GI 合并降噪](https://github.com/NVIDIA-RTX/NRD/blob/master/README.md#combined-denoising-of-direct-and-indirect-lighting)：同时核对了项目本地固定 4.17.3 的 README、NRDSettings.h 和 shader 源码，未假定最新 SDK 默认值等于项目版本。

## 可重复验证

`tools/test_nrd_shadows.py` 生成隔离项目：纯色地面、箱体、单个半径为零的点光源、冻结场景。192 帧中跳过前 64 帧，用其余 128 帧的实际线性 diffuse 输入均值作为参考，不经过曝光、tone mapping、雾或 HUD。

在 DI 阴影边界两侧两像素内选取 2708 个地面像素，计算 denoised diffuse 与 raw diffuse 均值的绝对亮度差，再除以受光地面平均亮度。以下对照只替换 RELAX 设置，保留相同 shader 输入修正；`direct`、`albedo`、`raw-diffuse`、`raw-specular` 四组读回逐值一致。

| 指标 | 旧 RELAX 设置 | 当前设置 |
| --- | ---: | ---: |
| 边缘相对误差 | 8.213% | 0.741% |
| 输入地面时域噪声 RMS | 0.016069 | 0.016069 |
| 输出地面时域噪声 RMS | 0.000823 | 0.000912 |

边缘误差降低约 91%；新配置仍降低约 94% 的输入时域噪声 RMS。相较旧设置，输出残余 RMS 增加约 11%，因此改善不是没有代价的锐化。测试采用 3% 边缘误差上限，旧结果在离线重算时触发断言，新结果通过。该结果是固定点光源场景的回归验证，不代表所有粗糙度、面积光或动态遮挡条件。

```powershell
python tools/test_nrd_shadows.py
python tools/test_nrd_shadows.py --analyze captures/nrd-shadow-a1388402
# 旧参数捕获应在边缘误差断言处失败：
python tools/test_nrd_shadows.py --analyze captures/nrd-shadow-90eca9a5
```

审计增加 `raw-diffuse`、`raw-specular`、`denoised-diffuse`、`denoised-specular` 四个线性信号读回，原有三个文件及每像素五个 float32 的布局保持兼容。只在 `--audit` 模式分配和执行，不进入正常渲染路径。

Rain Court 同样比较了原始版本、只关 diffuse prepass 和最终修改。仅关闭 prepass 改善有限，最终配置对物体接触阴影更清楚。对应截图分别位于 `captures/nrd-baseline/`、`captures/nrd-no-diffuse-prepass/`、`captures/nrd-combined-fixed/`；原始噪声参考位于 `captures/nrd-raw-reference/`。这些帧都启用了校验、没有非有限读回值，原始 DI 数据一致。

![Rain Court 阴影局部对照](../captures/nrd-comparison.png)

![点光源阴影：旧设置与新设置](../captures/nrd-shadow-comparison.png)

Release 构建、13/13 CTest、8 个构建目录 SPIR-V 校验通过。最终版本的 160 帧 smoke 包含相机运动、resize、最小化恢复、Map 重载，Vulkan/synchronization validation 为 0 errors。日志为 `build/nrd-final-smoke.log`，报告为 `captures/nrd-final-smoke-report.json`。

正常渲染保持一份 NRD、原来的 5 次 A-trous，没有新增光线、屏幕纹理或计算 pass；仅多了既有纹理的 hit-distance 写读以及解调/恢复运算。诊断读回有额外等待和统计开销，本次审计的 FPS 不用作性能比较。
