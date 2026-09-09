# RenderCore 与系统边界

RenderCore 是所有 GPU system 的基础层。RenderingSystem 是使用它生成画面的一个系统；物理、粒子、预计算等 GPU 后端可以依赖同一层，不需要通过 Renderer。

```text
应用运行时（生命周期、线程、更新节奏）
  ├─ Rendering：场景、材质、GBuffer、RTXDI、NRD、合成、呈现
  └─ 其他 GPU system：各自的算法、输入与状态
          ↓
      RenderCore：资源节点、图、程序、编译、提交、完成、读回
          ↓
      Vulkan backend
```

`whimsical_rendercore` 是独立 CMake target，不依赖 `whimsical_core`、World、UI、动画、NRD 或 RTXDI。它使用公共 diagnostics 和资产基础库中的 JSON 序列化；Vulkan 是私有后端依赖。`whimsical_rendering` 依赖 RenderCore，包含已有的渲染算法和平台呈现流程。

## 所有权

| 对象 | 所有者 | 职责 |
| --- | --- | --- |
| `rc::RenderCore` | 应用运行时 | 设备与队列；可选择无窗口、无 ray-query 的初始化 |
| `rg::Registry` | system 或协作系统的组合入口 | 声明逻辑资源、尺寸、生命周期和 shader 接口 |
| `rc::GraphContext` | system 或组合入口 | 该范围的资源池、程序、图、命令、fence 和 profiling |
| `rg::RenderGraph` | GraphContext | 各 system 向同一张图填入 pass 和资源依赖 |
| `rg::ResourceId/ResourceRef` | 图的使用者 | 逻辑资源及 history 角色，不是 VkBuffer/VkImage |
| Vulkan 分配和 program | RenderCore 后端 | 按图的活跃资源分配、复用并在执行结束后回收 |

应用先创建 RenderCore，再创建使用它的 system。当前主程序在 GPU/渲染线程建立 RenderCore，将引用传给 Renderer；Renderer 不再创建或销毁设备。

Registry 必须比 GraphContext 活得久，RenderCore 必须比全部 GraphContext 活得久。GraphContext 析构等待自己的在途提交；呈现系统在回收 swapchain 时通过 Core 等待设备空闲。

## 构图接口

普通 GPU 计算只需要：

```cpp
#include "renderCore/RenderCore.h"
#include "renderCore/graph/RenderGraph.h"
```

这些公共头文件不包含 Vulkan 或 Windows 类型。声明 resource、`read/overwrite/modify/bind`、compute dispatch、上传和读回均不需要原生 API。光栅命令、加速结构、NRD 等特殊集成显式包含 `renderCore/vulkan/GraphAccess.h` 或 `VulkanAccess.h`。

资源具有固定的 `byteSize` 或 `width/height`。只有调用方选择相对尺寸时，图才使用 `compile(referenceWidth, referenceHeight)` 的参考范围；`divisor` 也是调用方的尺寸规则。默认参考范围为 1×1，纯 buffer 计算不依赖视口。

shader 的 include 分组由 `Declaration::section` 字符串决定，stage 可见性由独立的 `ShaderStages` 决定。Core 不包含 RTXDI 分类。每个 Registry 生成自己的 `graph.<section>.glsl`，资源和 shader 的绑定通过 Registry 与反射校验，不需要修改全局渲染资源表。

示例使用方式（省略算法 shader 和数据准备）：

```cpp
rg::Registry resources;
rg::Declaration values;
values.name = "values";
values.kind = rg::Kind::Buffer;
values.byteSize = count * sizeof(float);
values.view = {rg::BindingType::Storage, false, {}, "Values", "float values[];"};
auto data = resources.declare(values);

rg::Declaration output;
output.name = "cpuResult";
output.kind = rg::Kind::Buffer;
output.byteSize = values.byteSize;
output.handover = rg::Access::Host;
output.lifetime = rg::Lifetime::Persistent;
auto result = resources.declare(output);

rc::GraphContext execution(core, resources);
// source 是完整 .comp 源码；可嵌入 resources.glsl() 生成的接口声明。
const auto& kernel = execution.compute("integrate.comp", source);

auto& graph = execution.graph();
graph.reset();
execution.upload(data, inputBytes);
graph.add("Integrate")
    .modify(data, rg::Access::Compute)
    .dispatch(kernel, rg::Extent3D{count, 1, 1});
execution.readback(data, result);
execution.compile();
execution.record({0, tick});
execution.submit();

// 稍后在该执行上下文所属线程消费，无隐式 GPU 等待。
if (execution.poll()) {
    auto bytes = execution.readbackData(result);
}
```

`dispatch(program, Extent3D)` 接收元素范围，工作组大小从 SPIR-V 读取。`dispatch(program, imageId)` 使用该图像的实际范围。shader 负责对向上取整后的多余线程做范围检查。

`upload` 拥有 CPU 字节快照，以 transfer node 写入整个 buffer；shader buffer payload 必须四字节对齐。`readback` 同样是图节点，destination 声明 `handover=Host`；完成后 `readbackData` 返回 CPU 拷贝。底层 image readback 仍用于截图和 Entity ID 读取。

## 时间、线程与组合

- 一个 GraphContext 当前允许一个在途提交。`poll()` 或 `wait()` 完成后再重建图、改资源或重新录制；缓存下来的 builder/reference 同样遵守这条规则。
- CPU 数据和算法状态仍由 system 拥有。GPU 设备与上下文操作在所属 GPU 线程执行，不能把活动 World/JS 对象捕获到跨线程命令中。
- `submit()` 不知道模拟 tick 或渲染帧，也不翻转历史；所有者显式调用 `advanceHistory()`。不同 GraphContext 的 history 奇偶与完成状态互不影响。
- 合作的 system 可以共同声明 Registry、向同一个 GraphContext 填图，并传递 ResourceRef。保持已有构图语义：消费者声明在生产者之后，编译器推导依赖、同步和裁剪；同一资源的后续写入建立后续内容版本。
- ResourceId 属于其 Registry，资源的物理存储属于 GraphContext。不能把另一个上下文的整数 ID 当作共享 GPU 分配；当前没有增加跨图资源发布或跨线程任务调度协议。
- 帧快照的 latest-wins 策略仍属于渲染宿主，未放入 Core。未来模拟 GPU 后端必须按自己的 tick 提交，不能把可靠任务塞入 FrameMailbox。

当前变更建立了模块边界和可复用 GPU 构图执行入口，没有将物理算法迁移到 GPU，也没有引入独立 compute 队列、GPU 调度线程或另一个图模型。

## 渲染侧保留什么

`RenderResources` 声明 GBuffer、光照等资源；`RenderPipeline` 组织具体的渲染 pass。材质 shader 拼装仍在 `render/ShaderCompiler`，完整源码编译交给 `renderCore/ShaderCompiler`。compute program 的创建与寿命由 GraphContext 管理；原生 raster 状态、材质绑定、场景 GPU 镜像、NRD 集成以及呈现流程属于渲染系统。

新增其他 GPU system 应链接 `whimsical_rendercore`，声明自己的资源和 kernel，然后构图。无需修改 `RenderResources`、`RenderPipeline` 或 `Renderer`。
