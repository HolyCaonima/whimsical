# Project、Asset 与 Scene 持久化

## 架构边界

`Project` 只描述项目配置和启动入口；`ContentMounts` 把虚拟根映射到独立 Content 目录，`AssetManager` 在每个来源内管理资产索引、ID 和缓存。一个管理器可以同时访问多个 Content，不要求它们包含 `.project`。`Asset` 是不可变资源；`SceneDocument` 是可序列化描述，其组件集合由 ComponentCatalog 统一编解码、校验与挂接；`World` 是描述实例化后的运行时世界。`RenderScene` 仍是常驻渲染代理集合，常驻不等于磁盘持久化。

本次移除了 `animation::Library` 和 ScriptRuntime、ControllerAsset、SkinnedMesh 中的项目文件访问。类型解码器接收字节流和已解析依赖；ONNX Runtime 从 AssetManager 读取的字节建立 session。Renderer 只消费 Frame 中的 mesh 与姿态，不认识项目目录或持久 ID。SPIR-V 和 captures 是引擎构建／诊断文件，不属于 Project Content。

```text
afterlight_assets: ContentMounts → AssetManager → Asset / Json；Project 独立描述启动配置
afterlight_animation: Skeleton / Asset / Instance / Solver → afterlight_assets
afterlight_core: World / ScenePersistence / RuntimeHost / ScriptRuntime → 上述两层
EngineAssets: 在组合入口注册 Map、SkinnedMesh、AnimationController、OnnxModel
Renderer: Frame → GPU 资源
```

通用 AssetManager 自带 Data、Script、Binary 类型，其余由 `registerEngineAssets()` 注册。增加后端只需注册类型解码器，无需修改 World。`animation::Asset` 继承公共 `afterlight::Asset`，保留 skeleton/createSolver/attributes 协议；每个角色仍有独立 Instance/Solver。

## Project 与路径

```text
Projects/Afterlight/
  .project                    UTF-8 JSON：version/id/name/startupMap/scripts
  Content/
    Maps/RainCourt.asset
    scripts/.../*.asset       内嵌 ES5 源码
    models/biped.asset        内嵌 SKN1 网格
    animations/ai4animation/biped/
      controller.asset       内嵌 A4C2 骨架/指导姿态，头部引用两个 ONNX 资产
      network.asset          纯头：source = "network.onnx"
      network.onnx           原始模型
      network_golden.asset   内嵌 PyTorch golden 测试载荷
      postprocessor.asset / postprocessor.onnx
```

`Project(directory)` 或 `Project(directory / ".project")` 打开项目；`Project::create(directory, name)` 创建空项目。项目 ID 为 128 位随机持久标识。startupMap 可以为 null；scripts 是按顺序加载的公共脚本虚拟路径数组。

挂载 `/Game` 后，`/Game/models/biped` 映射为该来源的 `models/biped.asset`；同样可以把另一个目录挂到 `/Library`。路径不带扩展名、区分大小写；段内使用 ASCII 字母、数字、下划线、连字符。不接受 `.`、`..`、重复斜线、反斜线、盘符、空段。注册与保存拒绝 Windows 大小写别名。目录层次就是虚拟路径层次，不另建手写 manifest。

所有 `.asset` 扫描注册，非 `.asset` 文件不作为独立资产暴露。外部载荷仅通过头部声明的相对位置访问，并检查真实路径仍在 Content 内；绝对路径、缺失载荷和逃逸 Content 的映射报错。Project 整体拷贝到另一目录无需改变资产引用。

## 多 Content 挂载与依赖边界

```cpp
AssetManager assets;                         // 不打开项目，也不执行脚本
registerEngineAssets(assets);
assets.mount("/Game", gameContent);
assets.mount("/Library", libraryContent);
auto value = assets.load<DataAsset>(AssetPath("/Library/config"));
auto ref = value->reference();                // id + /Library/config + source
assets.unmount("/Library");
// value 仍是有效的不可变快照；assets.load(ref) 现在报错。
```

挂载名是一个虚拟根，如 `/Game`、`/Library`；物理根必须存在且互不重叠。挂载时只扫描信封描述符，不解码 payload、不启动 Map、不执行 Script。挂载失败会撤销本次注册。`mount(alias, root, false)` 创建只读来源。

调用方可以显式访问所有挂载根。资产加载、类型解码、场景实例化和保存则进入所属来源的闭合作用域：其中的 `/Game/...` 总是指向该来源，外部别名和外部来源的 AssetRef 都被拒绝。ID 查找从不搜索其他挂载源。自定义解码器调用同一个 manager 时自动遵守此规则，不能在解码期间改变挂载表或退出依赖作用域。

运行时 AssetRef 增加 `source`（本次挂载的身份），并提供当前挂载名下的可读 path。延迟使用的 Map、Data 和 metadata 引用也保留这个身份。卸载清除该来源的索引和缓存；重用别名、甚至重新挂载同一个目录，都会生成新身份。旧 AssetRef 不能解析或保存到新来源；旧 shared_ptr 和已经录制的 Frame 可以继续使用。字符串路径是软定位器，访问当前挂载；需要跨操作保留身份时使用 AssetRef。

JSON 中带 `id` 和 `path` 的对象是保留的资产引用形态，可附带运行时 `source`。AssetManager 统一遍历 metadata 和声明为 `Encoding::Json` 的 payload，在解码前绑定来源，保存前验证来源并输出 `/Game/...`，移除运行时 `source`。二进制和文本解码器把结构化依赖放在 metadata 中；任意字符串不自动视为引用或改写。`registerLoader(type, decoder, encoding)` 的 encoding 为 Raw、Text 或 Json，与 embedded/external 存储方式独立。

`ScenePersistence::save` 在 capture 之前进入目标来源，混合了其他 Content 资产的 World 不能直接保存，即使目标来源存在同 ID 资产。内容迁移需要显式导入并重新绑定本地资产。普通加载和保存不会隐式复制依赖。

`read(path/ref)` 返回绑定来源的 `{ref, header, payload, encoding}`；`write(path/ref, header, payload)` 校验后写入完整内容，external 模式回写声明的 payload 文件。`save` 保留信封接口：external 模式传空 payload，读取并验证已有外置内容。每个文件原子替换；外置 payload 与信封的两次文件提交不是跨文件事务。

## JS 内容访问

`Engine.content` 独立于玩法当前场景，路径始终使用显式挂载名；`Engine.readJson` 和 `Engine.ui` 的 `/Game` 默认来源则由显式启动的项目或当前 Map 决定。一个脚本 realm 只属于一个 Content，以保证后续回调中的 `/Game` 也有固定含义。RuntimeHost 将常驻应用 realm 与场景 realm 分开：`Engine.scene.load` 可只装载另一来源的地图而保留应用脚本/UI，`Engine.simulation.play` 再显式启动目标程序。场景公共脚本与地图脚本在同一 realm 时仍须同源。详见 [运行宿主与视图](runtime-host.md)。

```js
var content = Engine.content;
content.mount('/Library', 'D:/Library/Content'); // 第三个参数 false 表示只读
var refs = content.browse('/Library');           // 只浏览 .asset，返回 AssetRef
var doc = content.load(refs[0]);                 // 也接受 '/Library/config'
// Json 类型返回 JS 值，Text 返回字符串，Raw 返回 Duktape buffer。
doc.payload.value = 20;
content.save(doc.ref, doc.header, doc.payload);  // 按原来源回写，包含 external payload
content.unmount('/Library');
```

| API | 行为 |
| --- | --- |
| `mount(alias, directory, writable=true)` / `unmount(alias)` | 注册或卸载一个 Content |
| `mounts()` / `browse(alias)` | 列出挂载身份、读写属性／所属资产引用 |
| `reference(pathOrRef)` | 获得或刷新带来源身份的引用 |
| `load(pathOrRef)` | 返回描述符和 payload；加载 Script、Map 不执行脚本或实例化世界 |
| `save(pathOrRef, header, payload)` | 创建或修改资产；JSON payload 自动按来源规范化；Raw 接受 buffer 或字节字符串 |
| `newId()` | 为新资产生成持久 ID，header 其余字段遵循 ALAS1 格式 |
| `scan(alias)` | 显式刷新一个来源的磁盘索引 |

UI 使用相同 `ContentMounts`，通过 `UiCore(assets.mounts())` 接入。UI 源文件保留真实扩展名，使用 `Engine.ui.loadDocument('/Library/UI/panel.rml')` 加载。RML、RCSS、纹理的相对路径与内部 `/Game` 都固定在各自文档来源；盘符、越界路径和跨来源引用失败。RmlUi 内部地址带挂载身份，使不同来源与重新挂载后的样式、纹理缓存互不混淆。引擎基础样式和系统字体由宿主显式挂载并组合，不依靠 UiCore 内的物理目录常量。

最小可运行示例：[ContentMountsExample.js](../tests/ContentMountsExample.js)。[ContentMountTests.cpp](../tests/ContentMountTests.cpp) 在 build 下生成两个不含 `.project` 的 Content，并运行这个 JS 示例及必要的 C++/UI 边界检查。构建 `content_mount_tests` 后运行 `ctest --test-dir build -C Release -R '^content_mounts$' --output-on-failure`。

## 统一文件格式

```text
ALAS1\n
单行 UTF-8 JSON 描述符\n
载荷直到 EOF
```

写入采用二进制模式，头部行也接受 Windows CRLF。描述符示例：

```json
{"version":1,"id":"5c3e6cd9137e4ded92663731200bb934","type":"Map","name":"Rain Court","storage":"embedded","metadata":{}}
```

id 为 32 位小写十六进制字符，不从真实路径、显示名或运行时句柄推导。type 选择加载器，version 是类型描述及载荷契约版本，name 为显示名，metadata 保存类型描述信息。未知版本明确报错。

| 当前项目类型 | 所选 storage | 载荷 |
| --- | --- | --- |
| Map | embedded | SceneDocument JSON |
| Data | embedded | 配置／manifest JSON |
| Script | embedded | UTF-8 ES5 源码 |
| SkinnedMesh | embedded | SKN1 little-endian 字节流 |
| StaticMesh | embedded | STM1：位置、法线、UV、切线、顶点色及三角形索引 |
| Texture | embedded | TEX1：宽高、RGBA8；头部声明色彩空间 |
| Shader | external 或 embedded | GLSL 表面函数体；metadata 定义 schema、material model 与 render state |
| Material | embedded | Shader AssetRef、命名属性值与 Texture AssetRef |
| AnimationController | embedded | A4C2 little-endian 字节流 |
| Binary | embedded | 带 format 描述的原始字节；目前用于 golden 数据 |
| OnnxModel | external | 没有内嵌载荷；source 相对头文件定位独立 `.onnx` |

资产有两种载荷存储形态，由每个 `.asset` 头部的 `PayloadStorage` 决定：`Inline`（文件字段 `storage: "embedded"`）和 `External`（`storage: "external"`）。存储形态与 `type` 独立：加载器只负责解码，AssetManager 按头部选择内嵌字节或 source 指向的外置字节。同一类型可以使用任一存储形态，不需要注册两套加载器。当前项目的 Map、网格、控制器等使用 inline，ONNX 使用 external。

external `.asset` 中只保存描述符及 payload 的相对地址，不含 inline 字节；inline `.asset` 不声明外部地址。source 是相对 `.asset` 的文件路径，只供 AssetManager 定位 payload，**不是资产虚拟路径或资产引用**。

所有 AssetPath / AssetRef 都只能定位已注册的 `.asset`。例如 `/Game/animations/ai4animation/biped/network` 唯一映射到 `network.asset`，再由它的 source 找到 `network.onnx`。虚拟路径保持无扩展名语法，`.onnx` 等 payload 扩展名不能进入 AssetPath；即使裸 payload 存在，也不会自动按名字或扩展名回退加载。移动 payload 只更新 `.asset` 的 source，不改变资产 ID 或其他资产的引用。

持久资产引用为 `{"id":"...","path":"/Game/..."}`。ID 只在所属 Content 内是权威身份，path 是可读定位提示。移动资产、保留头部 ID 并 scan 后，旧引用仍按 ID 找到新位置，保存时更新提示。ID 不存在时报错，不按旧路径误绑定另一资产。Project 和脚本中的字符串路径是软定位器，重命名时需要更新。

## 注册、缓存与生命周期

AssetManager 由程序组合入口持有，限定 simulation 线程访问。每个挂载源有独立索引、ID 表、依赖环检测和缓存；不同 Content 允许相同路径和相同资产 ID，同一 Content 内仍拒绝重复。扫描只读取描述符，不解码 ONNX 或网格。`load<T>(AssetPath/AssetRef)` 检查类型，以资产 ID 强缓存 `shared_ptr<const Asset>`；加载器依赖经过同一 manager，依赖环报错。

`scan(alias)` 事务替换该来源注册表并清理其缓存；`scan()` 重扫全部来源；失败保留原表。save 先校验类型载荷及引用归属，写同目录临时文件，再通过 Windows replace 提交，成功后更新所属来源注册表并清理其缓存。已有位置不能改变 ID 或类型。没有隐式文件监视、活实例热替换、后台加载或跨项目全局缓存。

clearCache、重扫、保存不销毁 World、Solver 或 Frame 持有的旧资产版本；最后一个 shared_ptr 释放后回收。GPU 资源仍由渲染线程在 fence 安全点管理。要使用磁盘新版本，先重扫，再显式重新加载场景。

## 身份和 Object Path

对象路径为 `/Game/Maps/RainCourt:<32位ObjectID>`。Map 路径确定命名空间，Object ID 确定对象。显示名可重复、可修改，对象顺序可变；同一 Map 保存／加载不改变 Object ID。Map 移动或 Save As 改变完整路径前缀，Object ID 保留。

`World::objectPath(entity)` 生成路径，`resolveObject(path)` 返回当前实例的 Entity，未加载或已删除对象返回 0；解析路径不会自动加载地图。

| 身份 | 用途 | 写入 Map |
| --- | --- | --- |
| Project ID、Asset ID | 项目／资源持久身份 | 是 |
| Object ID | Map 内持久对象身份 | 是 |
| Entity ID | World 某次实例的对象句柄 | 否 |
| Render slot | GPU 镜像槽位，销毁后可复用 | 否 |
| BodyHandle slot/generation | PhysicsScene 临时句柄 | 否 |
| 材质／灯表下标 | Map 自身的表内索引 | 是，与 GPU/Entity 句柄无关 |

切图一次转移准备好的组件与后端存储，并延续 Entity ID、物理 generation 和渲染版本序列。Registry 只存活实体，旧实体与物理句柄不会指向新对象。新渲染槽表通过 structural delta 发布，revision/topology 不归零。切图重置时域历史，首个加载快照被覆盖时该标记仍会保留。

当前是单 World、单已加载 Map。尚未引入 streaming、同一 Map 多实例或跨 Map 活实体解析。持久 ID 查询使用映射表，热点继续使用整数 Entity。

## Scene Save/Load

SceneDocument 保存：对象 ID/显示名/位置/旋转/启用/交互；RenderComponent 的 primitive、scale/offset/animationScale、材质索引和可见性；主碰撞体 shape/motion/layer/查询属性；关节碰撞体 joint/local/shape/blocking；动画与 mesh 引用、root-motion 选项、rootOffset 和实例属性；材质值表、灯表、相机、导航（含 planeTolerance）；player Object ID、命名 Object 引用、Map 脚本引用和显式 gameplay JSON data。

Map 现写入 `version: 7`，以 `entities[].components` 保存实际存在的能力与父级持久 ID，变换保存 local。版本 3–6 在读取边界转换，1/2 仍被拒绝；ALAS1 资产信封版本仍为 1。六张现有地图、生成器及验证脚本已同步迁移，详细字段、组件依赖和脚本入口见 [ECS 架构](ecs.md)。

材质可作为 Map 内嵌值；复用的材质是注册的 Material 资产。Shader 与 Texture 按持久 ID 解析。CPU 材质不保存 descriptor index，World 和 Frame 仅持有参数值与不可变资产引用，纹理绑定由 renderer 分配。

capture 跳过已删除对象，删除对象也移除其命名引用。未注册的自定义 Solver/mesh 无法重建，保存明确报错。save 始终把 SceneDocument 写成 inline payload；对同一 Map 保留 ID，Save As 创建新 Map ID，保留 Object ID。无 Solver 的手动关节姿态可保存；有 Solver 的姿态重新求解。选择、悬停、路径命令、计时器、推理序列、IK 历史、RenderDelta、GPU 句柄、物理缓存与 JS 闭包不序列化。

load 先在临时 World 检查全部依赖、骨架绑定、动画属性、关节碰撞体和物理形状；成功后建立身份和层级，再经各系统挂接实际组件，恢复 player 与命名引用。内容预检失败保留原场景。加载只在隔离 World 中准备一次组件及后端，再交换场景存储；内存耗尽等分配失败不承诺事务回滚。

显式调用 `RuntimeHost::initialize(project, mount)` 或游戏导航 `loadScene` 才启动场景程序。宿主在场景装载后创建场景 realm，依次载入同源 Project 公共脚本、Map 脚本，调用可选 initialize。数据入口 `Engine.scene.load` 不启动程序；常驻 `hostScripts` 和它的 UI 在场景替换时保留。Rain Court 的 initialize 只绑定控制、同伴、相机与交互。供电状态由 setSceneData 显式更新，门位置／碰撞和缓存材质随组件保存，因此加载后能继续交互。脚本初始化错误向调用方报告，此时 Map 已加载，不回滚整个 VM。

这是场景与显式玩法状态持久化，不是整个游戏进程的逐指令快照。新增玩法应定义稳定数据，在 initialize 恢复，不向持久数据写入 Entity。

## 使用入口

```cpp
Project project(projectDirectory);
AssetManager assets(project.content());
registerEngineAssets(assets);
World world;
RuntimeHost scripts(world, assets);
scripts.initialize(project);
auto mesh = assets.load<SkinnedMesh>(AssetPath("/Game/models/biped"));
scripts.saveScene(AssetPath("/Game/Maps/MySave"), "My save");
scripts.loadScene(AssetPath("/Game/Maps/MySave"));
```

```js
var settings = Engine.readJson('/Game/animations/locomotion');
var player = Engine.sceneObject('player');
var path = Engine.objectPath(player);
var entity = Engine.findObject(path);
Engine.saveScene('/Game/Maps/MySave', 'My save');
Engine.loadScene('/Game/Maps/MySave');
```

JS load 延迟到当前受保护调用返回后执行，避免销毁正在执行的 heap。save 同步执行；sceneData 返回拷贝，修改后须 setSceneData。sceneObject 解析 Map 命名引用，cameraState 用于加载后初始化相机控制器。

命令行：`Afterlight.exe --project <项目目录或.project> --map /Game/Maps/RainCourt`。默认 Project 是仓库 Projects/Afterlight，默认 Map 来自 startupMap。

## 验证和离线工具

`project_assets_scene` 覆盖同类型 inline/external 共用解码器、外置载荷移动后的引用稳定、拒绝 payload 直接引用、拒绝混合存储描述、外置载荷保存校验，以及注册、类型、重复 ID、版本、路径、外部载荷、依赖环、缓存、目录搬迁、资产移动、完整场景往返、Save As、重复加载身份隔离、快照存活、动画属性、关节碰撞体、交互闭包重建、失败预检与空地图。测试在 build 下创建独立项目，不改示例内容。

原 gameplay、physics、animation、companion、attribute 测试保留行为断言，只迁移资源入口。ONNX、A4C2、SKN1 和 golden 原载荷已逐字节核对；Rain Court 保留 48 个对象、10 个材质、8 盏灯和 BigSteps 初始风格。

`tools/animation/asset_format.py` 提供离线封装／读取，重新导出保留已有 ID。ONNX 导出产生模型、纯头 asset、内嵌 golden 和 metadata；rig/mesh 直接生成内嵌 asset；诊断工具也从 envelope 读取。旧 assets/scripts 目录已在迁移、内容核对和用户确认后删除。
