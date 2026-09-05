# 实际验证记录

2026-09-05，Windows x64 / Visual Studio 2019 16.9 / MSVC 19.28，显卡 NVIDIA GeForce RTX 3080，驱动 591.44。

## 构建和核心功能

- CMake Release 全工程编译／链接成功，产物 `build/bin/Release/Afterlight.exe`。
- NRD 4.17.3 静态库成功生成，159 个 SPIR-V shader variants 编译成功。
- 自有 6 个 shader entry modules 由 glslang 编译，并通过 spirv-val 的 Vulkan 1.2 SPIR-V 合法性验证。
- CTest `core_gameplay` 通过：实际加载并运行 Duktape JS，不是 mock。
- 核心测试覆盖：关卡创建、灯光生成、WASD 位移与停止、A* 绕障、路径线段 clearance、拒绝阻塞目标、大步长碰撞扫掠、相机反投影、交互物体拾取、点击后寻路／转向／开门／碰撞更新、取消选择后拒绝命令、mailbox 最新帧覆盖和关闭唤醒。

## GPU 场景

运行 `Afterlight.exe --smoke --width 960 --height 600`，完成 160 个 GPU 帧，流程包含点击移动、镜头 orbit／zoom、调整窗口到 1100×700、最小化、恢复、取消指令和 GPU readback。Khronos validation layer 与显式 synchronization validation 均开启，报告 **0 个 validation errors**，两条线程正常退出。

随后运行默认 1280×800 的 90 帧截图模式。实际输出包含 raster G-buffer、硬件 ray-query DI/GI、NRD RELAX 15 条 pipelines、合成和 HUD。validation errors 为 0，readback 图像并非全黑／常量。截图已实际打开检查。

日志与报告位于：

```text
captures/smoke.log
captures/smoke-report.json
captures/final-run.log
captures/render-report.json
captures/frame.bmp
captures/frame.png
```

GPU timestamp 是一次命令缓冲范围的实际 GPU 时间；报告中的 `gpuMs` 是末段一个已完成帧的值，并非均值、百分位或大型场景性能承诺。不同轮次见到约 3–12 ms；CPU validation、首次 driver pipeline 编译、窗口 present 和资源初始化时间不在这个 GPU timestamp 范围内。

验证范围是当前基础关卡。尚未完成 ReSTIR 对离线路径追踪的误差／收敛评估、大场景压力测试、长时间稳定性测试或跨厂商验证，不能把当前运行通过理解为生产级算法或性能认证。

## 地面 Z-fighting 修复（2026-09-05）

用户确认闪烁来自 F1 基础色视图中的几何重叠。原关卡 Foundation 和 Court paving 的顶面均为 Y=0，不同材质在深度测试中争抢像素。Foundation 顶面现为 Y=-0.07，与铺砖底面接合；四条不重叠的边沿填充外圈，铺砖、外圈和导航平面仍为 Y=0。南北方向的墙段在角点相接，避免墙顶和外侧面重叠。对当前关卡 47 个轴对齐 box 的同向共面面片进行交集检查，剔除内部被遮住的接触面后，没有发现剩余的暴露重合面。该检查适用于当前 box 场景，不是通用 mesh 几何认证。

增加可重复的 GPU 连续帧读回模式，先推进 600 次固定 gameplay tick 后冻结场景，采样随机种子仍按渲染帧推进；`--audit-motion` 仅给镜头施加确定性的 yaw 轨迹。跳过前 64 个 GPU 帧后，统计原始 DI、基础色、最终显示图的 RGB 均值、亮度时域方差及相邻帧差。

同轨迹 640×400、90 帧（26 个统计帧）的基础色相邻帧亮度差 RMS：修复前 **0.029614**，最终修复后 **0.00460471**。后者包含镜头移动、几何轮廓和砖缝产生的正常变化，不能解释成纯噪声。修复前截图中明显的铺砖条纹争抢已消失。本次没有修改 DI/GI 采样或 NRD 算法；这些统计不能用作 ReSTIR 无偏性或收敛证明。

复现命令：

```powershell
.\build\bin\Release\Afterlight.exe --audit fighting-after --audit-motion --frames 90 --width 640 --height 400 --view 1 --capture
.\build\bin\Release\Afterlight.exe --audit fighting-static --frames 90 --width 640 --height 400 --view 1 --capture
```

结果存放在 `captures/<audit-name>/`。`audit.json` 为汇总；每份 `.f32` 文件按从上到下的像素顺序保存五个 little-endian float32：mean R/G/B、亮度样本方差、平均相邻帧亮度差平方。`frame.bmp` 是最后一帧。诊断读回会等待 GPU 并增加开销，不用于性能测量。

最终布局的静止基础色视图在 26 个统计帧中，亮度时域方差与相邻帧亮度差均为 0。Release 构建、现有 `core_gameplay` 和 160 帧 smoke 操作场景通过；本轮 GPU 测试启用 Vulkan 与同步校验，均为 0 errors。F1 的直接光与间接光视图现明确标注 `(RAW)`，表示显示降噪前的信号。

## 实时帧率显示（2026-09-05）

窗口标题和 HUD 使用渲染线程的同一组统计。每约 0.5 秒，用成功渲染的帧间隔数量除以实际 wall-clock 时间计算 FPS；Frame ms 是该窗口的平均帧间隔，包含等待新场景快照、GPU 和 swapchain 的时间。GPU ms 单独平均已完成帧的 Vulkan timestamp。启动及 resize 清空统计，避免把管线初始化时间混入第一份 FPS；短窗口调度抖动可能产生略高于 60 的读数。当前仍为 60 Hz 快照供给与 FIFO present。

Release 构建通过。1280×800、180 帧截图实际显示 53.3 FPS / Frame 18.76 ms / GPU 5.48 ms。随后最终版本的 160 帧 smoke 从 640×400 调整到 1100×700，完成镜头、点击、最小化恢复流程，截图显示 60.2 FPS / Frame 16.61 ms / GPU 4.54 ms；Vulkan 与同步校验均为 0 errors。两张 HUD 截图已打开检查，标签和值没有截断。数值为该测试末段半秒窗口，均开启 validation，不能当作完整性能基准。

截图和数值见 `captures/fps-display.png`、`captures/fps-display-report.json`、`captures/fps-smoke.png`、`captures/fps-smoke-report.json`。

## 独立 Physics Scene（2026-09-05）

Release 构建成功，自有代码无编译 warning。CTest 的 `core_gameplay` 与新增 `physics_scene` 均通过，最终一次耗时分别约 0.63 s 和 0.12 s。

`physics_scene` 在无 renderer 的场景中覆盖：30 m 连续扫掠检测 1 cm 薄墙、切向滑移、地面相切、旋转 OBB、胶囊精确拾取及水平胶囊重叠、碰撞层／触发体、禁用／删除／generation 句柄、线程所有权、真实地面缺口、低顶蹲行与拒绝站起、抬高障碍解除净空阻挡、隐藏或缩放显示模型后保持物理障碍、碰撞约束后的 root motion、动画附属体姿态和生命周期。

`core_gameplay` 使用实际 Duktape JS 运行移动／寻路／开门流程，新增蹲起净空验证；另经真实 JS binding 执行独立形状配置、隐藏、raycast、sweep、overlap、关节附属体创建／姿态更新／启用／销毁、revision 和 root motion 请求。

运行 `Afterlight.exe --smoke --physics-debug --width 960 --height 600`，完成 160 GPU 帧以及点击移动、镜头变换、resize 到 1100×700、最小化恢复、取消指令。Vulkan 和 synchronization validation 报告 0 errors，正常关闭。物理线框截图已打开检查，见 `captures/physics-scene.png` 与 `captures/physics-scene-report.json`，日志在 `build/physics-smoke.log`。本次没有改动光照 shader 或 NRD 算法。

查询后端当前采用缓存 AABB 粗筛和 box/capsule 窄相，未做大型场景 BVH、动力学堆叠／约束、三角网格碰撞、多层导航或骨骼动画资源载入的验证。
