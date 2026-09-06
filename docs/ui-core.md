# uiCore / RmlUi / JavaScript UI

`uiCore` 是独立 CMake 库，使用固定版本 RmlUi 6.1 和 FreeType 2.13.3。主线程拥有 Context、DOM、布局与事件；`render/UiRenderer` 只消费不可变 `UiFrame`，在渲染线程以 Vulkan 绘制几何、字体和图片。GDI HUD 与手工动画面板命中区域已移除。

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
- `engine/ui/EngineUi`：RML 控制台、动画属性检查器、帧统计与小地图。检查器从属性 schema 生成控件，通过 RmlUi click 事件修改 World。
- `Projects/Afterlight/Content/UI` 和 `scripts/gameplay/hud.asset`：项目 HUD；内容和文字更新属于项目 JS。
- `engine/render/UiRenderer`：Vulkan 资源缓存与 UI pass；不包含 RmlUi 头文件，不访问 DOM、World 或 JS。

RmlUi 全局初始化由共享服务管理，各 uiCore 有独立 Context 和录制器。`ScriptRuntime` 可选接收 uiCore，因此无窗口的玩法测试仍可运行。窗口应用始终安装 `Engine.ui`。所有文档 API 与 JS 回调在同一主线程执行。

场景重载先释放旧 realm 的事件订阅和项目文档，再销毁 JS heap；引擎工具文档持续存在。元素句柄使用 RmlUi ObserverPtr，节点被移除后再调用会抛 JS Error。DOM 事件中可取消自己的订阅或移除自身元素；回调异常写入现有日志通道。`Engine.loadScene` 仍延迟到事件调用返回后执行，即使游戏时间暂停，UI 输入阶段也处理场景请求。

UiFrame 每次包含完整绘制列表，并持有几何和纹理的共享引用。mailbox 丢帧不影响资源创建/销毁顺序。Vulkan 后端在帧 fence 之后回收没有快照或 Context 引用的资源，重复呈现不会重传已有几何。字体/图片上传发生在首次使用时。

## 项目资源

JS 入口接受 `/Game/`（当前项目 Content）或 `/Engine/`（引擎 Content）虚拟路径，带真实扩展名。RML 内的 RCSS、图片和模板使用相对引用；`createDocument` 的第二个参数确定相对路径基址。UI 文件是 Content 下的源资源，无需将 RML/RCSS 包进 `.asset`。JS 继续使用现有 Script 资产加载规则。

每个文档在自己的 RCSS 前合并 `/Engine/UI/base.rcss`，提供 div、p、form、table 等基础 display 默认值。引擎随内容携带 LatoLatin 常规/粗体及 OFL 许可证；Windows 安装了 Microsoft YaHei 时将其作为中文回退字体。项目可使用 `loadFont` 加载自己的字体。

```javascript
// Put this in a project Script asset included by .project.
var inventory = Engine.ui.loadDocument('/Game/UI/inventory.rml').show();
var count = inventory.getElementById('count');
var amount = 0;
var subscription = inventory.getElementById('add').on('click', function(event) {
    amount += 1;
    count.setText('Items: ' + amount);
    Engine.status('Inventory', 'Picked up ' + amount + ' items');
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
| `getBounds()` | 当前布局的边框矩形 `{x,y,width,height}` |
| `on(type, callback, capture?)`, `off(token)` | RmlUi 事件订阅与取消；支持冒泡和捕获 |

事件提供 `type`、`target`、`currentTarget`、`parameters` 和 `stopPropagation()`；回调返回 false 也会停止传播。参数中的布尔、整数、浮点值保留 JS 类型，其余转为字符串。控件逻辑从项目 Script 注册；这里没有浏览器运行时，也不执行 RML 内联 `<script>` 或 `onclick` JavaScript。

## 输入与显示

平台保留按顺序的鼠标、按键、重复按键与 Unicode 字符事件；每个 simulation tick 只消费一次。RmlUi 布局承担命中检测。指针在 UI 上、拖动从 UI 开始、表单持有键盘焦点或存在模态文档时，对应玩法输入被消费。JS fixedUpdate 获得 `pointerCaptured`、`keyboardCaptured` 标志。装饰型全屏 HUD 应设置 `body { pointer-events: none; }`，交互区域显式使用 `pointer-events: auto`。

Console 保留现有命令编辑、历史、补全与优先输入权，展示改用 RML。`r.Hud` 同时控制引擎工具和项目文档的显示，不改变项目文档自己请求的 show/hide 状态；控制台独立显示。UI 使用真实时间更新，不受 `t.TimeScale 0` 停止。

## 后端边界与验证

当前 Vulkan 后端实现 RmlUi 基础渲染接口：编译几何、预乘 alpha、字体 atlas、WIC 图片解码（PNG/JPEG/BMP 等）、矩形裁剪及变换。**尚未实现**高级图层、filter、box-shadow、自定义 shader、非矩形 clip mask、SVG/Lottie 插件。需要这些效果时应扩展 UiFrame 的绘制协议与 Vulkan 后端。RmlUi 的 C++ DataModel/数据表达式尚未映射为 JS 数据模型，当前用 DOM API 驱动数据更新。Win32 已支持 Unicode 输入和系统剪贴板；未接入 IME 预编辑文本/候选窗定位 API。

`ui_core_js` 覆盖真实 JS DOM 修改、事件修改玩法、输入捕获、文本编辑与重复键、模态窗口、失效句柄、回调中自移除、字体/裁剪快照、mailbox 丢帧、独立 Context、Map 重载与 HUD 开关。`animation_attributes` 使用实际 RML 布局坐标点击检查器。

```powershell
powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Test
.\build\bin\Release\Afterlight.exe --smoke --width 960 --height 600
.\build\bin\Release\Afterlight.exe --console-smoke
```

接口约定参考 [RmlUi integration](https://mikke89.github.io/RmlUiDoc/pages/cpp_manual/integrating.html) 和 [RenderInterface](https://mikke89.github.io/RmlUiDoc/pages/cpp_manual/interfaces/render.html)，实现以仓库固定的 6.1 头文件为准。
