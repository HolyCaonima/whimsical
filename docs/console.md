# CVar 与游戏内控制台

按 **`~`（Esc 下方的键）或 F10** 打开/关闭控制台，Esc 关闭。控制台独立于游戏 HUD；`r.Hud 0` 和 `--no-hud` 不会让控制台失去入口。打开时键盘、鼠标点击、滚轮和 F2 归控制台所有，关闭的那一帧也不会把 Esc 或打字时仍按住的键传给角色。

## 使用

```text
r.Exposure                  # 查询值、类型、来源、默认值和范围
r.Exposure 1.4              # 设置；也接受 r.Exposure=1.4
help r.Present              # 详细说明
find render                 # 搜索名称和帮助
find r. view                # 多关键词匹配，例如 r.DebugView
reset r.Exposure            # 恢复注册时的默认值
stat                        # 实时 FPS、整帧间隔、CPU (Render)、GPU 耗时和对象数量
profileCPU                  # 抓取下一次 Game 更新及对应渲染帧的 CPU 分阶段耗时
profileCPU last             # 查看最近一次 CPU 报告
profileGPU                  # 抓取下一帧的 GPU Scope 耗时，结果异步输出
profileGPU last             # 查看最近一次 GPU 报告
map.reload                  # 重载当前 Map，保留 CVar
clear                       # 清空输出
quit                        # 关闭引擎
```

以上行尾注释仅用于说明；在控制台输入时省略注释。一行一条命令。名称不区分大小写；带空格或 `=` 的字符串用双引号包围，支持 `\"`、`\\`、`\n`、`\r`、`\t` 转义。错误值会被拒绝，保留原值，不做静默截断或钳制。

- Tab：按已注册名称模糊补全，多次按键循环原查询的候选。
- 上/下：有预测候选时循环选择并填入名称，竖排列表高亮当前项、自动滚动；下键从第一项开始，上键从最后一项开始。选择后可直接输入参数，Enter 执行，无需 Tab。
- 空输入或没有候选时，上/下浏览命令历史；浏览期间保持历史导航，返回末尾恢复未提交的草稿。编辑输入后恢复候选选择。
- 左/右、Home/End、Backspace/Delete：编辑；Ctrl+U 清空输入。
- Ctrl+V：粘贴 Unicode 文本；换行和制表符转换为空格，粘贴不会自动执行命令。
- PageUp/PageDown 或滚轮：查看输出历史。保留 256 行日志、64 条命令历史，输入上限 1024 个 Unicode 字符。

控制台显示命令结果、配置加载诊断以及 `Engine.log` 的脚本消息；不是操作系统 shell，也不转发任意 shell 命令。原有 Vulkan/驱动启动诊断继续写标准输出。

`profileGPU` 输出可嵌套的 GPU 阶段耗时和整帧占比，同时保存 `captures/gpu-profile.txt` 与 `captures/gpu-profile.json`。也可通过 `--profile-gpu` 或 `--exec "profileGPU"` 从启动命令行抓取。范围与扩展方式见 [GPU 分阶段计时](gpu-profiling.md)。

`profileCPU` 分别输出 Game / Render 线程的 inclusive 与 self 耗时，包含显式标注的等待阶段，保存 `captures/cpu-profile.txt` 与 `captures/cpu-profile.json`。也支持 `--profile-cpu` 和 `--exec "profileCPU"`。详细范围见 [CPU 分阶段计时](cpu-profiling.md)。

HUD、标题栏与 `stat` 的 `CPU (Render)` 是渲染线程从场景准备到队列提交的实际墙钟耗时，每半秒取平均；包含 CPU 蒙皮、上传、HUD、命令录制及提交，不包含帧 fence、交换链 acquire、Present、主线程逻辑或帧率限制等待。场景资源更换时，该区间内同步资源上传的等待仍会被计入。它不是 `Frame - GPU`，也不是 CPU 占用率。`Frame` 仍表示包含等待的完整呈现间隔。

名称候选和 `find` 支持按顺序匹配多个片段，忽略大小写及多余空白。例如输入 `r.  anim`，可以匹配已注册的 `r.xxx.xx.animaxxx`；输入 `r. view` 后按 Tab 可补全为 `r.DebugView`，再输入 ` 2` 并回车设置值。每个片段内部连续，片段之间可以跨过任意字符及点号层级；也可以从名称中间开始搜索。排序优先完整名称、连续前缀、连续子串，再到跨间隔匹配；同类优先间隔更小、位置更靠前、名称更短的结果，最后按名称排序。`find r. anim` 和 `find "r. anim"` 等价，名称命中优先于帮助文本命中。

已输入完整名称及空格后进入参数编辑，Tab 保留参数；模糊查询需要先用上/下或 Tab 选择完整名称再执行，回车不会猜测并执行相似命令。

## 已接入的变量

| 名称 | 类型 / 默认 | 行为 |
|---|---|---|
| `r.Exposure` | float / 1.15 | 0.05–8，下一帧调整色调映射曝光 |
| `r.Hud` | bool / true | 显示项目游戏 HUD，不影响开发工具 |
| `r.Stats` | bool / false | 单独显示引擎性能统计面板 |
| `r.DebugView` | int / 0 | 0–7；通过控制台设置，`help r.DebugView` 查看映射 |
| `r.FullUpload` | bool / false | 下一帧强制重写渲染场景，用于性能对比 |
| `p.DebugDraw` | bool / false | 与 F2 共用状态；独立于项目 HUD 和性能面板绘制物理线框 |
| `t.TimeScale` | float / 1 | 0–4；0 暂停游戏逻辑，控制台、窗口、渲染继续运行 |
| `r.Present` | string / fifo | fifo / mailbox / immediate，需要重启 |
| `r.Validation` | bool / Release false | Vulkan 验证，需要重启；Debug 默认 true |
| `sys.Engine` | string / Afterlight | 只读示例 |

`r.Present` 与 `r.Validation` 修改后显示请求值和当前 active 值，当前渲染器保持原设置。`stat` 和 HUD 显示实际测量数据，不把请求的模式假装成已生效模式。需要重启的值如要跨进程保留，须显式保存；`r.Validation` 未标记 archive，可用命令行或项目配置启用。

## 配置和启动参数

覆盖顺序：注册默认值 < 项目 `Config/ConsoleVariables.cfg` < 本机 `Saved/ConsoleVariables.cfg` < 命令行 < 交互控制台。两个配置文件均属于 Config 来源，按加载顺序覆盖；其他低优先级赋值会给出忽略原因。

配置文件使用 `名称=值`，空行及以 `#` 或 `;` 开头的整行注释忽略；配置只设置变量，不执行命令。错误逐行报告，后续有效行仍可加载。

```text
r.Exposure=1.25
r.Hud=true
r.Present="fifo"
```

`cvar.save` 仅保存 Archive 标记的变量到当前项目 `Saved/ConsoleVariables.cfg`，不在退出时自动保存，也不修改版本管理中的项目配置。`cvar.load` 重新读取两个配置文件，遵守已有命令行/控制台优先级。`Saved/` 已加入忽略列表。模型、材质和 Map 仍走 AssetManager；CVar 是独立于内容资产的进程运行配置。

```powershell
.\build\bin\Release\Afterlight.exe --console --cvar "r.Exposure=1.3"
.\RunHoneybud.cmd --console --exec "r.DebugView 2" --exec "help r.Exposure"
```

`--exec`/`--cvar` 可重复使用，按命令行顺序执行。原有 `--view`、`--no-hud`、`--physics-debug`、`--full-upload`、`--present`、`--validation`/`--no-validation` 转换为对应 CVar，来源同为 CommandLine。启动命令错误会明确报错退出。`--smoke` 和 `--console-smoke` 强制开启验证；`--audit` 强制关闭游戏 HUD。

## 扩展与线程归属

`ConsoleRegistry` 是引擎拥有的实例，不使用静态初始化注册或全局可变单例。变量与命令共用名称空间。注册、设置、命令回调在主线程执行；其他线程需要向主线程提交工作，不能直接写注册表。新增引擎变量在 `core/EngineSettings.cpp` 注册，域代码可使用同一个 registry 注册自己的变量和命令。

```cpp
auto& count = variables.variable("game.Count", 4, "Example bounded count",
                                  CVarArchive, CVarRange{0, 16});
count.changed = [](const ConsoleValue& next) {
    // Runs on the owning main thread for an effective value change.
    applyCount(std::get<int>(next));
};
int current = variables.get<int>("game.Count");

variables.command("game.inspect", "Inspect game state", [](const auto& args) {
    if (!args.empty()) throw std::runtime_error("Usage: game.inspect");
    return std::string("Inspection result");
});
```

`ConsoleValue` 支持 bool、int、double、string；可指定数值范围、字符串候选、ReadOnly、Archive、Restart 标记。Restart 变量在 `finishStartup()` 后只更新请求值，`get<T>()` 继续返回 active 值。需要保留调用来源的命令可使用 `(args, CVarSource)` 形式的回调；内置 reset 也遵守调用来源优先级。

渲染线程不查询 registry：`EngineSettings::decorate` 将曝光、HUD 等当前值装入已有不可变 `Frame`，控制台也只发布展示快照。渲染器通过统一 Globals 把曝光送入 shader。`ui::Console` 管输入和日志，`EngineUi` 将展示数据写入 RML 控制台文档；uiCore 录制几何快照，由 Vulkan UiRenderer 绘制。Console 保留最高输入优先级，RmlUi 文档使用同一套字体、裁剪与合成后端，详见 [UI Core](ui-core.md)。

## 验证

`console_system` 测试覆盖类型/范围/只读、来源优先级、延迟生效、回调、配置转义往返、多关键词搜索排序、上下键与 Tab 候选循环、高亮随选项滚动、参数保留、草稿历史、Unicode 编辑和输入拦截。原生 smoke 还检查上下方向键选择及高亮快照。运行完整测试：

```powershell
powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Test
.\build\bin\Release\Afterlight.exe --console-smoke --width 1280 --height 800
```

原生 smoke 通过 Win32 消息驱动控制台，检查输入、实际改参、错误拒绝、F1 不再改变视图、关闭 HUD 后重新打开、resize 和 Map 重载后参数保留，有限帧数后自动关闭并保存截图。`render-report.json` 包含实际呈现的 exposure、debugView、hudEnabled、consoleOpen 和验证错误计数。
