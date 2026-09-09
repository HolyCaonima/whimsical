# Afterlight — 人与狗的神经网络驱动动画

Afterlight 展示 Whimsical 的神经网络驱动动画：人形角色 **Kiln** 使用双足模型，狗 **Ash** 使用四足模型，两者均来自 AI4Animation 的预训练网络，通过 ONNX Runtime 在运行时推理。

## 体验神经动画

- **双足角色**：操纵 Kiln 移动、转向与停步，比较 11 种移动风格下的身体动作。
- **四足角色**：观察 Ash 寻路跟随、转向追赶，以及接近 Kiln 后减速停下时的步态。
- **动画与实际移动结合**：玩法提交速度、朝向和轨迹意图，动画求解器生成骨骼姿态及根位移 / 旋转；碰撞处理后的实际根姿态反馈给后续预测。

Rain Court 和 Honeybud Court 提供同一套角色与神经动画的交互场景。当前双足模型未接入专门的蹲姿动画；Ctrl 会调整移动速度与碰撞净空，角色仍保持直立姿态。

实现与模型来源见 [动画文档](../../docs/animation.md)。AI4Animation 的代码与模型使用 CC BY-NC 4.0，使用与分发时请查看该文档的许可说明。

## 启动

在仓库根目录运行 `Run.cmd` 打开 Rain Court，运行 `RunHoneybud.cmd` 打开 Honeybud Court。也可直接选择地图：

```powershell
.\build\bin\Release\Whimsical.exe --project Projects/Afterlight --map /Game/Maps/HoneybudCourt
```

- **Rain Court**：简洁庭院，便于观察人和狗的动作、风格切换，以及移动与交互的衔接。
- **Honeybud Court**：在暖色花园与陶瓷喷泉场景中展示同一套双足、四足神经动画。
- **Physical Lights**：`--map /Game/Maps/PhysicalLights` 打开五类光源示例；Space 切换发光几何动画。

## 操作

| 操作 | 行为 |
| --- | --- |
| 左键点击角色 | 选择角色；初始默认选中 Kiln |
| 左键或右键点击地面 | 寻路移动；新命令替换旧命令 |
| 点击交互物体 | 寻路到接近点、转身面对目标、执行交互 |
| WASD | 相机方向相对的直接移动，取消路径命令 |
| Shift / Ctrl | 跑步 / 蹲行 |
| E | 接近并使用附近交互物体 |
| Esc | 停止移动／取消交互命令 |
| Tab | 切换选中状态 |
| 中键拖动 | 旋转镜头、调整俯仰 |
| 滚轮 | 平滑缩放 |
| Q / R | 旋转镜头 |
| 方向键；Alt + 屏幕边缘 | 平移镜头，脱离自动跟随 |
| F 或 Space | 恢复平滑跟随 |
| F2 | 显示／隐藏 Physics Scene 碰撞体线框；也可用 `--physics-debug` 启动 |
| ~ / F10 | 打开／关闭控制台；Esc 关闭，上下键选择候选（空输入或无候选时浏览历史），Enter 执行；也支持 Tab 补全 |
| Alt+F4 或窗口关闭 | 正常结束两条线程并释放资源 |

Rain Court 左侧的 **Power console** 会切换青色灯光和建筑门的开合／碰撞；前方的 **Supply cache** 可激活。悬停高亮、角色轮廓、目标环和小地图路径提供指令反馈。

选中 Kiln 后，左侧 **ANIMATION** 面板的左右箭头可切换 11 种移动风格。先点击地面让角色行走，再切换比较；停下会保留选择。风格由动画资产声明，作为实例属性保存，与每帧速度／动作输入分离。

## 进一步阅读

[项目截图与仓库入口](../../README.md) · [Honeybud 美术资产管线](../../docs/honeybud-art-pipeline.md) · [引擎开发指南](../../engine.md)
