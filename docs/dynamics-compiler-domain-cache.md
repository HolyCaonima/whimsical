# Dynamics：逻辑域编译优化与执行实验

2026-09-11，基线 `061e708`。本轮只修改 dynamics/compiler；用户 DSL、项目代码、自由度、关系公式和求解策略均不变。没有新增或修改测试。

## 参考与边界

- [Simit 2016](https://people.csail.mit.edu/jrk/simit.pdf) 将拓扑与张量索引对应起来，在原位计算中避免不必要的中间表示。本轮据此把重复的实例描述改成集合范围上的行视图，并在集合成员上证明调度属性。
- [Ebb 2016](https://graphics.stanford.edu/~mdfisher/papers/ebb.pdf) 将模拟、关系数据模型和执行后端分层；其网格耦合示例由用户建立、使用和更新连接，并不是编译器内置碰撞求解。这里保留逻辑行与用户字段，空间索引只是自由度特征上的内部执行选择。
- [Taichi 2019](https://yuanming.taichi.graphics/publication/2019-taichi/taichi-lang.pdf) 的 MPM 示例将数据布局和计算分开，并比较不同布局与缓存方式。本轮也实际比较了共享归约、连续归约槽和不同布局，未因局部 kernel 更快就保留整体更慢的方案。
- [AsyncTaichi](https://mingkuan.taichi.graphics/publication/2020-asynctaichi/asynctaichi.pdf) 使用状态依赖消除重复的活动列表生成。本轮曾据此探索附带几何有效期检查的候选缓存，但实测整体回退，已经撤回。静态拓扑不足以证明迭代中的几何候选不变。
- [Newton v1.5.0 XPBD](https://raw.githubusercontent.com/newton-physics/newton/v1.5.0/newton/_src/solvers/xpbd/solver_xpbd.py) 分离网格候选与精确计算。[MPM 示例](https://raw.githubusercontent.com/newton-physics/newton/v1.5.0/newton/examples/mpm/example_mpm_granular.py) 则包含网格尺度、材料与隐式离散选择。由这些语义可知，MPM 不能直接作为任意现有关系的等价 lowering；本轮没有替换数值模型。

编译器不识别 cloth、绳子、弹簧或粒子等对象种类。它只消费公式、集合索引、结构读写效果和执行成本。

## 编译与调度

`InstanceRows` 只为每个逻辑行保存一个颜色字节；ID、集合、局部行、数学类型和端点数从不可变集合范围取得。顺序遍历维护集合游标，随机访问保留范围定位。原来每行 24 字节的实例描述变为每行 1 字节的状态，没有新增端点展开表。

`BindingDomain::Cursor` 对顺序的 Zip、Product、Directed、Upper 和 UpperDiagonal 遍历使用整数递推；随机跳转仍调用原有精确索引映射。着色优先级中的类型属性只计算一次，颜色顺序、预算及 Hybrid 的选择规则保持原有逻辑。

端点验证访问实际使用的成员范围。仿射引用列在确认不发生整数回绕后验证首末地址及空间；显式列仍逐成员验证。Upper 域分别排除左侧最后一个和右侧第一个未使用成员。关系元数据直接写入目标缓冲，省去完整临时副本。

Schedule 将完整积、上三角和至少三个成员的去对角有向积按成员建立写连通分量。两侧都可写时，实际被使用的二部图连通；只有一侧可写时，只连接同一成员的字段。两个成员的去对角域保留逐行路径，避免错误合并其可能独立的两个分量。

随后对仿射域的引用像证明写入是否都属于同一分量。证明成立时批量写入关系归属，并按成员登记只读输入的消费者；其余域保持逐行分析。结构只读和数值为零的逆度量仍严格区分。

以 8,022,000 行计算，实例描述有效载荷减少 184,506,000 字节，删除的元数据临时副本为 288,792,000 字节，合计约 451.4 MiB。这是具体数据结构的载荷差，不是实测进程峰值内存降幅。颜色表、工作表、公开可变字段和部分调度状态仍按逻辑行保存，整体编译尚未变成 O(N)。

## 执行实验：均已撤回

本轮尝试了归约分片、共享哈希、链表、连续归约槽、不同布局、扩大邻域，以及跨迭代的候选复用。候选复用保存扩大半径内的成员对，通过成员相对参考快照的位移限制决定是否重建，并处理容量不足及活动乘子的续接。它增加了缓存写入、检查和消费成本；两轮严格串行交替对照中，GPU 平均耗时约从 2.991 ms 增至 3.504 ms，回退 17.1%，因此已全部撤回。

这些探索没有提供可靠的整体执行收益，均不在最终源码中。现有空间索引、活动队列、归约、同步屏障与 GPU 程序生成路径保持基线实现。固定调度仍为 205 dispatch/tick，没有减少子步或迭代。最终保留的改动仅优化 CPU 编译过程。

## 对照与验证

基线为 `061e708`；最终版本只含上述 CPU 编译优化。使用同一份原项目 DSL，固定 seed 12345：4,000 个可写 R3 自由度，8,022,000 个关系，Hybrid、4 子步 × 12 次迭代、dt=1/30、90 ticks。输入文件为 `captures/compiler-perf/paired-dsl.js`，SHA256 为 `F0AC4C4FAC3E414762F6742EDAFE9FF7AFFDAE4D8B8B0599BDDED4481F16DFDD`。

Release、RTX 3080、Vulkan validation 开启、未锁 GPU 时钟。严格串行运行 baseline / optimized / baseline / optimized。编译统计是 `X.compile` 的 commit 与 CPU plan 编译时间，不含 GPU 程序创建；GPU 统计是整个 Dynamics submission 的时间戳区间，不是帧耗时。

| 轮次 | 基线编译 ms | 优化编译 ms | 基线 GPU ms | 优化 GPU ms |
| --- | ---: | ---: | ---: | ---: |
| 1 | 3,524 | 1,713 | 4.219 | 3.277 |
| 2 | 3,507 | 1,687 | 2.972 | 3.197 |
| 平均 | 3,515.5 | 1,700.0 | 3.595 | 3.237 |

编译时间减少 51.6%，吞吐约为基线的 2.07 倍。表中 GPU 使用 tick 46–90 均值；两轮变化方向相反，较早 tick 21–90 区间也不显示稳定收益。因此不将合并均值的表观 10% 降幅宣称为执行加速。本轮尚未达到“大幅提高 GPU 执行效能”的目标。

构建 `dynamics_run`、`dynamics_tests` 和 `Whimsical` 成功。运行现有 `dynamics_tests`，候选迭代/有效期与边界/回退检查、Dynamics focused checks 均通过。未新增或修改测试文件。四次 workload 在 tick 1、90 的 invalid/singular 均为零。

首 tick 基线与优化全状态最大绝对差为 9.54e-7；第 90 tick 的跨版本差为 1.356 / 0.973，基线自身两次运行差为 1.604，优化版本自身差为 1.106。这些数据表明长时状态比较受并行浮点归约及轨迹分歧影响，不能作为逐位等价证明，也不能仅凭差值认定数值等价。

原始日志和汇总保存在 `captures/compiler-next/`：最终结果为 `before1.log`、`after1.log`、`before2.log`、`after2.log`、`comparison.json`；候选缓存的失败对照单独保存在 `cache-paired-*`，其他探索样本不混入最终结果。

下一条待验证路径是原生 float32 atomic add。当前 RenderCore 未启用 `shaderBufferFloat32AtomicAdd`，SPIR-V 访问分析也尚不支持 `OpAtomicFAddEXT`。通用修复应由 RenderCore 暴露可选设备能力并识别该指令的读写效果，Dynamics 再据此选择归约实现。已准备 `captures/compiler-next/native-atomics-proposal.patch` 并通过 `git apply --check`，没有应用到源码。它只增加通用后端能力，没有求解器或项目语义；依据 AGENTS.md 第 4 条，超出本轮已明确授权的 dynamics/compiler 范围，需要开发者确认。潜在执行收益尚未测得。
