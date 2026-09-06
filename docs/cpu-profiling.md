# CPU 分阶段计时

控制台输入 `profileCPU` 抓取下一次 Game 更新及消费该请求快照的渲染帧；`profileCPU last` 重看最近报告。命令名支持控制台已有的模糊搜索和上下方向键候选。抓取未完成前再次输入只提示已有请求，不重复排队。关闭 HUD 不影响命令。

```powershell
.\build\bin\Release\Afterlight.exe --map /Game/Maps/HoneybudCourt --profile-cpu
.\build\bin\Release\Afterlight.exe --frames 1 --capture --exec "profileCPU"
```

结果写入控制台、标准输出，以及 `captures/cpu-profile.txt` / `captures/cpu-profile.json`，后一次覆盖前一次。启动命令抓取的 Game 树只有初始快照准备，渲染器启动初始化不在采样内；评估稳定的逻辑更新应在运行后执行命令。有限帧模式也会收集最后一帧的结果；未完成请求在退出时明确提示。

## 读数含义

- **Inclusive ms**：本阶段的墙钟耗时，包含嵌套子阶段和阻塞时间。
- **Self ms**：扣除直接子阶段后剩余的耗时，不是该函数所有机器指令的独占 CPU 周期。
- **Game**：从下一次主线程固定步更新到场景快照构建完成；可能包含多次补帧 tick。包含控制台命令、脚本输入/拾取、JS fixedUpdate、动画求值/根运动/关节碰撞代理，以及 World Snapshot。
- **Render**：该渲染帧的主机执行区间。单独列出上一帧 GPU fence、交换链 acquire、Present 等待；`Prepare / Record / Submit` 对应实时 `CPU (Render)` 的测量范围。

Render 的准备阶段进一步列出 CPU 蒙皮与模型绑定、场景/材质上传、HUD/控制台绘制、命令录制、队列提交。录制树包含 BLAS、TLAS、GBuffer、ReSTIR、Resolve、NRD 各 dispatch、合成、历史拷贝和截图等实际执行分支；这里测的是 **CPU 录制命令**，GPU 执行成本用 `profileGPU` 查看。

Game 与 Render 是异步线程，不能把两棵树相加当成帧时间。Game 树对应产生请求的更新，若渲染线程跳过了中间快照，Render 会使用保留相同请求的最新快照；报告中的 game tick 标识采样的 Game 更新。采样不包含主线程消息泵/休眠、快照队列等待和渲染器启动初始化。同步资源上传与驱动内部等待仍属于所在 CPU 阶段；它不是操作系统 CPU 利用率或纯计算周期采样。

实时 HUD、标题栏、`stat` 每半秒显示平均 FPS / Frame / CPU (Render) / GPU。`CPU (Render)` 排除常规 fence/acquire/Present 等待；抓帧 JSON 同时保存 `cpuRenderAverageMs`。帧率预热、resize 后重置与现有统计一致。

## 扩展

在需要分析的引擎模块中使用线程局部的 RAII scope：

```cpp
#include "core/CpuProfile.h"
void updateSomething() {
    CpuScope scope("Update Something");
    // 工作和更细的嵌套 CpuScope
}
```

`CpuProfiler` 为当前线程绑定一次采样，`finish()` 关闭根节点并产生值对象。`CpuScope` 随作用域退出结束，也支持 `finish()` 提前结束。未启用抓取时 scope 不分配内存、不读取时钟；实时统计只保留每帧一对时间读数。模块不依赖控制台，不使用全局共享计时栈。主线程的不可变采样结果随 Frame 发布，渲染线程追加自身树，再通过互斥保护的结果交回主线程；同一请求序号只消费一次。
