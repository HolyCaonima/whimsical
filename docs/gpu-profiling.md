# GPU 分阶段计时

控制台输入 `profileGPU`，抓取下一次实际提交的 GPU 帧。名称不区分大小写，支持现有模糊候选和方向键选择。报告异步回到游戏内控制台及标准输出，并覆盖保存最新的 `captures/gpu-profile.txt`、`captures/gpu-profile.json`。

```text
profileGPU
profileGPU last
```

`last` 查看最近已完成的报告，不重新抓取。已有请求未完成时重复输入不会排队多次。窗口最小化时请求保留，恢复渲染后抓取。地图重载和关闭 HUD 不会丢失请求。控制台 PageUp/PageDown 可以浏览完整结果。

从启动命令行请求：

```powershell
.\RunHoneybud.cmd --profile-gpu
.\RunHoneybud.cmd --console --exec "profileGPU"
.\build\bin\Release\Afterlight.exe --map /Game/Maps/HoneybudCourt --frames 1 --profile-gpu
```

启动请求测量首帧，可能包含历史初始化和 TLAS 首次构建。评估稳定运行时的成本，应在场景运行后输入控制台命令。这里只报告 GPU 时间戳区间，不把 CPU 提交时间或 FPS 换算成 GPU 耗时。

## Scope 覆盖

| Scope | 测量范围 |
|---|---|
| GPU Frame | 本帧主命令缓冲的 GPU 时间戳区间 |
| Initialize History / Reservoirs | 首帧或资源重新创建时的清理 |
| HUD Texture Upload | HUD 需要更新时的 GPU 图像拷贝 |
| Skinned BLAS Refit | 蒙皮几何变化后的 BLAS 更新 |
| Acceleration Structures → TLAS Build / Refit | TLAS 构建或更新及后续同步 |
| GBuffer Raster | GBuffer 光栅化 |
| ReSTIR Initial DI + Secondary GI + Specular | 初始采样 |
| ReSTIR Temporal + Spatial Reconnection | 时空复用 |
| Visibility + Radiance Resolve | 可见性和辐射求解 |
| NRD RELAX Diffuse Specular → 每个 NRD dispatch | 降噪整体及库提供的各个子阶段 |
| Composition + Tone Map + HUD | 合成、色调映射和 HUD 合成 |
| History Store | 历史资源拷贝 |
| Audit Readback / Screenshot Readback | 对应功能开启时的 GPU 回读拷贝 |
| Swapchain Blit / Present Transition | 交换链图像拷贝及布局转换 |

条件阶段只在实际记录时出现。报告显示 inclusive ms 和占整帧比例，子阶段已包含在父阶段中，不应再次累加。计时包含该区间已有的同步与可能的队列等待，不等于隔离运行某个 shader 的纯计算耗时；不测量 CPU 蒙皮、资源创建、初始化阶段单独提交的上传／静态 BLAS 构建，也不包含 `vkQueuePresentKHR` 的调用或显示耗时。

## 实现与扩展

`GpuScope` 以 RAII 记录开始和结束标记，支持嵌套。RenderGraph 自动为每个已命名 pass 加 Scope，NRD 按 dispatch 名称生成子 Scope；以后新增 graph pass 自然进入报告。图外记录可直接使用：

```cpp
{
    GpuScope scope(profiler, command, "My GPU Pass");
    // Record GPU commands here.
}
```

无抓取请求时仅保留整帧两个时间戳；详细 Scope 时间戳只写入被请求的那一帧。设备支持 `VK_EXT_debug_utils` 时，Scope 同时生成 RenderDoc 等工具可见的标记，Release 无需开启验证层。

Profiler 由渲染线程持有。请求序号随不可变 Frame 发布，相同快照重复渲染不会重复抓取；结果在既有帧 fence 完成后读取，通过互斥保护的值对象交给主线程。没有为抓取增加 `vkDeviceWaitIdle` 或逐 Scope 等待。有限帧运行会在最后一帧已有的 fence 等待后解析结果。

时间戳按队列的 `timestampValidBits` 处理回绕，再乘 `timestampPeriod` 转为毫秒。队列不支持时间戳时明确报告不支持；Scope 数超出 256 个时标记报告不完整。参见 [Vulkan 时间戳查询](https://docs.vulkan.org/samples/latest/samples/api/timestamp_queries/README.html)。

`gpu_profile` 测试验证单位转换、32/64 位回绕、层级百分比和 JSON 转义。`--console-smoke` 还通过 Win32 输入执行两次抓取，覆盖重复请求、HUD 关闭及地图重载后的使用；运行日志和 JSON 可用于检查实际 Vulkan 时间戳结果。
