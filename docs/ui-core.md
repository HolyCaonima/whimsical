# uiCore / RmlUi / JavaScript UI

`engine/uiCore` 是独立 CMake 库 `whimsical_uicore`，使用固定版本 RmlUi 6.1 和 FreeType 2.13.3。主线程拥有 Context、DOM、布局与事件；`render/UiRenderer` 只消费不可变 `UiFrame`，在渲染线程以 Vulkan 绘制几何、字体和图片。GDI HUD 与手工动画面板命中区域已移除。

## 模块与生命周期

```text
Win32 ordered input → Console input ownership → uiCore/RmlUi events → JS gameplay
                                                  ↓
EngineUi (native tools) ──────────────────────── RmlUi DOM ← Engine.ui (project JS)
                                                  ↓ Update / Render
                                  immutable UiFrame (geometry + texture refs)
                                                  ↓ FrameMailbox
                                      Vulkan UiRenderer → composition
```

- `engine/uiCore`：通用文档加载、默认样式、字体、输入路由、几何录制，不调用 World 或 ScriptRuntime。
- `engine/scripting/UiBindings`：Duktape realm 的 DOM 句柄、事件回调及项目文档所有权；独立于玩法绑定实现。
- `engine/ui/EngineUi`：仅拥有控制台和可选性能统计，不依赖 World，不查询角色或地图，也不处理玩法点击。
- `Projects/Afterlight/Content/UI`、`scripts/gameplay/hud.asset` 和 `scripts/ui/`：项目拥有品牌、地点、角色卡片、操作提示、小地图和动画风格选择器的布局、数据筛选与事件。新项目不导入这些脚本，就不会出现 Afterlight 项目的面板。
- `engine/render/UiRenderer`：Vulkan 资源缓存与 UI pass；不包含 RmlUi 头文件，不访问 DOM、World 或 JS。

RmlUi 全局初始化由共享服务管理，各 uiCore 有独立 Context 和录制器。`ScriptRuntime` 可选接收 uiCore，因此无窗口的玩法测试仍可运行。窗口应用始终安装 `Engine.ui`。所有文档 API 与 JS 回调在同一主线程执行。

场景替换关闭场景 realm 的事件订阅和文档，再销毁该 JS heap；常驻应用 realm 的文档及引擎工具持续存在。每个 UI 回调进入所属 realm 的 Content scope，两个来源中的 `/Game` 不会混淆。详见 [运行宿主与视图](runtime-host.md)。元素句柄使用 RmlUi ObserverPtr，节点被移除后再调用会抛 JS Error。DOM 事件中可取消自己的订阅或移除自身元素；回调异常写入现有日志通道。`Engine.loadScene` 仍延迟到事件调用返回后执行，即使游戏时间暂停，UI 输入阶段也处理场景请求。

UiFrame 每次包含完整绘制列表，并持有几何和纹理的共享引用。mailbox 丢帧不影响资源创建/销毁顺序。Vulkan 后端在帧 fence 之后回收没有快照或 Context 引用的资源，重复呈现不会重传已有几何。字体/图片上传发生在首次使用时。

### 文本值、几何身份与 GPU 存储

`setText` 是纯文本值更新：已有文本节点保持身份，相同值不触发布局更新；从富文本内容切换到纯文本时才替换子节点。文本不经过 RML 解析。`setInnerRML` 明确表示结构替换，仍会重建子树。这个契约由 uiCore 提供，脚本绑定只转发，不为具体项目缓存文案。

位置、变换、裁剪属于 `UiFrame::Draw`；顶点和索引属于不可变 `Geometry`。Vulkan 后端用现有 `RangeAllocator` 为几何分配共享顶点/索引缓冲区中的区间，几何失效只回收区间，不销毁 GPU buffer。容量不足时按几何增长扩容，并从仍有效的 CPU 几何恢复内容，不读取上传内存。常规帧只上传新增几何。

`prepare` 必须在上一帧 fence 完成后运行：该同步边界保证区间回收、覆盖和缓冲区扩容不会碰到 GPU 读者。CPU 快照引用保证跨线程和丢帧安全，GPU 容量由后端持有，两种生命周期互不绑定。纹理仍按独立资源缓存。

## 项目资源

`UiCore(assets.mounts())` 共享资产层的 Content 映射。JS 入口接受任意已挂载虚拟根，带真实扩展名；`/Game/` 默认为当前脚本或 Map 所属 Content。RML 内的 RCSS、图片和模板使用相对引用或来源内的 `/Game/...`，不能引用其他 Content；`createDocument` 的第二个参数确定相对路径基址。UI 文件是 Content 下的源资源，无需将 RML/RCSS 包进 `.asset`。JS 继续使用现有 Script 资产加载规则。

宿主显式挂载 `/Engine` 后，每个文档在自己的 RCSS 前合并 `/Engine/UI/base.rcss`，提供 div、p、form、table 等基础 display 默认值。引擎随内容携带 LatoLatin 常规/粗体及 OFL 许可证；宿主挂载 `/SystemFonts` 时加载 Microsoft YaHei 作为中文回退字体。项目可使用 `loadFont` 加载自己的字体。

RmlUi 的 FileInterface、JoinPath 和 WIC 纹理解码都经过同一个 Content 文件映射。内部 URI 包含挂载身份；卸载后不能再读取旧 URI，重挂同名目录不会命中旧样式或纹理缓存。已经生成的 DOM 和 UiFrame 保持有效。不同 UiCore 可以共享一张挂载表，也可以各自使用独立挂载表。

```javascript
// Put this in a project Script asset included by .project.
var inventory = Engine.ui.loadDocument('/Game/UI/inventory.rml').show();
var count = inventory.getElementById('count');
var amount = 0;
var subscription = inventory.getElementById('add').on('click', function(event) {
    amount += 1;
    count.setText('Items: ' + amount);
    Engine.log('Picked up ' + amount + ' items');
    event.stopPropagation();
});

// Also supports documents created entirely in JS.
var popup = Engine.ui.createDocument(
    '<rml><head><style>body { width: 260px; height: 100px; background-color: #172c35; }</style></head>' +
    '<body><div id="message"/></body></rml>', '/Game/UI/popup.rml');
popup.getElementById('message').setText('Hello from gameplay');
popup.show(true); // Modal; blocks gameplay input outside the popup too.
```

## JS 接口

`Engine.ui.loadDocument(path)` 和 `createDocument(rml, source?)` 返回初始隐藏的 document。`loadFont(path, fallback?)` 返回加载结果。`Engine.ui.off(token)` 取消事件订阅。

| 对象方法 | 行为 |
| --- | --- |
| `getElementById(id)`, `querySelector(selector)` | 返回元素或 null |
| `querySelectorAll(selector)` | 返回元素数组 |
| `setText(text)` | 将纯文本转义后设置，避免文字被当成 RML |
| `setInnerRML(rml)`, `getInnerRML()` | 更新/读取子树 |
| `setProperty(name, value)`, `getProperty(name)` | RCSS 属性；数值带单位，例如 `120px` |
| `setAttribute(name, value)`, `getAttribute(name)`, `removeAttribute(name)` | DOM 属性；传字符串值 |
| `setClass(name, enabled)`, `hasClass(name)` | 样式类 |
| `setValue(value)`, `getValue()` | input、select、textarea 等表单控件值 |
| `appendChild(tag)` | 创建并附加新元素，返回该元素 |
| `remove()` | 移除元素；文档等效于 close |
| `show(modal?)`, `hide()`, `close()` | 文档生命周期；默认非模态 |
| `focus()`, `blur()` | 焦点 |
| `select()` | 全选文本输入框或 textarea 的内容；input 支持 text / password 类型 |
| `getBounds()` | 当前布局的边框矩形 `{x,y,width,height}` |
| `on(type, callback, capture?)`, `off(token)` | RmlUi 事件订阅与取消；支持冒泡和捕获 |

事件提供 `type`、`target`、`currentTarget`、`parameters` 和 `stopPropagation()`；回调返回 false 也会停止传播。参数中的布尔、整数、浮点值保留 JS 类型，其余转为字符串。控件逻辑从项目 Script 注册；这里没有浏览器运行时，也不执行 RML 内联 `<script>` 或 `onclick` JavaScript。

## 输入与显示

平台保留按顺序的鼠标、按键、重复按键与 Unicode 字符事件；每个 simulation tick 只消费一次。RmlUi 布局承担命中检测。指针在 UI 上、拖动从 UI 开始、表单持有键盘焦点或存在模态文档时，对应玩法输入被消费。JS fixedUpdate 获得 `pointerCaptured`、`keyboardCaptured` 标志。装饰型全屏 HUD 应设置 `body { pointer-events: none; }`，交互区域显式使用 `pointer-events: auto`。

Console 保留现有命令编辑、历史、补全与优先输入权，展示改用 RML。`r.Hud` / `--no-hud` 只控制项目文档，不改变文档自己请求的 show/hide 状态。`r.Stats` 单独控制引擎性能面板，默认关闭；`p.DebugDraw` / F2 独立控制物理线框；控制台不依赖这些开关。

项目可实现可选的 `updateUI(dt)`：场景 initialize 完成后以 dt=0 调用，之后在 gameplay tick 完成后刷新；游戏暂停或固定审计时由宿主继续调用。该回调不推进玩法，允许暂停时的按钮和数据变化及时反映到 UI。RmlUi 布局与输入继续使用真实时间。

Afterlight 项目的动画面板通过 `Engine.animationAttributes(id)` 读取名字、solver 与枚举属性，用 `Engine.animationAttribute` 提交修改。小地图通过通用 `Engine.physicsBodies(filter)` 获取启用碰撞体的世界 AABB 与查询属性，自行决定过滤、投影、颜色和刷新频率；角色位置与路径直接来自项目控制器。Frame 不再携带小地图障碍、动画检查器或文案等仅供面板使用的数据。

## 后端边界与验证

当前 Vulkan 后端实现 RmlUi 基础渲染接口：编译几何、预乘 alpha、字体 atlas、WIC 图片解码（PNG/JPEG/BMP 等）、矩形裁剪及变换。**尚未实现**高级图层、filter、box-shadow、自定义 shader、非矩形 clip mask、SVG/Lottie 插件。需要这些效果时应扩展 UiFrame 的绘制协议与 Vulkan 后端。RmlUi 的 C++ DataModel/数据表达式尚未映射为 JS 数据模型，当前用 DOM API 驱动数据更新。Win32 已支持 Unicode 输入和系统剪贴板；未接入 IME 预编辑文本/候选窗定位 API。

`ui_core_js` 覆盖真实 JS DOM 修改、事件修改玩法、输入捕获、文本编辑与重复键、模态窗口、失效句柄、回调中自移除、字体/裁剪快照、mailbox 丢帧、独立 Context、Map 重载与 HUD 开关。`animation_attributes` 使用项目 RML 的实际布局坐标点击风格选择器。`ui_core_js` 还验证真正空项目没有默认游戏面板、HUD/统计四种开关组合、独立控制台/物理调试、物理查询快照、动态小地图及暂停时 UI 更新。

```powershell
powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Test
.\build\bin\Release\Whimsical.exe --smoke --width 960 --height 600
.\build\bin\Release\Whimsical.exe --console-smoke
```

接口约定参考 [RmlUi integration](https://mikke89.github.io/RmlUiDoc/pages/cpp_manual/integrating.html) 和 [RenderInterface](https://mikke89.github.io/RmlUiDoc/pages/cpp_manual/interfaces/render.html)，实现以仓库固定的 6.1 头文件为准。

## 2026-09-06 项目 UI 所有权迁移验证

Release 构建通过，13 项 CTest 全部通过。实际 Vulkan 运行分别检查项目 HUD、关闭项目 HUD 后单独显示统计、smoke 的缩放/最小化恢复/地图重载，以及 console-smoke；四次均正常退出，validation errors=0。截图和报告在 `captures/ui-ownership/`。

空项目在 uiCore/ScriptRuntime 层验证了不会继承 Afterlight 项目的面板；直接启动完全空的 3D 项目仍被 Renderer 的现有“无灯光/无实例”检查拒绝。该渲染能力边界未在本次 UI 迁移中修改，不能把 UI 层验证等同于完整空场景渲染支持。
