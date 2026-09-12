# Dynamics 编译器：候选域完整缓存与执行优化

2026-09-12，基线 `0f21b19`。本次保留的实现只修改 `CandidateDomains.cpp`、`Compiler.cpp`、`Kernels.cpp`。项目的 `lab.js`、`particles.js`、自由度定义、关系公式、集合绑定及求解策略没有修改。没有使用 subagent，也没有新增或修改测试。

## 结果与瓶颈

当前示例包含 40,000 个可写 R3 自由度和 800,220,000 个逻辑关系，其中上三角域为 799,980,000 行。已有编译器会推导有界候选域；正常迭代并没有扫描全部八亿行。剩余成本来自每次 Jacobi 迭代的候选活动计算、关系求解和并行归约。

Release、RTX 3080、Vulkan validation 开启，保持 Hybrid、4 子步 × 12 次迭代、colorBudget 12、dt=1/30。串行交替运行基线 / 优化 / 基线 / 优化，每次 180 ticks，以下为 tick 121–180 的 GPU submission 时间均值：

| 轮次 | 基线 GPU ms | 优化 GPU ms | 基线 CPU 编译 ms | 优化 CPU 编译 ms |
| --- | ---: | ---: | ---: | ---: |
| 1 | 11.606 | 8.726 | 97 | 84 |
| 2 | 11.315 | 8.612 | 94 | 78 |
| 平均 | 11.460 | 8.669 | 95.5 | 81.0 |

该采样区间的 GPU 时间减少 **24.36%**，吞吐约为基线的 **1.32 倍**。更宽的 tick 61–180 区间，两轮分别为 11.337 → 9.053 ms、11.401 → 8.856 ms，变化方向一致。CPU 编译时间减少约 15.2%；这里计量 `X.compile` 的 CPU commit/plan 工作，不包括随后异步创建 GPU 程序的耗时。GPU 时间也不等同于整个应用的帧时间。

另用 90-tick workload 开启阶段时间戳，取 sequence ≥60 的四个采样：

| 阶段 | 基线 ms | 优化 ms |
| --- | ---: | ---: |
| Index candidate domain | 0.258 | 0.312 |
| Query candidate domain | 4.431 | 2.672 |
| Solve Jacobi candidates | 5.685 | 5.735 |
| Gather R3 | 0.454 | 0.450 |
| 整个 Dynamics submission | 11.062 | 9.456 |

阶段测量采用独立运行和不同采样点，用来定位收益来源，不与 180-tick 对照混算。主要收益明确落在候选查询；关系求解及其 CAS 浮点累加仍是主要成本。正常稳定调度仍为 205 dispatch/tick，没有通过减少迭代、子步或同步阶段获得收益。

GPU 没有锁频；采样器记录了时钟、温度和功耗，整个进程也包含启动与读取状态的空闲区间。这是该设备和该输入上的两轮观测，不能推广成所有设备、所有状态下固定的加速比。

## 从相关系统吸收的原则

- [Ebb，SIGGRAPH 2016](https://graphics.stanford.edu/~mdfisher/papers/ebb.pdf)：把用户的关系抽象、计算阶段和底层数据布局分开。本次优化操作的是集合域、字段访问与生命周期，不引入物体类别。
- [Simit](https://people.csail.mit.edu/jrk/simit.pdf)：通过图及索引表达式的编译消除不必要的组装和中间数据。本次将候选集合的并集作为可复用执行表示，避免反复拼接相同工作；没有照搬其求解模型。
- [Taichi](https://yuanming.taichi.graphics/publication/2019-taichi/taichi-lang.pdf) 与 [AsyncTaichi](https://mingkuan.taichi.graphics/publication/2020-asynctaichi/asynctaichi.pdf)：稀疏数据布局与列表生成应受数据依赖管理，消除冗余工作必须保留状态变化的依赖。本次沿用已有位移证明决定缓存有效期，并补上乘子状态的完整性条件。
- [Newton 1.5.0 XPBD 实现](https://raw.githubusercontent.com/newton-physics/newton/v1.5.0/newton/_src/solvers/xpbd/solver_xpbd.py)：借鉴空间查询与精确求解的分工。候选列表只决定枚举哪些关系，精确活动判定和原有求解公式仍然执行；没有复制其对象目录或固定刷新频率。

这些原则都落实在编译器已经掌握的数学事实中：仿射绑定、单行不等式、有界差分特征、Jacobi 快照、关系乘子。编译器不识别 particle、cloth、rope 或 spring。

## 完整候选缓存

原来每轮有两个来源：几何候选缓存，以及上一轮带非零乘子的活动队列。前者跳过非零乘子行，后者重新执行普通活动判定。即使几何列表有效，仍需要遍历旧队列、解码逻辑行端点并读取状态。

现在对**恰好一个可索引域，且关系类型具有已知端点映射**的计划，缓存保存：

`完整缓存 = 构建快照中距离不超过 1.4r 的行 ∪ 距离超过 1.4r 且带非零乘子的行`

重建时，几何查询写入第一部分；旧队列只将第二部分补入缓存。两部分用同一快照、同一平方距离和同一阈值分割，因此互斥，不需要额外去重表。缓存命中时直接消费这个并集，使用已有 mapped activity 生成器处理关系启用状态、活动度和非零乘子的释放；不再遍历旧的 indexed 队列。普通关系继续按原调度处理。

单域限制来自当前缓存存储与融合查询的所有权。多索引域仍走原路径；本次没有为某个项目增加特殊分支。

### 完整性与生命周期

1. 已有缓存以 1.4r 构建；任一端点相对参考快照移动超过 0.19r 就触发重建。若当前距离不超过 r，则构建时距离最多为 r + 0.19r + 0.19r = 1.38r，仍在列表内，保留 0.02r 的浮点余量。
2. 非零乘子可能在距离范围外仍需释放，不能仅靠几何邻域判定无工作。构建时的非零乘子离群行单独保留；后续新产生的非零乘子只能来自已经参与求解的缓存行。因此有效缓存中始终包含需续接的行。
3. 当距离已超过几何界，但乘子仍非零时，缓存消费仍执行精确活动判定和求解；乘子归零后可跳过该行。缓存只改变执行表示，不缩小逻辑关系集合。
4. 参数补丁、无效特征坐标、缓存容量不足沿用已有失效机制。活动队列溢出或从完整域回退重新收集时，也使完整缓存失效：这些路径可能产生缓存之外的乘子。
5. 子步边界通常通过已完成的活动队列清零 indexed 乘子。若该队列溢出，截断列表无法覆盖完整域回退写过的所有乘子，此时显式清零对应逻辑域，再回到稀疏执行。这是溢出恢复路径，不是正常迭代成本。

缓存消费的工作组循环从各工作组自己的起点开始。完全不覆盖缓存行的工作组直接退出，避免原来所有查询工作组都经过一次无用的组内屏障。有效工作组中的尾部 lanes 仍共同经过屏障，保持组内归约契约。

## 编译过程与地址生成

`lowerCandidateDomains` 原来为全部逻辑关系建立 `vector<bool>` 成员表。仅此例就分配约 100.03 MB，尽管八亿行域已经有 deferred Jacobi 证明。

现在只对实际物化的 Jacobi 行建立排序成员表；deferred 域直接使用其证明。检查连续绑定域时，用 lower_bound、区间长度和首尾行判断覆盖；该判断依赖现有 solve batches 对逻辑行不重复分区的契约。本例额外成员表约 0.96 MB。这里消除的是编译临时分配，**没有**声称消除了 GPU 端稠密乘子存储。

集合映射的反解抽取为公共生成函数 `bindingMembers`，供普通端点读取、已知映射的关系输入和离群行保留共同使用。上三角映射保留原浮点估计与整数修正，不引入第三份不同实现。

## 输入、数值与验证记录

测量脚本原样嵌入当前 `lab.js` 和 `particles.js`，仅由外部 runner 固定随机种子 12345、推进 tick、读取状态及时间戳。脚本正文包含检查已通过，原项目文件哈希为：

| 文件 | SHA256 |
| --- | --- |
| `lab.js` | `12713cc25a69752e6116b2d91d5bc620f86e235dcb065233b75bf995cdb7668e` |
| `particles.js` | `c31b908ed29089c57e9a711aae07965d571a6ffce1180a0814067f50708698d9` |
| `paired.js` | `79ca820e702230a2ca8bd13d589f7ba48e635b5ae2b63f846c3131580d9d9f2b` |

四次运行的第一个 tick，全量 120,000 个状态分量相同，跨版本最大绝对差为 0。第 180 tick 的最大归一化重叠分别为：基线 0.1741 / 0.1707，优化 0.1985 / 0.1695；最大边界违约分别为 0.00260 / 0.00303、0.00276 / 0.00246。读取到的所有状态分量有限，invalid/singular 均为 0。

并行浮点归约的顺序不固定，长时轨迹会分歧；以上状态统计不是逐位或完整数值等价证明，也不支持声称收敛质量改善。正确性的依据是候选完整性、原活动判定与求解公式的保留，以及现有检查。本次没有增加迭代、降低精度或调整误差容忍值。

`dynamics_run`、`dynamics_tests`、`Whimsical` 的 Release 构建通过。现有 `dynamics_tests` 的 100,000 aliased relations、candidate iteration/lifetime、bound/fallback 和 Dynamics focused checks 均通过。没有新增测试文件。

可复查文件在 `captures/compiler-particles-sep12/`：`verified-before1.log`、`verified-after1.log`、`verified-before2.log`、`verified-after2.log`、各自的 `*-clocks.csv`，以及 `verified-comparison.json`、`verified-baseline-profile.log`、`verified-profile.log`、`final-existing-checks.log`。

最终对照二进制的 SHA256：基线 `b3dd2e43a42aedc901e2360536adb8f6d4a778312a3094977d69e5b9ed474f52`，优化 `286311034434488027196e6c5f731ba189e050fea3853614c42bb48281c9e138`。较早命名为 `final-before/after` 的一组由于恢复源码保留了旧时间戳，MSBuild 复用了基线二进制；这些数据已标记为无效优化对照，不计入本报告。

## 尚未解决的成本

求解阶段约 5.7 ms，当前通用浮点 scatter 仍使用 CAS 重试；逻辑上三角域的乘子数组也仍接近 3.2 GB。尝试过拆分求解核、增大并行组数、缓存更多成员信息、稀疏乘子寻址及动态 incidence 聚合，没有得到可靠的整体收益，均未保留。

下一项可独立评估的是可选的原生 float32 atomic add。通用设计应由 RenderCore 检测并启用 `shaderBufferFloat32AtomicAdd`，再由 Dynamics 选择归约实现；设备不支持时保留现有路径。依据 [Vulkan 特性定义](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceShaderAtomicFloatFeaturesEXT.html)，它是设备能力，不能由某个物理对象类型决定。

设备能力部分的可审阅补丁为 `captures/compiler-particles-sep12/native-atomic-support.patch`，已通过 `git apply --check`，**没有应用或测量**。依据根目录 `AGENTS.md` 第 4 条，这项 RenderCore 改动超出本轮明确授权的 dynamics/compiler 范围，等待开发者许可。当前提交内容不依赖它。
