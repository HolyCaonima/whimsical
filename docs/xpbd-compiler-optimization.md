# XPBD 通用编译器执行计划优化

已实现第一阶段：静态依赖分析、封闭区域打包、工作组内共享状态与求解循环融合。优化位于 Compiler / Runtime 内，不包含项目或具体约束名称的判断，RenderCore 不需要理解求解语义。

## 架构

```text
Model 数学定义 + 端点连接 + SolverPolicy
    ↓
Compiler：验证、稳定 SoA 布局、着色、Jacobi CSR、数值阶段
    ↓
Schedule：组件分析 → 容量 / 代码成本 → 区域打包 → 执行映射
    ↓
KernelFunction + 状态访问器 → 全局 kernel / 区域融合 kernel
    ↓
CompiledPlan → Runtime → RenderCore
```

`Kernels.cpp` 生成可调用的变量 / 关系操作，`Schedule.h` 中的 `KernelFunction` 将数学操作与 invocation、工作列表和 dispatch 分开。全局与融合路径使用同一份残差、Jacobian、局部稠密求解、投影和历史更新代码；状态访问器决定读写 SoA 还是区域共享内存。

`Schedule.cpp` 在原有数值阶段之后做执行映射，保留子步和迭代循环。数学 relation 不再必须对应独立 dispatch；融合多个 relation 也不会把它们变成一个更大的稠密方程。

## 依赖与区域

从所有静态端点构造并查集，不构建两两约束冲突图。共享只读变量也连接组件：只读只禁止约束修正，预测仍可受速度 / 加速度驱动，不能让不同工作组分别预测它。

每个工作组拥有若干完整组件。多个小组件按容量打包，避免十万个独立标量约束产生十万个空闲占比很高的工作组。超预算组件留在全局路径，能与融合组件共存。初版不切割大型连通组件；含动态端点的模型整体保留全局路径，因为后续输入可以改变连接范围。

当前后端采用以下与项目无关的编译预算：

- 128 个 lane；每区域最多 128 个变量、512 个关系。
- 最多 2048 个共享状态 float（8 KiB）；实际声明按该计划最大区域所需容量生成。
- 局部求解显式临时数组估算不超过 2048 个 float。
- 融合操作源代码总量不超过 256 KiB，只计实际参与局部区域的操作。

这些是编译启发式预算，不是设备占用率的精确预测。宽状态、多类型或高复杂度模型可能选择全局路径；后续可扩展目标成本模型。

## 执行与内存

每个区域在 tick 开始时加载值、前值、速度、history 和乘子到共享内存，执行完整子步与迭代循环，结束后写回原 SoA 地址。加速度、逆度量、参数、compliance、enabled 和端点表继续使用当前 GPU 数据，不按初始数值折叠。

阶段顺序保持为预测 → 乘子清零 → 多轮约束求解 / Jacobi 归并 → 速度恢复 → 历史提交。同一颜色 / 类型中的原有工作顺序不变。所有 lane 都经过区域屏障；某个数学操作中的奇异或非有限诊断只退出该操作。

Jacobi 贡献保留全局 CSR 布局，使用 coherent 访问与贡献阶段后的 buffer barrier；其余迭代状态使用共享内存和工作组 barrier。融合与全局组件没有端点交叉。全局部分先执行，融合部分随后执行，避免原有全局乘子清零覆盖融合区域的最终乘子。

变量 / 关系集合的稳定 ID、公开字段偏移、范围更新、回读和迁移契约保持一致。新增三个固定 buffer 角色保存区域阶段范围、共享状态布局和局部偏移；buffer 数量不随组件数量增长。被完整吸收的独立 kernel 不再编译。

## 使用与统计

```cpp
SolverPolicy policy;                       // execution 默认 Auto
policy.execution = ExecutionMode::Global; // 保留原全局路径，用于对照或主动选择
```

```javascript
Engine.xpbd.compile(handle, {mode:'hybrid', substeps:4, iterations:12, execution:'auto'});
```

`mode` 选择 Colored / Jacobi / Hybrid 数值方法，`execution` 选择执行映射。融合不减少子步或迭代次数，也不改变柔顺度、松弛、别名梯度合并、多行块求解和乘子投影语义。浮点后端可能产生低位差异，不承诺逐位一致。

`PlanStatistics` 新增：

- `components`、`localRegions`：静态组件与打包后的工作组数。
- `localVariables`、`localRelations`：映射到融合区域的工作量。
- `localSharedBytes`：融合 kernel 每工作组声明的共享内存。
- `referenceDispatches`、`dispatches`：每步数值阶段的原始 / 实际计算 dispatch 数，不含拓扑构建及数据传输。

Global 和动态拓扑路径跳过区域分析，其区域统计为零。

## 现有验证与实测

遵照开发者要求，没有新增或修改测试。Release 构建 `xpbd_tests`、`xpbd_run`、`Whimsical` 成功。现有 `xpbd_tests` 通过，覆盖：10 万条独立标量别名关系（Colored 和 Jacobi）、数值补丁、发布快照与迁移、大型高关联组件的 Hybrid 回退、动态端点，以及多行耦合求解与历史更新。10 万条关系的两条路径均只需一个融合 kernel。

实际应用使用原有 `ConsoleSmoke` 驱动 profile，RTX 3080，1440×900，同一 Release 构建、同一 ConstraintLab 初始模型（64 变量 / 255 关系）、Hybrid、4 子步 × 12 迭代。只临时切换执行选项，运行后恢复项目文件。

| 执行方式 | 每步计算 dispatch | 每步计算阶段时间戳区间总和 |
| --- | ---: | ---: |
| Global | 400 | 2.493 ms |
| Auto，共享内存融合 | 1 | 0.840 ms |

本次样本计算阶段约快 2.97 倍，时间减少约 66%。两次应用均收到 GPU 状态，初次求解诊断为 0 / 0，退出时 Vulkan validation errors = 0。此前仅融合调度、仍逐阶段同步全局缓冲区的中间版本约 2.4 ms，最终采用共享状态版本。

数据来自分别采集的一秒窗口：Global 16 个计算步、Auto 15 个计算步，按计算阶段区间总和除以对应计算步数归一化。统计排除回读与图外开销，不是整帧提速，也不是隔离 shader 基准。保留结果：`captures/xpbd-compiler-comparison.json`、`xpbd-compiler-global.log`、`xpbd-compiler-shared.log`、`xpbd-compiler-checks.log`。

没有新增针对所有空间 / 投影组合的等价性测试；本轮使用既有数值验证、代码审查和实际应用采样。共享内存调度的吞吐收益依赖模型规模和硬件，不由 dispatch 数量单独保证。

## 后续可独立推进

大组件的依赖分区与边界阶段、跨类型全局批次融合、结构化数学签名与稀疏导数、静态执行图复用仍未实现。这些可以沿数学操作与执行映射的分层继续扩展，不需要加入项目约束类型。
