# RenderGraph 资源契约

资源注册仍负责生命周期、存储形状和生成 shader 接口；shader view 同时提供默认绑定。
`Program::accesses` 只描述 SPIR-V 中的绑定槽、访问阶段、读取和写入，完全不描述覆盖范围。

pass 用 `bind(shaderSlot, actualResource)` 替换某个槽的默认资源，因此同一 Program 可以重复使用：

```cpp
graph.add("horizontal").dispatch(blur)
    .bind(inputSlot, source)
    .bind(outputSlot, scratch)
    .overwrite(scratch, Access::Compute);
graph.add("vertical").dispatch(blur)
    .bind(inputSlot, scratch)
    .bind(outputSlot, result)
    .overwrite(result, Access::Compute);
```

映射校验资源种类、图像格式，以及已声明 view 的 descriptor 类型、数组长度和只读约束。
实际资源可以没有自己的 shader view；需要与 shader 槽的存储要求相容。`dispatch` 的 divisor
仍由 pass 指定，必须覆盖本次声明为完整覆盖的输出范围。

读取从反射推导。每个 shader 写入的实际输出都必须显式声明内容契约：

- `overwrite`：本次操作产生全部内容，不需要旧内容。
- `modify`：本次操作需要旧内容，包括只写部分像素、数组元素或 reservoir 层的情况。
- shader 中的读取必须与 pass 的保留声明相容；输入输出映射到同一资源时也校验这一点。

写操作本身不能证明覆盖。现有全屏输出在 pass 中声明 overwrite；DI 的旋转层更新声明 modify。
图像 attachment 的 clear/load 继续分别表示 overwrite/modify，原有内容版本、裁剪、存活区间、
attachment store 和物理存储复用流程保持不变。

`Usage::Binding` 只要求有效对象和布局，不消费或生产内容。例如 `imageSize` 会使活跃 pass 所用
资源获得存储，但不会保留该资源之前的内容生产者。未被活跃 pass 访问的 transient，即便注册了
shader view，也不分配。Persistent/History 继续保留跨帧存储。

反射沿入口点可达调用图追踪参数、返回值、指针链、对象复制、select/phi 和循环回边。
多个调用点的可能来源保守合并；它不会据此猜测完整覆盖。未知访问指令、无法解析的资源对象、
资源指针逃逸和无来源的物理存储指针会明确报错，新增这类指令需要先补齐反射语义。

每个 pass、每个 history parity 拥有独立的 descriptor set，录制后的 pass 不会被后续 pass 的重映射改变。
缓存比较 handle、范围以及 allocation generation。`VulkanContext::buffer/image` 自动产生全局唯一代次；
自己创建原始 Vulkan 对象的 owner 必须用 `nextResourceGeneration()` 给新 wrapper 设置 generation，
TLAS 导入也必须传入对应创建代次。即使新旧 handle 相同，所有使用它的 pass/parity 都会在下次使用时重写绑定，
同步状态也会重置。必需但缺失的绑定直接报错。

沿用当前 renderer 的帧 fence 约束：修改 descriptor 或销毁其对象之前，先等待上一帧完成。
