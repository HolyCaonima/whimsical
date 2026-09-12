# Dynamics：固定执行计划与字段表示优化（2026-09-12）

基线 `bb3d3756f449645449212fbaa26f8f6861c9ea47`。同一份现有 cloth DSL、32×32、Hybrid、4 子步 × 12 迭代，最终两轮交替运行的 GPU 提交均值为 **4.189 → 2.941 ms，减少 29.8%**。这不是整帧时间，也不是锁频条件下的纯 shader 基准。

修改限于 Dynamics 的 Model 字段表示、Compiler 和 Runtime。项目文件、DSL 解析和用户输入、数学公式、自由度与关系定义、颜色分配、求解次数及 RenderCore 均未修改。没有使用 subagent，没有新增或修改测试。

## 资料与采用的思路

- [Simit 2016](https://people.csail.mit.edu/jrk/simit.pdf) 将图连接与张量索引联系起来，避免不必要的中间物化，并融合索引表达式。本轮用已有 incidence 遍历证明端点互异，把运行时的别名合并工作变成编译期事实。
- [Ebb 2016，第 6 节](https://graphics.stanford.edu/~mdfisher/papers/ebb.pdf) 把关系模型、列式布局和索引的实现分开。这里统一显式重复数组与广播字段的内部表示；用户仍使用原来的对象、成员和关系接口。
- [Taichi / AsyncTaichi](https://mingkuan.taichi.graphics/publication/2020-asynctaichi/asynctaichi.pdf) 用跨 kernel 的状态读写依赖支撑优化。本轮在完整执行序列上计算依赖，并区分 dispatch 间同步与 kernel 内跨 invocation 的可见性。
- [NVIDIA Newton 的图捕获说明](https://docs.nvidia.com/learning/physical-ai/getting-started-with-newton/latest/newton-fundamentals/core-concepts.html) 说明固定工作流复用能减少反复启动的开销。当前 Dynamics 原本已经每 tick 单次提交，本轮借鉴的是固定计划复用：缓存执行依赖、共用 descriptor 绑定；没有引入 CUDA graph，也没有减少数值 kernel 数量。
- [Khronos 同步示例](https://docs.vulkan.org/guide/latest/synchronization_examples.html) 推荐为适用的计算依赖使用全局内存屏障，并区分 WAR 的执行依赖与 RAW/WAW 的内存依赖。Runtime 使用已有 native pass 接口执行这些依赖，RenderCore 继续管理序列之外的资源状态。

这些优化只依赖数值字段、端点身份、数学类型、状态访问和执行计划。编译器不识别 cloth、绳子、弹簧或粒子。

## 最终实现

### 固定计算序列

`Instance::Storage` 将 Auto 计划的数值阶段记录为一个 RenderGraph pass。第一次执行时根据已编译程序的读写反射，构造并保存整个序列的资源访问并集与同步位置；之后每 tick 只更新各 invocation 的步长、时间、tick 和迭代常量。

同一序列只绑定一次 descriptor set，连续相同 kernel 不重复绑定 pipeline。原来的 252 次计算 dispatch 和顺序全部保留，但不再为它们分别构建和解析 RenderGraph pass、分别绑定 descriptor set。一次计算 tick 仍然只提交一个 command buffer；没有声称实现 Vulkan command buffer 的跨 tick 重放。

依赖缓存跟随不可变计划和 Storage 的生命周期。参数变化时需要更新的候选边界在固定序列之前单独执行，不能混入首次执行后才缓存的序列；动态端点上传、拓扑构建、状态写入和回读继续由外围图管理。安装新计划会建立新的 Storage 和依赖缓存。Global 继续走原来的逐 pass 执行。

内部同步保留三类状态：无依赖、仅执行依赖、执行加内存依赖。WAR 屏障只清除已排序的读记录，不能把尚未发布的其他写记录一起丢掉。内存屏障对所有先前 shader 写建立后续读写可见性。诊断计数器只有独立原子累加，不制造序列内部的伪依赖；最后到宿主回读的依赖仍由外围图建立。

### 字段表示规范化

`Field::dense` 对相邻行的完整字节序列进行比较。所有行相同时使用已有的 uniform 表示，精确保留浮点位模式，包括正负零。

此前示例显式上传 1,024 份相同单位矩阵，表示上却被当成非统一字段，无法进入现成的单位逆度量路径。现在编译器自然生成对应的快速路径。此规则适用于任意字段、任意宽度，没有对模型名称或空间维数作判断。

这不是把可变数值永久折叠进 shader。GPU 的完整逻辑字段存储仍然初始化；后续 patch 使用原有机制撤销字段模式，更新的行和未更新的行都保留正确值。逻辑 ID、公开偏移、迁移及回读契约不变。

### 静态端点别名证明

Compiler 复用着色和 Jacobi incidence 的既有遍历，在数学类型范围内累计“可写端点互异”事实。只有所有实例均满足时，关系函数才省略 Jacobian 别名合并及重复写入判断。

任意一行出现重复可写端点，整个该类型保留原路径。动态拓扑及尚未展开证明的 deferred 域也保留原路径，未为了优化额外展开大型笛卡尔积。

### Kernel 局部的内存一致性要求

`Kernel::coherentBuffers` 显式记录 kernel 内跨 invocation 交换的 buffer。颜色窗口要求 Values 为 coherent，完整局部区域要求 Contributions 为 coherent；普通全局 kernel 使用 dispatch 间屏障。

此前只要计划包含颜色窗口，整个模型的全部 shader 都对 Values 使用 coherent。现在限定到实际需要它的 kernel，预测、归并、恢复和普通关系函数不再继承整个计划的最强访存要求。

## 对照结果

现有输入：`captures/cloth-compiler-profile.js`，SHA256：
`5D2E46386D7577C0775E3176CE5C357565B8C8307B2A16881E308B4CD6D80ED9`。

Release，RTX 3080，Vulkan synchronization validation 开启，无渲染，dt=1/30，每次 180 tick。固定比较 tick 121–180；同时保留更早的 tick 61–180 统计。最终顺序为 baseline / final / baseline / final，没有混入探索版本或细粒度 profile 运行。

| 运行 | tick 121–180 平均 ms | tick 61–180 平均 ms | compile 提交 ms |
| --- | ---: | ---: | ---: |
| baseline 1 | 3.808 | 3.707 | 12 |
| final 1 | 2.916 | 2.954 | 12 |
| baseline 2 | 4.569 | 4.716 | 12 |
| final 2 | 2.967 | 2.996 | 12 |

后段两轮均值 **4.189 → 2.941 ms**，减少 **29.8%**，约 **1.42×**。CPU compile 提交在这组样本中没有可分辨变化。

没有锁定时钟；伴随采集的 `*-clocks.csv` 显示运行中存在 P0/P3/P5 切换和显存频率变化。两轮基线的波动也明显，因此以上数字只描述本机这组提交时间，不是固定频率下各项优化的独立收益，也不承诺每次低于 3 ms。

tick 1、60、120、180 的四份状态摘要在四次运行间完全一致：包括完整数组的 sum/sq、前 12 个 float、全结构边平均/最大应变、地面/球面违反量、invalid/singular。最后平均应变为 `0.0032157403308129087`，最大应变为 `0.04948957773780835`。摘要一致不是完整状态逐位比较；本轮没有改变浮点归约顺序或数值策略。

实际 ConstraintLab 在 1280×800、validation 开启时完成 180 个渲染帧和 381 个 Simulation tick；加载原 cloth 模型、收到 ECS sample，求解 diagnostics 为 0/0，退出时 validation errors=0。这里的 Simulation tick 是应用日志计数，不等同于 cloth 求解步数。

Release 构建 `dynamics_run`、`dynamics_tests`、`Whimsical` 成功。现有 Dynamics focused checks 通过，包含十万条重复端点关系、数值 patch、快照/迁移、Hybrid、动态拓扑、多行耦合求解和候选有效期/边界回退。未新增测试。

复现命令与数据位于 `captures/compiler-cloth-sep12/`：`measure.ps1`、`summarize.ps1`、`comparison.json`、`final-before{1,2}.log`、`final-after{1,2}.log`、时钟 CSV、`final-build.log`、`final-checks.log`、`app-run.log`。该目录按仓库现有规则被 git 忽略。

## 未保留的实验与剩余成本

小组件改成每 invocation 串行执行后，目标窗口的耗时反而增加，已撤回。min/max/select 公式的符号求导扩展没有显示可分辨的独立收益，也已撤回，FormulaGlsl 与用户数学输入均保持基线。

大型连通图仍有跨颜色和 Jacobi 归并的全局依赖，cloth 每 tick 仍需 252 个计算 dispatch。本轮优化其生成代码、内存访问约束和提交内执行成本，没有实现大组件跨迭代切块，也没有用减少迭代或更换物理离散来换取时间。
