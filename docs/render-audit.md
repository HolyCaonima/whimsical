# Render audit：采样、读回与后台统计

`--audit NAME` 是保留全部样本的渲染正确性测试。前 64 帧用于预热，之后逐帧记录七种信号；RGB 均值、亮度方差和相邻帧差依赖样本顺序，不能通过丢帧提速。`--capture` 则只保存最后一张显示截图。

信号按**名字**声明，由渲染器解析成 render graph 资源，`RenderAudit` 不持有 binding 表；读回本身是一个声明了 `TransferRead` 源图和 `TransferWrite` 目标 buffer 的普通 pass。`tools/compare-audit.ps1` 逐 float 比较两份 `.f32`，用于跨二进制的逐位对照，比只比 `audit.json` 的汇总强得多——汇总打印六位有效数字，掩得住真实差异。

## 数据流与线程边界

1. 渲染线程在原有 command buffer 中将七张图复制到 `BufferMemory::Readback` 缓冲。
2. 下一帧已有的 GPU fence 完成后，将读回内容整块复制到 CPU 工作队列。随后才能复用或销毁 GPU 缓冲；没有增加逐帧 GPU 等待。
3. `RenderAuditWorker` 独占统计状态，按提交顺序计算。两个可复用 CPU 缓冲限定队列容量，工作线程处理一个时渲染线程可以填充另一个。消费者跟不上时生产者等待可用槽位，既不无限积压内存，也不丢失样本。
4. 最后一帧复用退出时已有的 fence，接收最后一个样本，再排空工作队列。统计文件由工作线程批量写入，写入错误传回调用方；正常退出前保证文件已完成。

工作线程不访问 Vulkan，也不持有 mapped GPU 指针。尺寸重建会结束旧的统计会话并创建匹配新尺寸的会话，延续此前 resize 重置统计的语义。手动提前关闭测试不会生成完整的 audit 报告。

GPU 帧数及采样起点、冻结仿真、确定性镜头/遮挡物运动、信号名称、`.f32` 的五个 float32 排列保持原语义。半精度转换直接处理 IEEE 754 位表示，不再逐通道调用 `ldexp`；非有限值在 binary16 编码上识别，继续计数并用零参与统计。RGB 与亮度的四个均值使用 x64 基础指令 SSE2 同时更新，保留原来的减、除、加顺序，不用近似倒数或 fast-math。

## 内存接口

`VulkanContext::buffer` 使用 `BufferMemory::Device / Upload / Readback` 表达访问方向。Device 使用设备本地内存；Upload 使用 HOST_VISIBLE | HOST_COHERENT；Readback 在此基础上优先选 HOST_CACHED。没有兼容 cached 类型时保留 coherent 类型，保持正确性，但读回速度会取决于设备。

HOST_COHERENT 只解决缓存可见性管理，不能代替 HOST_CACHED 的 CPU 读取性能语义，见 [Vulkan 内存属性](https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryPropertyFlagBits.html)。普通截图与 audit 共用读回策略，避免调用者把上传内存误用于逐像素 CPU 读取。

## 耗时与验证

`CPU (Render)` 仍然表示准备、录制、提交的 CPU 耗时。CPU profile 另列 `Audit / Wait for CPU Slot`、`Audit / Copy Completed Readback` 和最后一次 `Audit / Drain Worker and Save`。`render-report.json` 增加 `auditEnabled`、`auditSamples` 和 `auditCpuAverageMs`；最后一个字段是工作线程每个样本的平均统计墙钟耗时，不能与异步重叠的 GPU/Render 时间相加。

2026-09-07，RTX 3080，Release，640×400，IMMEDIATE，开启 Vulkan 及同步 validation，固定双物体阴影场景：

| 场景 | 末段 Frame ms | 说明 |
| --- | ---: | --- |
| 不开 audit | 3.80 | 360 渲染帧 |
| 修改前 audit | 1096.29 | 80 渲染帧，16 个统计样本 |
| 修改后 audit | 14.02 | 192 渲染帧，128 个统计样本 |

原先 640×400 的 14.3 MB 读回被逐通道读取，单独执行原统计就花费约 850–880 ms；选择 HOST_CACHED 后，旧转换仍需约 168 ms。消除慢读回、标量 CRT 转换及渲染线程上的同步统计后，末段达到约 71 FPS。同场景提高到 1280×800，128 渲染帧 / 64 个统计样本实测为 61.67 ms/帧。以上是此测试场景的实测吞吐，包含 audit 的额外工作，不是正常游戏渲染成本；高分辨率下完整统计仍受像素数、CPU 带宽与后台吞吐限制。

新增 `render_audit` 测试覆盖全部 65536 个 binary16 编码、异步样本顺序与数量、生产者立即复用输入、排空/退出及写文件错误。原标量算法与异步 SIMD 统计输出逐字节比较；实际 GPU 的前后 16 样本对照中七份 `.f32` 输出也逐字节一致。14 项原生测试、实际阴影与运动回归通过；独立普通 smoke 完成 resize、最小化恢复、Map 重载和截图。以上 GPU 运行均为 0 validation errors。
