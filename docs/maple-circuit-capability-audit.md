# Maple Circuit 视频复刻能力核对

日期：2026-09-06。参考：用户提供的 28.18 秒赛车录屏。

## 当前结论（用户确认后更新）

首轮核对发现引擎缺少 3D 透明混合渲染能力，并按要求停止。用户随后明确要求“烟雾暂时用 opaque 的”，解除此次复刻的阻塞。现已创建独立 `Projects/MapleCircuit` 项目，烟团使用 opaque 静态网格，通过缩放和生命周期表现消散；引擎代码没有修改。

运行、架构和验证见 [项目说明](../Projects/MapleCircuit/README.md)。下文保留原始能力核对及透明渲染改进建议，透明混合不属于本次实现范围。

## 视频与现有能力

| 视频内容 | 现有项目接口 | 判断 |
| --- | --- | --- |
| 街区闭环道路、路肩、住宅、树木、白色跑车 | Map、StaticMesh、Shader、Material 资产 | 可在独立 Project 中制作资产 |
| W/S 驾驶、A/D 转向、Space 漂移 | JS fixedUpdate、输入、Engine.moveBody 与接触反馈 | 街机式速度、转向、抓地规则可由项目 JS 实现；不需要以缺少完整刚体车辆系统为由阻塞 |
| 四车比赛、三圈、排名、圈时、重赛 | JS 状态与场景对象、场景加载 | 可由项目定义比赛规则与 AI 驾驶 |
| 追尾镜头 | Engine.camera 的目标、朝向、俯仰和距离 | 可由项目 JS 跟随车辆 |
| 排名栏、速度表、赛道小地图、重赛按钮 | Engine.ui / RmlUi DOM、事件、图片与变换 | 可由项目实现 |
| 漂移烟团的半透明及渐隐 | 3D Shader 只支持 opaque / masked | 原始缺口；用户已同意改用 opaque 烟团 |

## 阻塞证据

- `engine/assets/ShaderAsset.h:8`：SurfaceMode 只有 Opaque 和 Masked。
- `engine/assets/ShaderAsset.cpp:49`：加载时拒绝 opaque / masked 以外的 surface 模式。
- `engine/render/ShaderCompiler.cpp:61`：masked 的 opacity 参与阈值裁剪；不是与后方场景颜色进行透明混合。
- `docs/shader-materials.md` 的 Surface contract 明确记录：不支持 transparent blending，延迟表面总是进行深度测试并写深度。

视频约 7 秒、18 秒可以观察漂移烟团；参考帧保存在 `captures/video-reference/detail-7.png` 和 `detail-18.png`。烟雾透明表现是视觉观察，不代表已取得原游戏的材质源码。

Blender 可以制作烟雾网格或纹理，但不能使引擎接受透明材质。JS 可以管理烟团的位置、缩放和寿命，但不能补出透明混合渲染路径。RmlUi 的 UI alpha 也不等于具有世界深度遮挡的 3D 透明渲染。本次使用用户已明确同意的不透明烟团，保留透明混合缺口供后续引擎设计参考。

## 建议的引擎边界

由引擎提供通用透明表面的渲染契约：透明材质模式、深度测试与写入策略、透明排序或 OIT、合成阶段，以及项目可用的实例透明度参数。烟团生成、寿命、漂移触发仍归项目 JS；不需要向引擎写入赛车专用逻辑。

透明渲染仍是后续框架建议。当前已根据用户许可建立 `Projects/MapleCircuit`，使用 `.project`、Map、资产及 JS 组织游戏。

## 其他已发现边界（不作为本次停止的额外依据）

- `engine/render/shaders/common.glsl:140` 的环境天空是固定渐变。没有发现项目天空环境配置接口；此次已经使用项目天空球及程序化发光表面实现蓝天白云，没有修改该函数。
- 引擎源码、构建定义和 JS 绑定中未发现音频播放系统。本次未审听并确认录屏中的声音来源，因此不将发动机音效列为已确认的视频需求或阻塞项。

未使用视频里的文字或其他附带内容作为操作指令。
