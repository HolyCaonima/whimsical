# RenderTarget、EntityID 与 JS 像素回读

`DrawEntityID`（组件名 `drawEntityID`）将当前相机视图中的可见场景实体写入通用 `RenderTarget` 资产对应的运行时纹理。格式必须为 `R32Uint`：背景清为 `0`，实体 ID 直接从 GPU instance 的 `uint` 写入整数 attachment，完全不经过浮点转换、混合、抗锯齿或颜色转换。

## 资产描述与运行时内容

资产类型为 `RenderTarget`，使用现有 `ALAS1` 资产封装；payload 例如：

```json
{"width":96,"height":72,"format":"R32Uint"}
```

宽高均为正整数表示固定像素尺寸；均为 `0` 表示随当前视图尺寸分配。只允许两者同时为零。格式支持：

| 资产格式 | 每像素字节数 | JS `result.data` |
| --- | --- | --- |
| `R32Uint` | 4 | `Uint32Array` |
| `R32Float` | 4 | `Float32Array` |
| `RGBA8` | 4 | `Uint8Array`，RGBA 顺序 |
| `RGBA32Float` | 16 | `Float32Array`，RGBA 顺序 |

格式属于通用资源描述。仅 `DrawEntityID` 这个生产者要求 `R32Uint`；回读层不解释 EntityID，也不要求存在该组件。

- **AssetManager / 仿真线程**：加载不可变的尺寸、格式、资产引用。每次实际加载有独立的 `assetGeneration`；资产头的 `version` 是封装版本，不是 GPU 内容版本。
- **World / 仿真线程**：管理运行时 RT 句柄、回读请求和场景生命周期。相同已加载资产实例共用一个运行时 RT。多个启用的输出组件指向它时，每个视图快照只输出一次。
- **渲染线程**：独占 Vulkan 图像、深度 attachment、staging buffer。完成 fence 后复制为独立 CPU 字节，并发布完成信息。
- **JS**：只持有字符串句柄和收到的 typed array。没有 GPU 映射指针，也没有渲染线程访问 Duktape。

保存资产或场景只保存描述/资产引用，不保存像素、运行时句柄或回读请求。`clearCache` / rescan 后重新获取的描述会创建不同的运行时 RT；已经持有的旧描述和旧 RT 仍按现有资产契约有效。要让输出使用重载后的描述，需要重新设置组件引用。脚本可以释放旧句柄。

## ECS 与渲染

```js
var asset = Engine.asset("/Game/EntityIDs"); // 通用 {id, path} AssetRef
var rt = Engine.renderTarget(asset);        // 也接受资产路径字符串
var output = Engine.create({components: {
    drawEntityID: {target: asset}
}});
```

该组件不要求 Transform 或 Renderable；它请求整个当前视图的输出。组件所在实体的有效启用状态决定是否提取输出。组件通过 ComponentCatalog 支持创建、更新、查询、移除及场景保存/加载。

ECS 提取 → Frame → `EntityID Raster` → RT。光栅 pass 复用 GBuffer 的顶点程序、`GpuScene::recordDraws`、静态/内建几何和 CPU 蒙皮结果。两个 fragment 程序共用 `raster_surface.glsl` 的材质上下文、`EvaluateSurface` 和 `AcceptSurface`；材质的正背面剔除状态也相同。独立深度 attachment 清为 `1`，使用与正常渲染相同的 `LESS` 深度测试和深度写入。不可见/禁用对象不绘制；是否投射阴影不影响可见实体输出。UI、调试线和后处理不属于实体表面。

RT 尺寸可以不同于窗口。它使用当前视图的投影，覆盖同一归一化视野，光栅分辨率取 RT 的实际宽高。像素坐标原点在左上角，x 向右、y 向下，和视图坐标方向一致。

## 按需异步读取

```js
var info = Engine.renderTargetInfo(rt);
var ticket = Engine.readPixels(rt, {x: 48, y: 36, width: 1, height: 1});
// 在后续 fixedUpdate 中轮询；返回 null 时保留 ticket。
var result = Engine.pollPixels(ticket);
if (result !== null && result.status === "ready") {
    Engine.log("entity=" + result.data[0] + " frame=" + result.renderFrame);
}
```

`readPixels` 只建立请求，不等待 GPU。省略区域时读取完整 RT；宽或高为 `0` 时从指定起点读到对应边界。区域在渲染线程按实际源尺寸验证。可以传 `rtVersion: info.rtVersion` 要求匹配指定分配版本，否则返回 `stale-version`，不会悄悄读取重建后的资源。

未提交的请求会进入每一个后续快照，直到被渲染线程认领。丢弃快照不会丢请求；重放同一快照不会重复提交。请求读取被消费快照中生产者完成后的内容；如果该快照没有生产者，则读取 RT 保留的最近内容。它不承诺读取发起请求时的历史帧。没有任何生产者写过的 RT 返回 `uninitialized`。

回读复用截图/渲染审计的 `ImageReadback` 路径：RenderGraph 声明图像 Transfer 读取、buffer Transfer 写入及 Host handover，GPU 拷贝后通过原有帧 fence 完成交付。不增加同步 GPU 等待；不发起请求就不会拷贝像素。

`pollPixels` 在未完成时返回 `null`；完成时返回结果并消费 ticket。随后重复查询该 ticket 会报无效句柄。`cancelPixels(ticket)` 取消并丢弃 ticket，释放请求名额，无需再轮询。最多保留 64 个尚未消费的请求；每个渲染帧最多提交 64 MiB 回读字节，超出的请求返回 `readback-budget-exceeded`。应轮询或取消不再需要的请求。

## 结果与版本

`renderTargetInfo(rt)` 包含 `asset`、`assetGeneration`、`format`、`bytesPerPixel`、`descriptorWidth/Height`，以及最近完成的 GPU 内容信息。首次写入完成前，实际 `width/height` 为 `0`、状态为 `uninitialized`；固定尺寸可直接查询 descriptor 字段。

成功回读返回：

| 字段 | 意义 |
| --- | --- |
| `request` / `target` / `asset` | 请求句柄、运行时 RT 句柄、资产引用 |
| `assetGeneration` | 绑定的不可变资产描述实例 |
| `requestedTick` | 请求首次进入快照时的 tick，跳帧不会改写它 |
| `sourceTick` | **产生这些像素**的场景快照 tick |
| `renderFrame` | **产生这些像素**的渲染帧序号，从 1 开始 |
| `rtVersion` | Vulkan 图像的分配身份，重建/resize 会改变 |
| `contentVersion` | 本次分配内的写入序号，每次光栅输出递增 |
| `width` / `height` | 源 RT 的完整像素尺寸 |
| `region` / `rowBytes` | 实际读取区域和紧密排列的每行字节数 |
| `format` / `data` | 像素格式与独立 typed array，逐行排列 |

所有句柄及 64 位版本/帧号都用十进制字符串，避免 JS Number 精度问题。32 位 EntityID 使用 JS 数值及 `Uint32Array`，包括超过 `2^24` 的 ID 都无损。

成功状态是 `ready`。错误状态包括 `uninitialized`、`out-of-bounds`、`stale-version`、`readback-budget-exceeded`、`invalidated`、`render-failed`；错误没有 `data`。版本约束/边界错误提供尝试读取的源信息；没有源的错误不提供有效内容版本。

两个渲染帧可能使用同一个 `sourceTick`。脚本收到结果前，渲染线程也可能已经输出了更多帧，因此应以结果自身的版本为准，不能用接收时的 `renderTargetInfo` 替换它。移除组件后的第一次读取可能比上一次回读更新；生产者停止后，重复读取的 `renderFrame/contentVersion` 才保持不变。

## 失效与释放

`Engine.releaseRenderTarget(rt)` 使该运行时句柄失效，取消尚未完成的请求；这些 ticket 可轮询到 `invalidated`。已交付/已完成的 CPU 结果仍属于其标明的历史版本。仍启用的输出组件会在下一次提取时获得新的运行时资源，因此要停止输出也应移除/禁用组件。

场景切换或 `World::clearScene` 使旧场景的全部 RT 和请求失效。旧快照无法复活它们；正在 GPU 上执行的拷贝在 fence 后释放，取消状态不会被迟到的完成结果覆盖。窗口 resize 在重用/销毁存储前先交付上一帧结果；随视图尺寸的 RT 下一次写入时获得新的 `rtVersion`。固定尺寸 RT 不随窗口重建。

## 可运行示例

```powershell
.\build\bin\Release\Whimsical.exe --project Projects/EntityID --width 640 --height 480 --frames 90 --validation
```

[`Example.js`](../Projects/EntityID/Content/Example.js) 创建两个互相遮挡的 box，验证中心 ID、远处可见边缘、背景 0，再隐藏近处物体验证远处 ID，最后移除组件验证通用 RT 保留内容回读。成功输出 `EntityID example PASS`。此示例不依赖物理拾取，也不修改其他项目。
