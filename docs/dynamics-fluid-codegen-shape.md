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
