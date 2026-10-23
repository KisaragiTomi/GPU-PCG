# TG 灌木材质移植（无自发光）

2026-09-30 当前状态：脚本及离屏材质验证完成，生产场景应用待完成。
主编辑器在第一轮失败编译后停在 `Retry compilation?` 对话框；失败原因（RGB 被当作 RGBA）
已修正，隔离目录 `/Game/__TGBushQA_20260930` 的新一轮编译正常。
需关闭主编辑器提示后重跑脚本，才能把下文的生产引用切换落地。

目标是建筑周边实际使用的 `bush_body` 和同变换叠加的 `bush_flowers`。
`bush` 是另一份带 `prim_center` 的树冠卡片资产，不能与 `bush_body` 混为一谈。
实现脚本：[TinyGladeMakeBushMaterial.py](../../Scripts/TinyGladeMakeBushMaterial.py)。

## 原版证据

源码依据是 `D:/MyProject/Tiny Glade/tmp/shaders/` 中
`_nani_bush.raster.hlsl_b903e43ffb3da915.{vs,ps}_main.glsl`、
同名 `f0adff76827e73ff` 阴影变体，以及 `_nani_flower.raster` 的对应着色器。
几何数据来自 `assets/meshes/bush_body.json`、`bush_flowers.json`；图像来自
`extracted/textures/bush_alpha.png` 和三个季节色板。

`bush_body` 有 400 顶点、200 三角形（100 张卡）。它的 UV0 只覆盖
`[0.8, 0.995695]²` 的图集区域。原版 VS 做 `uv.y = 1 - uv.y`，项目导入网格
没有预先翻转：实测第一个顶点仍是 `(0.995695, 0.8)`。因此材质必须翻 V。
PS 只读取 `bush_alpha.A`，小于 0.5 时裁剪。RGB 不是树冠的 height/AO 通道，
也不参与本体颜色；不能照搬 `canopy_alpha.R/G/B` 的语义。

颜色用世界坐标采色板，原公式是 `(world.xz + uv * 0.2) * 0.1`。
本工程坐标转换为 `(TG.x, TG.z, TG.y) * 100`，所以 UE 公式为
`world.xy / 1000 + flippedUV * 0.02`。底部颜色乘
`lerp(0.8, 1, smoothstep(20, 200, sourceLocalZ))`，高度使用缩放前的局部厘米。
冬季主题在朝上的法线上叠加灰白霜色：
`lerp(color, 0.56, smoothstep(0.2, 1, saturate(normal.z)) * 0.4)`。

原版还有 `abs(dot(view, transformedNormal)) > 0.5` 的裁剪。
这里的 transformedNormal 未归一化，保留了实例缩放，插值长度也参与门控；
不能随手替换成单位像素法线。移植从 PreSkinnedNormal 在顶点阶段经过
`TransformLocalVectorToWorld`，用 VertexInterpolator 传入像素阶段。
着色法线再归一化，并乘 TwoSidedSign 抵消 UE 对背面法线的翻转，保留原件的体积法线。
导入器已把原始法线归一化（源长度约 0.984–1.191），因此此处阈值不能与原版逐位相同。

风使用实例原点上的二维值噪声，四个格点的整型 hash 和三次插值来自原版。
沿世界 X 的偏移，换算为厘米后为：

```hlsl
noiseUV = instanceOrigin.xy * 0.001 + float2(time * 1.2, 0);
bend = sourceLocalZ * 0.005;
WPO.x = -bend * bend * (sin(noise * 3.14 - 1.57) + 0.3) * 7.35;
```

花沿用同一风场，其 `wind_bend` 全部 360 个顶点满足 `sourceLocalZ * 0.006`，
与原 JSON 的最大误差为 `9.54e-8`，无需为该属性重导网格。
花的夏、秋颜色分别为源属性 `(0.551, 0.690, 0.588)`、`(0.529, 0.263, 0.084)`；
按实例原点 hash 做 `pow(color, 1 + 1.5 * random)`，秋冬随机幅度减半。
冬季还包括原版的底色混白与朝上霜色。

## UE 落点

- 本体母材质 `M_TinyGladeBush`：Masked / Two Sided Foliage，世界空间体积法线。
- 已有 `MIA_bush` 改用新母材质，保留 `Tex` / `Alpha` 参数契约；场景无需换引用。
- 新增 `MI_TinyGladeBush_Summer/Autumn/Winter`，供单独选择季节。
- 花的母材质 `M_TinyGladeBushFlowers`：Opaque / Two Sided Foliage，同样无自发光。
- `bush_flowers` 的默认槽换成 `MI_TinyGladeBushFlowers_Summer`，另提供秋冬实例。

参照现有 `M_TinyGladeLeaves` 使用 Roughness=0.92、Specular=0.12、
Transmission=0.35。颜色接 BaseColor，透射接 SubsurfaceColor，EmissiveColor
直接接 `(0,0,0)` 常量。树叶的 `SoftFill` 路径没有复制。开启 Nanite 与实例化用途，
WPO 位移上限为 40 cm；默认本体理论最大风偏移约 23 cm。

两套材质均保留原始网格形状与法线，不引入树冠 PDO；原版灌木没有该机制。
UE 的实际太阳、天光和阴影负责照明。TG 的 G-buffer 编码与专用延迟光照不在材质图里
伪造，因而不是原版最终像素的逐位复制。交互推草贴图 `grass_object_push` 尚未接入，
本次只复刻独立风场。阴影由 UE 的阴影 pass 执行同一遮罩。

## 验证与回退

检查记录、材质文本导出和渲染结果放在 `Saved/TGBushMaterial_20260930/`。
修改前的 `MIA_bush` 与花网格备份在其 `backup/`，校验清单为 `backup-manifest.json`。
脚本重复执行会更新带 `TG Bush |` 标记的节点，不删除编辑器持有的节点；不会保存关卡。
独立离屏验证可调用 `build(install=False)` 并重定向 `MAT` 到临时包，避免改动生产引用。

离屏验证的 `preview/validation.json` 确认两套材质的 EmissiveColor 都连接零常量、
不存在 SoftFill 参数，三个季节实例都绑定正确色板。母材质分别 38 / 27 个节点，
重复生成节点数量不增长。第三轮日志 `Saved/Logs/TGBushMaterial_QA3.log` 未发现
本次材质编译失败，顶点/像素着色器均有有效指令统计。

固定曝光和固定机位下，关灯截图 RGB 最大值 1/255、均值 0.095/255；
切换风时间后 43,783 个像素的最大通道变化超过 10/255，切换光照方向后为 282,225 个。
这些是功能性渲染验证，主关卡应用后的最终观感验收尚待完成。

UE 5.7.4 的 `SetMaterialInstanceTextureParameterValue` 和 Scalar 对应函数在实际执行赋值后
仍返回 false（本地 `MaterialEditingLibrary.cpp` 的 bResult 从未置 true）；脚本使用回读值验证，
不能把返回值当成赋值失败。
