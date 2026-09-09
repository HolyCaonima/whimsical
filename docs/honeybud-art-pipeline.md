# 蜜芽庭院 / Honeybud Court

使用参考图中的暖色、圆润轮廓、布艺、陶瓷和微缩花园语言制作的原创环境套件。建模通过 **Blender MCP** 的 `execute_blender_code` 完成，原始参考图片没有被用作游戏纹理。

双击根目录 `RunHoneybud.cmd`，或运行：

```powershell
.\build\bin\Release\Whimsical.exe --map /Game/Maps/HoneybudCourt
```

Map 使用现有角色移动、相机和跟随狗逻辑。初始为庭院全景；滚轮缩放，中键旋转，F 恢复跟随。原来的 Rain Court 仍可通过 `Run.cmd` 启动。

## 内容

- `Projects/Afterlight/Content/Maps/HoneybudCourt.asset`：667 个对象，包括独立的简单碰撞体、角色和 648 个静态网格实例。
- `Content/models/Honeybud/`：21 类完整资产，87 个按材质拆分的共享网格，合计 118,010 个唯一三角形；计入重复摆放后为 403,250 个静态三角形。包括小屋、花瓣杯喷泉、花架、坐垫长椅、茶桌、茶杯、布盖果酱罐、花箱、栅栏、路灯、园丁机器人、植物、草簇、围墙和地台。
- `Content/materials/Honeybud/`：37 个可复用 PBR 材质定义。
- `Content/textures/Honeybud/`：8 组纹理（针织、木纹、格布、草地、灰泥、坐垫缝线、叶脉、描金陶瓷），每组包含 baseColor、normal、ORM，共 24 张 1024×1024 纹理。PNG 供美术检查，`.asset` 是运行时载荷。
- `SourceArt/Honeybud/HoneybudCourt.blend`：包含材质、打光、预览相机、可编辑组件库和链接实例的源文件。
- `SourceArt/Honeybud/HoneybudCourt.glb`：便于其他 DCC 查看和交换的场景副本；本引擎加载 `.asset`，不在运行时解析 GLB。
- `SourceArt/Honeybud/manifest.json`：Blender 组件、材质资产引用及场景摆放。
- `SourceArt/Honeybud/mesh-budget.json`：完整资产面数及重复摆放后的场景三角形审计。
- `SourceArt/Honeybud/project_courtyard.png`、`project_textiles.png`、`project_ceramic.png`：真实项目 GPU 截图。

## 面数与细节预算

面数统一按**三角形**计数，同一完整模型的所有材质子网格合计；不通过拆分材质绕开预算。单模型硬上限 100,000，常规模型 20,000，小屋单独收紧到 60,000。当前 21 类中 20 类在 20,000 以下，最高为小屋 44,462；喷泉 14,736、长椅 14,468、花架 6,244、灌木 4,248、花朵 1,104–1,428、草簇 288。

上一轮细化版的静态场景为 2,786,142 三角形，优化后为 403,250，减少 85.5%。叶脉、陶瓷花瓣描金/浅浮雕、坐垫缝线、草地短草转入颜色、法线和 ORM 贴图；轮廓、杯壁厚度、座垫体积、大褶皱、包边和少量草叶保留几何。贴图生成会检查非有限像素。`honeybud_budget.py` 在 Blender 导出和 Map 导入两个步骤检查完整资产预算；模型超预算会中止导入。

喷泉水带为静态几何体配合不透明 PBR 材质；未实现流体、折射或毛发。角色使用项目已有模型。这是一套可运行、可编辑的环境套件。

## 框架

`StaticMesh`、`Texture`、`Material` 都由现有 `AssetManager` 注册和解析。模型不依赖动画骨架。Map 对象的 `render.mesh` 为持久 AssetRef；`materialAssets` 将 Map 内材质槽映射到可复用材质资源，未引用资源的原有内嵌材质继续可用。

Renderer 从不可变 Frame 接收模型和纹理引用，不读取项目文件。重复网格按资源对象去重，共享顶点、索引和静态 BLAS；只有蒙皮网格参与每帧 BLAS refit。变换继续通过原有 RenderScene 槽位增量发布。隐藏静态对象保留几何绑定，避免仅因可见性变化重建几何；销毁释放 World 持有的网格引用，旧 Frame 的资源仍然有效。

网格载荷 `STM1`：4 字节 magic、两个 uint32（顶点数、索引数），随后每顶点 15 个 float32（position3、normal3、UV2、tangent4、color3），最后为 uint32 三角形索引。Blender 到引擎的坐标转换为 `(x,y,z) → (x,z,-y)`，单位为米。

纹理载荷 `TEX1`：magic、uint32 宽高、RGBA8 像素；元数据声明 `sRGB` 或 `linear`。Vulkan 后端使用原生 SRGB/UNORM sampled image 和 64 槽纹理数组，上传时由 GPU 生成完整 mip 链，使用三线性/各向异性过滤与重复寻址，替换原来的 storage buffer 软件采样。光栅按屏幕导数选 mip，光追次级命中使用固定 mip 2；后者仍是当前质量边界。BaseColor、切线空间法线、roughness/metallic 同时用于 G-buffer 与 ray-query 命中。ORM 的 R 通道保留但未乘入光照，环境遮蔽由实际光追可见性处理。

## 重建

当前使用上游 MIT 项目 https://github.com/ahujasid/blender-mcp ，固定提交 `c5f35d9cc54451d785ac4c00c48bf9e98a2e8db9`。依赖位于被 Git 忽略的 `third_party`，源资产和自有导出脚本归项目所有。

```powershell
git clone https://github.com/ahujasid/blender-mcp.git third_party/blender-mcp
git -C third_party/blender-mcp checkout c5f35d9cc54451d785ac4c00c48bf9e98a2e8db9
python -m venv third_party/blender-mcp-venv
third_party/blender-mcp-venv/Scripts/python.exe -m pip install ./third_party/blender-mcp
# 在独立 Blender GUI 会话执行 tools/art/start_blender_mcp.py，然后：
third_party/blender-mcp-venv/Scripts/python.exe tools/art/mcp_call.py tools/art/build_honeybud.py
python tools/art/import_honeybud.py
third_party/blender-mcp-venv/Scripts/python.exe tools/art/mcp_call.py tools/art/export_honeybud.py
python tools/art/capture_honeybud.py
```

MCP 桥通过真实 MCP stdio 协议初始化服务并调用工具，再由上游服务连接 Blender addon。重建会替换该独立 Blender 会话的内容；请使用专门的制作会话。导出保留既有资产 ID；地图对象 ID 由套件实例语义确定。桥设置关闭遥测，工具结果记录在 `captures/honeybud/`。`export_honeybud.py` 只导出源文件和 GLB，不启动离线渲染；项目截图按三个有限帧数运行获取并恢复原始 Map 相机。

## 验证

`tools/build.ps1 -Test` 包含原有六组测试，并在 `project_assets_scene` 增加网格与纹理解码、非法索引、网格/纹理共享、资产移动、可见性、旧 Frame 生命周期、材质地图往返及庭院绕障路径验证。真实 Vulkan 截图和报告位于 `captures/honeybud/`。

2026-09-06 优化版：六组测试全部通过。RTX 3080、1440×1000、validation 开启、每视角 140 帧：全景 42.8 FPS / 16.75 ms GPU，布艺近景 36.0 FPS / 21.24 ms GPU，陶瓷近景 32.6 FPS / 23.89 ms GPU；三个视角 validation errors 均为 0。这些结果不代表稳定 60 FPS，近景实时光追仍有开销。完整报告位于 `captures/honeybud/refined/`；构建结果为 `captures/honeybud/optimized-build.log`。87 个导出载荷的实际三角形数已逐一核对；多材质子网格合计超预算的拒绝路径已验证。Rain Court 的窗口缩放、最小化恢复、地图重载及交互 smoke 通过，validation errors 为 0，日志位于 `captures/honeybud/optimized-smoke.log`。性能来自实际项目运行，不是 Blender 离线渲染。
