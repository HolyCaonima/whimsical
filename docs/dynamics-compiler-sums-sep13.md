# 集合求和的并行线性化与执行映射（2026-09-13）

后续修正：本文记录的是最初 2,000 规模的性能优化阶段。其时间数据不构成收敛证明，初版也会在落地后发散。当前已增加缓存线性求解并修复 10,000 规模的 JS 显示统计；当前布局、性能、残差和 60 秒稳定性记录见 [收敛修正](dynamics-sum-convergence-sep13.md)。下文的 32 MiB 缓存预算和单轮求解描述均属于初版。

同一份 ConstraintLab Fluid 输入，在 Release / RTX 3080 / Vulkan validation 下，两轮对照的稳态 GPU 提交均值从 **21.436 ms 降到 5.233 ms**，约 **4.10 倍**吞吐、耗时减少 **75.6%**。项目文件、DSL、公式、参数和求解预算均未修改；代码改动仅在 engine/physics/dynamics/compiler。

## 性能问题

原路径有空间索引，但每条求和关系只映射到一个 GPU invocation。2,000 条关系只有约 16 个 128-lane workgroup，单个 invocation 串行查询相邻 cell、遍历全部候选成员，并在活动度统计、求解和散射过程中重复计算被积表达式及其导数。一个有界求和的逻辑关系实例不应强制对应一个执行线程。

原始分项样本中，索引占约 8.2 ms，活动度统计约 3.5 ms，求解约 10.7 ms。除此之外，summedRelations 会直接退出调度优化，连求和之前的固定端点颜色阶段也失去了已有的融合机会。

旁边的 40,000 成员示例使用二元不等式，当前例子使用带链式求导的集合约束，成员数量本身不能代表相同工作量。但这不能解释或合理化上述串行化和重复计算成本。

## 从相关系统采用的机制

- **Ebb**：关系、字段和执行后端分层；查询索引是运行时可选择的隐藏表示。其归约代码将生产计算与工作组共享内存归约结合，并明确区分读取、归约与独占写入阶段。这里保留查询域语义，在 compiler 中并行枚举候选、归约导数，且继续在全局活动度统计与修正消费之间保留 dispatch 边界。[论文 §4、§6](https://graphics.stanford.edu/~mdfisher/papers/ebb.pdf)
- **Simit**：图上的局部计算与矩阵视图之间的联系供编译器分析，而非要求用户手工转换数据；索引表达式可以融合。本次仅保留后续求解实际使用的局部块和加权导数，成员项导数留在寄存器中，不建立整个逻辑笛卡尔积的 Jacobian。[论文 §1、§9](https://compilers.stanford.edu/publications/simit.pdf)
- **Taichi / AsyncTaichi**：按访问局部性选择物理布局，依据状态依赖复用计算、融合任务。本次缓存仅属于当前 Jacobi 快照，固定端点颜色执行之后生成，下一次状态变化后重建；数据按关系行组织以服务成员并行的生产和散射。[Taichi](https://yuanming.taichi.graphics/publication/2019-taichi/taichi-lang.pdf)、[AsyncTaichi](https://mingkuan.taichi.graphics/publication/2020-asynctaichi/asynctaichi.pdf)
- **Newton / Warp**：区分 kernel 运算、提交和同步成本，减少小任务启动开销。本仓库 Runtime 已有固定执行序列，本次复用该机制并缩短 compiler 生成的阶段列表，没有引入 CUDA 后端或改动 RenderCore。[Newton 执行图说明](https://docs.nvidia.com/learning/physical-ai/getting-started-with-newton/latest/newton-fundamentals/core-concepts.html)、[Warp 性能说明](https://nvidia.github.io/warp/latest/user_guide/execution_and_performance.html)

Ebb 的 FluidsGL 使用规则网格和 CUFFT；Taichi 论文也包含不同离散方法。这些算法不能在锁定当前 DSL 时直接替换用户定义的数学关系。本次迁移的是编译和执行机制。

## 初版 lowering

    用户 Formula + BindingDomain
        → 已证明的有限支撑域
        → cell 查询：32 lanes / anchor，4 个查询组 / workgroup
        → 成员线性化：128 lanes / anchor
            terms 与投影 Jacobian 各计算一次，保留在寄存器
            共享内存归约 sum 值及固定侧导数
            outer AD → 链式法则 → 合并自由度别名
            活动度计数 + 局部有效质量矩阵 + 加权 Jacobian
        → 32-lane 归约全局活动度上界，求解并投影局部乘子
        → 按成员并行散射缓存修正
        → 原有 Gather 更新自由度

物理线程数与逻辑关系实例数分别保存。可变成员阶段不再被普通 relation dispatch 合并；其前面的固定端点颜色阶段仍可使用已有的颜色窗口。该示例每 tick 的计算 dispatch 从 644 降到 492，不含参数边界重建及传输。

局部块求解、乘子投影和有限值检查共用 solveAndProject()；流式回退与缓存路径不维护两套求解规则。加权切空间映射和修正散射复用原有生成函数。缓存记录包含自由度 ID、slot 和加权 Jacobian，不包含任何具体模拟对象类别。

## 边界与生命周期

- 并行线性化在 Auto 执行、非 Static 权重、已证明有限支撑、成员到可写自由度为单射时选择。该单射也证明固定侧别名导数在工作组内有唯一成员写入者；多个固定字段仍先合并后形成有效质量矩阵。
- 无法证明、重复成员自由度、只读成员域、超出资源预算的公式继续走原流式求和。多个求和输出、外层非线性及任意受支持空间仍由原有 AD 表达。
- 现有列表容量仍为 128。溢出或坐标/半径无法索引时，活动度和求解都完整遍历原逻辑域；缓存散射跳过这些行，不截断数学关系。查询溢出后无需继续收集成员。
- 共享存储按公式形状计算，预算 8 KiB；额外线性化存储只在当前 SumData 加缓存预计不超过 32 MiB 时选择。当前 2,000 规模增加约 5.19 MB 缓存，随保留侧成员数线性增长。没有把最坏情况逻辑积变成缓存容量。
- 缓存每次迭代重写，没有跨快照邻域复用。参数、enabled、inverseMetric、history 和状态更新继续走原接口。非活动行有本轮无输出标记；奇异或非有限局部块不发布旧修正。
- 颜色阶段、活动度统计、求解、散射与 Gather 的全局依赖保持分离；工作组屏障不依赖单个 lane 的有效性。32-lane 分组只使用普通 shared memory/barrier，不假定 Vulkan subgroup 的实际宽度。

## 对照和检查

测量驱动直接拼接原始 lab.js、fluid.js，只附加调用原 build/compile/step/read 接口的代码。每次从相同初始状态执行 90 tick，dt=1/30，Hybrid、4 子步 × 12 迭代、colorBudget=12；统计 tick 31–90。最终两轮交错运行 baseline / optimized，关闭详细分项采集，记录显卡频率。未锁频。

| 轮次 | 基线均值 | 优化均值 |
| --- | ---: | ---: |
| A | 21.498 ms | 5.244 ms |
| B | 21.374 ms | 5.223 ms |
| 两轮均值 | 21.436 ms | 5.233 ms |

这是无渲染测量驱动中的完整 Dynamics GPU 提交时间戳区间，包含提交内同步和传输，不是整帧 FPS，也不是隔离 shader 算术计时。分阶段试验从串行查询、线性化复用到并行归约逐步记录；独立物化所有成员项导数没有明显收益，已去掉。

Release 的 dynamics_run、dynamics_tests、Whimsical 构建成功。仅运行已有的 dynamics_solver、dynamics_sum_dsl，全部通过；覆盖显式展开对照、左右求和、别名、非对角 metric、多行块、history，以及有限支撑、活动权重、参数失效和列表溢出回退。没有新增或修改测试。四次性能运行的 invalid/singular 诊断均为 0，Vulkan validation 没有错误。

并行查询、树形归约和原子散射会改变浮点累加顺序。优化前后 tick 1 的最大状态差约 5.57e-6；tick 90 的 RMS 差约 0.377，基线自己重复运行的同一指标约 0.384。这些数据说明不能承诺长期逐粒子轨迹一致，不能用长时间逐位相等作为本次优化的结论。

原始日志、频率记录、完整状态回读、测量脚本和 SHA-256 输入指纹保存在 captures/sum-compiler-sep13/；[汇总数据](../captures/sum-compiler-sep13/summary.json)。源码边界通过 git diff 检查：Model、Adapter、Runtime、RenderCore、项目文件和 tests 均未改动。
