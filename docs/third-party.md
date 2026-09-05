# 固定依赖

外部依赖由 `tools/bootstrap.mjs` 下载到 `third_party/`，自有代码不修改这些库的接口。下载地址和 SHA-256 保存在 `tools/dependencies.lock.json`。

| 依赖 | 版本 | 用途 |
| --- | --- | --- |
| CMake | 3.31.8 | 工程生成 |
| Vulkan-Headers | 1.3.290 | Vulkan API 声明 |
| volk | vulkan-sdk-1.3.290.0 | 动态加载系统 Vulkan loader |
| GLM | 1.0.1 | 数学库 |
| Duktape | 2.7.0 | 嵌入式 ES5 JavaScript runtime |
| glslang | 16.5.0 | GLSL → SPIR-V |
| DXC | 1.8.2505 / 2025_05_24 | NRD HLSL → SPIR-V |
| NRD | 4.17.3 | RELAX denoiser |
| ShaderMake | 18f5a344e7ca8fa65daaf079d07bc8ce38453e05 | NRD shader variant 构建 |
| MathLib | v11 | NRD CPU/HLSL 数学支持 |

每个源码库内保留其原始 LICENSE/COPYING 文件，NRD 使用自身 `third_party/nrd/LICENSE.txt` 中的 NVIDIA RTX SDKs License。分发时同时保留对应第三方声明。

本机另外从 LunarG Vulkan SDK 1.3.290.0 官方安装包中提取了 validation layer、spirv-val 和 vulkaninfo，仅放在 `third_party/validation/`；没有运行系统安装过程。运行时如果该目录存在且用户未指定 `VK_LAYER_PATH`，程序在进程范围内使用它，启用 validation 和 synchronization validation。
