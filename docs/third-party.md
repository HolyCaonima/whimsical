# 固定依赖

外部依赖由 `tools/bootstrap.mjs` 下载到 `third_party/`，自有代码不修改这些库的接口。下载地址和 SHA-256 保存在 `tools/dependencies.lock.json`。

| 依赖 | 版本 | 用途 |
| --- | --- | --- |
| CMake | 3.31.8 | 工程生成 |
| Vulkan-Headers | 1.3.290 | Vulkan API 声明 |
| volk | vulkan-sdk-1.3.290.0 | 动态加载系统 Vulkan loader |
| GLM | 1.0.1 | 数学库 |
| Duktape | 2.7.0 | 嵌入式 ES5 JavaScript runtime |
| ONNX Runtime | 1.20.1 | 原生 CPU FP32 动画模型推理，MIT |
| glslang | 16.5.0 | GLSL → SPIR-V |
| DXC | 1.8.2505 / 2025_05_24 | NRD HLSL → SPIR-V |
| NRD | 4.17.3 | RELAX denoiser |
| ShaderMake | 18f5a344e7ca8fa65daaf079d07bc8ce38453e05 | NRD shader variant 构建 |
| MathLib | v11 | NRD CPU/HLSL 数学支持 |

每个源码库内保留其原始 LICENSE/COPYING 文件，NRD 使用自身 `third_party/nrd/LICENSE.txt` 中的 NVIDIA RTX SDKs License。分发时同时保留对应第三方声明。

AI4AnimationPy 的完整源代码、训练与数据工具、原始模型单独放在 `external/AI4AnimationPy/`，固定提交 `bfb5866681f7ea6dac9984be05181de5955eb48b`，来源为 [facebookresearch/ai4animationpy](https://github.com/facebookresearch/ai4animationpy)。版权归 Meta Platforms, Inc. and affiliates，采用 **CC BY-NC 4.0**；移植的运行时与导出的 ONNX 模型沿用该许可，不能当作可自由商用的 MIT 依赖。原文件的逐项校验记录见 `external/AI4AnimationPy.provenance.json`，运行时映射和改动见 [animation](animation.md)。

本机另外从 LunarG Vulkan SDK 1.3.290.0 官方安装包中提取了 validation layer、spirv-val 和 vulkaninfo，仅放在 `third_party/validation/`；没有运行系统安装过程。运行时如果该目录存在且用户未指定 `VK_LAYER_PATH`，程序在进程范围内使用它，启用 validation 和 synchronization validation。

## UI dependencies

- RmlUi 6.1: https://github.com/mikke89/RmlUi/tree/6.1 — MIT.
- FreeType 2.13.3: https://github.com/freetype/freetype/tree/VER-2-13-3 — FreeType License / GPLv2; this integration uses the FreeType License option.
- LatoLatin Regular/Bold are copied unmodified from RmlUi 6.1 sample assets into `engine/Content/Fonts`; the accompanying `LICENSE.txt` preserves the SIL OFL notice.
- Windows WIC and the optional installed Microsoft YaHei fallback are system resources; no Windows font is redistributed.

RmlUi and FreeType archives are version-pinned and SHA-256 checked by `tools/dependencies.lock.json`.
