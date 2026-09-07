# Maple Circuit / 街区大奖赛

使用 Afterlight 引擎现有功能制作的独立赛车项目，参考用户提供的 Maple Circuit 录屏。没有修改 `engine/`、引擎构建配置或 Afterlight 示例项目。赛车、环境和天空都是项目资产；车辆、AI、比赛规则、特效与 HUD 由项目 JavaScript 驱动。

## 运行

双击仓库根目录 `RunMapleCircuit.cmd`。也可以从仓库根目录运行：

```powershell
.\build\bin\Release\Afterlight.exe --project Projects/MapleCircuit --width 1280 --height 800
```

点击 **START RACE** 或按 W / Enter，倒计时后发车。初始待发车状态不推进时间，首次着色器编译不会让对手提前开走。

| 操作 | 功能 |
| --- | --- |
| W / S，↑ / ↓ | 加速；刹车，低速时倒车 |
| A / D，← / → | 转向 |
| Space | 漂移，降低侧向抓地并生成不透明烟团与轮胎印 |
| R | 回到当前所在赛道段，保留比赛进度 |
| Esc / P | 暂停或继续 |
| V / WATCH AI | AI 接管玩家车；再次操作恢复手动驾驶 |
| RESTART RACE | 重置四辆车、圈数、计时及烟团／轮胎印 |
| Enter（完赛后） | 再赛一场 |
| F10 / ~ | 原有引擎控制台 |

四辆白色敞篷跑车，三圈比赛。HUD 显示排名、当前圈、圈时、最佳圈、实际速度、抓地／漂移状态和各车在赛道小地图上的位置。结算按完赛时间排序；先完赛的车辆会继续低速巡航，避免挡住终点。

## 项目结构与引擎边界

```text
.project                        独立项目入口与公共脚本顺序
Content/Maps/MapleCircuit.asset  场景、资产引用、碰撞体及赛道数据
Content/models/                 15 个共享 STM1 网格
Content/shaders/                 普通表面与天空的 opaque Shader
Content/scripts/track.js         等弧长赛道采样、投影与检查点判定
Content/scripts/vehicle.js       街机驾驶、AI、车轮变换及特效对象池
Content/scripts/race.js          发车、16 个顺序检查点、三圈比赛与镜头
Content/scripts/hud.js           RmlUi DOM 与事件
Content/scripts/main.js          引擎生命周期与输入入口
Content/UI/                     RML、RCSS 与小地图 PNG
Content/tests/controls.js        仅诊断 Map 加载的原生验证脚本
SourceArt/build.py              原创程序化网格、Map 与小地图生成器
Tests/gameplay.test.cjs          项目规则与输入测试
```

- 约 **458.29 米**赛道，423 个场景对象，15 个唯一网格、9,288 个唯一三角形。包含住宅、树、路灯、路肩、护栏、轮胎堆、带独立旋转车轮的敞篷跑车。Map v5 将车轮挂为车身的子实体；项目只提交局部轮胎旋转，ECS 负责层级世界变换。
- 车辆加速、刹车、侧向抓地与 AI 前视驾驶属于项目规则。玩家和 AI 都调用 `Engine.moveBody`，实际位移、接受的旋转和撞击法线来自引擎 PhysicsScene。
- 每圈要求按序通过 16 个检查点；逆行、漏过检查点、离开道路跨过检查点均不会获得该次进度。每辆车的正常三圈需要 49 次检查点通过（包含首次发车越线）。
- JS 使用 ES5；Script 资产为 external `.asset`，引用相邻 `.js` 文件，便于直接阅读与维护。生成的资产使用确定性持久 ID。
- 48 个不透明烟团、128 段不透明轮胎印循环复用。烟团只使用缩放与生命周期消散，符合用户的 opaque 要求。
- 天空使用跟随相机的项目天空球和程序化发光表面；没有修改引擎固定天空函数。基础 PBR 与实际光追阴影仍由引擎计算。
- 这是平面街机赛车，没有加入真实悬挂／动力学车辆或声音。当前引擎没有音频播放接口。

## 重建与验证

生成器只写入本项目。Python 需要 Pillow；已生成资产可以直接运行，不需要先运行 Python 或 Blender。

```powershell
python Projects/MapleCircuit/SourceArt/build.py
node Projects/MapleCircuit/Tests/gameplay.test.cjs

# 真正使用引擎物理和 Vulkan：四辆 AI 车完整三圈
.\build\bin\Release\Afterlight.exe --project Projects/MapleCircuit --map /Game/Maps/Verification --frames 1650 --capture --validation --width 960 --height 600 --cvar "t.TimeScale=4"

# 加速、漂移烟团、暂停、回正与重赛
.\build\bin\Release\Afterlight.exe --project Projects/MapleCircuit --map /Game/Maps/ControlsVerification --frames 220 --capture --validation --width 960 --height 600

# 自动驶入漂移并冻结画面，供截图检查；不是正常游戏入口
.\build\bin\Release\Afterlight.exe --project Projects/MapleCircuit --map /Game/Maps/DriftPreview --frames 240 --capture --width 1280 --height 800
```

2026-09-06 验证结果：

- 10 项 Node 项目测试通过，覆盖待发车、倒计时、输入捕获、加减速、左右转向、防刷圈、精确端点、三圈结算、对象池寿命与 HUD 按钮状态。Node 中的碰撞接口是替身，不将它作为原生物理验证。
- 原生四车比赛通过：完赛时间 64.72、68.05、70.09、71.49 秒，每车 49 次检查点，行驶 1,376.59–1,431.71 米；日志包含 `MAPLE_NATIVE_RACE_PASS`。这组比赛验证的 GPU validation errors = 0。
- 原生操控场景通过，日志包含 `MAPLE_NATIVE_CONTROLS_PASS`；生成 54 个烟团，暂停位置不变，回正与重赛状态恢复，GPU validation errors = 0。
- RTX 3080、1280×800、Release 默认关闭 validation 的冻结漂移截图，实测约 60 FPS、GPU 约 6 ms。此项是特定冻结场景测量，不代表整场比赛的稳定帧率。

日志、GPU 报告与原生截图保存在仓库 `captures/maple-circuit/`。正常游戏的 `.project` 不加载测试脚本；测试 Map 显式引用诊断脚本或启用诊断数据。
