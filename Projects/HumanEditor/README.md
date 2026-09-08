# HumanEditor

使用 Afterlight 引擎的项目级场景编辑器。编辑器行为由本项目的 JavaScript 实现，界面使用 RmlUi；变换 gizmo 使用临时网格实体和通用 `render.overlay` 渲染。首次使用此版本需重新构建引擎，后续编辑器脚本修改只需重启。

编辑器选择轮廓通过 `Engine.view.outlines` 显式提交，支持多选；它不设置引擎玩家或玩法状态，加载地图不会产生角色脚下标记。

## 启动

双击本目录的 **Run.cmd**，或在仓库根目录运行：

```powershell
.\build\bin\Release\Afterlight.exe --project Projects/HumanEditor --width 1600 --height 1000
```

默认挂载 Afterlight 的 Content 并打开 RainCourt，**仅加载场景数据，不执行它的游戏脚本**。Content Browser 从 Content 根目录显示文件夹，地图加载不改变正在浏览的目录。启动不会保存或修改目标项目。建议使用 1440×900 及以上窗口；左侧工具、对象树、属性和内容列表可以滚动。

## 打开另一个项目

1. 点击 **Open Project**，再点击 **Browse...**。
2. 在系统文件选择框中选择项目根目录的 **.project** 文件。
3. 确认项目名称和启动地图，点击 **Open Project**。

Content 路径和公共脚本从项目文件读取，无需手填。启动地图只加载数据，点击 Play 才运行公共脚本和地图脚本；目标项目的 `hostScripts` 不会替换编辑器。没有启动地图时打开空场景。默认项目由 `Content/Settings.asset` 中的 `project` 字段指定。

首次挂载使用独立别名，重复打开时复用已有挂载。编辑器自身 UI、操作历史和目标项目资产保留各自的来源身份。打开另一个项目前会提示处理未保存的场景修改。

## 基本操作

| 操作 | 入口 |
| --- | --- |
| 选中 / 多选 | 点击视口中的可见表面；Ctrl+点击增减选择；也可点击 Outliner |
| 父子对象树 | 点击 `+ / -` 展开折叠；输入框搜索；双击对象聚焦 |
| 移动 / 旋转 / 缩放 | W / E / R 直接切换选中对象处的箭头 / 半圆环 / 方块网格实体；红 X、绿 Y、蓝 Z，悬停及拖拽轴显示黄色。旋转时固定参考平面并显示角度，侧视轴使用屏幕切线拖动；拖拽期间固定当前模式 |
| 世界 / 本地坐标 | 视口工具栏 World / Local 控制移动和旋转；缩放始终使用物体本地坐标轴 |
| 双轴移动 / 缩放 | 拖动 XY / XZ / YZ 平面小方块，同时调整两个轴；悬停高亮方块及对应轴，第三轴不变。接近侧视的平面自动隐藏 |
| 吸附 | 视口 Snap 开关；Settings 设置位置、旋转、缩放步长，默认 0.25 米 / 10 度 / 0.1 |
| 对象右键菜单 | 视口右键单击对象或右键 Outliner 条目；包含聚焦、复制粘贴、副本、删除、启停和父级操作；右键已选对象保留多选，Esc 或点击菜单外关闭 |
| 相机 | 右键拖动环绕，右键按住配合 WASD / Q / E 移动，Shift 加速；中键平移；滚轮缩放 |
| 聚焦 / 重置相机 | F / 右键 Focus selected；Perspective 恢复地图相机 |
| 添加实体 | Place Actors 的 Basic / Shapes / Lights 分类；搜索覆盖全部类别，点击放置在相机观察目标处 |
| 重命名 / 启停 | Details 的 Rename / Enabled；多选启停使用右键 Toggle enabled |
| 修改组件 | Details 使用布尔开关、带轴标识的向量和可展开的嵌套属性；修改后显示 Apply / Revert，Enter 应用当前组件，Esc 恢复当前字段；复杂数组及完整数据使用标题栏 JSON |
| 查找 / 折叠组件 | Details 的 Filter 按组件名或属性名筛选到具体字段；组件和嵌套分组可折叠，搜索期间自动展开匹配项，无匹配时显示提示 |
| 添加 / 删除组件 | + Component 从引擎 ComponentCatalog 枚举；依赖规则由引擎验证 |
| 设置父级 | 右键 Set parent / Detach from parent，保持世界姿态，拒绝循环层级 |
| 复制 / 粘贴 / 副本 | Ctrl+C / Ctrl+V / Ctrl+D；包含子树，副本使用新持久 ID |
| 删除 | Delete / 右键 Delete；删除选中根对象的整个子树 |
| 撤销 / 重做 | Ctrl+Z / Ctrl+Y；最多保留 50 个场景操作，一次拖拽为一个操作 |
| 新建 / 保存 / 另存为 | New Level、Ctrl+S、Save、Save As；路径不带 `.asset` 后缀 |
| 场景资源 | World Settings 编辑地图相机、导航、脚本、引用和场景数据 |
| 文件夹导航 | 单击卡片选中、双击或 Enter 进入；Up / Backspace 返回上级；Back / Forward 恢复目录及当时的搜索和类别条件；顶部路径可点击 |
| 目录树 | 左侧 +/- 展开折叠，单击目录进入；Find 搜索目录名并保留祖先层级，网格进入子目录时展开对应父级 |
| 内容搜索 | Search 搜索当前目录及子目录中的文件夹和资产；类别筛选保留直接子文件夹入口；Refresh 重新扫描 |
| 内容视图 | 文件夹优先排列，Name A-Z / Z-A 排序；List / Tiles 切换列表和卡片；底部显示数量及选中项完整路径 |
| 资产创建 | + New asset 创建 Data / Material / Script；选中资产显示高亮，双击打开 |
| 资产打开 | 双击 Map 加载、StaticMesh 放置、Material 指派；文本 / JSON 资产可编辑保存 |
| 模拟 | Play 启动目标程序，Pause / Resume 暂停恢复，Stop 恢复 Play 前场景和选择 |
| 日志 / 帮助 | Output Log / Help；引擎控制台继续通过 F10 使用 |
| 面板布局 | 拖动左右分隔条、内容面板上沿、Outliner / Details 之间的分隔条；Window → Reset layout 恢复默认 |
| 视口最大化 | Maximize / Restore；保留面板尺寸，继续支持拾取与变换 |
| 内容抽屉 | 底部 Content Drawer 或 Ctrl+Space 收起 / 展开；Window 可重新打开 Content Browser / Output Log |
| 相机速度 | 视口 Settings 中 Camera speed，Shift 仍可临时加速 |

多选支持成组变换、启停、删除、层级和材质指派；Details 编辑最后选中的活动对象。灯光或没有可见网格的实体通过 Outliner 选中。变换的 rotation 字段遵循引擎的四元数 `[x,y,z,w]`，轴向旋转工具负责生成正确旋转值。

Details 的 Material / mesh 等资产字段显示资产名称，点击选择同类型资产，选择后通过 Apply / Revert 提交或恢复。资产字段整体保存 `{id,path,source}` 引用，不展开成可编辑的 ID/path 文本。Material 选择器按真实资产类型筛选；新建几何默认沿用场景已有材质，空场景使用目标 Content 的现有材质。

Details 内的启停和父级操作同样只作用于活动对象。字段草稿按对象与组件保留，切换选择或应用其他组件不会丢失；组件原值被其他操作改变时丢弃对应旧草稿。Apply 将当前组件的修改记录为一次撤销操作，Revert 放弃该组件草稿；草稿尚未写入场景，保存前需先 Apply。数字以紧凑格式显示，编辑向量某一轴不会舍入其他轴的原始值。模拟期间 Details 属性只读。

缩放修改 `render.scale`，碰撞体尺寸是独立组件数据，按引擎契约单独编辑。复制重映射 Transform 父级；自定义脚本数据内的引用仍保持原值。

布局、折叠状态和视口设置在当前编辑器会话内保留，不写入地图，也不进入场景撤销历史。资源类别沿用项目目录约定（Maps / Materials / Models / textures / scripts）；未按这些目录组织的资源归入 Other，打开时仍以资产头的真实类型执行操作。没有搜索或类别筛选时，内容网格只显示当前目录的直接子项。

Content Browser 获得焦点时，Delete / Ctrl+D 等演员快捷键不会操作场景选择。Enter 打开选中项，Backspace 返回上级，Esc 清空网格搜索和选择；文本框内保留正常输入行为。

**当前 Content 接口边界：** `Engine.content.browse` 只返回资产引用，目录树由资产路径建立，因此不显示空文件夹或仅含未索引源文件的目录。完整支持空目录需要 Content 层导出按挂载来源枚举目录条目的接口；资产类型与名称可以通过 `Engine.content.describe` 读取而无需加载载荷，Details 选择器已使用它。目录枚举不是 UI Core 缺少导出。目录列表仍按上述边界工作；“..”只是返回上级的导航入口。

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
- `Content/scripts/gizmo.js`：临时实体 gizmo、屏幕尺寸、半圆朝向、悬停、旋转与变换事务；颜色及选轴共用 GPU 网格和深度排序。
- `tools/gizmo-meshes.mjs`：生成普通 STM1 格式的箭头、缩放手柄、平面方块、半圆环与整圆环网格；运行 `node Projects/HumanEditor/tools/gizmo-meshes.mjs` 可重新生成。
- `Content/scripts/panels.js`：对象树、对象右键菜单与通用对话框。
- `Content/scripts/inspector.js`：属性布局、嵌套字段、组件草稿、筛选及编辑交互。
- `Content/scripts/browser.js`：Content 目录索引、导航历史、搜索、网格 / 列表及资产打开操作。
- `Content/scripts/main.js`：UI 绑定及宿主生命周期。
- `Content/UI/`：项目自身的 RML / RCSS。
- `Content/Settings.asset`：默认目标项目的 `.project` 路径。
- `Content/Maps/Workbench.asset`：独立小场景；清空 Settings 的 project 字段可直接编辑它。
- `tools/setup.mjs`：重新生成本项目的资产信封、设置和 Workbench。

## 验证

```powershell
node Projects/HumanEditor/tools/smoke.mjs
$editorTest = Get-Content Projects/HumanEditor/Saved/smoke-project.txt
.\build\bin\Release\Afterlight.exe --project $editorTest --width 1440 --height 900 --frames 100 --validation --capture
```

测试只在本项目 Saved 中生成隔离的宿主和目标 Content。真实 Duktape + Vulkan 验证覆盖：ID 选中、组件修改、子树复制删除、撤销重做、轴向拖拽、保存重载、Save As 目的地、Play/Pause/Stop、失败回滚，以及 UI 接管输入后的相机与快捷键。另检查视口最大化 / 恢复、抽屉与分隔条布局、目录及资源筛选、放置分类，以及属性折叠和关键控件尺寸。

2026-09-08：1440×900 隔离验证输出 `SMOKE PASS`、`layout PASS`、`browser PASS`，正常退出，Vulkan validation errors=0。浏览器验证包含嵌套目录进入、前进 / 后退、搜索状态恢复、资产打开、键盘焦点隔离，以及滚动时的网格和列表布局。另以 1600×1000 打开默认 RainCourt 并检查 GPU 截图。交互验证调用项目输入处理函数，不视为外部鼠标端到端测试。

2026-09-08 材质引用迁移：需要重新构建 Afterlight；地图使用 v9 与独立 Material 资产，旧材质编号接口已移除。详见 [框架审计](../../docs/material-asset-references.md)。
