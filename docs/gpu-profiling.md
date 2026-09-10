# GPU 分阶段计时

控制台输入 `profileGPU`，采集随后约 1 秒内通过同一个 RenderCore 的所有 GraphContext 记录并提交的 GPU 工作。渲染、Dynamics 和其他计算图自动参与，不需要把请求逐个转发给子系统。

```text
profileGPU
profileGPU last
```

报告异步回到控制台和标准输出，并覆盖保存 `captures/gpu-profile.txt`、`captures/gpu-profile.json`。`last` 只查看最近的报告；重复请求不会排队。采样结束后，RenderCore 等待被采集的提交完成再发布结果，不要求各子系统主动提取自己的计时。

报告列出每个执行图及其阶段的：

- GPU 时间戳区间总和（inclusive ms）。
- 占所有被采集提交的区间总和的比例。
- 执行次数，以及每次的平均区间耗时。

同名阶段按父级路径归并，所以多次迭代的 `Colored distance` 会显示累计耗时和实际调度次数，不会打印数百行重复名称。子阶段已包含在父阶段中，不能再次累加。渲染帧、模拟步和回读的提交频率不同，不能把执行图平均提交耗时直接当成每帧成本。Dynamics 图同时包含求解和回读提交，可通过 `Predict` 等阶段的次数进一步区分。

根节点显示的是 **GPU 提交区间的总和**，不是现实经过的时间或 GPU 利用率。时间戳区间包含 GPU 同步和流水线影响，不代表隔离 shader 的纯运算成本。CPU 等待、Present，以及绕过 GraphContext 的原生上传 / 资源构建提交不在报告内。

## 命令行与生命周期

```powershell
.\Projects\ConstraintLab\Run.cmd --profile-gpu
.\Projects\ConstraintLab\Run.cmd --console --exec "profileGPU"
```

启动时的请求可能覆盖初始化，长时间的 CPU 编译也会占用采样窗口。评估稳定成本，应在运行后输入命令。有限帧退出或关闭窗口时，应用提前结束采样并收集已有提交，报告使用实际缩短后的窗口时长；不会假装采满一秒。没有工作被提交的窗口可以返回空报告。

## 实现与扩展

`RenderCore::requestProfile(request, windowMilliseconds)` 发起采样；`takeProfile()` 非阻塞轮询所有执行图的 fence 并汇总报告；`endProfile()` 可提前关闭窗口。请求和结果仍通过应用的跨线程值对象传递，Renderer 不再接收或收集全局 GPU profile 请求。

每个执行图可提供诊断名称：

```cpp
rc::GraphContext work(core, registry, "My compute system");
// Declare passes, compile, record and submit as usual.
// Participation in a RenderCore capture is automatic.
```

RenderGraph 自动为命名 pass 加入 `GpuScope`，原生图内扩展可创建嵌套 scope。报告因此包含 Renderer 的 GBuffer、TLAS、ReSTIR、NRD 子阶段，以及 Dynamics 的预测、约束迭代、速度恢复、历史提交、数据传输等实际执行分支。

各 GraphContext 仍拥有独立 query pool 和 fence；RenderCore 负责采样决策、完成收集和汇总，不把不同系统的执行节奏绑在一起。查询池按页增长，不再因超过 256 个 scope 截断报告。普通运行仅记录每次提交的首尾两个时间戳，详细阶段标记只在采样窗口或显式局部请求时启用。

`GraphContext::record(ProfileRequest)` / `takeProfile()` 保留单图显式抓取能力，可以与全局采样同时使用。计时结果在 fence 完成后解析，采样不增加 `vkDeviceWaitIdle` 或逐 scope 等待。时间戳仍按设备 `timestampValidBits` 处理回绕，再乘 `timestampPeriod` 转换为毫秒。
