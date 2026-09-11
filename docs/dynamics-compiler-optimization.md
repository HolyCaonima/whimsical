# Dynamics 通用编译器执行计划优化

当前实现保留高层连接域，再按读写效果、数学需求和后端容量选择导数专门化、端点布局及区域融合。优化位于 Compiler / Runtime 内，不包含项目或具体约束名称的判断，RenderCore 不需要理解求解语义。下文架构描述当前状态，后续各节保留各轮优化的历史实测。

## 架构

```text
Model 数学定义 + 对象字段 / pair + SolverPolicy
    ↓
BindingIR：集合域 / 索引映射 + 结构只读效果
    ↓
Compiler：按需实例遍历、稳定 SoA 布局、着色、Jacobi CSR、数值阶段
    ↓
Schedule：组件分析 → 容量 / 代码成本 → 区域打包 → 执行映射
    ↓
KernelFunction + 导数需求 / 状态访问器 → 全局 kernel / 区域融合 kernel
    ↓
端点布局：紧凑索引描述 / 显式表 → 专用 GPU 地址计算
    ↓
CompiledPlan → Runtime → RenderCore
```

`Kernels.cpp` 生成可调用的变量 / 关系操作，`Schedule.h` 中的 `KernelFunction` 将数学操作与 invocation、工作列表和 dispatch 分开。全局与融合路径使用同一份残差、Jacobian、局部稠密求解、投影和历史更新代码；状态访问器决定读写 SoA 还是区域共享内存。

`Schedule.cpp` 在原有数值阶段之后做执行映射，保留子步和迭代循环。数学 relation 不再必须对应独立 dispatch；融合多个 relation 也不会把它们变成一个更大的稠密方程。

## 依赖与区域

从静态可写端点构造并查集，不构建两两约束冲突图。BindingIR 的仿射笛卡尔积可直接按两侧成员分析连接，无需逐组合重复合并。共享只读变量不会合并写组件：只读只禁止约束修正，预测仍可受速度 / 加速度驱动，因此共享输入由全局每子步预测一次，各区域消费预测后的值。

每个工作组拥有若干完整写组件及其独占只读输入。多个小组件按容量打包，避免十万个独立标量约束产生十万个空闲占比很高的工作组。超预算组件留在全局路径，能与融合组件共存。完整共享状态融合尚不切割大型连通组件；含动态端点的模型整体保留全局路径，因为后续输入可以改变连接范围。

当前后端采用以下与项目无关的编译预算：

- 128 个 lane；每区域最多 128 个变量、512 个关系。
- 最多 2048 个共享状态 float（8 KiB）；实际声明按该计划最大区域所需容量生成。
- 局部求解显式临时数组估算不超过 2048 个 float。
- 融合操作源代码总量不超过 256 KiB，只计实际参与局部区域的操作。

这些是编译启发式预算，不是设备占用率的精确预测。宽状态、多类型或高复杂度模型可能选择全局路径；后续可扩展目标成本模型。

## 执行与内存

封闭区域在 tick 开始时加载值、前值、速度、history 和乘子到共享内存，执行完整子步与迭代循环，结束后写回原 SoA 地址。若局部区域消费跨组件共享输入，当前计划统一改为每子步加载和写回，融合子步内部的迭代；外部只读状态从全局读取。加速度、逆度量、参数、compliance、enabled 和端点映射继续使用当前 GPU 数据，不按初始数值折叠。

阶段顺序保持为预测 → 乘子清零 → 多轮约束求解 / Jacobi 归并 → 速度恢复 → 历史提交；乘子清零后来融合进第一次关系求解，不再形成独立阶段。所有 lane 都经过区域屏障；某个数学操作中的奇异或非有限诊断只退出该操作。

Jacobi 默认保留全局 CSR 布局；静态标量不等式在 Auto 下可改用自由度槽直接稀疏累加。两条路径都在贡献阶段后建立 buffer 可见性；其余迭代状态使用共享内存和工作组 barrier。融合与全局组件不共享可写端点；共享只读输入的预测依赖由每子步的全局预测 → 局部融合顺序保证。只含只读输入的关系保留全局执行。

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
- `directJacobiRelations`：使用自由度槽直接稀疏累加的 Jacobi 关系实例数。

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

## Newton 对照后的 Hybrid 调度（2026-09-10）

Newton 的 VBD 路径在 model finalize 后使用预计算颜色组，支持颜色组均衡与 CUDA tile solve；CUDA graph capture 则用于固定工作流的重复提交。可迁移到 Dynamics 的原则是：拓扑元数据在编译期确定、GPU 工作组保持足够饱和、不要让颜色同步成本掩盖关系求值本身。不能迁移的是 Newton 求解器中的 particle、mesh、contact 等对象分类；Dynamics compiler 仍只观察 `Space`、`RelationType`、唯一可写端点 incidence 和 `SolverPolicy`。

实际 profile 表明当前 Runtime 已把一个 model tick 记录到单个 command buffer 并单次提交，CPU 提交不是本轮主瓶颈。32×32 ConstraintLab 模型（1,024 variables / 8,898 relations，4 子步 × 12 迭代，`colorBudget=12`）中，原计划每步有 192 次颜色窗口调用；它们占一次详细 profile 的约 67%。盲目切到全 Jacobi 虽把稳定 GPU 区间降到约 1.11 ms，但同一 tick 的结构应变从 `0.00429` 增至 `0.03404`，因此没有采用。

Hybrid 现在把 `colorBudget` 明确作为上限。first-fit 先照常产生候选颜色；如果静态拓扑已经必然存在 Jacobi overflow，compiler 再选择满足以下条件的最短颜色前缀：每个参与关系的可写自由度，至少一半静态 incidence 仍由原位 Colored 更新。余下候选颜色并入已经存在的 Jacobi relation/gather 阶段。若用户选择 Colored、拓扑可动态变化、没有既存 overflow，或给定预算无法达到覆盖条件，计划完全保留原颜色结果。

这个截止规则只使用端点 incidence，不读取关系名称、项目名称、初始数值或 enabled 当前值。关系公式、导数、compliance、迭代数、子步数和颜色内依赖规则不变。`PlanStatistics::candidateColors` 记录截止前颜色数，`colors` 记录实际执行颜色数，便于检查计划选择。

在上述模型上，计划自然从 12 个候选颜色选择 8 个执行颜色：

- Colored / Jacobi relation 从 `7,578 / 1,320` 调整为 `5,624 / 3,274`。
- 颜色窗口调用从每步 192 次降为 96 次，计算 dispatch 从 348 降为 252。
- 完成 tick 的稳定 GPU 区间约从 3.01 ms 降至 2.15 ms，减少约 28.6%。
- 同一 tick 的结构应变为 `0.00410`，未劣于原 12 色样本；diagnostics 为 0 / 0，Vulkan validation errors 为 0。

共享内存 tile 版本也按相同端点组件实现并实测，但在当前生成 kernel 上增加了共享内存占用和状态搬运，原 `colorBudget=12` 场景反而约为 3.74 ms，因此未保留。编译器不能仅因另一个系统默认启用 tile solve 就复制该策略；后续若引入设备能力 / occupancy cost model，应在 Schedule 后端统一选择，而不是把项目对象泄漏进 Compiler。

Newton / Warp 对照资料：

- <https://github.com/newton-physics/newton/blob/v1.5.0/newton/_src/solvers/vbd/solver_vbd.py>
- <https://github.com/newton-physics/newton/blob/main/newton/_src/sim/graph_coloring.py>
- <https://github.com/NVIDIA/warp/blob/34e627b9/warp/_src/coloring.py>

## 保留连接域、按读写效果融合（2026-09-10）

四层 DSL 的对象字段和 pair 声明现在先进入 `BindingIR`。它只描述 Zip、笛卡尔积、有向非自配对、上三角及含对角上三角，以及各字段的成员索引映射；没有项目对象分类，也不依赖 XPBD 的乘子或修正公式。字段名称在这一边界解析一次，之后的分析与调度按需访问逻辑行，CPU 不再保留一份完整的 `VariableRef` 端点副本。

执行映射完成后才选择物理端点布局。仿射域在描述比显式表更小时保存 `5 + 2 × arity` 个整数，GPU 根据稳定 relation ID 计算成员及 DOF 地址；小域、不规则显式字段和动态端点继续使用显式表。两种布局共用原端点 buffer。映射种类参与 kernel 专门化，使固定映射的 shader 能消除其他解码分支；没有为每个域生成独立 kernel。逻辑行顺序、逐行 patch 和 history 迁移保持原来的契约。

结构只读信息同时进入数学与调度两条路径：

- 导数需求只保留可写状态列；对应的切向变换、局部矩阵列及 Jacobi 贡献存储随之收缩。只读输入的当前数值仍参与公式计算。结构信息来自 `readOnly`，不依据初始逆度量或参数猜测。
- 可写端点决定求解组件。独占只读输入可随组件私有预测，共享只读输入统一在全局预测，再交给各局部区域执行整个子步的迭代。由此，多个区域可以共享驱动参数而继续独立融合，且输入每子步只推进一次。
- 标量不等式先计算原始 residual；当前已满足条件且乘子为零时，跳过导数与局部求解。乘子初始化、Jacobi 贡献清零仍在跳过之前，关系历史仍走原来的独立更新阶段。这是数值后端的活动条件优化，没有移除逻辑关系。

`Engine.dynamics.scene.plan(entity)` 提供计划摘要。`bindingDomains` 是保留的非空域数，`implicitEndpointReferences` 是采用索引映射的逻辑端点数，`endpointStorageWords` 是实际端点表的 32 位整数数，`eliminatedDerivativeColumns` 是按实例累计裁剪的切向列数，`compileMilliseconds` 只包含 CPU 计划生成，不含 Vulkan shader 编译。`localPerSubstep` 表示局部区域按子步提交，其余组件与 dispatch 指标沿用 `PlanStatistics`。

### 本轮实际运行

Release `Whimsical` 构建成功，没有新增或运行测试套件。使用原有 ConstraintLab 六个示例各运行 350 个渲染帧，临时采集完成 tick 21–120 的 100 个 `gpuMilliseconds` 样本，之后恢复项目脚本。设备为 RTX 3080，Hybrid、4 子步 × 12 迭代、`colorBudget=12`；下表为相同示例和设置的优化前后单次运行均值。

| 示例 | 优化前 GPU 提交均值 | 优化后 GPU 提交均值 | 耗时变化 |
| --- | ---: | ---: | ---: |
| 64 组绳子 | 1.845 ms | 0.894 ms | −51.5% |
| 100 粒子 | 6.520 ms | 4.983 ms | −23.6% |
| 32×32 布料 | 3.964 ms | 3.957 ms | −0.2%（基本持平） |

粒子示例保留 3 个域、5,550 个逻辑关系，11,500 个端点引用使用 27 个整数存储；裁剪 2,000 个切向导数列。64 组绳子自然得到 64 个写组件，全部 16,320 个关系进入局部区域，每 tick 数值计算 dispatch 从当前阶段列表的参考值 260 降到 20。布料仍是一个大组件，没有强行改变求解方法。

另三个示例的优化后提交均值为：星仪 0.507 ms、绳网 3.206 ms、穿绳布幕 3.884 ms。六个示例均收到 GPU 回读，求解 diagnostics 为 0 / 0，退出时 Vulkan validation errors 为 0。三个对照示例的末次采样应变分别为：绳子 `0.013936 → 0.013292`、粒子 `0.098817 → 0.098816`、布料 `0.003237 → 0.003222`。这些是示例诊断，不是一般数学等价性的证明。

提交时间包含该提交的传输、同步和 GPU 队列影响，不是纯 shader 耗时或整帧 FPS。未进行重复轮次统计，不把单次均值作为普遍加速保证。曾使用通用映射解码器的中间版本使布料回退到约 4.82 ms，最终保留映射专门化版本。原始日志在 `captures/compiler-before-{rope,particles,cloth}.log` 和 `captures/compiler-specialized-{rope,particles,cloth,clockwork,woven,threaded}.log`。

### 尚未降低的复杂度

BindingIR 延后的是连接和地址的物化。当前着色、Jacobi CSR、逐关系元数据、参数 / history / 乘子存储以及活动条件检查，仍按逻辑关系数增长；全连接集合没有变成稀疏求解。数值后端仍使用现有 XPBD，集合域与访问效果层已经与它分开，尚未实现其他数值算法。

grid 可以作为将来的候选域生成策略，但任意用户公式并不必然具有空间局部性。下一步应先提供可验证的有限作用域表达或数学分析，再由编译器选择空间索引、剪枝与候选更新阶段；不能从字段或关系名称猜测哪些组合可丢弃。大组件切分则还需要边界状态与跨区域同步的显式表示。这些机会可以继续沿域、效果和后端成本演化，不需要在编译器加入绳子、布料或粒子的语义。

## 布料性能复核与无效批次裁剪（2026-09-11）

此前的约 4 ms 采样额外开启了 Vulkan validation，不能直接与默认启动时的 2–3 ms 印象比较。本轮临时构建并运行历史版本 `16cfc55`，使用相同的 32×32 示例、Hybrid、4 子步 × 12 迭代、`colorBudget=12`、350 个渲染帧和 tick 21–120 的 100 个样本；旧版在 validation 模式下的均值也是 3.800 ms。持续运行时曾观察到 GPU 为 855 MHz，未锁定频率或修改驱动设置。当前记录不足以将历史时差全部归因于延迟展开。

本轮保留以下通用优化：

- Jacobi Gather 按实际可写 incidence 生成工作列表。静态关联度为零的变量不执行归并；动态拓扑仅排除结构只读变量。新增只读标量空间不再产生每 tick 48 次空 Gather。
- 预测、归并、速度恢复分别合并不同空间的独立变量批次。保留各数学函数和连续工作范围，通过静态范围分派，不需要每个变量新增类型表。局部区域先完成原有映射，再合并剩余全局工作；`ExecutionMode::Global` 跳过这项融合。
- BindingIR 将单元素一侧的笛卡尔积规范化为线性索引，并将该侧字段变为广播引用；这只化简地址计算，保持 pair 的逻辑组合与行顺序。显式端点生成直接数组访问，线性域使用单独的仿射访问函数，避免经过复杂组合解码。
- 颜色窗口内改用 `groupMemoryBarrier()` 加执行屏障。组件划分保证窗口中的写依赖局限于同一工作组，跨 dispatch 的可见性仍由 RenderCore 提供。该范围对应 GLSL 对工作组内内存排序的保证，见 [Khronos GLSL 内建函数规范](https://docs.vulkan.org/glsl/latest/chapters/builtinfunctions.html)。

默认运行配置（1280×800、validation 关闭、FIFO）的样本如下；均为 dynamics 提交的 GPU 时间戳区间，不是整帧耗时：

| 版本 | 每 tick 计算 dispatch | GPU 提交均值 |
| --- | ---: | ---: |
| 本轮修改前 | 308 | 3.402 ms |
| 最终版本，第 1 轮 | 252 | 3.082 ms |
| 最终版本，第 2 轮 | 252 | 3.014 ms |

最终两轮平均约 3.048 ms，较记录的前样本减少约 10%；运行间频率与 GPU 队列存在波动，前样本还包含一次细粒度 profile，因此这不是隔离 shader 的精确加速比。当前结果接近 3 ms，不能承诺每次运行都低于 3 ms。额外开启 validation 的最终样本为 3.216 ms，应与同模式记录单独比较。

着色仍为 5,624 个 Colored / 3,274 个 Jacobi 实例、8 个执行颜色。修改前后及历史版本在 tick 120 抽取的 12 个状态 float 完全一致，完整结构边计算的平均应变均为 `0.0032157234891912433`。Release 构建成功；最终布料、100 粒子、64 组绳子在 validation 模式下均完成运行，求解 diagnostics 为 0 / 0，Vulkan validation errors 为 0。没有新增测试或运行测试套件，临时项目脚本和 profile 入口均已恢复。

没有保留缺乏收益的固定阶段展开、缩小工作组、窗口共享缓存和逐变量前缀融合实验。最终变化集中在现有索引、工作列表及同步范围，数学模型、子步数、迭代数和项目脚本保持不变。汇总数据见 `captures/cloth-compiler-comparison-2026-09-11.json`；原始数据见 `captures/cloth-default-before.log`、`cloth-default-after-{1,2}.log`、`cloth-historical-16cfc55.log` 和 `cloth-final-{validation,particles,rope}.log`。

## Newton 粒子路径启发与直接稀疏累加（2026-09-11）

本轮重点对照 Newton 1.5 的粒子 VBD / XPBD 实现。可迁移的经验是：在拓扑稳定时预计算调度元数据、让颜色组保持足够工作量、沿自由度邻接关系组织求解，以及对经过活动条件筛选的稀疏修正直接累加。Newton 的 CUDA graph 主要减少固定工作流的宿主提交开销；当前 Dynamics 已将一个 tick 记录为一个 command buffer 并单次提交，因此它不是本轮的主要瓶颈。

这些经验只落在 `Formula + 条件种类 + 可写端点 incidence + BindingIR 索引域 + 后端成本` 上。用户仍定义自由度、对象、关系公式和 pair；Compiler 没有粒子、接触、布料、绳子或弹簧分类。

### 通用 lowering

- 静态拓扑、Auto 执行下的标量不等式 Jacobi 关系先执行已有 feasibility guard。满足条件且乘子为零时不求导，也不写任何贡献。
- 活跃关系不再写完整的 `relation × tangent` 贡献表，而是用 32 位 CAS 浮点累加到目标自由度的切向槽。Gather 每轮消费并清零这些槽，同时继续归并同一自由度可能具有的普通 CSR 贡献。
- Jacobi 的 degree 仍取该自由度全部静态 Jacobi incidence；直接与 CSR 混合时不会因省略 CSR entry 而改变原有缩放规则。等式、多行关系、动态端点和显式 `ExecutionMode::Global` 保留原 CSR 路径。
- 仿射 Zip / Product / Directed / Upper 域在每次关系调用中只解码一次左右成员；多个端点字段复用结果，避免为同一关系重复执行除法或三角索引反解。
- Hybrid 如果在给定颜色上限内无法让每个参与自由度获得至少一半的原位 incidence，且既存 overflow 全是可早退的标量不等式，Auto 会撤掉收益不足的颜色前缀并统一进入 Jacobi。该规则不适用于 Colored、动态拓扑或 Global，也不会触发布料样本的 equality overflow 图。

原子累加会使活跃修正的浮点加法顺序依赖 GPU 调度，因此一般只保证相同求解语义，不承诺逐位一致。`ExecutionMode::Global` 继续提供确定顺序的 CSR 对照路径。Hybrid 的划分本来就是策略的一部分；上述截止规则可能把某个模型从 Colored/Jacobi 混合更新变为全 Jacobi，因此不能把结果宣称为与旧 Hybrid 路径数值相同。

### 粒子样本与回归检查

独立的 100 自由度、5,550 关系基准使用 Release、RTX 3080、Vulkan validation、Hybrid、4 子步 × 12 迭代。相同进程中的 Global CSR 均值为 `2.304361 ms`；两次 Auto 均值为 `0.457695 ms` 和 `0.682585 ms`，相对该次参考减少约 70%–80%。三次最终 300 个状态 float 的最大绝对差均为 `0`，最小间距重叠诊断均为 `0.110385237`。

真实 ConstraintLab 粒子示例在 validation + immediate present 下统计完成 tick 21–120，Auto 均值为 `1.062153 ms`；同文档前一轮记录为 `4.983 ms`，本次样本减少约 78.7%。计划由 12 个候选颜色自然选择 0 个执行颜色，5,550 个关系进入 Jacobi，其中 5,450 个使用直接累加；每 tick 计算 dispatch 为 108，参考阶段列表为 212，端点描述仍为 27 个 word。运行收到 GPU 回读，diagnostics 为 0 / 0，Vulkan validation errors 为 0。

作为非目标图回归，32×32 ConstraintLab 布料计划保持 5,624 Colored / 3,274 Jacobi、8 个执行颜色、252 个计算 dispatch，`directJacobiRelations=0`。相同 validation + FIFO 模式下均值为 `3.278 ms`，接近此前记录的 `3.216 ms`；diagnostics 与 validation 均无错误。不同 present 模式会改变渲染与计算的队列竞争，不能直接混合比较。

没有新增测试。Release 的 `dynamics_tests`、`dynamics_run` 和 `Whimsical` 构建成功；现有 Dynamics focused checks 通过。以上数据说明当前粒子式高候选、低活动率数学图的收益，不构成其他关系图或设备上的普遍加速保证。

参考实现：

- <https://github.com/newton-physics/newton/blob/v1.5.0/newton/_src/solvers/vbd/solver_vbd.py>
- <https://github.com/newton-physics/newton/blob/v1.5.0/newton/_src/solvers/vbd/particle_vbd_kernels.py>
- <https://github.com/newton-physics/newton/blob/v1.5.0/newton/_src/sim/graph_coloring.py>
- <https://github.com/newton-physics/newton/blob/v1.5.0/newton/_src/solvers/xpbd/kernels.py>

## 数学候选域与活动关联缩放（2026-09-11）

在不修改用户 DSL、Model、脚本输入解析或 RenderCore 的前提下，Compiler 现在从平方差之和的不等式推导有限查询范围，再由后端成本决定完整活动扫描或 GPU 哈希网格。适用于仿射集合域的全局静态 Jacobi 工作；未知公式、动态端点和局部融合计划保留原路径。编译器不识别任何具体模拟对象。

网格每轮读取当前快照，精确比较 cell，并合并仍有非零乘子的旧关系；参数 patch 使 GPU 边界归约缓存失效。活动队列按数学类型打包，省去重复的 feasibility primal 和乘子初始化；活动收集及下一轮清理分别融合进查询、Gather。活动 degree 同时缩放乘子和对应修正，其数值变化与索引优化分开对照。

500 自由度采用完整活动扫描，最大重叠从约 62.4% 降至 0.61%，但固定预算单步时间从约 1.0 ms 增至 2.6 ms。2,000 自由度采用网格，在相同活动缩放和相同采样结果下，相比完整扫描的 12.45 ms，记录到 2.12 / 3.69 / 7.39 ms。无界面运行自动变频，以上是样本范围，不是稳定加速倍数。公开字段及乘子仍保留 dense 布局，编译、存储和部分清理成本仍随全部逻辑组合增长。

Release 构建、现有回归和经用户许可的两组候选覆盖/生命周期检查均通过。详细算法边界、Newton 固定版本来源、残差量纲和原始数据位置见 [候选域实施说明](../engine/physics/dynamics/compiler/CandidateDomains.md#已实现的第一阶段2026-09-11)。

## Simit / Ebb 对照后的候选执行优化（2026-09-11）

在原有数学候选域上，新增精确三角域反解、已知成员索引传递、每快照 cell 缓存，以及按类型组织的工作组内活动队列归并。邻域零乘子行与旧队列非零乘子行形成互斥工作集合，删除去重位图；子步边界按完成的活动队列清理乘子，删除完整索引域的清扫。公式、数值策略和用户 DSL 不变，RenderCore 与项目文件未改。

真实界面同条件各两次对照：500 规模 GPU 提交均值 3.750 → 2.994 ms，CPU 计划生成 124.10 → 101.46 ms；2,000 规模分别为 3.889 → 3.068 ms、2,281.02 → 1,549.64 ms。未锁频，运行间有明显波动，数据不构成跨设备或逐次提速保证。两种规模完整状态在三个固定 tick 的回读均与基线一致，诊断与 Vulkan validation 均无错误。Release 构建和既有检查通过，没有新增测试。

网格门槛、dense 公开字段及逻辑关系容量不变。详细来源、队列不变量、显示容量限制、重复采样和剩余复杂度见 [候选域后续优化说明](../engine/physics/dynamics/compiler/CandidateDomains.md#后续优化索引复用与活动队列2026-09-11)。
