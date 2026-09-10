# Dynamics 通用编译器执行计划优化

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

阶段顺序保持为预测 → 乘子清零 → 多轮约束求解 / Jacobi 归并 → 速度恢复 → 历史提交；乘子清零后来融合进第一次关系求解，不再形成独立阶段。所有 lane 都经过区域屏障；某个数学操作中的奇异或非有限诊断只退出该操作。

Jacobi 贡献保留全局 CSR 布局，使用 coherent 访问与贡献阶段后的 buffer barrier；其余迭代状态使用共享内存和工作组 barrier。融合与全局组件没有端点交叉，两条执行路径的先后顺序不形成数据依赖。

变量 / 关系集合的稳定 ID、公开字段偏移、范围更新、回读和迁移契约保持一致。新增三个固定 buffer 角色保存区域阶段范围、共享状态布局和局部偏移；buffer 数量不随组件数量增长。被完整吸收的独立 kernel 不再编译。

## 使用与统计

```cpp
SolverPolicy policy;                       // execution 默认 Auto
policy.execution = ExecutionMode::Global; // 保留原全局路径，用于对照或主动选择
```

```javascript
Engine.dynamics.compile(handle, {mode:'hybrid', substeps:4, iterations:12, execution:'auto'});
```

`mode` 选择 Colored / Jacobi / Hybrid 数值方法，`execution` 选择执行映射。融合不减少子步或迭代次数，也不改变柔顺度、松弛、别名梯度合并、多行块求解和乘子投影语义。浮点后端可能产生低位差异，不承诺逐位一致。

`PlanStatistics` 新增：

- `components`、`localRegions`：静态组件与打包后的工作组数。
- `localVariables`、`localRelations`：映射到融合区域的工作量。
- `localSharedBytes`：融合 kernel 每工作组声明的共享内存。
- `referenceDispatches`、`dispatches`：每步数值阶段的原始 / 实际计算 dispatch 数，不含拓扑构建及数据传输。

Global 和动态拓扑路径跳过区域分析，其区域统计为零。

## 现有验证与实测

遵照开发者要求，没有新增或修改测试。Release 构建 `dynamics_tests`、`dynamics_run`、`Whimsical` 成功。现有 `dynamics_tests` 通过，覆盖：10 万条独立标量别名关系（Colored 和 Jacobi）、数值补丁、发布快照与迁移、大型高关联组件的 Hybrid 回退、动态端点，以及多行耦合求解与历史更新。10 万条关系的两条路径均只需一个融合 kernel。

实际应用使用原有 `ConsoleSmoke` 驱动 profile，RTX 3080，1440×900，同一 Release 构建、同一 ConstraintLab 初始模型（64 变量 / 255 关系）、Hybrid、4 子步 × 12 迭代。只临时切换执行选项，运行后恢复项目文件。

| 执行方式 | 每步计算 dispatch | 每步计算阶段时间戳区间总和 |
| --- | ---: | ---: |
| Global | 400 | 2.493 ms |
| Auto，共享内存融合 | 1 | 0.840 ms |

本次样本计算阶段约快 2.97 倍，时间减少约 66%。两次应用均收到 GPU 状态，初次求解诊断为 0 / 0，退出时 Vulkan validation errors = 0。此前仅融合调度、仍逐阶段同步全局缓冲区的中间版本约 2.4 ms，最终采用共享状态版本。

数据来自分别采集的一秒窗口：Global 16 个计算步、Auto 15 个计算步，按计算阶段区间总和除以对应计算步数归一化。统计排除回读与图外开销，不是整帧提速，也不是隔离 shader 基准。保留结果：`captures/xpbd-compiler-comparison.json`、`xpbd-compiler-global.log`、`xpbd-compiler-shared.log`、`xpbd-compiler-checks.log`。

没有新增针对所有空间 / 投影组合的等价性测试；本轮使用既有数值验证、代码审查和实际应用采样。共享内存调度的吞吐收益依赖模型规模和硬件，不由 dispatch 数量单独保证。

## 后续可独立推进

大组件的完整共享状态切割与边界阶段、静态执行图复用仍未实现。跨类型全局批次、结构化数学图裁剪和大组件的有界颜色窗口已沿数学操作与执行映射的分层实现，不需要加入项目约束类型。

## 小矩阵代码生成与只读输入（2026-09-10）

本轮保持用户定义自由度、关系条件和连接的边界；不识别项目名称或关系名称，不修改公式、柔顺度、子步数、迭代数或着色顺序。

- `Kernels.cpp` 对不超过 4 行的局部矩阵生成固定下标的部分选主元消元代码，使 GPU 编译器能够将小矩阵标量化。主元选择、消元顺序、奇异判断和诊断保持原有规则；更大矩阵继续使用通用循环，控制生成代码体积。
- `Instance.cpp` 把 GPU kernel 不写入的模型字段和编译表声明为 `readonly`。这包括逆度量、参数、柔顺度、启用字段、端点和布局表。它们仍可在提交之间通过原有上传接口更新；当前值、速度、加速度、关系历史、内部乘子及动态关联表仍保留各自的写入路径。

评估使用真实 ConstraintLab，16 条绳子、1,024 个变量、4,080 条关系，Hybrid、4 子步 × 12 迭代，Release、RTX 3080、1440×900、VSync 与 Vulkan validation 开启。每次运行 480 个渲染帧，临时记录完成步的 `gpuMilliseconds`，退出后恢复项目脚本。比较固定的模拟步 20–120，排除初始编译并保持相同的模拟进度。

这里的耗时与界面“GPU 求解耗时”同源，是一次求解提交的 GPU 首尾时间戳区间，包含该提交的传输、同步及流水线影响，不是隔离的纯 shader 算术耗时，也不是整帧 GPU 耗时。完整采样及重复运行统计见 `captures/dynamics-performance-comparison.json`。

| 对照轮次（各 101 个相同 tick） | 优化前平均 | 优化后平均 | 耗时减少 |
| --- | ---: | ---: | ---: |
| 第一轮 | 1.51 ms | 0.73 ms | 约 52% |
| 重复运行 | 0.97 ms | 0.72 ms | 约 26% |

运行间的 GPU 提交计时存在波动，因此保留两轮结果，不把单次最好值当作稳定加速比。两轮模型与求解设置相同，优化版本均完成 480 帧并报告 `validation errors=0`。临时尝试的全量共享缓存和阶段范围提前加载没有带来收益，未保留在实现中。

没有新增测试。使用现有 `dynamics_tests` 检查数值求解、别名端点、参数更新、迁移、发布、动态关联表和关系历史，并运行实际项目确认 GPU 回读及 validation 诊断。优化保持数学过程，浮点后端仍不承诺逐位一致；以上收益限于记录的模型与设备。

## 数学图专门化、颜色窗口与稀疏写入（2026-09-10）

本轮继续保持 `Formula + 导数需求 + 端点拓扑 + 后端预算` 的编译边界。自由度、关系条件、端点和公式仍由用户层定义；编译器没有项目对象或具体关系名称的分支。

### 数学图

- `FormulaGlsl` 在编译副本上执行常量折叠、零 / 一恒等式、双重负号消除、交换操作数规范化和第二次 CSE，并在化简后重新裁剪失活节点。Model 中的原始 Formula 不变。
- 代码生成可以只输出 Jacobian。求解阶段将 retract 的切向线性化与值映射拆开，避免求导时计算未消费的值，也避免应用修正时重算 Jacobian。
- 编译器识别常量 residual / retract Jacobian。单端点且完整 Jacobian 为常量时，直接生成稀疏的 `WJᵀ` 与 `JWJᵀ` 表达式，不物化 `rawJ`、`j`、`tangent` 中已知的零项。
- 每个子步或颜色窗口只计算一次 `1 / h²`。同色的局部同构批次直接调用专用函数，类型批次之间不插入无意义的 barrier；只有依赖颜色边界保留同步。

### 静态颜色窗口

完整连通组件超过共享状态预算时，Schedule 不再只能逐颜色提交。它会在最多四个连续颜色上构造由可写端点诱导的关系组件；共享只读端点不制造伪依赖。总关系数不超过后端预算的组件留在同一 workgroup，互不相干的组件线性打包到同一工作组范围。

窗口内保持原颜色顺序，颜色之间使用 buffer memory barrier 与 workgroup barrier；窗口之间仍由正常 dispatch 建立设备范围可见性。同色关系原本就不共享可写自由度，因此可以并行。动态端点无法在编译期证明这一条件，`ExecutionMode::Global` 也明确作为对照路径，两者均跳过颜色窗口。

窗口使用原关系函数或跨类型 dispatch 函数，不复制数学实现。`PlanStatistics::colorWindows` 和 `colorWindowRegions` 记录最终映射。ConstraintLab 的 32×32 大连通模型（1,024 variables / 8,898 relations，Hybrid，4×12）中，每步彩色求解调用从 576 次降为 240 次；一次 Release / RTX 3080 profile 中，彩色求解区间从 7.020 ms 降到 3.773 ms，约减少 46.3%。完整提交样本为 9.831 ms 与 5.168 ms，但渲染与计算共用 GPU 队列，整提交数字存在运行间波动。

Global 与 Auto 在模拟时间 1.000000052 s 的结构应变和抽取的 12 个状态 float 完全一致；两条路径均为 diagnostics 0 / 0、Vulkan validation errors 0。该对照验证的是实际项目中的窗口重排；它没有替代新的单元测试。

### Packed state scatter

大量 `TickInput::writes` 以前按每个 SoA 列生成一个 transfer pass。Runtime 现在先验证全部输入，按 `(StateField, GPU word address)` 打包，并在 CPU 上保持重叠写入的 last-write-wins 语义，再通过一个只含通用字段编号、地址和值的 compute scatter 写入 Value / Velocity / Acceleration / History。

少于 8 个原始列 pass 的小更新继续走直接 range upload。打包表最多容纳 4,096 个最终赋值；超过容量时自动回到原路径，因此计划不会为最坏情况复制整份状态存储。该表是固定的单一 buffer role，模型或实例数量不会改变 shader 接口数量。

64 个独立区域的真实项目样本原来产生 192 次 `Update Dynamics q`，耗时 0.964 ms；packed 路径变为一次 0.006 ms 表上传和一次 0.009 ms scatter。对应完整提交样本从 1.691 ms 降到 0.833 ms。移动端点实测在 1 s 时写入预期位置，diagnostics 与 Vulkan validation 均无错误。

## 导数投影与计算图裁剪（2026-09-10）

公式仍由用户层定义，编译器只根据求解操作实际消费的数学量投影导数：relation residual 只生成端点状态列，space retract 只生成切向增量列。参数、历史、步长和时间仍参与 residual 值计算，但不再生成永远不会被求解器读取的导数。

`FormulaGlsl` 在每个输出的反向传播图上进一步做静态可达性分析，只为能够到达所请求输入的节点分配 adjoint 并生成传播指令。分支选择、非光滑操作的导数约定和未选分支的惰性求值保持不变。输出 Jacobian 改为紧凑行主序，relation 和 retract 的局部临时数组及下标随之收缩；`Schedule` 的临时空间成本估算使用同一投影形状，避免因已不存在的导数列错误拒绝局部区域融合。

这一层只有 `Formula + requested derivative inputs → specialized kernel`，没有关系名称、项目类型或高层对象判断。Release 编译、现有 `dynamics_tests` GPU 数值检查以及 ConstraintLab 真实运行均通过；后者完成 333 个模拟 tick，诊断为 0 / 0，Vulkan validation errors 为 0。

## 成本感知着色与乘子初始化融合（2026-09-10）

本轮借鉴 Taichi 将前端数学定义先降为 IR、再按状态访问关系形成后端任务的分层方式。可迁移的是状态流、任务融合和无用全局访存消除，而不是任何项目对象语义。编译器仍只接收 `Formula`、可写端点 incidence、数值规模和用户给定预算；高层对象名称与用途不会进入排序、着色或 kernel 生成。

关系实例现在先完整 lower，再执行有界着色。Hybrid 的确定性优先级依次为：唯一可写端点较少、残差行数较多、公式 DAG 较大、切空间较大、稳定 relation ID 较小。随后仍使用 `colorBudget` 内的 first-fit；无法放入的实例继续进入 Jacobi，动态端点和显式 Jacobi 不参与静态着色。这样把有限颜色优先留给单位 incidence 成本较低、数值工作较重的关系，并保持同一输入必然生成同一计划。该顺序是通用后端成本启发式，会改变 Hybrid 的 Gauss-Seidel/Jacobi 划分，但不改变公式、端点、子步或迭代次数。

XPBD 乘子清零不再是每个子步一次的独立全 buffer kernel。每个非 history-update 的关系函数在第一次迭代、enabled 判断之前清零自己的乘子行，因此禁用关系也维持原清零语义；全局批次、颜色窗口和共享区域使用同一个生成函数。关系在计划中恰好属于一个 solve 路径，且求解公式不会读取其他关系的乘子，所以这项 task fusion 删除了全局写 pass 及其 dispatch，而没有引入关系间依赖。

实测使用 ConstraintLab 的 32×32 stage：1,024 variables / 8,898 relations，Hybrid、4 子步 × 12 迭代、`colorBudget=12`，Release、RTX 3080、Vulkan validation、immediate present。每次运行 480 个渲染帧，统计完成 tick 5–40 的 `gpuMilliseconds`；它是一次 dynamics 提交的 GPU 时间戳区间，不是隔离 shader 或整帧时间。

- 原计划：6,318 colored / 2,580 Jacobi，reference / actual dispatch 为 1,792 / 400，均值 5.390273 ms。
- 成本感知着色：7,578 colored / 1,320 Jacobi，688 / 352 dispatch；重复均值 3.983035 / 4.012119 ms。
- 再融合乘子初始化：684 / 348 dispatch；重复均值 3.570301 / 3.479593 ms。

最终两次均值为 3.524947 ms，相对原样本减少约 34.6%；实际计算 dispatch 减少 13%。两次最终运行的 diagnostics 均为 0 / 0，Vulkan validation errors 为 0。运行间存在 GPU 队列波动，因此这些数字只说明记录设备和模型上的收益，不作为跨模型保证。

没有保留两项负收益实验：把整个大连通分量放入单个 persistent workgroup 的均值为 11.749680 ms；放大颜色窗口预算也没有稳定收益。后端预算继续保持有界，不为某种高层对象硬编码特例。

参考的 Taichi 资料：

- <https://docs.taichi-lang.org/docs/compilation>
- <https://docs.taichi-lang.org/docs/performance>
- <https://arxiv.org/abs/2012.08141>
