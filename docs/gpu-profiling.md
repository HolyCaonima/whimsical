# GPU 分阶段计时

控制台输入 `profileGPU`，抓取 Render 和 Dynamics 各执行上下文的下一次 GPU 提交。报告按系统分段，分别显示耗时与系统内占比，不再累计一秒内的工作，也不计算跨系统的总占比。

```text
profileGPU
profileGPU render
profileGPU dynamics
profileGPU last
```

不带参数同时抓两个系统；`render` 只抓一帧渲染，`dynamics` 只抓当前各模型的下一次提交。`last` 查看最近报告。结果异步回到控制台和标准输出，并保存到 `captures/gpu-profile.txt`、`captures/gpu-profile.json`。重复请求不会排队。

每个系统单独显示 GPU 区间总和，各模型/执行图显示所采集的 sequence。阶段耗时包含子阶段，不能再次累加。同名阶段按父级路径合并；重复约束迭代显示累计耗时、调用次数和平均耗时，避免数百行重复名称。

Render 帧与 Dynamics 提交独立取样，不保证发生在同一时刻。Dynamics 样本包含该次实际执行的求解、传输或回读；多个模型各取一次提交，其总和不表示单个模拟步的墙钟耗时。GPU 时间戳还包含同步和流水线影响，不代表隔离 shader 的纯运算成本；CPU 等待、Present 和图外原生提交不在报告内。

## 命令行与生命周期

```powershell
.\Projects\ConstraintLab\Run.cmd --profile-gpu
.\Projects\ConstraintLab\Run.cmd --console --exec "profileGPU dynamics"
```

请求只选择发起时已经存在的执行上下文。没有对应上下文时返回空报告；暂停或没有新工作的上下文等待下一次提交。启动时可能采到初始化工作，评估稳定成本应在运行后输入命令。退出或上下文销毁会结束尚未开始的样本并标明报告不完整，不会无限等待。

## 实现与扩展

`RenderCore::requestProfile(request, group)` 选择诊断分组，空字符串选择所有已命名分组。每个目标上下文只采一次，完成后不再开启详细计时。`takeProfile()` 在目标样本完成后返回按分组组织的报告；`endProfile()` 取消尚未开始的样本，保留已在途的数据。

```cpp
rc::GraphContext work(core, registry, "My model", rc::QueueClass::Compute, "My system");
```

分组名称是通用诊断元数据，Core 不识别 Render 或 Dynamics 的业务含义。未指定分组的临时执行上下文不参与系统采集。各上下文持有自己的 query pool 和 fence，由所有者调用 `poll()/wait()` 解析完成数据；Core 只同步请求和报告元数据，不跨线程操作上下文。

RenderGraph 自动为命名 pass 加入 `GpuScope`，原生图内扩展可创建嵌套 scope。普通运行只记录提交首尾时间戳，详细阶段只对被选中的一次提交或显式局部请求启用。`GraphContext::record(ProfileRequest)` / `takeProfile()` 保留单图抓取能力。
