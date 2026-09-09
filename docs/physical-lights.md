# 物理光源

光源是普通 `LightComponent`，目录名 `light`，只依赖 `Transform`。一处组件契约同时支持原生增删改、JS、Map 保存/加载；不在 World、脚本或场景结构里为每种光源增加独立存储。RenderSystem 从刚体 world pose 提取 96 字节 GPU 数据，包含当前和上一帧使用的所有几何及归一化参数。

## 单位与坐标

距离为米，角度为弧度，锥角均为**半角**。Point、Spot、Rect、Capsule 的 `intensity` 是总辐射功率 W；Directional 是垂直于中心方向的平面上接收的辐照度 W/m²。无限远光无法定义有限总功率，因此使用辐照度。没有 lumen/candela、曝光或经验衰减系数混入接口。

`color` 是非负线性 RGB 能量比例，提取时按三通道之和归一化。例如 `[1,1,1]` 将总功率均分到三个通道；把 `[1,.5,.2]` 改成 `[2,1,.4]` 不改变能量。全黑颜色或零强度不发光。这里的 W 属于渲染器的 RGB 辐射模型，不是灯具电功率，也不进行未定义光谱的光度学换算。

| type | 发光形状及朝向 | 几何参数 | 零尺寸语义 |
| --- | --- | --- | --- |
| `directional` | 无穷远均匀角圆盘，沿 local +Z 传播 | `angularRadius`，默认 0.00465 | 0 为 delta 平行光 |
| `spot` | local XY 平面上的圆盘，沿 +Z 发射，锥形 smoothstep 分布 | `radius`、`innerAngle`、`outerAngle` | radius=0 为具有相同角分布的点发射器 |
| `point` | 均匀 Lambert 球面，朝外发射 | `radius` | 0 为各向同性 delta 点光 |
| `rect` | local XY 平面、法线 +Z 的矩形 | `width`、`height`、`twoSided` | 宽高必须 >0 |
| `capsule` | 沿 local Y 的封闭圆柱与两个半球 | `radius`、`length`（半球中心间距） | length=0 精确退化为球；radius 必须 >0 |

`twoSided` 只影响 Rect。开启后总功率在两个面之间平分。Point/Capsule 是朝外发光的凸面，背面样本不产生穿过自身的光；内部接收点不受其照亮。Spot 满足 `0 <= innerAngle <= outerAngle <= π/2` 且 outerAngle>0，两角相等是硬边锥。Directional 满足 `0 <= angularRadius < π/2`。

## 脚本与组合

```js
var lamp = Engine.create({name:'Ceiling panel', components:{
    transform:{position:[0,4,0], rotation:[Math.SQRT1_2,0,0,Math.SQRT1_2]},
    light:{type:'rect', color:[1,.8,.6], intensity:600, width:2, height:1}
}});
var light = Engine.component(lamp, 'light');
light.intensity = 300;
light.width = 3;
Engine.setComponent(lamp, 'light', light);
// 同样可以改 type、通过 Engine.parent 继承姿态、启停实体、移除 light。
```

`Engine.component` 返回描述副本；修改后写回。`setComponent` 替换完整描述，省略的字段恢复默认值，因此局部改值应先读取。原生使用 `add<LightComponent>` / `edit<LightComponent>` / `remove<LightComponent>`，同样受组件目录验证。

保留的 `Engine.light(...)` 简写创建 Point 类型，`Engine.lightIntensity` 修改总功率；新内容建议直接使用组件接口。灯具外壳、摄像机可见发光材质和碰撞体按需组合 `render`、`collider`，不会仅因拥有 `light` 就分配实例槽或 BLAS。解析发光面用于光照采样，不自动成为主相机或材质射线可命中的网格，也不互相遮挡；实体网格负责几何遮挡。不要把采样发光面埋在不透光外壳内部。

## 归一化和采样

设总功率向量为 Φ，发光面积为 A。Lambert 面辐亮度 `Le=Φ/(πA)`，双面矩形为 `Φ/(2πA)`。球面积 `4πr²`，Capsule 面积 `2πr(length+2r)`。Point delta 辐射强度为 `Φ/(4π)`。

Spot 在 `c=cos θ` 上使用 smoothstep。设 `ci=cos(innerAngle)`、`co=cos(outerAngle)`、`d=ci-co`，则点发射器归一化积分为 `π(2-ci-co)`，圆盘发射器为 `2π[(1-ci²)/2+d(co/2+0.35d)]`；后者包含发光面投影余弦。实际实现存储 `1-cos θ=2 sin²(θ/2)`，用等价形式计算归一化，避免窄锥角的浮点相减丢失。因而改变锥角或圆盘面积时总功率保持一致。

Directional 的角圆盘辐亮度为 `E/(π sin² α)`，条件立体角 PDF 为 `1/[2π(1-cos α)]`，所以正对圆盘中心的平面仍接收 E。α=0 单独采用 delta 测度。

所有有限面光源在面上均匀采样；Capsule 的圆柱和半球按面积选分支，再把 UV 重映射到对应分支。样本显式携带位置、法线、方向、距离、Le、面积 PDF、立体角 PDF 和 `Le/pdfOmega`。几何 Jacobian 是 `pdfOmega=pdfArea × distance² / |nLight·(-wi)|`。没有距离下限钳制或 `distance²+radius²` 衰减。恰好与 delta 光重合的点没有定义方向，返回零贡献。

Lambert 发光与采样测度参考 [PBRT Area Lights](https://pbr-book.org/4ed/Light_Sources/Area_Lights) 和 [PBRT Light Interface](https://pbr-book.org/4ed/Light_Sources/Light_Interface)。

## DI、GI、阴影与历史

`engine/lighting/LightSampling.h` 是可由 C++ 和 GLSL 编译的纯采样数学，原生物理测试直接执行 GPU 所用实现。

RTXDI 的样本域是 `(light ID, uniform UV)`。Target 包含 BRDF、接收面余弦以及 `Le/pdfOmega`；初始 streaming 只再除以离散选灯概率。不要在 reservoir normalization 中重复除面积或立体角 PDF。所有当前、历史、空间邻居和梯度 replay 均通过相同函数重算目标位置的 UV 积分核，重采样不混用面积与立体角测度。

CPU 选灯使用 90% 辐射能量亮度 / 10% 均匀混合，Directional 在该启发式中使用 1m² 参考截面；这是提议分布，不改变估计值。二次表面使用均匀选灯和同一个条件采样器，计算朝返回方向的 diffuse+GGX BRDF，同时服务漫反射 GI 和镜面间接光。

有限光源的阴影射线终点是实际采样位置；Directional 使用独立于相机 far plane 的长射线。所有阴影继续通过相同 masked/cull 材质求值及当前 TLAS。朝背面发射和凸发光面的自遮挡在采样器中处理。

灯实体增删、启停和同数量替换通过身份数组使 DI/NRD 历史重启；功率、色彩、姿态、尺寸、类型变化保留槽位，上传上一帧完整 Light，重评估样本并通过同样本梯度更新 NRD confidence。GI 储存二次辐亮度，任何灯参数变化都会拒绝其旧时域样本。光源变化不触发几何重建。

`render.castShadow` 默认 true。设为 false 的物体不遮挡光源阴影射线，仍可光栅显示并被 GI/反射射线命中。Maple 的天空球使用此属性保留原背景，同时允许 Directional 日光进入场景。三张地图的新布光见 [场景打光](scene-relighting.md)。

继承的实时渲染限制仍存在：GI 只有一次二次表面命中，带历史长度和 Jacobian 限幅；没有多跳 ReSTIR PT、发光网格重要性采样或 BRDF/light MIS。这些不是新的光源类型分支。

## 场景迁移与验证

Map v7 保存完整 `light` 组件。v3–v6 在读取边界将旧 Point 强度乘 `4π × sum(color)`，保留远场 RGB 照明；旧半径继续作为球面半径。由于移除了经验距离软化，近场和软阴影会变化。仓库地图、内容生成器及 Rain Court 开关脚本均已迁移；资产和对象 ID 保持不变。`tools/migrate_ecs_scenes.py` 可离线升级旧地图。

```powershell
powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Test
python tools/test_physical_lights.py
python tools/test_physical_lights.py --temporal
python tools/test_rtxdi_lights.py
python tools/test_rtxdi_motion.py
```

`physical_lights` 检查平方反比、球体近场、单双面、Directional 角盘、Spot 角分布、封闭球面的总通量、采样 PDF Jacobian，以及五类组件的真实 JS 创建/修改、层级、磁盘保存加载和停用移除。GPU 测试用独立面片积分对照未经 NRD 的线性 DI，另测 300m Directional 遮挡和零光源。`--temporal` 在 64 帧后移动光源并降为 1/4 功率，检查随后八帧的实际历史恢复。

运行示例：`.\build\bin\Release\Whimsical.exe --map /Game/Maps/PhysicalLights`。从左到右是 Spot、Point、Rect、Capsule，另有 Directional 填充；五盏灯由地图脚本通过普通组件接口创建。Space 切换恒功率尺寸动画，Q/R 或中键转动视角，滚轮缩放。

### 2026-09-07 实测

Release 构建、16/16 CTest 通过，设备为 RTX 3080。下列 GPU 测试均启用 Vulkan validation，错误数与审计非有限值均为零。

| 验证 | 结果 |
| --- | --- |
| 五类光源，独立面积积分对照 | 平均 DI 能量误差：Directional 0.067%、Point 0.147%、Spot 0.157%、Rect 0.076%、Capsule 0.126% |
| 同五类光源移动并降为 1/4 功率，随后八帧 | 平均 DI 能量误差均 <0.086% |
| 256 盏不同功率 RGB 点光 | 总能量误差 1.33%，区域均值误差 3.09%；零灯为精确零 DI |
| Directional 的 300m 外遮挡物 | 接收面 DI 为零，阴影未受相机 far plane 截断 |
| 移动遮挡物，10573 个变化像素 | NRD confidence 将相对误差从 27.89% 降至 7.50%，直接光恢复比例 98.45% |
| masked 表面 versus 移除表面 | albedo、DI、最终显示最大差均为零；真实叶片覆盖产生预期差异 |
| PhysicalLights 示例 | 96 帧正常退出，五种脚本光源、14 个几何实例、零非有限值 |

本地原始记录分别保存在 `build/physical-lights-188e2bc6`、`build/physical-lights-c81d025f`、`build/rtxdi-lights-fb919a54`、`build/rtxdi-motion-38cd102e`、`build/shader-surfaces-342e577d` 和对应 `captures/` 目录。示例截图为 `captures/physical-lights-showcase/frame.bmp`。
