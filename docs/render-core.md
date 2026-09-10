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

应用先创建 RenderCore，再启动使用它的 system。当前主程序拥有设备，Render 和 Dynamics 分别在自己的线程创建／使用／销毁 GraphContext；Renderer 不创建或销毁设备，也不负责推进 Dynamics。

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
- CPU 数据和算法状态仍由 system 拥有。每个 GraphContext 始终由一个线程操作，不同上下文可以并发使用同一 RenderCore。不能把活动 World/JS 对象捕获到跨线程命令中。
- `submit()` 不知道模拟 tick 或渲染帧，也不翻转历史；所有者显式调用 `advanceHistory()`。不同 GraphContext 的 history 奇偶与完成状态互不影响。
- 合作的 system 可以共同声明 Registry、向同一个 GraphContext 填图，并传递 ResourceRef。保持已有构图语义：消费者声明在生产者之后，编译器推导依赖、同步和裁剪；同一资源的后续写入建立后续内容版本。
- ResourceId 属于其 Registry，资源的物理存储属于 GraphContext。不能把另一个上下文的整数 ID 当作共享 GPU 分配。原生 buffer 导入可携带所有者和访问状态，使消费者保留其生命周期；解除导入仍须经过完成边界。
- 帧快照的 latest-wins 策略属于渲染宿主。Dynamics 的可靠 tick 通道由其接入层管理，不使用 FrameMailbox。

[Dynamics](dynamics.md) 与 Rendering 是平级使用者。Dynamics 的服务循环通过条件变量等待请求，通过自己的 fence 等待 GPU 完成，再通知主线程；不由 render/present 轮询，也不使用固定毫秒轮询定时器。主线程在普通消息循环消费 GPU 完成，模拟时间仍只在固定更新中累积。

`GraphContext(core, registry, name, QueueClass::Compute)` 声明计算队列偏好，默认 `General`。当前 Vulkan 后端从同一个 graphics/compute 队列族申请最多两条队列，General/Present 使用第一条，Compute 在可用时使用第二条。设备只有一条队列时同步访问同一队列；逻辑系统仍独立，但单队列的提交／呈现调用可能互相等待。队列数量不意味着硬件吞吐翻倍，也不保证物理并行。

每个执行上下文和原生命令 scope 使用独立命令池；录制期间不用全局锁。Core 对各条队列、着色器编译缓存及跨上下文性能采集分别同步，不用一个大锁包住整个 GPU 工作。`waitIdle()` 同步设备级队列访问，适用于重建与退出，不用于正常求解推进。

跨上下文资源使用仍要求明确的完成边界和所有权。不可把正在写入的 buffer 直接交给另一个线程／队列。当前 Dynamics 的不可变发布快照在发布时已等待复制完成；消费者持有快照到解除导入。未来重叠生产／消费需要显式的 GPU 依赖接口，不能依赖不同队列的提交顺序。参考 [Vulkan 同步契约](https://docs.vulkan.org/spec/latest/chapters/fundamentals.html)。

跨系统 GPU profile 由 Core 聚合，各上下文所有者通过 `poll()/wait()` 解析自己的完成数据；`takeProfile()` 不再跨线程轮询别人的上下文。退出时先停止输入，等待系统执行循环结束，再回收设备。

`Registry(pushConstantBytes)` 声明通用 push constant 范围，pass 的 `constants(bytes)` 提供本次 dispatch 参数。`uploadRange` 更新 buffer 子范围，`copyBuffer` 支持整块及范围复制。它们都形成图内 transfer 节点，不包含 Dynamics 概念。完整源码编译缓存属于 RenderCore 设备，GraphContext 保留按自身 layout 建立的程序。

## 渲染侧保留什么

`RenderResources` 声明 GBuffer、光照等资源；`RenderPipeline` 组织具体的渲染 pass。材质 shader 拼装仍在 `render/ShaderCompiler`，完整源码编译交给 `renderCore/ShaderCompiler`。GraphContext 管理图内 compute program 的缓存；graphics 和 NRD compute 也复用 Core 的程序工厂与 RAII 句柄。Rendering 选择材质、顶点布局、attachment 格式、混合/深度状态以及缓存键；Vulkan shader module、pipeline 创建和销毁由 Core 实现。场景 GPU 镜像、NRD 集成以及呈现策略属于渲染系统。

新增其他 GPU system 应链接 `whimsical_rendercore`，声明自己的资源和 kernel，然后构图。无需修改 `RenderResources`、`RenderPipeline` 或 `Renderer`。

## 原生接入的收敛边界

- `vulkan/Programs.h` 接收通用 graphics 描述或 compute 字节码与 pipeline layout。它不认识 GBuffer、Material、UI 或 NRD；这些使用者都不再自行创建、销毁 shader module 和 pipeline。程序句柄及其使用者必须先于设备销毁，GPU 执行结束后才能释放最后一个句柄。
- `NativeResources` 是按 GraphContext 绑定的原生导入入口。导入和同步动态 import 都检查在途提交已完成；它不开放 pool 的分配、alias、descriptor 写入或 history 翻转。原生 pass 只通过 `PassContext::image/buffer` 取得已声明资源，资源池和图的内部指针不再公开。profile 通过当前 pass 上下文取得。
- 队列与 command pool 句柄在 Vulkan 后端内部。图提交与原生同步工作共享后端提交入口；呈现系统通过后端 `present` 发起呈现。
- `VulkanContext::uploadImage` 管理 staging、复制、mip 生成和消费前的同步。它接收调用方选择的格式、尺寸及消费阶段；场景纹理和 UI 共用该实现。原生 AS 初始化通过 `execute` 提供录制回调，由 Core 管理 command 和 fence。
- `execute/uploadImage` 当前是同步完成接口，等待本次提交的 fence，不调用 `vkQueueWaitIdle`。由于共用单队列，仍可能等待排在它之前的其他任务；跨上下文的 buffer 共享使用显式导入、访问状态和所有者引用，没有自动跨队列调度。
- Renderer 在帧提交完成、读回收集之后显式推进 history。Core 不自动推进 system 的时钟，也不允许在途执行期间改变上下文的 history 角色。

原生扩展仍允许 SDK descriptor、加速结构和 swapchain 集成使用 Vulkan 类型；这里是明确的后端接入面，并非另一套后端无关图形 API。
