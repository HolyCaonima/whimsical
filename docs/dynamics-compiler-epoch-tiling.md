# Dynamics：依赖闭包与迭代 epoch（2026-09-12）

在任务开始时已有的未提交优化之上，本轮将大型静态依赖图编译成有界重叠分块。相同 32×32 cloth 输入、Hybrid、4 子步 × 12 迭代，两轮交替对照的 GPU 提交均值为 **2.634 → 2.110 ms，减少 19.9%，约 1.25×**。每 tick 数值 dispatch 从 **252 降到 156**。

这是本轮增量，不与前一份文档不同运行条件下的历史耗时累计计算。用户 DSL、项目文件、公式、自由度与关系定义、着色、Colored/Jacobi 划分、步长和迭代次数均未改变。没有使用 subagent，没有新增或修改测试。

## 从资料到编译边界

- [Ebb 2016，第 6 节](https://graphics.stanford.edu/~mdfisher/papers/ebb.pdf)：关系表示和列式存储由运行时实现，几何领域留在上层。本轮分块只读通用 incidence；端点地址预计算和贡献列布局属于编译器的物理表示选择。
- [Simit 2016，索引表达式融合](https://people.csail.mit.edu/jrk/simit.pdf)：融合索引计算、避免不必要的中间张量物化。本轮在块内直接消费 Jacobi 贡献，保留原 CSR 的归约顺序，不再逐阶段物化完整全局贡献结果。
- [Taichi 的块局部存储](https://docs.taichi-lang.org/docs/performance)及 [MeshTaichi 2022](https://github.com/taichi-dev/meshtaichi)：从访问范围准备局部关系及片上存储，同时评估装入和写回成本。本轮沿实际求解依赖反向求闭包，不要求用户提供网格、分块或邻域提示；不引入几何对象分类。
- [AsyncTaichi 的跨 kernel 状态依赖](https://mingkuan.taichi.graphics/publication/2020-asynctaichi/asynctaichi.pdf)：融合的依据是状态流。这里把整个迭代的读状态和最终写状态分开，保留各块之间的提交边界。
- [Newton/Warp 的固定工作流图捕获](https://docs.nvidia.com/learning/physical-ai/getting-started-with-newton/latest/newton-fundamentals/core-concepts.html)：减少重复启动开销。仓库已有固定提交序列，本轮继续复用它；实际减少的是序列中的全局数值阶段，没有移植 Newton 的对象或预设求解器，也没有实现 Vulkan command buffer 跨 tick 重放。

## 原因与执行图

此前的局部融合要求完整可写连通组件归一个工作组；大的连通图无法进入这条路径。颜色窗口缓解了问题，但窗口边界和 Jacobi 归并仍反复经过全局缓冲区。把整张图强行装进一个工作组，则会损失设备并行度。

新映射在每次迭代中执行：

```mermaid
flowchart LR
    P[独立前缀：每个自由度一个 invocation] --> S[所有块读取同一状态]
    S --> T[块内执行原颜色顺序和 Jacobi 归并]
    T --> O[暂存各块独占的值和乘子]
    O --> C[全局提交 epoch]
    C --> N[下一迭代]
```

预测仍在每子步开始；速度恢复和用户历史更新仍在该子步的全部迭代完成后执行。新路径不对剩余全局求解阶段进行跨迭代融合。

### 分块与反向依赖

`TiledSchedule.cpp` 从通用 incidence 做广度优先分区，每块最多拥有 16 个核心变量。先收集核心变量归并所需的 Jacobi 行，再逆着原颜色阶段找到这些行需要的可写端点。新加入的端点继续向更早颜色追溯，得到完整输入依赖闭包。

这些附加变量和关系是重复计算的边界，不拥有最终写回权。每个变量只有一个核心所属块；每个关系的乘子和诊断也只有一个所属块。因此不同工作组可以独立计算重叠部分，不竞争最终写入位置。

所有块读取当前 epoch 的相同全局状态，在共享内存中执行原先的更新顺序；输出先写 `EpochOutput`，等块全部结束后，第二个 kernel 才更新原 Values/Multipliers。没有用旧边界近似替换原 Gauss–Seidel 依赖，也没有丢弃边界关系。

### 独立前缀与局部存储

如果开头的连续 Colored 阶段中，每行只访问一个可写变量，其更新顺序可以由该变量的一次 invocation 执行。该前缀先统一计算一次，避免昂贵但不扩展依赖范围的公式在每个边界副本中重复执行。其余只读端点保持当前全局输入。

块内端点的局部地址在编译时写入索引表，求解时直接访问。Jacobi 临时贡献按数学类型的阶段做列式布局，列跨度补齐到 32 的倍数。归并仍按原关系 ID、端点槽顺序累加，不引入浮点原子归约。

`KernelFunction` 继续提供同一套残差、Jacobian、逆度量、消元、投影及状态回缩操作。`StateStorage::{Global,Region,Epoch}` 选择访问实现；端点值、贡献和诊断经统一访问器进行。Epoch 路径的 `loadMultiplier` 在每子步第一次迭代读取零；禁用或异常提前退出的关系仍由原初始化及上次已提交结果维护乘子生命周期。

实际只读标记继续参与访问判断：类型级“非统一只读”并不意味着该类型的每个实例端点都可写。数值字段仍从原缓冲区读取，原有 inverseMetric、enabled、参数和 compliance patch 都保留。

## 选择预算和回退

完整局部组件先按原策略融合，再对剩余静态全局工作尝试迭代分块。两种映射只写各自拥有的变量与乘子，互不覆盖；共享只读输入仍服从原子步预测边界。加入一个独立小组件不会直接禁用大图的分块候选。动态端点、deferred 域、直接原子贡献、显式 Active weighting、Global 执行模式以及剩余工作含无可写端点关系的图保留原路径。

后端预算包括：至少 8 个块、每块状态与临时贡献最多 4,096 个 float、源码函数体合计最多 256 KiB、元数据预算 16M 个 word，累计块内关系求值不超过原逻辑关系数的 8 倍。共享内存上限为 16 KiB，符合 [Vulkan 基础最低保证](https://docs.vulkan.org/spec/latest/chapters/limits.html)。这组预算是成本启发式，不是跨设备提速保证。闭包过大时完整回退，不截断依赖。

本轮新增两个固定 buffer 角色 `EpochData` 和 `EpochOutput`，数量不随块数增长。物理 epoch 放在计划的 `iteration` 阶段，和仍可供关系域分析的 `solve/apply` 分开；后续候选分析不会把块元数据误当成关系 ID。Runtime 在每次迭代执行这些阶段，补充元数据只读声明，继续依赖现有反射、固定执行序列和同步管理；RenderCore 未修改。小组件完整融合、动态关联和候选域使用各自已有的存储路径。

`Engine.dynamics.scene.plan(entity)` 新增只读统计：

- `overlapTiles`：选择的重叠块数。
- `overlapSharedBytes`：生成 kernel 每个工作组声明的共享内存容量。
- `overlapEvaluations`：每次迭代块内关系求值总数，含边界副本，不含已提出的独立前缀。

## 对照与验证

Release、RTX 3080、Vulkan validation 开启、无渲染、dt=1/30；每次 180 tick，统计预先固定的 tick 121–180。调用顺序为 baseline / final / baseline / final。基线可执行文件在修改本轮源码前保存，包含任务开始时已有的未提交优化。

| 运行 | tick 121–180 均值 ms | tick 61–180 均值 ms | CPU compile 提交 ms |
| --- | ---: | ---: | ---: |
| baseline 1 | 2.635 | 2.598 | 12 |
| final 1 | 2.343 | 2.414 | 19 |
| baseline 2 | 2.634 | 2.456 | 12 |
| final 2 | 1.878 | 1.750 | 18 |

两轮后段均值为 **2.634 → 2.110 ms（−19.9%）**。额外的依赖闭包和索引准备增加了本次 CPU compile 提交时间；这一列不含 GPU shader 编译，也不是单独的编译器内部计时。

没有锁频。时钟 CSV 显示 P0/P3/P5 切换，最终两轮也有明显波动；这组数据描述当前设备上的提交时间，不能据此承诺每次相同提速，也不是整帧 FPS 或隔离 shader 的速度。

原测量脚本 `captures/cloth-compiler-profile.js` 未改，SHA256 为 `5D2E46386D7577C0775E3176CE5C357565B8C8307B2A16881E308B4CD6D80ED9`。四次运行在 tick 1、60、120、180 的状态摘要完全一致，包括完整数组 sum/sq、前 12 个 float、全结构边平均/最大应变、地面与球面违反量及诊断。这是摘要对照，未宣称完整数组逐位验证。tick 180 平均应变 `0.0032157403308129087`，最大应变 `0.04948957773780835`。

既有 `dynamics_tests` focused checks 通过，覆盖重复端点、数值 patch、快照和迁移、Hybrid、动态关联、多行耦合及候选域生命周期/边界回退。没有新增测试。Release 的 `dynamics_run`、`dynamics_tests`、`Whimsical` 均构建成功。

收尾补充了完整局部组件与剩余全局分块共存的调度，并将物理迭代阶段单独表示。最终构建再次运行原 180-tick 输入，四份状态摘要仍与基线相同；生成的 7 份 shader 源码 SHA256 集合与上述性能测量版本完全一致。记录见 `final-verification.json` 和 `final-shaders.sha256`。没有新增混合分块的专门测试，以上既有检查和目标示例不代表穷尽所有关系图。

真实 ConstraintLab 使用原项目文件，在 1280×800、validation 开启时完成 180 个渲染帧，收到原 cloth 模型的 ECS sample，求解诊断 0/0，退出 validation errors=0。日志另记 366 个 Simulation tick，该计数不等同于 cloth 求解步数，也未用于性能对照。

复现文件在被忽略的 `captures/compiler-sep12-next/`：`measure.ps1`、`baseline.exe`、`final.exe`、`final-before{1,2}.log`、`final-after{1,2}.log`、对应时钟 CSV、`comparison.json`、`final-checks.log`、`final-build*.log`、`app-run.log`。

## 未保留的实验与剩余成本

更小的窗口打包、单 invocation 串行窗口及寄存器缓存、扩大窗口上限、运行时对角矩阵分支、整组件单工作组执行均未显示足够收益，没有留在默认代码中。最终前缀加分块方案每迭代使用三次 dispatch；直接把独立前缀也重复放入边界的原型虽只有两次 dispatch，却增加了求值和访问成本。选择依据同时包含工作量和同步成本。

剩余成本主要是边界重复计算、阶段屏障、参数/历史读取和 epoch 提交。当前一次只编译一个迭代的精确依赖闭包；没有跨迭代时域分块，没有覆盖动态拓扑和稀疏候选执行，也未建立针对每类设备自动调优的成本模型。
