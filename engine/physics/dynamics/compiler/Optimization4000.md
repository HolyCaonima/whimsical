# 4000 规模编译与执行优化（2026-09-11）

本轮基线为 `f44c1cff1ba15102c1f77d15f95c7b3cf35ecb42`。改动仅在 Dynamics compiler 内，用户 DSL、项目脚本、公式、自由度、集合关系、求解策略和 RenderCore 均未改。这里的示例名称只用于描述测量对象；优化选择不读取任何对象、字段或关系的名称。

## 参考与设计边界

- [Simit 2016，第 9 节](https://commit.csail.mit.edu/papers/2016/simit.pdf)：利用图与张量索引之间的对应关系，避免不必要的中间物化和数据转换。这里保留逻辑关系行，独立选择元数据的物理表示，并复用已经解出的成员编号。
- [Ebb 2016，第 6.1 节](https://graphics.stanford.edu/~mdfisher/papers/ebb.pdf)：仿射索引可以编译成 ID 的算术表达式而无需存储；关系分组与隐藏索引属于底层执行表示。本轮将前者用于九列关系元数据，将稳定分组用于 CPU 着色优先级。没有引入用户 GroupBy 或空间结构声明。
- [Newton XPBD 源码](https://github.com/newton-physics/newton/blob/main/newton/_src/solvers/xpbd/solver_xpbd.py)：借鉴空间候选与精确关系计算分离的方式。现有编译器已从平方差公式推导 grid；本轮继续降低候选的精确判断成本。Newton 的具体对象类型、方程和刷新频率不迁入编译器。
- [Newton MPM 文档](https://newton-physics.github.io/newton/latest/api/_generated/newton.solvers.SolverImplicitMPM.html)：MPM 涉及网格离散和材料模型。当前输入表达的是既有自由度上的关系，不能把它自动替换为 MPM 后仍声称数学模型相同。因此没有引入 MPM。

## 最终实现

### 在读取关系行之前筛选特征范围

已有 `differenceBound` 证明标量不等式具有 `sum((a[k]-b[k])²) - R(parameters) >= 0` 或反号等价形式。网格只负责提供保守的邻格候选，邻格中的许多组合仍远大于真实作用范围。此前这些组合会读取乘子、参数、端点元数据并进入完整活动函数。

现在查询先读取证明选出的特征列，计算平方差。只有平方差不超过 `1.5625 * maxBound` 才调用原来的活动函数。1.5625 是现有 1.25 倍半径裕量的平方，覆盖有效数值范围内的浮点重排误差。这一步只做保守排除；实际 residual、导数、enabled、degree 和修正仍由原生成器决定。

两侧特征仍来自同一 Jacobi 快照。无效边界或坐标继续直接遍历完整域，绕过新增筛选。非零乘子的旧队列继续独立参与，已经离开范围的关系仍能释放乘子。参数 patch 继续使最大边界缓存失效。没有固定邻居上限、近似截断或减少数值迭代。

### 将仿射元数据编译成寻址算术

完成 Schedule、candidate lowering 和端点物理布局后，检查每个关系集合的九列不可变元数据是否都满足 `base + row * stride`，逐行验证整数值完全一致。符合条件的大集合只保留内部描述，shader 中直接生成对应算术；参数、compliance、history、乘子和 enabled 的公开行地址保持原样。

小于 128 行的集合和非仿射集合继续存表；相邻的存表范围合并。`relation(id)` 用平衡的范围分支选择算术或表地址，避免为每个小集合展开一份代码。动态端点仍写原来的端点缓冲，拓扑变化仍重新编译。

本例的 8,022,000 行元数据全部可压缩，原九个 uint/行占 288,792,000 bytes（275.41 MiB），最终只需空缓冲占位和三份内部仿射描述。这是该元数据表的节省，**不是总显存或编译峰值内存降到常数**。CPU 前面的分析仍会物化关系行。

### 复用端点与稳定优先级分桶

验证和 writable incidence 遍历中，每行只定位一次 binding domain、解一次成员编号；同一行的端点去重复用已解析的 ID，不再重复三角域反解。

着色优先级从逐行比较排序改为稳定分桶。键仍然是 writable 端点数升序、残差行数/公式节点数/tangent 列数降序；同键的逻辑 ID 按原始升序追加。贪心着色收到的顺序完全相同，没有改变颜色选择、覆盖策略或 Jacobi weighting。省去每条关系的一份完整优先级记录，也避免在大量相同优先级之间反复比较。

稳态调度仍为每 tick 205 次，4 子步 × 12 迭代。按类型拆开 candidate solve 的探索版本因额外调度更慢，未保留；最终求解程序仍使用既有融合方式。

## 实测

RTX 3080，Release，Vulkan validation 开启，无界面 `dynamics_run --profile`。性能脚本直接拼接项目原始 `lab.js` 和 `particles.js`，只补运行壳及固定种子 12345；保持 4000 个三维可写自由度、原有只读输入、8,022,000 条关系、Hybrid/colorBudget 12、1/30 秒 tick、4 子步、12 迭代。每次运行 90 tick，在 tick 1 和 90 回读完整状态。

按 before1 → after1 → before2 → after2 交替运行。GPU 数字是完整 Dynamics 提交的时间戳区间，不是渲染帧率。编译数字在原生 `X.compile` 调用前后计时，**包含 model commit 与 CPU 计划编译，不包含随后 GPU 程序创建**。

| 指标 | 优化前，两轮 | 最终，两轮 | 两轮均值变化 |
| --- | --- | --- | --- |
| 编译提交 ms | 5602 / 5615 | 3663 / 3635 | 5608.5 → 3649，减少 34.9% |
| GPU ms，tick 46–90 | 7.740 / 7.815 | 4.565 / 4.986 | 7.778 → 4.776，减少 38.6% |
| GPU ms，tick 21–90 | 7.433 / 7.476 | 4.302 / 6.215 | 7.454 → 5.259，减少 29.5% |

没有锁定 GPU 频率，第二轮最终版本存在明显耗时波动；上表保留两轮和两个窗口，未只取最快探索结果。分阶段探索表明候选查询由约 5–6 ms 降至约 1.3–1.7 ms，具体采样点不同，不能把这些单点当成配对加速比。

### 数值观察与验证

| 比较 | tick 1 全量状态最大绝对差 |
| --- | ---: |
| before1 / after1 | 7.15e-7 |
| before2 / after2 | 9.54e-7 |
| before1 / before2 | 7.15e-7 |
| after1 / after2 | 9.54e-7 |

tick 1 的最大相对重叠为 2.5351%，边界违反为 0.00087409，四次相同。tick 90 的最大相对重叠分别为 before 11.2266% / 9.9538%、after 12.8039% / 11.4778%；边界违反分别为 before 0.004317 / 0.004253、after 0.004434 / 0.003710。固定迭代预算仍有误差，本轮不宣称改善收敛质量。长期状态会因 GPU 浮点原子累加顺序而分化，基线重复运行也会分化；数学筛选的正确性来自上述证明与既有针对性回归，而非长期逐位一致。

四次运行 invalid/singular 均为 0/0，进程正常退出，无 Vulkan validation 错误。Release 的 `dynamics_run`、`dynamics_tests`、`Whimsical` 构建通过；既有 Dynamics focused checks、候选生命周期、单行参数扩大和范围回退对照检查通过。没有新增或修改测试。

原始数据在 `captures/compiler-perf/`：`paired-dsl.js`、`before{1,2}.log`、`after{1,2}.log`、`comparison.json`、`final-build.log`、`final-tests.log`。日志保留逐 tick GPU 时间、分阶段 profile 和两次完整状态采样。重跑命令：

```powershell
& build/bin/Release/dynamics_run.exe captures/compiler-perf/paired-dsl.js --profile
```

## 尚存架构成本

网格索引已从用户公式自动产生，本轮消除了更多不必要的查询和元数据读取。CPU 编译仍按全部逻辑行分析，参数与乘子等公开字段、最坏容量双队列仍为 dense；本例的求解核也仍有约 2 ms 的成本。因此不能宣称端到端 O(N) 或完全稀疏化。

进一步降低编译时间与容量，应把内部逻辑域和可变字段的物化延迟到真正需要时，支持 uniform 字段、稀疏页和可增长队列，同时完整保留逐行 patch、读取、迁移及密集回退语义。这个方向属于内部表示演进；不应要求用户改公式、添加 grid 声明或选择项目专用求解器。
