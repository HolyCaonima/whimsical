# HumanEditor

使用现有 Afterlight 引擎的项目级场景编辑器。所有编辑器行为由本项目的 JavaScript 实现，界面使用引擎已有的 RmlUi。无需重新编译，不修改 `engine/`、构建配置或其他项目源码。

## 启动

双击本目录的 **Run.cmd**，或在仓库根目录运行：

```powershell
.\build\bin\Release\Afterlight.exe --project Projects/HumanEditor --width 1600 --height 1000
```

默认挂载 Afterlight 的 Content 并打开 RainCourt，**仅加载场景数据，不执行它的游戏脚本**。启动不会保存或修改目标项目。建议使用 1440×900 及以上窗口；左侧工具、对象树、属性和内容列表可以滚动。

## 打开另一个项目

1. 点击 **Open Content**。
2. 选择 Afterlight / MapleCircuit 预设，或填写任意项目的 Content 目录。自定义目录建议使用绝对路径。
3. 如果需要 Play，填写该项目 `.project` 的 `scripts` 数组；预设会自动填入。地图自己的脚本自动加载，无需重复填写。
4. 点击 **Mount Content**，在 Content Browser 中选择 Maps 文件夹，双击地图卡片。

每次挂载使用独立别名。编辑器自身 UI、操作历史和目标项目资产保留各自的来源身份。挂载另一个目录只是浏览内容；打开地图时才替换当前场景，并提示处理未保存的场景修改。

## 基本操作

| 操作 | 入口 |
| --- | --- |
| 选中 / 多选 | 点击视口中的可见表面；Ctrl+点击增减选择；也可点击 Outliner |
| 父子对象树 | 点击 `+ / -` 展开折叠；输入框搜索；双击对象聚焦 |
| 移动 / 旋转 / 缩放 | W / E / R；拖拽选中对象处的 X / Y / Z 轴手柄 |
| 世界 / 本地坐标 | 左侧 World coordinates / Local coordinates |
| 吸附 | 位置 0.25 米、旋转 10 度、渲染尺寸增量 0.1；左侧按钮可关闭 |
| 相机 | 右键拖动环绕，右键按住配合 WASD / Q / E 移动，Shift 加速；中键平移；滚轮缩放 |
| 聚焦 / 重置相机 | F / Focus selected；Perspective 恢复地图相机 |
| 添加实体 | 左侧 Entity、Box、Capsule 或五类 Light；放置在相机观察目标处 |
| 重命名 / 启停 | Details 的 Rename / Enabled；多选启停使用左侧按钮 |
| 修改组件 | Details 输入字段后点 Apply fields；复杂引用和嵌套数据使用 Complete JSON |
| 添加 / 删除组件 | + Component 从引擎 ComponentCatalog 枚举；依赖规则由引擎验证 |
| 设置父级 | Set parent / Detach from parent，保持世界姿态，拒绝循环层级 |
| 复制 / 粘贴 / 副本 | Ctrl+C / Ctrl+V / Ctrl+D；包含子树，副本使用新持久 ID |
| 删除 | Delete / 左侧 Delete；删除选中根对象的整个子树 |
| 撤销 / 重做 | Ctrl+Z / Ctrl+Y；最多保留 50 个场景操作，一次拖拽为一个操作 |
| 新建 / 保存 / 另存为 | New Level、Ctrl+S、Save、Save As；路径不带 `.asset` 后缀 |
| 场景资源 | World Settings 编辑材质表、地图相机、导航、脚本、引用和场景数据 |
| 内容操作 | 目录浏览、搜索、Refresh；New asset 创建 Data / Script |
| 资产打开 | 双击 Map 加载、StaticMesh 放置、Material 指派；文本 / JSON 资产可编辑保存 |
| 模拟 | Play 启动目标程序，Pause / Resume 暂停恢复，Stop 恢复 Play 前场景和选择 |
| 日志 / 帮助 | Output Log / Help；引擎控制台继续通过 F10 使用 |

多选支持成组变换、启停、删除、层级和材质指派；Details 编辑最后选中的活动对象。灯光或没有可见网格的实体通过 Outliner 选中。变换的 rotation 字段遵循引擎的四元数 `[x,y,z,w]`，轴向旋转工具负责生成正确旋转值。

缩放修改 `render.scale`，碰撞体尺寸是独立组件数据，按引擎契约单独编辑。复制重映射 Transform 父级；自定义脚本数据内的引用仍保持原值。

## 拾取、历史与保存

- 编辑器创建 `persistent:false` 的 `drawEntityID` 输出实体，写入本项目的 `Picking` RenderTarget（R32Uint，跟随视口尺寸）。点击经 `Engine.view.pixel` 映射到纹理位置，然后异步读取一个整数像素。**没有使用物理射线或 `input.picked` 作为对象选取来源。**
- RT 与颜色视图使用同一相机和投影。请求匹配分配版本；相机、窗口、场景或选择变化时取消过期请求。场景恢复后重新创建工具实体与 RT 句柄。
- 场景历史使用宿主 capture / restore 契约，并通过持久 Object ID 恢复选择。Save As 后撤销不会悄悄改变保存目的地。复合命令中途失败时恢复操作前场景。
- 编辑器相机和临时 ID 输出不写入地图。场景保存沿用引擎的原子写入与 Content 依赖闭合检查。另存为通常应选当前地图所属 Content；复制到一个不包含依赖的 Content 会被拒绝。
- **资产文本 / JSON 文件保存独立于场景撤销历史。** Refresh 重新扫描资产；已有实例需要重新打开地图才能装入磁盘上的新资源版本。
- 切换地图和新建场景时提供未保存提示。窗口关闭由原生宿主处理，目前不能通过项目 JS 拦截关闭，因此关闭前请先保存。

这是单视口编辑器，提供现有 ECS 与资产接口上的基本创作操作。未实现 UE 的蓝图图形编辑、任意格式模型导入器、任意窗口停靠或多视口；二进制资产继续使用已有资产制作管线。Play 的游戏 UI 使用目标项目原有布局。

## 项目结构

- `.project`：常驻 `hostScripts`，`startupMode: load`。
- `Content/scripts/editor.js`：文档状态、命令、历史、层级、Content 与模拟生命周期。
- `Content/scripts/viewport.js`：UI 输入、相机、ID 回读、投影与轴向变换。
- `Content/scripts/panels.js`：对象树、组件面板、资产浏览与对话框。
- `Content/scripts/main.js`：UI 绑定及宿主生命周期。
- `Content/UI/`：项目自身的 RML / RCSS。
- `Content/Settings.asset`：默认目标与项目预设。
- `Content/Maps/Workbench.asset`：独立小场景；清空 Settings 的 content 字段可直接编辑它。
- `tools/setup.mjs`：重新生成本项目的资产信封、设置和 Workbench。

## 验证

```powershell
node Projects/HumanEditor/tools/smoke.mjs
$editorTest = Get-Content Projects/HumanEditor/Saved/smoke-project.txt
.\build\bin\Release\Afterlight.exe --project $editorTest --width 1440 --height 900 --frames 65 --validation --capture
```

测试只在本项目 Saved 中生成隔离的宿主和目标 Content。真实 Duktape + Vulkan 验证覆盖：ID 选中、组件修改、子树复制删除、撤销重做、轴向拖拽、保存重载、Save As 目的地、Play/Pause/Stop、失败回滚，以及 UI 接管输入后的相机与快捷键。另检查属性面板宽度和资产卡片排列。

2026-09-08：上述验证通过，输出 `SMOKE PASS` 与 `layout PASS`，正常退出，Vulkan validation errors=0。同时检查了引擎 GPU 截图。外部鼠标自动复测收到物理 Esc 停止信号后已停止；不将其计为通过。
