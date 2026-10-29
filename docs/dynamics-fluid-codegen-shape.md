# Dynamics：40k 流体邻居遍历的代码形状与固定字段模式的 kernel 变体

ConstraintLab 流体例子（40,000 粒子，Hybrid，4 substeps × 12 iterations，`colorBudget=12`，dt 1/30）的完整 Dynamics GPU tick 相对上一轮提交（`536f0c8`）：剧烈运动阶段（tick 11–30）从约 6.97 降到约 5.87 ms（约 −16%），静止阶段（最后 30 tick）从约 4.98 降到约 4.00 ms（约 −20%）。相对最初基线（12.7 / 12.4 ms），剧烈运动约 2.2 倍，静止约 3.1 倍。质量指标与上一轮处于同一水平，invalid/singular 均为 0。首次编译（从 `compile` 到第一次结果）从约 4 s 降到约 1.1 s（第 7 节）。

## 保持的边界

- 用户的 `lab.js`、`fluid.js` 未修改，SHA-256 与上一轮文档记录一致。
- 编译器只依据编译期字段模式掩码（`variableFieldModes` / `relationFieldModes`）和计划数据（遍历方式、是否线搜索）做决定；没有识别流体、墙或阻尼等具体对象。
- runtime（`Instance.cpp`）的改动：
  - `DYNAMICS_SUBGROUP32` 额外要求 `VK_SUBGROUP_FEATURE_SHUFFLE_BIT` 并启用 `GL_KHR_shader_subgroup_shuffle`（从任意 lane 广播需要非相对 shuffle；RenderCore 的 SPIR-V 访问白名单已包含这些指令）。不支持的设备走已有的非 subgroup 路径。
  - 固定字段模式的 kernel 变体及其选择（第 6 节，经开发者确认的方案 A）。
- RenderCore 只增加了经批准的通用批量编译接口（第 7 节），不含 Dynamics 概念。
- 没有新增测试。

## 保留的变化

第 1–5 项位于 `engine/physics/dynamics/compiler/SumRelations.cpp`，作用于排序区间遍历的大集合（projected-line-search）。

### 1. 球查询的区间按 lane 并行定位（行构建）

球查询拆成 `BallQuery` 结构和 `ballIntervals` / `ballCell` / `ballInterval` 三个函数：稠密排序中每个裁剪后的 (z, y) 行是一个区间，哈希索引中每个单元是一个区间，`full` 时是整个成员范围。行构建（`locate=true`）在稠密或 full 模式下按 `REDUCTION_TEAM` 个区间一组，由 team 的每个 lane 各定位一个区间（裁剪计算加 heads/bucketEnds 读取），再用 `subgroupShuffle` 广播给整个 team 扫描，区间定位的读取延迟不再逐行串行。哈希模式（少见）仍逐个区间扫描并检查单元。

消费者中溢出行的回退查询使用 `locate=false`（串行）：回退代码编译在同一个 kernel 里，并行定位会抬高该 kernel 所有路径的寄存器。

强制重建时构建阶段每次从 75.6 µs 降到约 64–66 µs。

### 2. 行遍历一次解码一个字中的两个成员

每个行字打包两个 16 位偏移。原来对字内两个 slot 再套一层循环并用 `bitfieldExtract`；现在先解出两个成员并读取两者的位置，再依次访问。每次访问包在各自的 `do{...}while(false)` 中，使 body 里的 `continue` 跳到下一个成员。

### 3. 线性化尾部在寄存器中求解

线搜索与结构化加权计划（`solveHere`）中，对角值和残差直接留在寄存器里交给 `solveAndProject`，不再 `rowStore` 后再 `rowValue` 读回；锚点自身的梯度取已经归约的 `total`，不再调用第二次 `diagonal(anchor)`；shared `live` 数组只在需要它的计划中声明。线搜索的转置直接写入位移（不再读取旧值累加），因此线性化不再清零这些列。

第 2–3 项合计：相对已含第 1 项的构建交替运行（开启 profiler，300 tick），tick 平均 6.10 / 6.09 → 5.50 / 5.70 ms，静止约 −11%。

### 4. 统一字段模式的循环外提

新增 `uniformMembers(modes, required, uniformBody, body)`。若编译期掩码包含全部 `required` 位，生成

`if((fieldModes(first,mask)&required)==required){统一路径的成员循环}else{一般路径的成员循环}`

否则只生成一般路径。runtime 的编辑只会清除模式位，编译期没有的位运行时不可能出现，因此此时不需要统一路径。

- 线性化：变量集为 `UniformEnabled | EnabledValue | IdentityMetric` 时，成员项的度量为 `dot(g,g)`，不再对每个成员调用 `metric(member,g)`（其中有逐成员的模式判断和 mapped/logical/affine 查找）。
- 转置（无历史项）：关系集为 `UniformParameters` 时直接用 `gradient(anchor,member,entry,a)`，否则通过 `term(member,...)` 反向求值。原来这个判断在每个成员内部。

收益来自热循环的代码形状，静止约 −0.19 ms/tick，剧烈运动约 −0.13 ms/tick。第 6 节的固定模式变体中这个判断被常量折叠，循环外提此后只在模式被编辑清除后的一般变体中起作用。

### 5. 线搜索的乘子回写

线性化的 `solveAndProject` 已经写入完整步长的 λ+dl。所有者输出改为 λ += (w−1)·dl，只读 dl，dl 为 0 的行（包括未激活行）跳过；线搜索计划的线性化不再把 λ 存入第 3 列（只有 Jacobi 的 `operatorSolve` 读取它，线搜索计划不生成该步骤）。约 −0.025 ms/tick。

### 6. 固定字段模式的 kernel 变体

字段模式字只会被编辑清除位，不会重新置位。因此在任何位被清除之前，每个模式字都等于编译期掩码，kernel 可以把它当作常量。上面的循环外提做不到这一点：一般路径仍在同一个 kernel 中，寄存器按最差路径分配；所有者链若在源码中整体复制两份，二进制会翻倍。

- 计划接口（`CompiledPlan::interface()`）：所有模式字的读取都经过三个函数。`fieldModes(word, compiled)` 用于编译期已知的模式字；`variableModes(word)` / `relationModes(word)` 用于从 Variable/Relation 记录取得的模式字。一般情况下三者都读 `x_fieldModes[word]`。定义了 `DYNAMICS_FIXED_MODES` 时，`fieldModes` 直接返回编译期掩码；后两者返回所有集合共有的位，只有集合之间不同的位才读 buffer。统一值本身（参数、compliance）仍从 buffer 读取。
- 生成器：`fieldAccess` 中的 `variableEnabled` / `identityMetric` / `relationEnabled` / `loadParameter` / `loadCompliance`、求和关系的 `metric` 与 `uniformMembers`、候选域的两处统一参数判断，改为通过上述函数读取模式字（`Schedule.h` 新增生成器辅助 `fieldModes(const FieldModeLayout&)`）。
- runtime：每个 kernel 先编译一般版本；若反射显示它读取 FieldModes buffer，再加上 `#define DYNAMICS_FIXED_MODES 1` 编译固定模式版本，否则两者是同一个程序。`apply()` 第一次清除任何模式位时 `releaseFixedModes()` 换回一般版本，并让原生序列重新推导访问与屏障。

流体计划的 23 个 kernel 中有 9 个得到固定模式版本（线性化、转置、线搜索回写、所有者链、所有者推进、预测、恢复、位移阻力提交、求和边界）。对于流体，所有变量集和关系集都是统一的，`relationEnabled`、`loadParameter` 等全部折叠成统一路径，循环外提的一般分支被删除。同一会话交替运行 3 次，静止阶段 4.25 → 4.02 ms（约 −5.4%），剧烈运动 5.97 → 5.72 ms（约 −4.2%）；profile 中线性化、转置、所有者推进分别约 −6%、−5.5%、−8.5%。

在串行编译下，多出的 9 次 GLSL 编译使首次编译从约 4.0 s 增加到约 5.4 s，由第 7 节解决。

### 7. 批量并发编译 GLSL（RenderCore）

RenderCore 的 `ShaderCompiler` 每个程序串行启动一次 glslang 进程。空 shader 也需约 136 ms，本例最大的 kernel 也只需约 160–240 ms，所以原有约 4 s 的首次编译中约 3 s 是 23 次进程启动。

- `rc::ShaderCompiler::compile` 新增批量重载，参数为 (name, source) 列表：未缓存的程序同时启动 glslang，同时运行的进程数不超过硬件线程数，结果按输入顺序返回。批内与缓存中重复的源码只编译一次；任一失败时等所有已启动的进程结束后抛出第一条错误日志。原单个 `compile` 变为一个元素的批量调用。
- `rc::GraphContext::compute` 新增对应的批量重载。
- Dynamics runtime 分两批提交：先提交全部一般版本，再根据反射结果提交需要固定模式版本的 kernel。
- glslang 子进程通过 `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` 只继承自己的诊断句柄。渲染器和 Dynamics 各自持有一个编译器，在不同线程里同时启动进程，而 CRT 打开的文件句柄默认可继承。如果不加限制，一个 glslang 子进程可能继承另一个线程正在写入的源文件句柄，导致那边的 glslang 打开源文件失败（`fopen_s` 不允许共享写）。

流体的首次编译从约 4 s 降到约 1.1 s（新可执行文件的第一次启动约 3.6 s，是冷启动开销）。`dynamics_solver` 测试从约 18 s 降到约 7.8 s，`dynamics_sum_dsl` 从约 2.6 s 降到约 1.1 s。材质系统等其他调用方仍逐个编译，可以按需改用批量接口。

## 测量结果

RTX 3080，Release，`captures/fluid-40k/run.ps1`（300 tick）。`dynamics_run` 总是开启 Vulkan validation，时间来自 GPU 时间戳。同一会话中交替运行 HEAD（`536f0c8`）构建的 `head_dynamics_run.exe` 与最终构建 `par_dynamics_run.exe` 各 3 次。流体运动是混沌的，单次之间约有 ±0.2 ms 波动。

| 取样窗口 | HEAD | 最终 |
| --- | ---: | ---: |
| tick 11–30 平均（剧烈运动） | 7.023 / 6.967 / 6.907 ms | 5.884 / 5.827 / 5.911 ms |
| 最后 30 tick 平均（静止） | 5.008 / 4.988 / 4.948 ms | 4.018 / 3.980 / 4.015 ms |
| tick 2–300 平均 | 6.448 / 6.323 / 6.215 ms | 5.309 / 4.601 / 4.581 ms |
| 首次编译 | 4.78 / 3.97 / 3.92 s | 3.60（冷启动）/ 1.09 / 1.10 s |

tick 2–300 平均包含编译后 GPU 从低频升起的前几个 tick。最终构建后两次明显更低，可能是编译变短后 GPU 尚未降频；这一列不再适合比较求解器本身。

300 tick 末帧质量：HEAD 密度超额均值 0.001747 / 0.001759 / 0.001699、P99 0.0089 / 0.0087 / 0.0085、最大 0.058 / 0.051 / 0.049；最终 0.001724 / 0.001742 / 0.001681、0.0085 / 0.0086 / 0.0082、0.052 / 0.053 / 0.054。invalid/singular 均为 0。

每 tick GPU profile（开启详细 profiler，300 tick，跳过前 11 个 profile 样本，取静止阶段的 10–12 个样本；最终列取自 GPU 代码与最终构建相同的 `fixed_dynamics_run.exe`）：

| 阶段 | HEAD | 最终 |
| --- | ---: | ---: |
| 线性化（48 次） | 1.645 ms | 1.266 ms |
| 转置并计算二次型（48 次） | 1.649 ms | 1.200 ms |
| 所有者迭代推进（44 次） | 1.136 ms | 1.008 ms |
| 邻居行构建（每次迭代一次 launch，大多为空） | 0.414 ms | 0.370 ms |
| 所有者关系链（4 次） | 0.108 ms | 0.088 ms |
| 调度合计 | 5.471 ms | 4.429 ms |

每次调用：线性化约 34 → 26 µs，转置约 34 → 25 µs，所有者推进约 26 → 23 µs。

## 没有保留的实验

- 构建 team 4：80 µs，不如 team 8 的 66 µs。
- 皮肤 0.15 / 0.2（在新代码上重测）：剧烈运动阶段更慢。
- 0.5R 单元宽度配合并行区间定位：更慢。
- 构建阶段用 `subgroupBallot` 压缩代替 shared `atomicAdd`：87 µs 对 63 µs，而且需要把 ballot 指令加入 RenderCore 白名单，已撤回。
- 构建阶段同样按字两两展开写入：无变化。
- 项公式中的 a/b 改为 a·(1/b)（每锚点预先求倒数）：无可测收益。
- 只把模式读取提到循环外而不复制循环：收益很小。
- 用上一次迭代的步长 ω 代替精确线搜索（去掉对全局归约的依赖）：三次中一次发散，精确线搜索是必要的。
- 消费者回退查询也并行定位区间：抬高消费者所有路径的寄存器，保持串行。完全去掉回退路径（仅作探针）线性化和转置合计也只快约 2%。
- 记录按 16 字节对齐：仍然无效果，已删除。

## 剩余瓶颈

- 两次邻居遍历约 25–26 µs/次。溢出回退的球查询仍编译在同一 kernel 中。上一轮的向量读取探针（线性化约 −10%）需要 buffer 的第二种类型视图，仍未实现。
- 所有者推进约 23 µs/次，其中约一半是关系链（5 个墙面不等式和一个 3 行阻尼）。
- 邻居行真实重建约 64 µs/次；静止阶段约 0.37 ms/tick，运动阶段约 0.9–1.0 ms/tick。重建由少数快粒子触发且是全局的；按行判断有效性、只重建漂移过大的行属于算法改变，而且行中存的是排序号偏移，每次索引重建都会使其失效，实现较复杂。
- 每个 substep 11 个索引阶段，每个约 2–3.5 µs 的固定开销；把阶段 3 并入 4、把扫描阶段 5–10 合并成 2 个 dispatch 约可省 1%。
- 与手写 PBF（同预算估计约 2.5–3.5 ms）的差距主要来自线搜索带来的每次迭代第三次全局 pass，以及通用代码的额外开销。

## 验证与原始记录

- Release 构建通过：`dynamics_run`、`dynamics_tests`、`Whimsical`。
- ctest 全部 4 项通过（`dynamics_solver`、`dynamics_sum_dsl`、`console_system`、`content_mounts`）。`dynamics_solver` 中已有的 RelationEnabled / Parameters 补丁会清除模式位，覆盖了换回一般版本的路径。
- `dynamics_run` 有 validation 错误时以非零退出；上述全部流体运行均正常退出。`Whimsical --smoke --validation` validation errors=0。
- `git diff --check` 通过。
- `captures/fluid-40k/`：`head_dynamics_run.exe`（HEAD 构建）、`final_dynamics_run.exe`（第 1–5 项）、`fixed_dynamics_run.exe`（加第 6 项，串行编译）、`par_dynamics_run.exe`（最终构建）、`prof_head.log` / `prof_final.log` / `prof_fixed.log`（profile）、`mk.py`（实验用的覆盖 kernel 生成脚本；依赖的临时 dump/override 钩子已从 runtime 删除，不能直接在最终构建上运行）。

## 80k 一轮（`7dc699f` 之后）

80,000 粒子，其余设置不变。只改了编译器（`SumRelations.cpp`、`FormulaGlsl.cpp`）；`lab.js` / `fluid.js`、JS 接口、physics IR、runtime 与 RenderCore 均未改动，没有新增测试。

### 保留的变化

- 掩码访问：每次成员访问提供 `delta`、`distance2` 和 `inside`，body 用 select 屏蔽支撑外的贡献，不再 `continue`，team 内各 lane 在成员之间保持收敛。原来的 `gradient()` 改为 `coefficient(anchor,entry,distance2)`，只返回系数，delta 与距离不再重复计算。
- 行字预取：每个 lane 在访问当前字之前先读下一个字（相隔一个 128 字块），首字与计数一起读，行读取与成员位置读取的延迟得以重叠。
- 线性化：乘子提前读入，`solveAndProject(multipliers)` 复用已读的值；统一参数时 `prepareTerm` 读第 0 行的不变参数（`parameterRow`），不再依赖 logical member 的读取；成员对角不再累加锚点自身项（其 delta 为 0）。
- 转置：`sum+=delta*(inside&&dl!=0.0?k*dl:0.0)`，dl 为 0 时不再分支。
- `isolatedCorrection` 计划不读 `x_contributions`，转置 apply 与线搜索输出中对它的清零是死存储，已删除。
- 例外加入：`join` 所需的 built 计数和 listed 点由调用方与标志位一起读取。
- 符号微分（`FormulaGlsl.cpp`）：零伴随保护只用于依赖 min/max 分支选择的值，以及 divide / sqrt / log / atan2 这类在有限值处奇异的偏导。其他零伴随来自为零的原始因子，乘积、exp、sin 的偏导若非有限，原始值也非有限，不需要 select。

### 测量结果

RTX 3080，Release，`captures/fluid-80k-s4/abx.ps1`（开启 profiler，300 tick，HEAD 构建与最终构建交替各 3 次，validation 开启）：

| 取样 | HEAD（`7dc699f`） | 最终 |
| --- | ---: | ---: |
| 调度合计（profile 平均） | 7.706 / 8.024 / 7.964 ms | 6.403 / 6.452 / 6.231 ms |
| tick 11–30 平均 | 7.968 / 8.351 / 8.429 ms | 6.798 / 6.885 / 7.038 ms |
| 最后 30 tick 平均 | 6.983 / 7.249 / 7.275 ms | 5.781 / 5.865 / 5.572 ms |
| 线性化（48 次） | 2.186 / 2.221 / 2.246 ms | 1.472 / 1.473 / 1.456 ms |
| 转置并计算二次型（48 次） | 2.243 / 2.266 / 2.303 ms | 1.602 / 1.575 / 1.589 ms |
| 所有者迭代推进（44 次） | 1.477 / 1.489 / 1.516 ms | 1.422 / 1.472 / 1.446 ms |

调度合计约 −20%，线性化约 −34%，转置约 −30%。末帧密度超额均值 HEAD 0.00345–0.00348，最终 0.00348–0.00358；同一构建多次运行之间约有 ±0.0001 的混沌波动。P99 分别为 0.0171–0.0176 和 0.0175–0.0180。diag（invalid/singular）均为 0/0。

### 没有保留的实验

- 行字循环展开 2 倍：调度 6.52 / 6.59 ms，对照 6.35–6.49 ms，无收益。
- 合并行重建与例外两个索引阶段：合并后约 0.92 ms，分开时约 0.52 + 0.40 ms，无收益；空的 launch 门控 dispatch 只需约 1 µs。
- 消费者 team 4：线性化约 1.59 ms，转置约 1.67 ms，都比 team 2 慢。
- 在生成代码中对微分乘积做 CSE：驱动已经做了，无变化。
- 回退球查询把区间扁平化后再遍历：行构建约 0.70 ms，对照约 0.52 ms（+35%）。
- 用 subgroup 做组内归约：在噪声范围内，无收益。

### 剩余瓶颈

- 线性化部分受指令发射限制（trace 中 sm/inst 约 61%）。上一轮的 `uvec4` 向量读取探针（线性化约 −10%，转置约 −3%）需要 runtime 为 `x_sumData` 提供第二种类型视图，仍未实现。
- 所有者推进受带宽限制，约 183 B/粒子。
- 与手写 PBF 的差距仍主要来自线搜索带来的每次迭代第三次全局 pass，以及全局行重建；只重建失效行、减少排序 dispatch 属于算法改变。

### 验证

- Release 构建 `dynamics_run`、`dynamics_tests`、`Whimsical` 通过；ctest 4/4 通过；`Whimsical --smoke --validation` validation errors=0；`git diff --check` 通过。
- 编译警告 C4458（`prepareGroupedIndex` 中的 `input`）和 C4456（`Schedule.cpp`）在 HEAD 中已存在。
- 用于测量的临时 dump/override 钩子已从 `Instance.cpp` 删除。

## 阶段 1：编译器内部的所有者 piece IR（`838c404` 之后）

80,000 粒子，设置不变。只改了编译器（`CompiledPlan.h`、`Compiler.cpp`、`Kernels.cpp`、`Schedule.h/.cpp`、`SumRelations.h/.cpp`）；`lab.js` / `fluid.js`、JS 接口、physics IR、公式、参数、子步/迭代数、runtime（`Instance.cpp` 与 HEAD 一致）和 RenderCore 均未改动，没有新增测试。每 tick 的 dispatch 从 300 个减到 296 个。

### IR：`OwnerPiece`

在所有者 `first+stride*row` 上逐点执行的 kernel 都可以表示为一个 `OwnerPiece`：

- `source`：基于所有者状态访问层的函数，访问层由组合 kernel 提供；
- `body`：可以使用 `row`、`stateOwner` 和 `ownedState`；
- `groupBegin` / `groupEnd`：workgroup 集合操作，越界的尾部 invocation 也参与；
- `defines`：body 直接定义整个所有者状态，不读取旧值。

`ownerKernel` 把同一所有者域上的多个 piece 组合到“每个所有者一个 invocation”上。所有者状态只读一次（第一个 piece 为 `defines` 时不读），也只发布一次，中间结果留在寄存器中。线搜索回缩、快照、关系链、recover、commit 都用 piece 表示。它取代了 `dynamics-ordered-traversal-sep13.md` 和 `dynamics-fluid-neighbour-rows.md` 中的 `OwnerOutput`：原来“本次迭代末尾接下次迭代前缀”的专门拼接，现在只是组合规则的一个实例。

### 规则

1. 组合（`fuseOwnedRelations`）。合法条件：
   - 所有 piece 的宽度、数量和所有者映射（`owners[i]==first+stride*i`）完全一致；
   - 中间没有 apply / iteration 阶段；
   - 最后一个阶段每个子步只执行一次（发生了循环旋转，或者迭代数为 1）。

   在这些条件下生成三种 kernel：
   - 头部：关系链 + 快照；
   - 迭代衔接：线搜索 + 关系链（`step.iteration+1u`）+ 快照；
   - 尾部 “Finish owner substep”：线搜索 + recover + commit。

   recover / commit 只有在一个共享 batch 包含全部所有者 ID 时才从中取出；commit 只取 RelationWork 中的 batch，summed commit 的 `first` 是关系 ID，会被跳过。
2. 所有权寄存器提升。同一条关系链上的各关系拥有互不重叠的乘子行，所以在第一次调用前一起读入。读写通过 `ownedRelation` / `ownedMultiplier` 路由到寄存器，只把被写过的行（`ownedWritten`）写回。这样链内不再出现 store→load 的串行化。
3. 迭代常量。每个子步只执行一次的头部就是第 0 次迭代的实例，迭代号用字面量 `0u` 传入。这样 `iteration==0` 的乘子分支被常量折叠，读取旧乘子成了死代码。
4. 快照运动检查先读后写。打包状态写入的字与运动检查读取的字互不重叠，但同一个 SSBO 上的读写会被串行化；把读取提前，读取就不再等待写入。

### 测量结果

RTX 3080，Release，`abx.ps1`，开启 profiler，300 tick，HEAD（`838c404`）构建与最终构建交替各运行 5 次，validation 开启：

| 取样 | HEAD（`838c404`） | 最终 |
| --- | ---: | ---: |
| 调度合计（profile 平均） | 6.457 / 6.370 / 6.432 / 6.274 / 6.350 ms | 6.429 / 6.276 / 6.176 / 6.209 / 6.187 ms |
| 所有者迭代推进（44 次） | 1.533 / 1.488 / 1.440 / 1.434 / 1.450 ms | 1.438 / 1.381 / 1.378 / 1.369 / 1.370 ms |
| 头部关系链（4 次） | 0.134 ms | 0.123 ms |
| 线搜索 + commit + recover（各 4 次） | 0.049 + 0.018 + 0.024 ms | 尾部 0.058 + 剩余 recover（n=10）0.018 ms |
| 线性化（48 次） | 1.482 ms | 1.503 ms |
| 转置并计算二次型（48 次） | 1.604 ms | 1.606 ms |

调度合计平均 6.377 → 6.255 ms（约 −1.9%），5 轮全部更快；所有者相关 kernel 合计约 −0.11 ms。线性化和转置的源码与 HEAD 逐字节相同，两者的差异属于噪声或缓存状态。末帧密度超额均值 HEAD 0.00347–0.00351，最终 0.00343–0.00360；P99 分别为 0.0174–0.0178 和 0.0171–0.0188；diag 均为 0/0。

### 没有保留的实验

- 在乘子读取中加 `iteration==0u?0.0:` select（代替字面量 `0u` 头部和写回脏位）：头部恢复正常，但迭代衔接 kernel 也要付出这个 select，推进阶段变慢。
- 全局标量由最后到达的 workgroup 统一完成（R2）：投影二次型的部分和由 producer 用 fence 加同地址原子计数完成最终归约，消费者只读一个值。结果是转置 1.59 → 1.80 ms（producer 每次调用约 +4.4 µs），推进 1.37 → 1.67 ms（所有消费者 workgroup 读同一地址，形成 L2 热点，每次调用约 +7 µs），调度合计 6.25 → 6.79 ms。在这块 GPU 上，每个消费者 workgroup 冗余地做并行归约更便宜，因此已完全撤回，撤回后生成的 kernel 与撤回前逐字节一致。
- 把 predict 组合进头部：runtime 以 `!plan->predict.empty()` 作为子步循环条件，`dispatchCount` 对空 predict 返回 0；组合后 predict 为空会破坏这个约定，需要修改 runtime，没有做。

### 剩余瓶颈

线性化、转置、推进以及 Index 12/13 的行维护，合计约占调度时间的 87%（5.44 / 6.26 ms）。它们是每次迭代的三个全局 pass 加上行重建：转置需要所有行的 dl，线搜索步长需要一次全局归约，这些全局依赖使阶段 1 的组合规则无法再合并它们。要减少 pass，需要在编译器中做方法选择（例如按问题结构选择不需要全局线搜索的局部投影形式），或者做缓存放置（按空间排序的所有者使用 workgroup 本地 tile）。这两项还没有开始，方法选择留待之后决定。

### 验证

- Release 构建全部目标通过；ctest 4/4 通过；流体 300 tick 运行（validation 开启）没有 VUID 消息，diag=0/0；`Whimsical --smoke --validation` 没有 VUID 消息；`git diff --check` 通过。
- 编译警告 C4456（`Schedule.cpp` 中的 `writer`）和 C4458（`input`）在 HEAD 中已存在。
- `captures/fluid-80k-s4/`：`base838.exe`（HEAD 构建）、`phase1b.exe`（最终构建）、`r2.exe`（R2 实验）、`logs/x-base838-*.log` / `logs/x-phase1b-*.log`（profile）。

## 阶段 2：异常行、转置顺序和线搜索的行局部项（`d7ca44a` 之后）

80,000 粒子，设置不变。只改了 `SumRelations.cpp`。`lab.js` / `fluid.js`、JS 接口、physics IR、公式、参数、子步/迭代数、runtime（`Instance.cpp` 与 HEAD 一致）和 RenderCore 均未改动，没有新增测试，dispatch 数不变。

### 规则

1. 异常由整个 workgroup 处理（`BallScan::Workgroup`）。每个异常占一个 128 lane 的 workgroup（`ExceptionTeam=128`，快照的 launch 请求随之变为 `slot+1`）。球查询的区间由各 lane 并行定位并写入共享内存，lane 0 求前缀和，之后各 lane 用二分查找把全部候选当作一个扁平序列分摊。原来 32 lane 的 team 一次扫一个区间，而稠密排序中一个区间只有约 30 个成员，team 大部分时间空闲。join 分两步：扫描时只收集成员，行完整后各 lane 并行对收集到的成员调用 join。超过行容量的成员在扫描中直接 join，所以溢出时仍然精确。哈希区间照旧检查单元。
2. 转置反向遍历 workgroup。线性化刚按锚点顺序流过同样的行和成员记录，转置从末尾开始，先用上 L2 中还在的数据。超出 dispatch 的 group 回绕到无效锚点。
3. 线搜索的行局部项在线性化中归约。步长分子中的 r·dl 和分母中的 α·dl² 只依赖本行，线性化已把它们放在寄存器里。现在由线性化写入每个 workgroup 的部分和，转置读取同一槽位，只加上位移的二次型 `dot(raw,sum)`。线搜索模式下，活动标志、对角和残差三列不再存储；转置尾部也不再读取它们，以及 logical→relation→compliance 这条依赖链。两处 workgroup 归约在 `DYNAMICS_SUBGROUP32` 下用 subgroup shuffle 加一个 barrier 完成，否则仍走共享内存树。为了让无效求解之后所有 invocation 仍能到达 barrier，`solveAndProject` 新增了不提前返回的形式。

### 测量结果

RTX 3080，Release，`s5/ab.ps1`，开启 profiler，120 tick，HEAD（`d7ca44a`）构建与最终构建交替各运行 4 次，validation 开启：

| 取样 | HEAD（`d7ca44a`） | 最终 |
| --- | ---: | ---: |
| 调度合计（profile 平均） | 6.672 / 6.577 / 6.699 / 6.593 ms | 6.185 / 6.089 / 6.330 / 6.132 ms |
| 线性化（48 次） | 1.530 / 1.489 / 1.480 / 1.452 ms | 1.499 / 1.453 / 1.542 / 1.462 ms |
| 转置并计算二次型（48 次） | 1.600 / 1.569 / 1.666 / 1.579 ms | 1.417 / 1.377 / 1.382 / 1.380 ms |
| 所有者迭代推进（44 次） | 1.363 / 1.361 / 1.358 / 1.365 ms | 1.366 / 1.364 / 1.453 / 1.374 ms |
| Index 12：完整行构建（48 次） | 0.606 / 0.608 / 0.607 / 0.609 ms | 0.607 / 0.606 / 0.610 / 0.612 ms |
| Index 13：异常（48 次） | 0.695 / 0.676 / 0.705 / 0.710 ms | 0.382 / 0.413 / 0.423 / 0.425 ms |

调度合计平均 6.635 → 6.184 ms（约 −6.8%），4 轮全部更快。之前一组较安静的交替测量中，规则 1、2 使 HEAD 6.513 / 6.478 / 6.494 ms 变为 6.181 / 6.191 / 6.169 ms；规则 3 在此基础上再从 6.316（第一轮偏高）/ 6.168 / 6.173 ms 降到 6.088 / 6.031 / 6.024 ms，转置约 1.50 → 1.37 ms，线性化不变。末帧密度超额均值 HEAD 0.00350–0.00360，最终 0.00352–0.00363；P99 分别为 0.0182–0.0193 和 0.0185–0.0198；diag 均为 0/0。前 30 tick（每次迭代约 1,250 个异常）的超额均值 HEAD 0.00492–0.00517，最终 0.00471–0.00521。

两个构建都偶尔把个别粒子弹出。约 22 次 HEAD 运行中 velMax 最高 14.3、maxY 最高 6.3；约 25 次最终构建运行中有一次 velMax 57.6、maxY 40，包围盒随之变大，那次运行的线性化和转置升到约 10 ms。三条规则都不改变数学，只是改变了浮点求和顺序，轨迹会混沌地分叉。这个尾部风险值得单独研究：一个逃逸粒子就会拖垮整个索引。

### 没有保留的实验

- `uvec4` 向量读取：四字记录按 16 字节对齐，粒子位置加 dl 用一次 `uvec4` 读取。转置 −8%，调度合计 6.158 → 5.965 ms（−3.1%）；`uvec2` 视图约 −1.5%。这需要在 `uint x_sumData[]` 的同一 binding 上再声明一个类型视图，而 binding 号由 RenderCore 的 `Registry` 分配，编译器无法自行声明。实验中 binding 号是写死的，已删除。要保留，需要 runtime 或 RenderCore 提供别名视图，等待决定。
- 只对齐、仍用标量读取：没有收益（驱动不会合并相邻的标量读取），已删除。
- 更小的皮肤：0.07R 时行缩短约 8%，但线性化和转置只快约 3%，异常增加，合计只少 0.03 ms；0.05R 更慢。
- 把行不变量 h² 的倒数分级预计算：无变化，驱动已经外提了倒数。
- 转置在开头预取 built count：无变化。
- 同一轮的其他探针：完整行构建用 32 lane team 扁平扫描慢约 50%；推进按逻辑顺序布局、位移改成 AoS、链端点特化、调整异常容量都没有收益或更慢。

### 剩余瓶颈

线性化约 1.47 ms，转置约 1.39 ms，推进约 1.36 ms，行维护（Index 1–13）约 1.4 ms。推进受通用关系链中的依赖读取延迟限制；去掉墙约 −0.25 ms，去掉阻尼约 −0.15–0.2 ms。要继续缩小与手写 PBF 的差距，需要结构性改动，例如按空间顺序给自由度做物理重排或为关系链特化端点，或者引入上面的类型视图。

### 验证

- Release 构建全部目标通过；ctest 4/4 通过；`dynamics_tests` 通过（validation 开启，检查 `core.errors()==0`）；流体 120 tick 运行（validation 开启）没有 VUID 消息，diag=0/0；`Whimsical --smoke --validation` validation errors=0；`git diff --check` 通过。
- 编译警告 C4458（`input`）在 HEAD 中已存在。
- 用于探针的临时 override 钩子已从 `Instance.cpp` 删除，该文件与 HEAD 一致。
- `captures/fluid-80k-s4/`：`headd7c.exe`（HEAD 构建）、`final.exe`（最终构建）、`k1.exe`（规则 1、2）、`k4.exe`（规则 1–3，含探针钩子）；`s5/`（探针源码与 `mk.py`）；`s5/logs/head-*.log` / `s5/logs/final-*.log`（profile）。

## 阶段 3：逃逸粒子不再拖垮索引（`fff6eeb` 之后）

80,000 粒子，设置不变。只改了 `SumRelations.cpp` 和 `CandidateDomains.cpp` 生成的着色器。`lab.js` / `fluid.js`、JS 接口、physics IR、公式、参数、子步/迭代数、runtime（`Instance.cpp` 与 HEAD 一致）和 RenderCore 均未改动，没有新增测试，dispatch 数不变。

### 问题

阶段 2 记录的尾部风险在默认设置下就会出现。HEAD 的有序索引有两级退化：

1. 包围盒超出桶表（80k 粒子为 262,144 个桶）时，尺寸留为 0，索引退回哈希：每个单元一个区间，并逐个检查单元。一个 90 m/s 的逃逸粒子就让此后每 tick 约 58–70 ms（平时约 5.6 ms）。
2. 任一成员的单元坐标超过 ±2¹⁸（单元宽 R = 0.13 m 时约 34 km）时，`cellOf` 失败，整个索引无效，所有查询退成全扫描，复杂度 O(N²)。只做了规则 1 的构建此后每 tick 约 1.8 s；HEAD 在 10⁶ m/s 逃逸后几秒内退出（0xC0000409，像是 GPU 超时造成的 device lost）。一次默认设置的运行（规则 1 构建）中，求解器自发弹出的粒子在约 tick 40 到达 maxY 1.6×10⁶ m、velMax 4.6×10⁵ m/s，此后每 tick 1–1.8 s。

### 规则

1. 包围盒超出桶表时，最长的轴回绕。最小轴截到桶数的立方根，中间轴截到剩余部分的平方根，最长轴截到剩下的容量：放得下的盒子不变，任何盒子都放得下。桶为 `(c−origin) mod size`。一个回绕桶混合了多个单元，由精确距离测试区分。球查询在 z 或 y 的跨度达到一个周期时退成全扫描（只有极大的查询才会如此）；x 区间越过回绕点时拆成两段。不回绕时区间与 HEAD 完全相同，取模只在越过周期时执行，所以常态没有额外的整数除法。有序索引的哈希路径（`ballCell`、逐单元检查）已删除，只有桶索引域还用哈希。
2. 越界单元钳到边界单元，而不是让索引失效。钳位单调且 1-Lipschitz：相距不超过一个单元的两点钳位后仍不超过一个单元，所以 27 单元模板仍然精确。球查询对范围做同样的钳位，并把边界行视为无界（行距和弦长按无界计算）。NaN、Inf 和 |f|>1e15 仍然使索引失效。candidate domain 的 `cellOf` 同样处理。
3. 散射不再用原子操作。计数阶段 `atomicAdd` 的返回值就是成员在桶内的槽位，存进单元记录中空闲的第 4 个字，散射直接用 begin+slot。`bucketCursors`（1 MB）已删除。每 tick：散射 0.117 → 0.099 ms，计数 0.042 → 0.050 ms，finalize 0.027 → 0.024 ms，合计约 −13 µs。

### 测量结果

RTX 3080，Release，`captures/fluid-opt/ab.ps1`，开启 profiler，150 tick，HEAD（`fff6eeb`）构建与最终构建交替各运行 4 次，validation 开启：

| 取样 | HEAD（`fff6eeb`） | 最终 |
| --- | ---: | ---: |
| 调度合计（profile 平均） | 5.992 / 5.962 / 6.030 / 6.105 ms | 6.029 / 6.026 / 6.111 / 6.089 ms |
| 线性化（48 次） | 1.468 / 1.470 / 1.487 / 1.471 ms | 1.467 / 1.472 / 1.475 / 1.482 ms |
| 转置并计算二次型（48 次） | 1.390 / 1.388 / 1.395 / 1.400 ms | 1.376 / 1.385 / 1.387 / 1.386 ms |
| 所有者迭代推进（44 次） | 1.353 / 1.350 / 1.350 / 1.380 ms | 1.348 / 1.350 / 1.370 / 1.386 ms |
| Index 12：完整行构建（48 次） | 0.611 / 0.610 / 0.615 / 0.616 ms | 0.612 / 0.615 / 0.618 / 0.618 ms |
| Index 13：异常（48 次） | 0.304 / 0.275 / 0.317 / 0.377 ms | 0.361 / 0.337 / 0.403 / 0.353 ms |

常态持平：本轮全部 HEAD 运行（11 次）调度合计平均 6.023 ms，新代码（14 次）6.039 ms，差 0.3%。差异落在异常阶段，它随异常数在两边都于 0.27–0.40 ms 之间波动。第一版回绕在每个区间里都做取模和除法，完整行构建因此慢了 7%；改成移位并按需取模后与 HEAD 一致。

逃逸场景：`driver.js` 在 tick 30 把粒子 40000 的速度设为 (0, v, 0)，共 80 tick，下表为逃逸之后的 GPU 平均每 tick 时间：

| 逃逸速度 | HEAD | 只做规则 1 | 最终 |
| ---: | ---: | ---: | ---: |
| 90 m/s | 57.6 ms（之前一次 70.4 ms） | 5.7–6.0 ms | 5.64 ms |
| 1,000 m/s | 71.0 ms | — | 5.64 ms |
| 10⁶ m/s | 几秒内退出 | 1,794 ms | 5.65 ms |

末帧密度超额均值：逃逸场景 0.0038–0.0041（HEAD 与最终相同），不逃逸 0.0036–0.0037。

### 逃逸的来源

逃逸来自求解方法，而不是代码生成。`Lab.compliance` 为 1e-7，α=compliance/h²≈0.0014，而 Σ|∇C|² 约 250 m⁻²，相当于没有正则。粒子几乎重合时梯度趋于 0，C 却大于 0，于是 dl≈−C/Σ|g|²，Δx≈C/|g| 非常大；线搜索按线性化后的二次模型取步长，对这一行给出 w≈1，挡不住。要消除逃逸，需要改数学，例如给每行的步长设上界、对消失的梯度做正则，或者按非线性残差做线搜索，这需要开发者决定。本阶段只保证逃逸不再拖垮性能。

### 没有保留的实验

- 把索引重建阈值从 0.1R 提高到 0.2R / 0.35R / 0.5R：在本场景中每个子步仍重建行。0.1R 时由索引重建触发；0.5R 时异常列表在 1–2 个子步内超过 10,000 的容量，同样触发（计数探针：tick 100 之前两种阈值都是每 tick 4 次行构建）。异常阶段更贵，调度合计 6.17–6.71 ms，没有明确收益。

### 剩余瓶颈

与阶段 2 相同。线性化、转置和推进各约 1.35–1.5 ms，行维护约 1.35 ms。之前的 GPU Trace 显示前两者受指令发射限制。进一步的大幅收益需要开发者决定的改动：Hybrid 模式不用全局线搜索（每次迭代可少一遍全局 pass）、RenderCore 提供 `uvec4` 别名视图，或者上节的求解器步长保护。

### 验证

- Release 构建全部目标通过；ctest 4/4 通过；`Whimsical --smoke --validation` validation errors=0；`git diff --check` 通过。
- 编译警告 C4458（`input`）在 HEAD 中已存在。
- 探针（kernel dump/override 钩子、`PROBE_INDEX_MOTION`）已删除，`Instance.cpp` 与 HEAD 一致。
- `captures/fluid-opt/`：`bin/head.exe`（HEAD 构建）、`bin/final.exe`（最终构建）、`bin/wrap.exe`（只做规则 1）；`ab.ps1` / `run.ps1` / `driver.js`（含逃逸注入）；`logs/final-*.log`（profile）、`logs/esc*-head-1.log` / `logs/fesc*-final-1.log` / `logs/far-*.log`（逃逸）。

## 编译速度（`182257d` 之后）

80k 流体从 `compile` 到第一次结果约 1.5 s → 约 0.45 s。拆分（HEAD）：CPU 计划编译约 370 ms，glslang 约 1050 ms（23 个一般版本与 8 个固定模式版本两批），管线创建约 75 ms。

### 保留的变化

- `fuseOwnedRelations`：所有者链改为按所有者索引的一张表（两遍计数），不再为 80k 个所有者各分配一个 vector；配方签名复用缓冲；`extract` 的成员判断用按 id 的位表代替 `std::set`。约 160 → 45 ms。
- `Compiler.cpp`：`RelationType::summedObject()` 每次都扫描公式节点，逐行遍历（着色优先级、着色、分组）改为读每个类型预先求好的结果。着色与分组约 −40 ms。
- 生成的 kernel 源码与 HEAD 逐字节相同。CPU 计划编译约 370 → 185 ms。
- RenderCore `ShaderCompiler`：不再每个程序启动一次 glslang.exe（单次启动约 200 ms），改为进程内多线程编译，批量接口不变。预编译 glslang 库使用 release CRT，因此放在 `whimsical_glslang` DLL 中（强制 `/MD`、`_ITERATOR_DEBUG_LEVEL=0`），边界只传 C 类型，Debug 与 Release 都可用。31 个 kernel 的 SPIR-V 与 `glslang --target-env vulkan1.2 -V` 的输出逐字节一致。
- 预编译 glslang 库由比 VS2019 新的 STL 构建（VS2019 链接时缺 `__std_find_trivial_1`），工程按开发者决定切换到 Visual Studio 2022（`tools/build.ps1`、README、engine.md）。

### 验证

- VS2022 Release 全量构建通过；ctest 4/4 通过；Debug `dynamics_tests` 通过。
- 80k 流体 300 tick：diag 0/0，末帧密度超额均值 0.0035，与之前处于同一水平。

## 打开 100k 流体例子的耗时

`Whimsical --project Projects/ConstraintLab` 从 `Lab.rebuild` 到第一次结果约 12.4 s → 约 1.56 s。原来约 11.5 s 花在 JSON 文本上：`X.describe` 生成的 11.6 MB 模型文档（主要是 300k 个初始坐标）以及 `stage` 的 100k 个实例变换，经过 `Json::dump` → duktape 解码 → JS 对象 → duktape 编码 → `Json::parse`，而 `Json::parse` / `dump` 每次都新建一个 duktape heap 再遍历。

- `Json::parse` / `dump` 改为原生实现，语义与 ECMAScript `JSON.parse` / `JSON.stringify` 相同（数字按 Number::toString 的最短表示输出）。仓库中 376 个 JSON / asset 文件（头部与 JSON 负载）加一个 300k 浮点和边界数字的样本，新旧实现输出逐字节一致，同样的输入同样报错；120 MB 上 parse 1276 → 73 ms，dump 700 → 36 ms。
- 新增 `assets/ScriptJson`：脚本值与 `Json` 直接互转，不经文本，语义同 `JSON.stringify`（undefined、函数在对象中省略、在数组中为 null，非有限数为 null）。`Engine.create` / `addComponent` / `setComponent` 等脚本桥、Dynamics 的 `describe` / `state` / `plan` 都改用它。
- 剩余（单次）：`build` 0.13 s，`describe` 0.13 s，`Engine.create`（含计划编译）0.37 s，`stage` 0.71 s（主要是 JS 里构造 100k 个初始实例变换），GPU 编译到第一个 tick 约 0.2 s。
- 验证：ctest 4/4；`Whimsical --smoke --validation` 与 ConstraintLab `--validation` 均为 validation errors=0。

## 100k：按空间顺序存放子步内的所有者状态（`4ab516c` 之后）

100,000 粒子（`fluid.js` 默认 50×50×40），4 substeps × 12 iterations，其余设置不变。只改了编译器（`CompiledPlan.h`、`Kernels.cpp`、`Schedule.h/.cpp`、`SumRelations.cpp`）；`lab.js` / `fluid.js`、JS 接口、physics IR、公式、参数、子步/迭代数、runtime 与 RenderCore 均未改动，没有新增测试。数值方法不变：精确线搜索、精确邻居维护和关系的 Gauss-Seidel 顺序都保留，只改变存储位置、访问顺序和查询界。

### 测得的事实

用 Nsight Graphics GPU Trace（`ngfx.exe --activity "GPU Trace Profiler" --auto-export`，命令行可用）和"重复一段代码、结果不变"的探针（不改变物理，因此计时可靠）得到：

- 线性化和转置的 SM 发射约 62–65%，L1 约 55–70%，主要耗在流式读取邻居行（约 9 MB/遍，放不进 L2）和循环本身。复制一份不含公式运算的遍历就要 +0.9 ms/tick；手工内联核函数最多快 5%。
- 推进阶段（线搜索回缩 + 墙/阻尼关系链 + 快照）受 DRAM 限制。在本机上，100k 线程的一条分散标量写入约耗 4 µs，分散读取和合并写入则便宜得多。原来推进按逻辑顺序遍历所有者，却按排序号写打包位置（3 条分散写，约 13 µs/次），这是它的主要开销之一。
- 线搜索最优步长在整个模拟中非常稳定（第 0 次迭代约 0.42，其后约 0.28）。把推进并入转置（PBF 的两遍形状）本身几乎不省时间：所有者工作在两个 kernel 里一样贵。按开发者的选择，这一轮不改数值方法。

### 保留的变化

1. **关系链端点特化**（`Kernels.cpp`、`Schedule.cpp`）。关系函数拆成解码入口和 `<name>_endpoints(Relation r, uint id0, Variable v0, …)` 主体。所有者链中，如果某次调用的各端点 id 及其变量记录（Variables 表在调度时仍是逐行的）对所有者行是仿射的，就直接以表达式 / 常量传入，例如墙法向 `Variable(300000u,…,1u,2u,100000u)`。flags、模式字、度量和地址因此成为程序文本里的事实，驱动可以常量折叠并外提读取。推进 2.05 → 1.72 ms/tick。
2. **所有者 piece 的物理顺序**（`OwnerPiece::order/slot`、`ownerKernel(…, ordered, publish)`）。求和域的线搜索 piece 和快照 piece 声明 slot → row（排序号 → 逻辑成员）和 row → slot。有序组合按 slot 遍历所有者，于是打包位置、位移、线搜索乘子和快照写入都合并成连续访问。
3. **子步内状态按 slot 存放**。只有有序区间索引才使用（`substepSlots()`），因为它只在子步第 0 次迭代重建，排序号在子步内固定；乘子又只在一个子步内存活（第 0 次迭代清零），属于求解器私有。具体做法：
   - 密度关系的乘子在线性化和线搜索中按排序号寻址；
   - 有序组合中的所有者链把每条拥有关系的乘子行存在 `relation(column(call,"slot"))` 处，是同一关系集合内的置换，参数和 history 仍按逻辑行读取；
   - 调度改为：预测 → 预测状态的独立快照 → 子步索引 → 头部（链 + 快照，按 slot）→ 行 / 例外 → 线性化 / 转置 / 推进。这样从头部清零乘子起，一个置换在整个子步内都成立。头部和独立快照都标为 `substep`。
   - 唯一的求和域从自己的打包快照读取所有者，所以头部和推进不再每次迭代发布 `x_q`，由子步末尾的 Finish 发布。为此线性化的锚点状态改从打包快照读取，值与原来相同。
   - 桶索引（小集合）可能在任意迭代重建，不使用上述按 slot 的存放。开发中曾在这里出错（1 万粒子穿底），已按此限制修正。
4. **位移记录**。线搜索位移由转置按排序号写、由所有者读，改为每行 4 字的记录，不再是 3 个 SoA 列。
5. **查询界改用"到索引单元格的距离"**（精确）。球查询的附加半径原来是成员相对建索引时位置的最大位移，现在是成员到其被索引单元格盒子的最大距离：仍留在原单元格内的成员贡献 0；被钳到边界的单元格在外侧方向视为无界。这比原来的界更紧，覆盖所有球查询（行重建、例外、溢出回退）。它也抵消了调度重排的副作用：墙投影发生在建索引之后，否则会抬高全局界。
6. **快照不再读 `cells` 记录**（有序区间）。单元格坐标取自有序组合本来就要读的排序号记录 `grouped`；索引重建的启发式条件改为"相对行列出位置的位移或单元格距离超过 0.1R"。这只影响何时重建，不影响正确性。
7. **对齐**。逐粒子记录数组按 32 字节分配，邻居行 tile 按 128 字节分配（每个 warp 步读一条完整的缓存线）。分配位置变化后，行 tile 曾意外失去原来的对齐，线性化慢了约 1%，这一项把它修正了回来。

### 测量结果

RTX 3080，Release，validation 开启，`captures/fluid-100k/ab.sh`（开启 profiler，150 tick），HEAD（`4ab516c`）构建与最终构建交替各运行 6 次，取中位数：

| 取样 | HEAD | 最终 |
| --- | ---: | ---: |
| 调度合计（profile） | 7.69 ms | 6.98 ms（−9%） |
| 线性化（48 次） | 1.80 ms | 1.76 ms |
| 转置并计算二次型（48 次） | 1.72 ms | 1.67 ms |
| 所有者迭代推进（44 次） | 2.13 ms | 1.45 ms（−32%） |
| 行构建（Index 12） | 0.73 ms | 0.74 ms |
| 异常（Index 13） | 0.37 ms | 0.38 ms |
| 其余索引阶段（含新增的独立快照） | 0.42 ms | 0.50 ms |
| 最后 30 tick GPU 平均 | 7.07–7.42 ms | 6.27–6.50 ms |

末帧密度超额均值两者都是 0.0043–0.0044，diag 均为 0/0。逃逸测试（tick 30 把一个粒子发射到 90 m/s 或 10⁶ m/s）逃逸后平均每 tick：HEAD 7.27 / 7.33 ms，最终 6.52 / 6.43 ms。小规模流体走桶索引路径：1 万粒子 2.52–2.59 对 2.65–2.69 ms，2.7 万粒子 2.76–3.05 对 3.07–3.34 ms，质量一致。

### 没有保留的实验

- 推进并入转置（固定 / 滞后步长，PBF 两遍形状）：转置升到约 82 µs/次，约等于原来两者之和，因为所有者工作的代价不随 kernel 变化。它还会改变数值方法，没有做。
- 行构建把每个 team 的区间拉平（共享内存前缀和，各 lane 均匀跨步）：0.79–0.96 ms，原来是 0.72–0.77 ms，更慢。与之前 32 lane 扁平扫描的结论一致。
- 按排序号存放的被索引点（让快照合并读取）：索引散射多出一条分散写，Index 11 从 0.115 升到 0.151 ms，推进没有改善。最后改成第 6 项，完全不读。
- 头部按 slot 收集 history 副本，推进合并读取：推进 −3%，总量没有可测收益，代码更复杂，已撤回。
- 所有 SSBO 加 `restrict`：无变化。
- 锁定 SM 时钟（`nvidia-smi -lgc`）：整体变慢约 12%，噪声没有减少（其他进程也在使用 GPU），已恢复。

### 剩余瓶颈

线性化和转置约占调度的 50%，接近当前数据格式（16 位排序号偏移的行、16 字节打包记录、标量读取）下的极限；进一步收益需要 128 位读取，也就是 runtime / RenderCore 提供类型别名视图（未做）。推进约 33 µs/次，受 DRAM 限制。每个子步的行重建约 185 µs，SM 发射 77%：每个锚点扫描约 280 个候选，只保留约 44 个。例外阶段约 8 µs/次，几乎全是延迟。与手写 PBF 的差距主要来自这个 IR 要求而 PBF 没有的部分（精确邻居维护、墙和阻尼的 XPBD 乘子、精确线搜索）以及每帧 48 次迭代；在保持语义的前提下，剩下的每一项都在 1–5% 量级。

### 验证

- Release 全量构建通过（只有 HEAD 已有的 C4456 / C4458 警告）；ctest 4/4。
- `dynamics_run` 全部流体运行（validation 开启，有错误时非零退出）正常退出；`Whimsical --smoke --validation` 与 `Whimsical --project Projects/ConstraintLab --frames 400 --validation` 均为 validation errors=0，实验面板 diag 0/0。
- 测量脚本与日志在 `captures/fluid-100k/`（`ab.sh`、`med.py`、`line.py`、`run_small.py`、`trace_summary.py`，GPU Trace 导出在 `trace*/BASE`）。
