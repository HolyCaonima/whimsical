# Dynamics：40k 流体的邻居行、索引复用与迭代尾部融合

ConstraintLab 流体例子（40,000 粒子，Hybrid，4 substeps × 12 iterations，`colorBudget=12`，dt 1/30）的完整 Dynamics GPU tick 从基线约 13.2 ms 降到约 6.3 ms，静止阶段从约 12.4 ms 降到约 5.05 ms，即整体约 2.1 倍、静止约 2.45 倍。每 tick 的 dispatch 从 736 降到 252。之后加入的按需 launch（第 6 节）让静止阶段再降约 0.1 ms（约 4.9 ms），整体均值在噪声内不变。质量指标与基线处于同一水平，invalid/singular 均为 0。这仍然慢于通常估计的手写 PBF（同预算约 2.5–3.5 ms），差距见“剩余瓶颈”。

## 保持的边界

- 用户的 `lab.js`、`fluid.js` 未修改；关系、残差、回缩、compliance 和自动微分表达式不变。
- 编译器只依据集合映射、有限支撑半径、差分范数和所有权等数学结构做决定；没有识别流体、墙或阻尼等具体对象，也没有匹配场景参数。
- RenderCore 只增加了经批准的通用 indirect dispatch（第 6 节），不含 Dynamics 概念；没有新增测试。

| 文件 | SHA-256 |
| --- | --- |
| `Projects/ConstraintLab/Content/scripts/lab.js` | `35dba5489310615561b22f0bfc2e92f010c88d38d51f9b201c3cb640e4d71ea0` |
| `Projects/ConstraintLab/Content/scripts/fluid.js` | `95736c3b3e73a5b699e6e4f7971932958063d44c59c55a6e013444c86b8ae866` |

## 保留的编译器变化

以下都只作用于 `CollectionTraversalPlan` 选择排序区间遍历的大集合（projected-line-search，固定行缓存超过 8 MiB）；小集合继续使用原来的行缓存路径。

### 1. 位移界的精确球查询，索引每个 substep 只建一次

排序索引记录建索引时的位置。快照在每次迭代后用 `atomicMax` 归约所有成员相对索引位置的最大平方位移 δ²（header 第 16 字）。若 |a−m| ≤ R，且成员自建索引以来最多移动 δ，则以当前锚点为中心、半径 R+δ 的球在旧索引上必然覆盖所有真实贡献者。查询按 z/y 行裁剪球体，在稠密排序中每一行的 x 区间是一个连续段；哈希模式逐单元检查。单元宽度改为 1.0R。

因此排序索引对任意运动都保持精确，只需在 substep 的第一次迭代重建：`SumDomain::IndexStage::substep` 标记这些阶段，`Batch::substep` 让 runtime 只在 iteration 0 记录它们，`dispatchCount` 同步计数。成员相对索引位置移动超过 0.1R 时请求下一个 substep 重建索引。

### 2. 带皮肤的邻居行与重建请求

每个锚点保存半径 (1+s)R（s = 0.1）内的成员行，供线性化和转置两次遍历在多个迭代中复用。行有效性的数学条件：若建行后任何成员漂移都不超过 sR/2，则任意两点的距离至多缩短 sR，行外成员不可能进入 R。快照比较当前位置与建行位置，超过 0.499·s·R 时请求重建；构建阶段只在被请求时启动，线性化的第 0 个 invocation 确认（请求与确认的形式见第 6 节）。没有邻居数量截断：超过容量（96）的行标记为溢出，消费者对这些行退回到上面的精确球查询。

行的存储：

- 每个字打包两个成员，存为相对锚点排序号的 16 位偏移。偏移超出 ±32767 的行同样标记为溢出、走精确球查询，因此不需要第二种行格式。
- 32 行组成一个 tile，每个 team 宽度的字块连续存放，使一个 warp 步读取一条连续缓存线。
- 构建阶段每行 8 个 lane，先在 shared memory 中收集成员，再一次写出打包后的字。
- 消费者每行 4 个 lane（`planCollectionTraversal` 返回 4），每个 lane 一次处理一个字中的两个成员。

### 3. 所有者输出按逻辑顺序执行

`lineSearchOutput` 的 invocation 按逻辑所有者排列，所有者程序及其写回都按逻辑 ID 合并访问；只有行数据经过排序号映射。

### 4. 线搜索步长归约并入所有者阶段

`OwnerOutput` 新增 `groupBegin`：在边界检查之前，由 workgroup 的所有 invocation 执行。投影二次型的部分和不超过 4096 对时，每个所有者 workgroup 直接归约全部部分和并计算步长，原来每次迭代单独的 “Minimize projected quadratic” dispatch 被删除；更多部分和时，先用多级 “Reduce projected quadratic” 降到 4096 以内。求和顺序与原单组归约相同；NaN/Inf 诊断只由全局第 0 个 invocation 报告一次。

### 5. 所有者关系链先发出独立的表读取

`fuseOwnedRelations` 生成的关系链中，各次调用只通过所有者状态互相依赖。现在所有调用的 region 表读取在第一次调用之前一起发出。

### 6. GPU 发起的按需 launch（indirect dispatch）

索引阶段和构建阶段是否有工作由 GPU 上的快照决定，CPU 录制时并不知道。原来它们每次都以完整网格 dispatch，再由每个 invocation 比较请求/确认 epoch 后早退。现在“请求”直接表示为工作组数：

- RenderCore：新增 `Access::Indirect` 和 `dispatch(program, buffer, byteOffset)`；图分配的 buffer 都带 indirect 用途。这是通用接口，不含 Dynamics 概念。
- 计划：新增 `BufferRole::Launches`，`Batch::launch` 指向其中一组 (x, y, z) 工作组数，`UINT32_MAX` 表示按 `count` 直接 dispatch。runtime 对带 launch 的 batch 录制 `vkCmdDispatchIndirect`。Auto 模式的原生序列把 indirect 读取并入已有的冒险分析：读取前若 Launches 被写过，屏障加上 `DRAW_INDIRECT` 阶段和 `INDIRECT_COMMAND_READ` 访问；序列 pass 向图声明 `Access::Indirect` 读取，跨 tick 的屏障也由图推导。
- 编译器：factor 域的索引阶段和构建阶段各自分配一组 launch，y 按 `invocation()` 的 65535 组换行规则在编译期写好，x 初始为 0。快照的 workgroup 尾部发现需要重建索引时写入全部索引阶段和构建阶段的 x，只需要重建行时只写构建阶段的 x；线性化的第 0 个 invocation 把它们清零。这与原 epoch 条件逐项等价，因此 header 第 3/6/17/18 字的 epoch 和各阶段开头的早退判断都已删除（header 从 19 字缩到 17 字）。

结果：同一会话中交替运行修改前后的可执行文件各 4 次（450 tick）：

| 取样窗口 | 修改前 | indirect |
| --- | ---: | ---: |
| tick 2–450 平均 | 6.022 / 5.848 / 5.842 / 5.914 ms | 5.910 / 5.884 / 5.806 / 6.113 ms |
| 最后 30 tick 平均（静止） | 5.012 / 4.964 / 5.012 / 5.094 ms | 4.820 / 4.933 / 4.894 / 5.048 ms |

静止阶段平均从 5.02 降到 4.92 ms（约 2%），整体均值差异在噪声内。收益小的原因：空闲阶段的代价主要是它前面的屏障，而不是早退的工作组；profile 中每个空闲索引阶段仍约 3.5 µs，和原来的早退 dispatch 相当。静止阶段构建阶段平均每次 6.5 µs，主要是真实重建：少数快粒子（velMax 0.3–1.0，而速度 RMS 只有 0.015–0.03）每一两个 substep 就超过 0.05R 的漂移阈值。早退 dispatch 的代价随网格增大，indirect 的空闲代价不随粒子数增长，因此更大的集合收益会更明显。Vulkan synchronization validation 在流体与应用 smoke 运行中均无报错。

## 测量结果

RTX 3080，Release，无 validation，`captures/fluid-40k/run.ps1`（300 tick）。基线是本轮修改前 HEAD（`28e313a`）构建的 `captures/fluid-40k/baseline_dynamics_run.exe`。最终版本连续运行三次。流体运动是混沌的，并行归约顺序会改变轨迹，因此单次运行之间有约 ±0.2 ms 的波动。

| 取样窗口 | 基线 | 最终（三次） |
| --- | ---: | ---: |
| tick 2–300 平均 | 13.160 ms | 6.327 / 6.365 / 6.078 ms |
| tick 11–30 平均（剧烈运动） | 12.703 ms | 6.900 / 6.951 / 6.851 ms |
| 最后 30 tick 平均（静止） | 12.401 ms | 5.058 / 5.058 / 5.047 ms |

300 tick 末帧质量：基线密度超额均值 0.001708、P99 0.008508、最大 0.0521；最终三次分别为 0.001706 / 0.001687 / 0.001765、0.0084 / 0.0083 / 0.0087、0.062 / 0.054 / 0.054。invalid/singular 均为 0。最大值有波动，不能声称所有质量指标逐次无退化。

每 tick GPU profile（开启详细 profiler）。最终列取 450 tick 运行（含第 6 节）静止阶段的最后 13 个 profile 样本；基线各阶段相差不大，取其 profile 的全部样本（跳过前 3 个）：

| 阶段 | 基线 | 最终 |
| --- | ---: | ---: |
| 转置并计算二次型（48 次） | 4.845 ms | 1.651 ms |
| 线性化（48 次） | 4.745 ms | 1.666 ms |
| 所有者迭代推进（44 次） | 1.420 ms | 1.141 ms |
| 索引与邻居行（基线每次迭代 11 个阶段；最终每个 substep 11 个索引阶段加每次迭代一次构建，空闲时 launch 为 0） | 2.245 ms | 0.493 ms |
| 二次型归约（48 次） | 0.158 ms | 0（并入推进） |
| 调度合计 | 约 14.2 ms | 约 5.36 ms |

剧烈运动阶段的调度合计约 6.6–7.4 ms，其中邻居行几乎每次迭代都被请求重建，构建约 1.6–2.4 ms/tick。

## 没有保留的实验

- 统一参数时让行参数读第 0 项（去掉每锚点两次分散读取）：无可测收益，已撤回。
- 构建 team 4：与 8 无差别。消费者 team 2 与 4 持平，8 更慢。
- 皮肤 0.15 / 0.2 / 0.3：0.2 让剧烈运动阶段的 profile 明显下降，但重复的 300 tick 均值为 6.14 / 6.25 ms（0.15 / 0.2），均不如 0.1 的 6.06 ms；0.3 全面更慢。
- 小变量集的仿射描述符、常量绑定宽度、去掉 `loadMultiplier` 早退判断、去掉快照 shared `atomicMax`：每项至多约 0.05–0.08 ms/tick，作为探针测量后撤回。
- 更早的实验：0.5R 单元宽度、流水化的行循环、每次迭代直接球查询而不建行，均更慢。
- 关系函数拆成 gather（全部不依赖可写端点的读取：region、端点 id、只读输入、multiplier、compliance、degree）和 solve 两段，所有者关系链先发出全部 gather 再依次 solve：所有者推进从 1.14 升到 1.80 ms/tick。管线统计（`VK_KHR_pipeline_executable_properties`）显示寄存器从 39 升到 72，并有约 12.8 KB 的 shared memory 溢出；把 gather 结构缩到最小后仍为 1.28 ms，逐个调用 gather+solve 则与原来持平。链路受延迟限制的假设不成立，已撤回。
- 求和数据各数组按 16 字节对齐：驱动没有自动把同一记录的标量读取合并成向量读取（二进制大小不变），无收益，已撤回。
- 探针：在同一 binding 上额外声明一个 `uvec4[]` 视图，邻居成员的位置和 Δλ 一次 16 字节读取。线性化约 −10%，转置约 −3%，调度合计约 −2～3%（静止约 0.1 ms/tick）。实现需要让编译器拿到 buffer 的第二种类型视图，牵涉 runtime/RenderCore 的绑定声明，收益不足以支撑，未实现。

## 剩余瓶颈

- 两次邻居遍历各约 34 µs/次。行字节减半只改善约 4%（静止）到 8%（运动），team 宽度也几乎无影响；成员记录改为单条 16 字节读取（读取指令约为原来的 1/3）也只改善 3–10%，因此主要瓶颈不是 L1 读取次数，更可能是每个成员的依赖延迟与每锚点的固定开销。本机没有 Nsight Compute，无法进一步定位。
- 所有者推进约 24 µs/次，其中约 12 µs 是关系链（5 个墙面不等式和一个 3 行阻尼），其余是步长归约、所有者更新和快照。拆成 gather/solve 两段以提高内存级并行的尝试因寄存器压力反而更慢（见上节），逐项探针也没有找到单一原因。
- 索引与邻居行在静止阶段约 0.45–0.5 ms/tick。空闲阶段已用 indirect dispatch 跳过工作组，剩下的是每个阶段前的屏障（11 个索引阶段每 substep 一次）和由少数快粒子触发的真实重建。进一步降低需要算法层面的改变，例如按行判断有效性、只重建漂移过大的行，或把 11 个排序阶段合并成更少的 dispatch。

## 发现的既有问题（未处理）

把流体临时缩成 10,000 粒子（20×25×20 网格，走原有的非排序行缓存路径）时，当前版本三次中有两次在约 200 tick 内发散（坐标达到 1e5–1e6），基线可执行文件两次中也有一次发散；未发散的运行两者一致（超额均值约 0.00025）。诊断计数仍为 0/0。该问题在本轮修改之前就存在，与 40k 排序路径无关，未在本轮处理。

## 验证与原始记录

- Release 构建通过：`dynamics_run`、`dynamics_tests`、`Whimsical`。
- ctest 全部 4 项通过（`dynamics_solver`、`dynamics_sum_dsl`、`console_system`、`content_mounts`）。
- `dynamics_run` 默认开启 validation 与 synchronization validation，流体运行无报错；`Whimsical --smoke --validation` 正常退出，validation errors=0。
- `git diff --check` 通过。
- 性能场景与脚本位于 `captures/fluid-40k/`：`run.ps1`（`-Exe` 可指定可执行文件）、`driver.js`、`profile.py`；第 6 节之前的 profile 为 `final-profile.log`，含第 6 节的为 `indirect-profile.log`，基线 profile 为 `baseline-profile.log`；`rows_dynamics_run.exe` 是第 6 节之前的构建，`baseline_dynamics_run.exe` 是 HEAD 构建。
