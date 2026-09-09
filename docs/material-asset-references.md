# Material 资产引用：框架审计与修复

## 根因

此前 `render.material` 的组件 codec 直接读写整数。这个整数同时被用作
World 材质数组的位置、Map 内嵌材质表的位置，以及 `materialAssets` 表的键。
即使材质来自资产，组件仍然只知道场景中的编号。

因此同一个材质有两处创作描述：Map 中的参数副本和外部 Material 资产；
复制组件需要理解来源场景的表，保存临时工具实体时还需要过滤材质并重排所有引用。
Details 展示数字只是这个所有权错误的表现，换成名称不会修复引用契约。

## 修复后的边界

| 层 | 所有权与表示 |
| --- | --- |
| Material 资产 | 唯一的参数来源；引用 Shader 和 Texture 资产 |
| Render 组件的 JSON / Map v9 | `material: {id, path}`；运行时引用还带 Content source |
| 运行时 RenderComponent | `shared_ptr<const MaterialAsset>`，与不可变网格资源采用同样的生命周期 |
| RenderScene | 根据实际使用的 MaterialAsset 分配紧凑的内部槽位；多个代理共享同一资产槽位 |
| GPU / Frame | 使用派生的材质索引和参数快照；不参与组件编辑或地图序列化 |
| HumanEditor | 显示资产名称、按真实资产类型选择，整个引用参与草稿、Apply / Revert 和撤销 |

材质的最后一个渲染代理释放后，RenderScene 回收槽位。尾部槽位移入空位时，只更新
受影响的渲染代理并发布属性变化；组件里的资产身份不变。这个工作发生在材质绑定变化时，
不需要每帧扫描组件来重建材质表。释放后的材质也不再占据 Shader / Texture 绑定容量。

资产的类型检查、按 ID 寻址、Content 来源身份、保存时的依赖闭合以及分阶段场景加载，
继续使用现有 AssetManager 和 ComponentCatalog。没有引入材质专属的资产注册体系，
也没有在引擎中加入 HumanEditor 或 RainCourt 的业务逻辑。

## 接口与迁移

```js
var surface = Engine.asset('/Game/materials/RainCourt/Foundation');
Engine.setComponent(entity, 'render', {
    mesh: Engine.asset('/Engine/Meshes/Box'), scale: [1, 1, 1], material: surface
});
Engine.setMaterial(entity, surface); // 也接受资产路径
var description = Engine.content.describe(surface); // {ref, header}，不加载 payload
```

- 删除 `Engine.material(...)`、`Engine.scene.addMaterial(...)` 和数字材质绑定契约。
  创建材质走常规 `Engine.content.save`；参数属于 Material 资产。
- Map v9 不再包含 `materials` / `materialAssets`。已有资产绑定保留 ID；
  v7/v8 内嵌材质使用 `python tools/migrate_material_assets.py <Map.asset> ...` 提取。
  加载旧格式会明确报错，读取地图不会隐式写入资产。
- 迁移器只理解通用的组件引用。项目自定义 JSON 中的数字别名必须由项目迁移；
  本次已将 RainCourt 的 `data.materials` 转为 AssetRef，并迁移反馈标记材质。
- 已迁移仓库中的 8 张地图，提取 34 个 Material 资产；Honeybud 的 37 个已有资产绑定
  保留原 ID。另为编辑器 gizmo、Afterlight 项目反馈标记和 EntityID 示例提供常规材质资产。
- HumanEditor、MapleCircuit 和 Honeybud 的生成入口已使用资产引用。
  临时 gizmo 实体仍通过 `persistent:false` 排除，不再需要临时材质表和编号重映射。

## 验证边界

仅构建 Release 的 `Whimsical` 目标，并使用实际程序加载项目、渲染及检查退出日志。
没有新增、修改或运行测试用例。旧测试中使用数字材质契约的代码仍待单独迁移，
本次应用目标构建不等同于全量测试目标构建。

Release `Whimsical` 目标构建通过。HumanEditor、Afterlight / RainCourt、MapleCircuit
和 HoneybudCourt 各完成 60 帧运行，正常退出且 Vulkan validation errors=0。
迁移数据核对未发现材质参数或实体其他字段发生改变。

运行记录保存在 `captures/material-assets/`。材质选择器尚未完成实际点击验证；
按用户要求不再使用 computer-use。
