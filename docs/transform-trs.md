# Transform TRS

Transform 是实体及其子树唯一的空间状态。local 和 world 都保存 position、rotation、scale；world 是 TransformSystem 推导的缓存，不入盘。Render 只保存几何、材质和渲染属性，没有 offset、scale、animationScale 或单独的显示姿态。

## 无切变组合

采用 UE FTransform 风格的直接 TRS 组合。P 为父级世界变换，L 为局部变换，逐轴乘法记为 `*`，四元数作用于向量记为 `rotate`：

```text
W.scale    = P.scale * L.scale
W.rotation = normalize(P.rotation * L.rotation)
W.position = P.position + rotate(P.rotation, P.scale * L.position)
```

世界矩阵仅在渲染消费端由最终 TRS 生成，不参与层级组合。非均匀父缩放与子旋转也遵循以上规则，不产生切变；因此结果不等于一般仿射矩阵连乘。骨架模型姿态接入实体世界变换、关节碰撞附件也使用同一规则。

重挂父级默认保持世界 TRS，先用逆父旋转与逐轴除法求相对 TRS，再一次提交父级和局部姿态。`keepWorld=false` 保持局部 TRS。沿用当前引擎有限正缩放的约束。

## 渲染与物理

Collider.shape 保存局部尺寸；PhysicsScene 中的刚体姿态和世界形状由 Transform 派生。盒碰撞逐轴缩放半尺寸，胶囊只支持均匀世界缩放。修改整个子树前会检查碰撞形状约束，不在部分写入后才发现不支持的缩放。角色高度请求仍使用世界尺寸，接受后的尺寸换回局部保存。

仅影响外观的偏移、尺寸或动画放到带 Render 的子节点。父节点持有碰撞和 gameplay，视觉子节点持有自身 Transform。整组缩放改父 Transform，仅缩放外观改视觉子节点 Transform。

```js
var body = Engine.create({name:'Crate', components:{
    transform:{position:[0,1,0]},
    collider:{shape:{type:'box',halfExtents:[.5,.5,.5]},blocking:true}
}});
var visual = Engine.create({name:'Crate Visual', components:{
    transform:{parent:Engine.entity(body).id,position:[0,.1,0],scale:[1.2,1,1.2]},
    render:{mesh:Engine.asset('/Engine/Meshes/Box'),material:Engine.asset('/Game/Materials/Crate')}
}});
Engine.scale(visual,{x:1.4,y:1,z:1.4});
```

## API 与迁移

- `Engine.component(id,'transform')`：局部 TRS 和父级，向量使用数组。
- `Engine.position(id)`：世界 `{x,y,z,yaw,rotation,scale}`。
- `Engine.transform(id,pose)` / `Engine.localTransform(id,pose)`：写世界／局部 TRS，命令向量使用 `{x,y,z}`；省略 scale 保留对应空间的当前缩放。
- `Engine.scale(id,scale)`：修改局部缩放，自动更新子树、渲染和物理。
- 原生 `setWorld` / `setLocal`：完整 TRS；物理运动使用 `setTransform(PhysicsPose)`，只更新世界位移与旋转，保留缩放。

Map v11 删除 Render 的三组变换字段。`tools/migrate_transform_trs.py` 将 v10 的外观变换迁到 Transform；涉及碰撞、灯光或已有子树时创建视觉子节点，保留原实体坐标系和持久 ID。仓库内 8 张 Map 已迁移。运行时拒绝旧字段；旧的 `renderScale`、`visualPose` API 已删除。HumanEditor 的 Details、缩放 gizmo 和撤销快照均编辑 Transform。
