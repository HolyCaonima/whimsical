# Dynamics：分段数学图与稀疏执行优化（2026-09-13）

同一份 10,000 规模示例，完整 Dynamics GPU 提交在落稳阶段从 **20.124 ms 降到 16.072 ms**，约 **1.252 倍**吞吐、耗时减少 **20.13%**。优化前后均完成 1,800 tick，即 60 秒模拟时间。没有达到专用 GPU 流体实现的性能，也没有用数量不同、数学不同的示例推算加速比。

项目脚本、Model、Adapter、DSL、初值、参数、时间步和求解轮数均未修改。保留 Hybrid、4 子步、12 次非线性迭代、每个快照 4 次缓存线性迭代。自由度、关系、求和对象及公式继续由用户定义；实现没有新增具体物理对象或关系名称判断。

修改位于 `FormulaGlsl.cpp`、`SumRelations.cpp`、Dynamics Runtime `Instance.cpp`，以及开发者明确批准的 RenderCore `ShaderAccess.cpp`。没有新增或修改测试代码，没有使用 subagent。

## 编译与执行变化

### 分段导数继续作为数学图优化

此前公式中出现任意 `min` / `max`，整个投影 Jacobian 就退出符号图路径，退回逐节点更新 adjoint 的代码。示例的有限支撑表达式因此生成了一串零 adjoint 判断；它们也阻断了编译器自身的公共子表达式合并。

现在将 `min` / `max` 的导数选择编码到编译器内部 DAG，继续做共享子表达式和可达性裁剪。内部比较节点保留原有相等时选择左侧的约定，并区分仅右侧需要导数的情况；没有向用户 DSL 添加运算。零 adjoint 仍通过值选择屏蔽未选路径中的奇异导数。含 `abs` / 显式 `select` 的原回退路径保留。

### 稀疏块按静态数学形状展开

缓存的 `J Δq` 原来逐条关联读取 slot，再用动态切空间宽度循环遍历分量。现在按已知切空间形状展开分量；宽度一致时删除 slot 读取与 switch，混合宽度仍只访问实际存在的分量。小块的数组下标成为常量，后端可以标量化计算与地址。

这项转换由空间和矩阵形状决定，适用于所有满足原缓存预算的集合关系。

### 转置记录同时照顾生成和消费

原加权转置将各分量放在相隔很远的整列中。线性化产生一条完整的小块时，需要向这些列分别散射写入。

编译器现在对不超过 16 字节的转置记录，连续存放行号和系数；例如一个行号加三个系数占 16 字节。更宽的块保留分列布局。`inverseWord` 统一表达物理地址，拓扑装配、系数写入、清零和归约消费使用同一映射。总内存预算、候选容量、正向 Jacobian 布局和 CSR 的生命周期没有扩大或改变。

### 根据硬件能力选择协作归约

Runtime 查询 compute subgroup 的宽度及 shuffle-relative 能力。满足条件时，缓存求解和转置归约在寄存器间交换值；128 线程线性化中的跨组部分继续使用共享内存，同组部分使用 shuffle，保留原归约树的顺序。

Vulkan 的 subgroup lane 编号不必等于 local invocation 编号，而且仅查询宽度不能证明所有协作组完整。因此快速路径还要求 128 个调用恰好形成四个 32-lane subgroup，再用 subgroup ID 和 lane ID 映射逻辑调用；其余情况使用原 local ID 和共享内存路径。没有依赖 CUDA、厂商名称或驱动版本。

RenderCore 的通用 SPIR-V 访问分析原来拒绝标准 shuffle 指令。现在将 345–348 四条指令归类为值交换，继续检查操作数和结果的资源来源，不将其视为隐藏的内存访问，也没有放宽其他未知指令的检查。

## 实测

设备为 RTX 3080，Release，Vulkan validation 开启。使用已有 `dynamics_run` 和 `captures/sum-layout-sep13/` 中的原始驱动；两份驱动都包含当前完整的 `lab.js`、`fluid.js`。SHA-256 和包含关系记录在汇总中。未锁 GPU 时钟，计时运行关闭详细 profiler，没有渲染。

时间是一次 Dynamics 提交内的 GPU 首尾时间戳区间，包含同步与传输，不是隔离算术吞吐或整帧 FPS。

| 相同模拟进度 | 基线 | 优化后 |
| --- | ---: | ---: |
| 长程运行中的 tick 31–180 | 19.357 ms | 15.643 ms |
| 独立重复运行的 tick 31–180 | 18.626 ms | 15.603 ms |
| tick 1621–1800 | 20.124 ms | 16.072 ms |

独立分项 profiler 只统计 tick 31 之后的完整求解提交，并按实际 tick 数归一化。分项采样与上表不同，不能直接与完整提交时间相加。

| 阶段，每 tick 合计 | 基线 | 优化后 |
| --- | ---: | ---: |
| 线性化 / AD / 系数生成 | 5.016 ms | 4.829 ms |
| 四轮缓存线性求解 | 5.507 ms | 4.044 ms |
| 四轮转置归约 | 4.916 ms | 3.681 ms |
| 候选检查、索引及转置结构 | 2.669 ms | 2.705 ms |

改进主要来自缓存线性求解与转置归约。数学图转换没有单独隔离出稳定加速比；不能把上述全部收益归因于 AD。

### 数值状态

两版全过程 invalid / singular 均为 0。复用已有 CPU 分析，对全部 10,000 个锚点累计原密度公式；不是显示面板的采样值。

| tick 1800 | 基线 | 优化后 |
| --- | ---: | ---: |
| 最大 Y | 0.53779 | 0.56709 |
| 平均密度超限 | 0.7002% | 0.7192% |
| 密度超限 P99 | 3.0843% | 3.2319% |
| 最大密度超限 | 15.7023% | 11.3871% |
| 速度 RMS | 0.004816 | 0.000788 |
| 最大速度 | 0.26779 | 0.02161 |

第一 tick 的最大位置差为 `9.5367e-7`。浮点求值、归约及并发候选排序会改变长期轨迹，以上结果说明本次模拟保持有界并有相近残差；不证明所有关系为零，也不证明优化后的残差总是更小。

### 构建与运行检查

- Release 的 `dynamics_run`、`dynamics_tests`、`Whimsical` 均已重编译。
- 仅运行已有 `dynamics_solver`、`dynamics_sum_dsl`，两项通过；没有增加测试代码。
- 将实际生成的五个相关 shader 关闭 subgroup 宏后，以 Vulkan 1.2 目标编译，全部通过。没有其他 subgroup 宽度设备的性能实测。
- 实际 ConstraintLab 完成 360 个渲染帧，收到 10,000 自由度 / 70,000 关系的 ECS 输出，求解诊断 0 / 0，正常退出，Vulkan validation errors = 0。截图中的求解 tick 为 106、GPU 提交为 16.08 ms；主程序的引擎 tick 不等于 Dynamics tick。

[完整数据和输入 / 源码 / 二进制校验值](../captures/sum-execution-sep13/summary.json)、[已有检查日志](../captures/sum-execution-sep13/checks.log)、[应用日志](../captures/sum-execution-sep13/application.log)、[应用截图](../captures/sum-execution-sep13/preview.bmp)。原始完整位置 / 速度、分项 profiler、生成 shader 与独立基线二进制保存在同一目录。

## 参考系统与边界

- **Simit**：图结构承载稀疏张量，并按数据归属执行计算。这里沿同一绑定关系选择正向与转置的物理布局，保持局部数学块的含义。[论文 §9](https://commit.csail.mit.edu/papers/2016/simit.pdf)
- **Ebb**：关系查询通过内部反向索引执行，存储表示由后端负责。用户继续声明集合关系，本次布局和归约选择无需用户维护邻域或反向关联。[论文 §6](https://sing.stanford.edu/site/assets/publications/ebb-tog2016.pdf)
- **Taichi / AsyncTaichi**：计算、存储布局与跨 kernel 状态依赖分层。这里保留符号导数图，按消费者的形状及访问方式降低执行成本。[Taichi](https://yuanming.taichi.graphics/publication/2019-taichi/taichi-lang.pdf)、[AsyncTaichi](https://mingkuan.taichi.graphics/publication/2020-asynctaichi/asynctaichi.pdf)
- **Newton / Warp**：执行图减少提交开销，协作计算使用寄存器与共享存储。当前 Runtime 已有固定执行序列；本次进一步处理序列内部的协作方式，没有替换数值模型。[Newton](https://newton-physics.github.io/newton/1.2.0/tutorials/00_introduction.html)、[Warp tiles](https://nvidia.github.io/warp/v1.16/user_guide/programming_model/tiles.html)
- subgroup 映射与完整性条件遵循 [Vulkan subgroup 规范](https://docs.vulkan.org/spec/latest/chapters/shaders.html)，标准 shuffle 指令见 [SPIR-V 规范](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html)。

Ebb 的 FluidsGL 采用规则网格和 FFT，不是当前用户定义的集合密度约束。旁边的成对关系示例也没有相同的耦合线性工作量，不能只按 10,000 与 40,000 的数量计算编译器开销。

当前每 tick 仍有 48 次非线性线性化、192 次缓存线性迭代，profile 中约 1,164 次计算 dispatch（包含候选失效检查及无需重建时提前返回的阶段）。线性化约 4.8 ms、线性求解约 4.0 ms、转置归约约 3.7 ms，仍是后续工作重点。本轮没有通过减少求解轮数、邻域成员或模拟时间来掩盖这些成本。

减少协作线程、整列标量遍历、将小块求解拆成单独阶段、将活动度原子计数替换为额外反向遍历，都没有在本次样本上得到稳定收益，未保留。特别是单线程遍历实验约 43.8 ms，反向计数实验约 17.5 ms；这说明消除同步或原子操作必须同时考虑数据访问与额外调度，不能仅凭指令种类判断性能。
