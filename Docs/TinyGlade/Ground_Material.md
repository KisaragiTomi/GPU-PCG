# 地面三层材质：草地、深色土与浅色土路

2026-09-12 根据用户参考图和 TG 反编译修正 `M_TinyGladeGround`。当前 `L_HouseGroundDemo` 的 `Ground_Demo` 通过 `MI_TinyGladeGround` 使用此材质。原先的两图线性混合缺少深色土层，也没有高度噪声控制的露土斑块。

## 用户参考图

下面是用户提供的原图，原样复制到文档目录。判读目标是草土接壤处的深色土带，以及浅土内部零散露出的深色碎石土；蓝色夜间光照不作为白天地表底色的标定。

![TG 原版地面：草土边缘与噪声土斑](img/tiny-glade-ref-ground-three-layers.png)

## 反编译确认

原始文件目录为 `D:/MyProject/Tiny Glade/tmp/shaders/`。

| 证据 | 结论 |
| --- | --- |
| `_terrain_editing_floor.raster.hlsl_b903e43ffb3da915.ps_main.glsl` 第 145–149、422–427 行 | 地面采样 `dirt_1`、`dirt_2`、`grass` 三张颜色图，另有 `dirthpath_heightmap` 高度混合图 |
| 同文件第 444–464 行 | 土路遮罩与深土遮罩各自经过平方根、0.6 系数和高度竞争，得到两个混合权重 |
| 同文件第 520 行 | 先在草地上混深土，再覆盖浅土；两层分别使用 `smoothstep(0.2, 0.65, …)` 与 `smoothstep(0.6, 1.0, …)` |
| `_paths_path_mask_visual.cs.hlsl_cs_main_0.cs_main.glsl` 末尾 | `dirt_mask = max(土路, 石路1, 石路2, 石路3)`，深土覆盖域可包含多种道路 |
| 同 CS 第 51–182 行及对应石路段 | 两层值噪声扭曲模糊路径遮罩的采样位置，幅度受坡度限制，产生不规则路径轮廓 |
| Floor PS 第 469–513 行 | 主题 ID 4 另有减去最高 0.4 噪声的分支；它只属于该主题，不能当作所有主题的通用规则 |

高度噪声图的低谷会让浅土层的权重先退去，露出下面的深色土；在路缘，由于深土的混合区间更早开始，也会先看到深土。完全没有路径遮罩时，两层土的权重均为零。

提取清单 `D:/MyProject/Tiny Glade/extract/texture_index.json` 将 `dirtpath_1`、`dirtpath_2`、`dirtpath_grass`、`dirtpath_heightmap` **全部记录为 `BC7_SRGB`**。本轮保留四张纹理的 sRGB 解码，其中高度图也按原版格式读取。不能因为文件名含 `heightmap` 就直接改成线性采样。

## 项目实现

[TinyGladeMakeGroundMaterial.py](../../Scripts/TinyGladeMakeGroundMaterial.py) 原位重建材质图，保留材质资产身份和已有 `Grass`、`Dirt` 实例覆盖。实例新增 `DarkDirt` 与 `GroundBlendHeight`。材质按以下顺序采样和混合：

| 用途 | 提取纹理 | UV0 倍率 | 默认世界平铺周期 |
| --- | --- | --- | --- |
| 草地 | `dirtpath_grass` | 0.8 | 625 cm |
| 深色土 | `dirtpath_2` | 1.2 | 约 417 cm |
| 浅色土路 | `dirtpath_1` | 0.5 | 1000 cm |
| 高度噪声 | `dirtpath_heightmap` | 0.4 | 1250 cm |

地面 UV0 来自 `CSGroundActor.cpp::BuildSnapshotFromMirror` 的局部 XY / `UVWorldPeriod`；上表使用当前默认 500 cm。四层倍率对应原版米坐标乘 0.2 后的采样倍率。改变 `UVWorldPeriod` 会统一改变这些周期。

```hlsl
// 每种遮罩分别进行高度竞争；当前项目两者均来自道路顶点色 R。
float t = saturate(sqrt(mask) * 0.6);
float a = 0.15 * (1 - t);
float b = height * t;
float cutoff = max(a, b) - 0.1;
float weight = max(b - cutoff, 0) / (max(a - cutoff, 0) + max(b - cutoff, 0));

// soilWeight 与 roadWeight 分别来自对应遮罩的上式。
float3 color = lerp(grass, darkDirt * 1.1, smoothstep(0.2, 0.65, soilWeight));
color = lerp(color, lightDirt * 1.1, smoothstep(0.6, 1.0, roadWeight));
```

当前项目只用顶点色 R 表达土路，因此两种遮罩共享 R。此次落地的是三层着色、原版高度竞争和纹理采样倍率；原版单独的模糊／噪声扭曲路径栅格、石路遮罩合并、主题 ID 4 分支没有在本轮新增。地面仍有现有 50 cm 顶点格造成的轮廓折线，诊断图中可以看到。草地调色保留项目现有 RGB 纹理分支，光照继续使用 UE；上述范围不等于整个 TG 地面渲染管线已完全移植。（模糊与噪声扭曲已于 2026-09-29 补上，见下一节。）

## 2026-09-29：地面遮罩、深土跟草、采样扰动

三轮用户反馈，同一天落地。材质不再直接读顶点色 R，而是读地面 actor 喂进来的**地面遮罩**贴图 `GroundMask`（RG32F，一 texel 对一地面格点）；草 / 花 / 灌木读的也是这张，土与草构造上同源。

| 反馈 | 做法 |
| --- | --- |
| "道路的 mask 变化太剧烈，结果需要做平滑延申"；"地形 mask 决定草" | R = 顶点色 R 在 GPU 上糊开、×2、与原值取 max（`CSGroundRoadField.usf`，σ = `GroundMaskExtendDistance` / 2）：路缘以内恒 1，路缘以外平滑衰减 |
| "草长在了土上" | 草闸门 = `smoothstep(草.MaskStart, 草.MaskEnd, R)`（`GroundMaskSoilStart/End`，actor 按草的拒绝区间喂），两层土都乘它：土的概率 ≤ 不长草的概率 |
| "深色图应该再向中间蔓延一些" | G = 只糊、不 ×2、不取 max 的场（画出来的路缘处 0.5）= TG 的 path_mask_blurred。中间一版按 G 取硬阈值（PathStart/End）让浅土止于路缘，结果是一条均匀的深色带，已被下两行取代 |
| "mask 还是需要有一些扰动的"；"交界过于生硬" | 照 TG 的路径遮罩噪声扭曲（上表第 21 行）：采样前把采样点挪 `fbm × GroundMaskWarpStrength`，见下 |
| TG 道路截图："还是需要一些 noise 贴图辅助表现"；"直接看 tg 的反编译" | 照 TG floor PS 第 416–457、520 行：深土与浅土对**同一个** `W = HeightWeight(G, dirtpath_heightmap)` 分别取 `smoothstep(0.2, 0.65, W)` / `smoothstep(0.6, 1.0, W)` —— 哪里深哪里浅由高度噪声图决定（路边深浅交错、路面低谷露深色土斑），再乘草闸门 |

材质参数 `GroundMaskFloorSharpness`（默认 1 = TG 原样）把 G 以 0.5 为中心拉陡后再做高度竞争：G 的模糊宽度跟着 `GroundMaskExtendDistance`（草要的"平滑延申"），比 TG 的宽，边会偏软。用户 2026-09-29 比过 1 / 2 / 3（2、3 边更清楚，但路变窄、草与土之间露出一圈短草地表），选定 1。

**采样扰动**（`Shaders/Private/CSGroundMaskWarp.ush`）照抄 TG `_paths_path_mask_visual` 第 48–164 行：输入 `P × 0.75/m + 7`，两层 value noise（TG 同款哈希），振幅 0.5 起每层 ×0.55，频率每层 ×1.9 并旋转 36.87°，位移 = fbm × 0.6 m（`GroundMaskWarpStrength` 默认 60 cm、`GroundMaskWarpWavelength` 默认 133.3 cm）。实测位移每个分量 σ ≈ 15 cm、最大约 ±46 cm，合位移中位 18 cm、p99 41 cm。

- 材质（Custom 节点经 IncludeFilePaths 引这个头）与草 / 花 / 灌木散布（`CSGroundCover.usf` 的 `CSCover_SampleMask`）调**同一个函数、同一对参数**，输入都是地面局部坐标（材质 `UV0 × UVWorldPeriod`，散布 `P − 地面原点`）。只挪一边会让土边与草线各抖各的。
- 遮罩贴图本身不带扰动：贴图是 50 cm 一格，在生成时挪的话 1.33 m 波长的抖动会被双线性抹平；逐像素 / 逐株采样时挪才保得住。
- 与 TG 的差别：TG 把同一个标量加到 x、z 上（位移恒沿对角线，对角走向的路缘不抖），这里两个分量各取一份；TG 按地形坡度把位移关小，这里没搬（材质与散布拿到的坡度口径不同，各算一份会在坡上错开）。
- 测试 `Ground.GroundMaskDrivesCover` ⑤：扰动开后遮罩贴图逐位不变、路缘草线沿路游走、扰动够不着的地方逐位不变。

## 截图验证

当前地面与地被效果使用 2026-09-29 的最终场景捕获，包含路缘扰动和 Nanite 地被。专项验证见 [地被文档](Ground_Cover.md)。

![当前地面、道路与草花地被](img/cover-nanite-20260929-usercam.png)

以下统计记录于 2026-09-12 的三层地面材质修复验收：

材质编译后统计为 VS 158、PS 228 条指令，4 次像素纹理采样；材质和实例已保存。验证记录位于项目 `Saved/TGGroundAudit/validation.json`，包含纹理绑定、着色器统计以及修改前后道路采样对照。本轮未改 C++，无需重编引擎模块。

修改前快照保留在项目 `.ue-safe-modify-backup/tg-ground-20260912/`。后续重建运行同一脚本即可；脚本不删除材质资产、不切换地图，也不覆盖已有道路笔刷数据。
