# EntityID 输出示例

在仓库根目录运行：

```powershell
.\build\bin\Release\Whimsical.exe --project Projects/EntityID --width 640 --height 480 --frames 90 --validation
```

`Content/Example.js` 创建前后两个 box，使用通用 RT 资产引用挂载 `drawEntityID`，通过异步 ticket 读取 `Uint32Array`。它验证近处遮挡、背景 0、隐藏近处后的远处 ID，以及移除输出组件后的内容保留。

成功时日志出现 `EntityID example PASS`，并打印像素的渲染帧、快照 tick、RT 分配版本和内容版本。

完整 API、资源归属和失效规则见 [RenderTarget 文档](../../docs/render-targets.md)。
