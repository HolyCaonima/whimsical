# 三张场景重新打光

2026-09-07，使用统一物理 light 组件重新布光，保留原相机、材质、场景几何和玩法。没有通过提高全局曝光来替代灯光调整。

| 场景 | 方案 | 光源 |
| --- | --- | --- |
| Rain Court | 冷蓝月光与天空柔光托底，四盏暖色壁灯形成光池，门口面板光与青色控制台灯管强调交互位置 | 1 Directional、2 Rect、4 Spot、1 Capsule、1 Point |
| Honeybud Court | 暖阳花园，宽面天空柔光，屋顶暖边光；喷泉单独使用柔边重点光，窗户、灯笼及藤架保留暖色实景光 | 1 Directional、3 Rect、1 Spot、1 Capsule、4 Point |
| Maple Circuit | 覆盖整条赛道的统一日光，起跑区宽面反射补光，16 盏路灯按实际灯头位置向下发光；原云层天空保留 | 1 Directional、1 Rect、16 Spot |

Rain Court 的控制台 Capsule 总功率为 40W，开关读取 `scene.data.cyanLightPower`，与场景参数保持一致，不再使用旧点光单位常量。Honeybud 的太阳为 6.5W/m²，Maple 的太阳为 9.5W/m²。全部局部光源使用 W，角度及几何按 [物理光源约定](physical-lights.md)。

## 内容源文件

- `Projects/Afterlight/SourceArt/lighting.json`：Rain Court 与 Honeybud 的完整灯具定义。
- `Projects/MapleCircuit/SourceArt/lighting.json`：赛道灯具和天空的投影属性。
- Honeybud 导入器与 Maple 生成器读取这些定义，不会重新生成旧版全 Point 布光。Maple 的三张诊断地图也使用同一灯光方案。
- 现有灯实体的持久 ID 与 Rain Court 的交互角色引用保持不变；新增灯使用确定性 ID。

## 天空与投影

`render.castShadow` 默认为 true，可通过普通 `Engine.component` / `Engine.setComponent` 编辑，随场景持久化。false 只让物体退出光源可见性射线，仍保留光栅可见性、GI/反射表面命中与原有 collider。实现使用 TLAS instance mask：阴影查询使用 bit 0，材质射线查询保留完整 mask；属性更新沿用 RenderScene 增量通道，不重建几何。

Maple 的原天空球是 opaque 表面，会遮住无限远日光。天空实体现在设置 `castShadow:false`，云层显示与反射仍由原 Sky 材质提供。

## 验证与预览

三张地图均完成原生 GPU 捕获，Vulkan validation 错误数和审计非有限值均为零。ECS 脚本/持久化、资产场景、Shader 编译和玩法检查通过。`tools/test_shadow_visibility.py` 对照同一物体的投影开关，验证光栅结果完全相同、原阴影区域恢复受光。

| 场景 | 新打光截图 | 原打光对照 |
| --- | --- | --- |
| Rain Court | `captures/relight-after-RainCourt/frame.bmp` | `captures/relight-before-RainCourt/frame.bmp` |
| Honeybud Court | `captures/relight-after-HoneybudCourt/frame.bmp` | `captures/relight-before-HoneybudCourt/frame.bmp` |
| Maple Circuit | `captures/relight-after-MapleCircuit/frame.bmp` | `captures/relight-before-MapleCircuit/frame.bmp` |

入口仍为 `Run.cmd`、`RunHoneybud.cmd` 和 `RunMapleCircuit.cmd`。
