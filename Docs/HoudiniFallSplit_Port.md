# AnimationFallSplit 移植：角色碎成布条

Houdini 场景 `D:\MyProject\HoudiniCloth\AnimetionFallSplit` 做的是「角色沿身体切成一圈圈条带，条带像布一样甩出去、旋转、再被拉回来」。本文整理它的分块逻辑与模拟逻辑，并记录移植到本插件 GPU 布料（[`GpuClothSim_Plan.md`](GpuClothSim_Plan.md)）上的做法、对应关系与差异。参数的含义写在源码声明的注释里（`CSClothBandSplit.h`、`CSClothSimComponent.h` 的 Anchors / Field 两节、`CSClothSplitActor.h`），本文不重复。

## 来源

| 文件 | Houdini 存盘 | 帧率 / 范围 | 说明 |
| --- | --- | --- | --- |
| `AnimationFallSplit19_uec_save.hip` | 2023-12-29，19.5.605 | 60 fps，1001–1175 | 最新；分析以它的网络为准 |
| `AnimationFallSplit19_uec.hip` | 2023-12-27 | 60 fps | 网络同上，76 MB（带锁定几何） |
| `AnimationFallSplit19.hip` | 2023-12-24 | 25 fps，1001–1280 | 绳的关键帧以它为准，见「时间线」 |
| `AnimationFallSplit.hip` 与 `backup/` | 2021-03，18.0 | — | 早期版本 |

- 输入就是本工程导出去的：`geo/Mage_GroundSlam_RM.FBX`（第三人称模板的砸地动画）与 `geo/UE5_Character.bgeo.sc`（UE5 小白人，cm、Z 朝上、A 姿态，4.6 万点）。
- 角色缓存 `geo/CharCache/*.bgeo.sc`、烟雾缓存都不在盘上，场景不能整条重新 cook；以下逻辑来自节点、参数与 VEX 的静态读取（hython，不求值——见「读取方法」）。
- 渲染 `render/AnimationFallSplit/CharVellumOFFix.1001–1080.exr` 正着放是条带旋着汇聚成人、最后变回实体角色；`retime` / `RetimeMesh` 节点说明模拟是正向「碎开」、渲染时倒放的。
- 场景里另有一条不模拟的变体（`CharSplit1` → `DefEndChar`，地面砸击动画上的同一套分块 + `attribvop1` 程序化形变）、pyro 烟雾、几个 popnet，以及把结果烘成顶点动画贴图的 `ropnet2/vertex_animation_textures1`（原先进 UE 的方式），均未移植。

## Houdini：分块（`Char` → `CharSplit`）

| 步骤 | 节点 | 做了什么 |
| --- | --- | --- |
| 冻结姿态 | `timeshift9` | 动画角色定在第 1 帧，切分在这一个姿态上做 |
| 重网格 | `remesh2` | 目标边长 0.01 m（1 cm 的均匀三角） |
| 起点 | `drawcurve3` → `ray1` → `deformbyinfluence3`（kalou_attribfromgeometry） | 手画在身上的一条笔画投到表面，1 cm 以内的点 `start = 1` |
| 测地距离 | `distancealonggeometry3` | 从 `start > 0.5` 的点出发，写 `dist`（m） |
| 条带号 | `attribvop2` | 见下面的公式 |
| 面取最小 | `attribpromote3/4` | `split` 提升到面，取三个角的最小值 |
| 切开 | `primitivesplit3`（splitpoints） | 相邻面 `split` 不同处拆点 |
| 切口平滑 | `divide4` → `fuse3` → `convertline2` → `attribblur3` → `ray2` → `attribwrangle18` | 每块的轮廓转成线、模糊 9 次（步长 0.643）、投回表面，4.4 cm 以内的点往平滑后的曲线上拉 |
| 再切 | `primitivesplit4` | 平滑后再按 `split` 拆一次 |
| 块与参数 | `connectivity22`、`kalou_createcurveuv2` | 块号 `class`、沿条带的参数 `curvev` |
| 绑回动画 | `attribwrangle18` 的 `xyzdist` + `attribinterpolate3` | 每点记下在冻结角色上的 `sourceprim` / `sourceprimuv`，按它插回动画角色 |

`attribvop2` 与 `attribwrangle18` 的核心（VOP 网络按连线写成 VEX）：

```c
// attribvop2：aanoise(xnoise, freq 39, amp 5) 加上放大的测地距离，0.1 倍取整 —— 每 10/222 m ≈ 4.5 cm 一条
float insource = aanoise(@P * 39) * 5 + @dist * 222;
i@split = floor(insource * 0.1);

// attribwrangle18：切口附近的点往平滑后的切口曲线上拉（input 1 = 原切口线，input 2 = 平滑并投回表面的切口线）
float dist = xyzdist(1, @P, prim, uv, chf("maxdist"));           // maxdist 0.044
@P = lerp(@P, primuv(2, "P", prim, uv), chramp("deform", fit(dist, 0, maxdist, 0, 1) / maxdist));
```

## Houdini：模拟（`EndChar` → `vellumsolver3`）

`EndChar` 合并两份几何：条带（组 `cloth`）与只有点的一份（组 `staticpt`）。每个布点被 stitch 到同位置的 `staticpt` 上，`staticpt` 每帧贴回动画角色——条带就这样挂在会动的身体上。

| 部分 | 节点 | 参数 |
| --- | --- | --- |
| 初速度 | `trail5` + `attribwrangle6` | 动画速度 × 5，`curvev < 0.5` 的那一半取反 |
| 布 | `vellumcloth4` | 拉伸 0.0003e10（拉伸组改名 `clothstretch`），压缩 1e3，弯曲 0.0003e-4（角度型），密度 0.1，厚度 0.01 |
| 绳 | `vellumstitch1` | `cloth` 的点 → `staticpt` 最近点，拉伸组用默认名 `stretch` |
| 绳的随机长度 | `attribrandomize1` | 每条约束 `restlengthnoise` ∈ [1.9, 2.0] m，种子 8278 |
| 绳的高度 | `KaLou_faceattrib1` + `FitValue5` | `high = @P.y`，再按全体约束归一化到 [0, 1] |
| 解算器 | `vellumsolver3` | 1 substep、100 次约束迭代（默认），重力 0，地面 y = 0，风 0、风阻 0.1，从 1004 帧起算 |
| 改绳 | `forces/sopsolver1/attribwrangle2`（每步，组 `stretch` = 只有绳） | 见下 |
| 锚点贴回动画 | `forces/sopsolver2/attribwrangle3` | `staticpt`：`i@stopped = 1`，`@P` = `EndChar` 的当前位置 |
| 速度场 | `forces/popadvectbyvolumes1` | `BackVel`（体积 `v`）当风用：velscale 0.98、velblend 0.844、airresist 1 |
| 噪声风 | `forces/popwind1` | amp 2.05、swirlsize 4.67、airresist 0.17 |

```c
// sopsolver1/attribwrangle2，作用在绳上；restlengthchange / stiffnesschange 是动画通道（见「时间线」）
f@restlength = lerp(f@restlengthorig, f@restlengthnoise, clamp(f@high + 1 - chf('restlengthchange'), 0, 1));
f@stiffness  = lerp(1000, 0, chf('stiffnesschange'));
```

绳原本两端重合（`restlengthorig` = 0）。刚度从 0 绷到 1000 时，绳长约 2 m 的绳把条带推到离身体约 2 m 的一层壳上（渲染里那圈大旋涡）；`restlengthchange` 走到 1 时绳长 = 2 m × `high`，脚下的条带被收回身上、头顶的仍在外面。

`BackVel` 由三部分 rasterize 成 2 cm 体素的速度场：

- 表面旋流：第二条笔画（`drawcurve4`）做起点，`normalalonegeo1` 算出沿表面离开起点的方向 `up`，`v = cross(N, up)`。
- 流线：表面上散点（每 m² 约 33 个），每点一条沿法线、弯曲 0.262 的线，带着该点的 `v`，沿线按 `pscale` 衰减；与表面点一起 `v *= 5`，再在 30³ 的包围盒格点上 `attribtransfer`。
- 烟雾：条带驱动的 pyro（`pyrosolver3`，缓存缺失）的速度采样，`v *= 2`。

## 时间线：25 → 60 fps 把绳挪出了镜头

| 通道 | `AnimationFallSplit19.hip`（25 fps） | 后两版（60 fps） |
| --- | --- | --- |
| `stiffnesschange` | 1001 帧 1 → 1010 帧 1 → 1049 帧 0 | 2401 → 2422.6 → 2516.2 |
| `restlengthchange` | 1004 帧 0 → 1037 帧 0.857 → 1055 帧 1 | 2408.2 → 2487.4 → 2530.6 |

后两版每个关键帧都满足 `新帧 = (旧帧 − 1) × 2.4 + 1`：改帧率时关键帧按秒保留了，镜头范围却还是 1001–1175，于是绳在整个镜头里刚度为 0、完全不起作用。移植取 19.hip 的时间，换成以模拟起点（1004 帧）为 0 秒的秒数，作 `AnchorStiffnessFade` / `AnchorReelIn` 的默认曲线。

## UE 移植

```text
ACSClothSplitActor（编辑器视口里也跑）
  Body：USkeletalMeshComponent 播动画，藏起来但每帧求骨骼
  重建：Body 在 StartTime 的蒙皮快照（CPU：ComputeSkinnedPositions / ComputeSkinnedTangentBasis）
        → CSClothBandSplit::Split → UCSMesh → UCSClothSimComponent（SetVertexBinding + SetupFromMeshSync）
  每帧：Body 的蒙皮位置 → SetAnchorSourcePositions → 每粒子锚点（CPU 收集、上传）
  GPU，每 substep：Predict（放开前跟着锚点；放开那一下给初速度；之后往场速度收敛）
                  → SolveAnchors（绳）→ SolveColor（布）→ Collide（地面）
```

| Houdini | UE | 程度 |
| --- | --- | --- |
| `remesh2`（1 cm） | 不重网格，直接用 LOD 0（Quinn 边长约 1.3 cm） | 近似 |
| 笔画 + 1 cm 捕获 | `Split.SeedPoints` + `SeedRadius`（actor 局部；空 = 头顶） | 手工：笔画换成点 |
| `distancealonggeometry` | 网格边 + 跨相邻三角摊平的捷径上做 Dijkstra（规则网格误差从约 41% 降到几个百分点） | 近似 |
| `attribvop2` 的条带号 | 同一公式：`floor((dist + 2.25 cm × noise) / 4.5 cm)`，噪声周期 2.6 cm | 近似：Perlin 代替 aanoise |
| `attribpromote` min + `splitpoints` | 面取最小 + 焊接键（切口两侧不焊回） | 一致 |
| 切口平滑 | 切口曲线拉普拉斯 9 次 × 0.643，4.4 cm 以内的点跟着平移（线性衰减），开放边界不动 | 近似：平移代替往曲线上拉 |
| `createcurveuv` 的 `curvev` | 每块两次最远点扫描，`u < 0.5` 取反 | 近似 |
| `sourceprim` / `sourceprimuv` 绑定 | 锚点 = 源渲染顶点；被平滑挪过的点取原网格上最近三角的重心坐标 | 一致 |
| `vellumcloth4` | 拉伸柔度 1/3e6、压缩 1e-3、密度 0.1；弯曲 1（跨边距离） | 近似：弯曲模型不同，见下 |
| `vellumstitch1` + `staticpt` | 每粒子一条到锚点的双边距离约束 | 一致 |
| `sopsolver1` 两行 | `AnchorReelIn`、`AnchorStiffnessFade` 两条曲线，同一公式；`AnchorRestLengthRange` 190–200 cm | 一致（默认关键帧取 19.hip） |
| `sopsolver2` 锚点贴回动画 | 每帧骨骼网格的 CPU 蒙皮位置 | 一致 |
| trail × 5、`curvev` 半段取反 | `ReleaseVelocityScale` = 5 × 正负号；放开时刻 `ReleaseDelay` | 一致 |
| `BackVel` | 每粒子的表面旋流方向 `cross(N, ∇dist)` × `SwirlSpeed`（500 cm/s），`FieldDrag` 1/s 收敛 | 近似：只有表面项，没有流线外延与烟雾速度；起点复用分块的起点 |
| `popwind1` | `WindTurbulence` 205、`WindTurbulenceScale` 467、两向阻尼 0.17 | 近似：噪声函数不同 |
| 重力 0、地面、风阻 0.1 | `Gravity` 0、跟随 actor 的地面平面、`Damping` 0.1 | 一致 |
| 1 substep × 100 次迭代 | 16 substep × 1 次迭代，绳排在布之前解 | 不同解法，见下 |
| `retime` 倒放、VAT 导出、pyro、`DefEndChar` 变体 | — | 未移植 |

两处有意的偏离：

- **解算顺序。** 粒子质量约 1e-5 kg，刚度 1000 的绳与 3e6 的布在 XPBD 里都近乎硬约束。Vellum 靠每步 100 次迭代让刚 3000 倍的布赢；这里每个 substep 只解一次，谁最后解谁说了算。绳排在最后时，条带每步都被硬拽到绳长的壳上、拉成锯齿（2026-10-28 实测），所以 `SolveAnchors` 排在布的约束之前，布最后保持边长，绳像一股拽整条带的力。
- **弯曲。** Houdini 的弯曲刚度 3e-8 几乎为零，但 100 次迭代把条带磨平了；这里的弯曲是跨边距离、每步一次，柔度 100 时条带在三角尺度上皱成锯齿，`ACSClothSplitActor` 默认取 1、substep 取 16。

## 用法

1. 放一个 `ACSClothSplitActor`，在 `Body` 上设骨骼网格，`Animation` 设要播的动画（单节点模式）。
2. 布料组件的 `ReleaseDelay` 定放开时刻；`Split` 调条带（`SeedPoints` 为空时从头顶切）。
3. 编辑器视口里自动播放；`LoopPeriod` > 0 时到点 `Restart`。改任何属性都会在下一帧重建。
4. 示例关卡 `/Game/ClothSplitDemo/L_FallSplitDemo` 由 [`Scripts/MakeFallSplitDemo.py`](../Scripts/MakeFallSplitDemo.py) 生成：Quinn + `Mage_GroundSlam_RM`，放开时刻取骨盆下落最快的 1.02 s，条带材质仿 Houdini 渲染。资产属于第三人称模板，所以关卡在工程里、不在插件里。

⚠️ 蒙皮位置在 CPU 上算：编辑器里骨骼网格的 CPU 数据常在；cook 出去的包要在骨骼网格 LOD 0 上勾 Allow CPU Access，否则重建失败并写日志。

## 验证

- `PCGPlugins.ComputeShaderGenerator.ClothSplit.*` 4 条（`ClothSim.*` 11 条照旧全过）：
  - `BandSplit`：100 cm 见方、2 cm 一格的竖网格从顶边中点切，27 条带 / 30 块；每个三角只在一条带里；不看焊接键焊回 51² 个点、看键时切口分开；旋流在面内且垂直于半径（沿等距线）；平滑过的点按锚点插回原平面误差 < 1e-3 cm。
  - `SetupKeepsCuts`：setup 后粒子 = (位置, 条带) 的组数，没有一条拉伸 / 弯曲约束跨过切口。
  - `AnchorsFollowAndRelease`：放开前跟着宿主平移误差 < 0.01 cm；满刚度、绳长 20 cm 时整块布被推开 20 ± 2 cm；收绳后回到锚点 < 1 cm。
  - `SplitActor`：引擎自带的 UE4 小白人（`TutorialTPP`），44 条带 / 229 块 / 4867 粒子；放开前粒子包围盒与参考姿态顶点的包围盒差 < 1.5 cm（切口平滑把指尖往里收约 1.2 cm）；放开 1 s 后有限、不停摆、条带离开身体。
- 示例关卡（Quinn）：45 条带 / 110 块 / 50975 粒子，显存 15 MiB。真编辑器里（不进 PIE）从头跑一遍，0.56 s 是在动的角色，放开后 0.5 s 条带成环旋开，2.5 s 时脚下的条带被收回、头顶的仍在外面，全程不停摆。

## 读取方法

`hython` 读 `.hip` 时只能读原文，不能求值：`parm.eval()`、`parm.keyframes()` 都会求表达式，表达式引用解算器输出时整个 Vellum 模拟会被 cook（2026-10-28 实测 22 GB 内存、十分钟不出来）。先 `parm.expression()` 拿原文，只对 `bezier()` 这类纯动画曲线读关键帧。`FitValue` 与 `KaLou_faceattrib` 在 hip 里是空壳嵌入定义，逻辑取自 kalou 库快照与 houdini-node-translator 技能里的重建记录。

## 未做与开放问题

- `BackVel` 只移植了表面旋流项；要更像原效果，可以把 Houdini 里的速度场烘成 UE 的 Vector Field，GPU 上按位置采样。
- 每帧在 CPU 上蒙皮全部 LOD 0 顶点（Quinn 约 4.6 万），耗时没量；要上运行时应改用 GPU 蒙皮缓存或只蒙锚点用到的顶点。
- 条带只在 `StartTime` 的姿态上切一次；Houdini 也是在冻结的第 1 帧上切。
- 没有自碰撞（Houdini 的 Vellum 同样关着自碰撞）；运动矢量、光追 BLAS 的限制同 [`GpuClothSim_Plan.md`](GpuClothSim_Plan.md)。
- 倒放（条带汇聚成人）在实时里不能直接倒着解；用曲线让绳从长收到 0 可以做出「汇聚」，没做成默认。
