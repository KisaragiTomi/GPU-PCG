# TG 树冠着色：叶卡片的变形、打包与专用光照

Tiny Glade 的"树叶"不是一个 shader，是一条从 prefab 加载、实例变形、紧凑 G-buffer 到**树冠专用**延迟光照的六段链路。
本篇只写着色侧；几何侧（卡片编码、`recalculate_canopy_quad_attribs` 的四个属性）由 `build_tree.py` / `Tree.hip` owner，本篇不复述。

证据一律来自 `D:/MyProject/Tiny Glade/tmp/shaders/` 的反编译 GLSL、`tmp/pdb_symbols.txt` 与 `extracted/`
的 mesh／贴图实测。推断项在文中逐条标注，不与实证混排。

链路总图：[`Tree_CanopyShading_Pipeline.svg`](Tree_CanopyShading_Pipeline.svg)

## 结论速查

按"值不值得抄"排列。末栏是本项目的落地状态。

| # | 机制 | 一句话 | 本项目 |
| --- | --- | --- | --- |
| 1 | **深度浮雕** | 平面卡片在深度缓冲里变成 1.6 m 厚的叶球，两树相交得到弯交线 | **已落地**（PDO，符号与 TG 相反） |
| 2 | **逆光旁路** | 相机与太阳异侧时，`N·L` 与厚度遮蔽两项被同时旁路，只剩边缘项 | 近似（TwoSidedFoliage 透射，未复刻旁路） |
| 3 | **随机 UV 采色板** | 用一张 512² 色板当随机取色器，零叶片贴图 | **已落地**（逐卡 hash + 随机 UV） |
| 4 | **缩放淡出** | 快被 cone-cull 掉的叶簇先缩成一点再消失，无 pop | **已落地**（WPO 收向卡心） |
| 5 | **height 一通道三用** | 同一个 8 位通道跑深度浮雕、花的高度合成、间接光遮蔽 | 部分（浮雕 + AO；花未做） |
| 6 | **镜头挖洞** | 相机贴近树时屏幕中心一圈叶子按椭圆锥 + 蓝噪声抖动剔除 | 未做 |
| 7 | **大时间值防掉精度** | `sin((T·f mod 2π) + frac·f + φ)`，`game_time_sec` 是 uint 单独存 | 未做，任何长跑动画都该抄 |
| 8 | **实心阴影 + 法线外扩** | 阴影 pass PS 为空、顶点沿法线外扩 1 单位补偿浮雕 | 未做 |

## 链路与文件

| 文件（`tmp/shaders/`） | 角色 |
| --- | --- |
| `_clearing_tree_canopy.raster` · `b903e43ffb3da915` | 树冠叶卡片主 pass，VS 变形 + PS 写紧凑 G-buffer |
| `_clearing_tree_canopy.raster` · `f0adff76827e73ff` | 同源**阴影变体**，PS 为空 |
| `_deferred_canopy_light_sun_sky.raster` | **树冠专用**延迟光照，全屏三角 |
| `_clearing_tree_trunk.raster` | 树干，与冠共用 prefab、分属另一 slot |
| `_clearing_instanced_background_tree.raster` | 背景树，前向，不进 G-buffer |
| `_ivy_instanced_ivy_leaf.raster` / `_nani_bush.raster` | 藤蔓叶 / 灌木，各自另一套，见「其它叶类」 |
| `_leaf_particles.cs` | 落叶粒子模拟（`leaf_particle_draw` 是另一条 draw pass） |

整个场景走**统一延迟**：`tmp/shaders/` 里有 **44 个 raster shader 写同一个 `uvec2` G-buffer**
（墙、屋顶、地形、楼梯、花、草、树冠、树干、灌木、藤蔓、旗帜、烟、水……），
但**只有两个 pass 读它** —— `_deferred_canopy_light_sun_sky` 与 `_deferred_grass_light_sun_sky`。
⚠️ 两者对 G-buffer 高 16 位的解读**完全不同**（见「专用延迟光照」节的对照表），
因此必然按区域分开跑，不是一个跑完再跑另一个。

对应 PDB 符号：

```text
country_core::systems::render_loop::draw_passes::draw_shaders::clearing_shaders::tree_canopy
country_core::startup::load_prefabs::recalculate_canopy_quad_attribs::Quad   # 加载时重算卡片属性
country_core::startup::load_prefabs::filter_mesh_by_vertex_color             # 按顶点色拆 canopy / trunk
country_core::resources::history::tree_edit::TreeCanopyMaxCullDistance       # 挖洞的远端距离，玩家可调
```

## 资产侧契约

### mesh

`extracted/meshes/central_tree.glb` 实测（`mesh_sections.json` 记为 `slots: ["canopy","trunk"]`）：

| prim | tris | 顶点 | `COLOR_0` |
| --- | --- | --- | --- |
| 0 · canopy | 488 | 976 | `.xy ∈ {(0,0),(0,1),(1,0),(1,1)}` 角码兼 UV；`.z = 0` |
| 1 · trunk | 516 | 323 | `.xy = (0,0)`；`.z = 1` |

488 tris = **244 张四边形叶卡片**，每卡 4 个独立顶点。`.z` 就是 `filter_mesh_by_vertex_color` 的判据。
`prim_center` / `prim_normals` / `appear_pos` / `age` 是 TG 私有顶点属性，glTF 导出时丢失，由
`recalculate_canopy_quad_attribs` 在加载时重算 —— 与 `build_tree.py` 的产出口径一致。

### 逐实例数据

`PlacementData` SSBO（`set0/b15`），经 `instance_indirection_buffer`（`b16`）间接索引：

```glsl
struct PlacementData {
    vec3  position;        // 世界位置
    float scale;           // 均匀缩放
    float rotation_angle;  // 绕 Y，单位为度，VS 里 radians()
    int   id;              // 树实例 id，PS 的逐树取色种子
    uint  custom0;         // uintBitsToFloat 读成 float：view-aligned 卡片的附加半径
    uint  custom1;         // 树冠 VS 未用
    uint  anim_t_packed;   // 低 16 位 half = 生长 t；高 16 位 half = 挖洞强度
    vec2  bend_vector;     // 整树弯曲，按高度加权
    int   pad0;            // 树种 / 变体 id，PS 里作 C9 用
};
```

⚠️ `custom0` 的实际量级只能抓帧确定：mesh 的四角本来就是展开的，billboard 偏移是**叠加**上去的，
`custom0` 决定它是主导还是微调。静态文件里读不出来。

### 贴图

`extracted/textures/`，通道语义由 PS 用法 + 逐通道统计共同确定：

| binding | 文件 | 尺寸 | 通道 |
| --- | --- | --- | --- |
| `canopy_alpha` | `canopy_alpha.png` | 256² | `R` 剪影 alpha（`< 0.2` discard）· `G` height · `B` AO · `A` 恒 255 未用 |
| `canopy_flowers_color` | 同名 | 256² | 花色 |
| `canopy_flowers_depth` | 同名 | 256², **L8** | 花的"高度"，⚠️ 实测最大只到 `54`（0.212） |
| `canopy_color` | `autumn_` / `winter_` / `flowery_canopy.png` | 512² | **色板，不是叶片贴图** |
| `blue_noise_texture` | — | 256² | 挖洞边缘抖动 |

`canopy_alpha` 逐通道统计：`R` 0–255 均值 123.2、`G` 15–255 均值 77.2、`B` 0–255 均值 30.1。
`B` 均值很低说明树冠默认带很强的 AO。

## 主 pass VS

八段串行，任何一段判否都写 `gl_Position = NaN` 剔掉整个顶点。

### ① 生长动画

```glsl
float t = clamp(unpackHalf2x16(anim_t_packed & 0xFFFFu).x, 0.0, 1.0);  // 逐实例 0..1
float a = clamp(mix(-1.0, 1.0, t) + (1.0 - age), 0.0, 1.0);            // age 大的先长（逐顶点错峰）
float g = 1.0 - (a2*a2 - a2*sin(a2 * PI));                             // a2 = 1-a，ease-out-back
vec3  p = mix(appear_pos, Vertex_Position, g);
if (g < 0.01) { gl_Position = NaN; return; }                           // 未长出的顶点整个剔掉
```

`g` 后面还乘在 `custom0` 上 —— 叶片是"从枝梢位置、零尺寸开始长大"的。

### ② 低频摇摆

不受 `wind_strength` 影响，是树干／枝的位置域扰动：

```glsl
float amp = p.y * 0.03;                                                // 越高摆幅越大
x' = x + amp * sin(0.823529*T + 0.2*x) * sin(0.893617*T + 0.2*z);
z' = z + amp * sin(0.792453*T + 0.2*x') * sin(0.688525*T + 0.2*z);     // 注意用的是 x'
```

四个互质频率相乘。时间一律写成 `sin((T*f - 2π*trunc(T*f/2π)) + game_time_frac*f + φ)` ——
`game_time_sec` 是 uint、小数单独存，这是长跑后 `sin` 不掉精度的写法。

### ③ 锥形背面剔除 + 缩放淡出

```glsl
float ndotv = dot(prim_normal_ws, dir_cam_to_center);
float k = ndotv - abs(ndotv) * clamp((d - 5.0) * -0.2, 0.0, 1.0);      // d<5 时 k 恒 ≤0 ⇒ 近处永不剔
if (k >= 0.3) { gl_Position = NaN; return; }
float fade = 1.0 - smoothstep(0.3, -0.2, k);                           // 接近被剔的程度 0..1
```

`(1.0 - fade*fade)` 乘在卡片相对 `prim_center` 的偏移上 —— **叶簇不 pop，先缩成一点再消失**。
另叠一个距离项 `mix(0.8, 1.0, clamp(viewZ * -0.05, 0.0, 1.0))`：20 m 外全尺寸，近处缩到 0.8。

### ④ 卡片展开：mesh 四角 + view-aligned billboard

```glsl
vec2 o   = COLOR_0.xy * 2.0 - 1.0;                                     // [-1,1]²，UV 与角码同一份数据
float phi = 0.2 * sin(3.529416*W*T + x') * sin(1.538462*W*T + z');     // W = wind_strength
o = mat2(cos(phi), sin(phi), -sin(phi), cos(phi)) * o;                 // 屏幕空间自转
vec3 off = (vec4(o, 0.0, 0.0) * view_to_world).xyz;                    // w=0 ⇒ 只取相机右/上基
final = prim_center + roll(theta) * ((p_swayed - prim_center) + off * custom0 * g) * fade;
```

`theta = mix(-0.5, 0.5, 0.3 * hash(prim_center.xz))` 是绕**视线轴**的随机 roll，
实际范围 `[-0.5, -0.2]` rad —— 一个带偏置的随机翻滚，破掉所有卡片朝向一致的死板感。

⚠️ `glade_theme_id == 4`（冬）时 `wind_strength` 被硬替成 `0.4`。

### ⑤ 整树弯曲

```glsl
xz += bend_vector * smoothstep(3.0, 20.0, Vertex_Position.y);           // 局部高度加权，高处弯得多
```

### ⑥ 深度前推

见下一节。`smoothstep` 的结果存进 `C12` 交给 PS。

### ⑦ 逐树噪声与开花标志

```glsl
C10 = value_noise_2d(instance.position.xz * 0.05);                      // 波长 20 m 的地域色变
C8  = hash(id + 4) < 0.4 && prim_center.y > 3.0 && hash(posKey) < 0.9;  // 开花树，约 36% 且只在冠上部
```

`posKey = int(10.0 * (center.x + center.y + center.z))`，同时作为 `C5` 传给 PS 当逐卡随机种子。

### ⑧ 插值器输出

| 槽 | 内容 | 槽 | 内容 |
| --- | --- | --- | --- |
| `C1` | `COLOR_0`（`.xy` 即 UV） | `C7` | `prim_center` 世界位置 |
| `C2` | 旋转后的法线 | `C8` | 开花标志（flat） |
| `C3` | 最终世界位置（含 bend） | `C9` | `pad0` 树种 id（flat） |
| `C4` | 实例 `id`（flat） | `C10` | 逐树噪声（flat） |
| `C5` | 位置哈希键（flat） | `C12` | 深度前推强度 |
| `C6` | 挖洞强度（flat） | | |

## 深度浮雕

让树冠"蓬松有体积"的核心招式，由 VS / PS / deferred 三处配合完成。
不熟悉这个套路先看图：[`Tree_CanopyShading_DepthRelief.svg`](Tree_CanopyShading_DepthRelief.svg)（问题 → 做法 → 结果三栏）。

**VS 端**把线性深度改成 `d - 1.6 * smoothstep(1.6, 3.2, d)`，即 3.2 m 外整张卡片**整体拉近 1.6 m**，
1.6 m 内完全不做（避免近处深度穿到相机后面）：

```glsl
float lin  = -1.0 / ((clip.z / clip.w) * proj_c);
float s    = smoothstep(1.6, 3.2, lin);                                 // → C12
clip.z     = (1.0 / (-(lin - 1.6 * s) * proj_c)) * clip.w;
```

**PS 端**按 `canopy_alpha.G`（height）逐像素推回去：

```glsl
layout(depth_less) out float gl_FragDepth;                              // reverse-Z 下 less = 更远
gl_FragDepth = (gl_FragCoord.z / gl_FragCoord.w)
             / ((1.0 / gl_FragCoord.w) + C12 * (1.0 - height) * 1.6);
```

- `height = 1`（叶簇中心最厚）→ 保持拉近 1.6 m，**凸向相机**
- `height = 0`（卡片边缘）→ 完全推回卡片平面

净效果：**一张平面四边形在深度缓冲里变成一个 1.6 m 厚的叶球**。两棵树的卡片相交时得到弯曲的交线
而不是平片相交的直线，SSAO 与遮挡关系全部跟着走。

**deferred 端**把假深度还原回真平面，再做光照：

```glsl
world_pos = reconstruct_from_depth(...) + view_ray * height * 1.6;
```

阴影与 GI 因此用的是卡片真实平面位置（稳定、无自遮挡瑕疵），深度缓冲里保留凸起只服务遮挡与交错。

## 主 pass PS

### ① 镜头挖洞

相机贴近树时，屏幕中心一圈叶子被挖空：

```glsl
float strength = (render_overrides & 2u) != 0u ? C6 : 0.0;              // 受一个 override 位控制
if (strength > 0.0 && !is_firstperson && -viewPos.z < far) {
    vec3  p    = vec3(viewPos.xy + (uv - 0.5), min(viewPos.z, -10.0));
    float cone = cone_angle_mult * smoothstep(far, near, -viewPos.z);   // 越近角度越大
    if (-normalize(p * vec3(0.75, 1.0, 1.0)).z
        > cos(cone * strength + min(1.0, cone*16.0) * 0.075 * (bluenoise - 0.5)))
        discard;
}
```

`vec3(0.75, 1.0, 1.0)` 把圆锥压成横向更宽的椭圆；蓝噪声抖动边缘，避免硬圆边。
`near = 6.66 * P11`、`far = TreeCanopyMaxCullDistance * P11`，`P11 = view_to_clip[1][1]`，即按 FOV 缩放。

### ② 花：高度域合成，不是 alpha 混合

```glsl
float leafH   = alpha.G - (has_flower ? 0.3 : 0.0);
float flowerH = mix(0.0, 1.5, hash(C5) * flowers_depth);                // 上限 1.5*0.212 ≈ 0.32
bool  useF    = has_flower && flowers_depth > 0.01 && C10 < 0.6 && flowerH > leafH;
if ((useF ? 1.0 : alpha.R) < 0.2) discard;                              // 花像素跳过 alpha test
gbuffer_height = max(leafH, flowerH);
```

开花只发生在 `C10 < 0.6` 的区域 —— **成片的花树，不是随机散点**。
花色再乘一个 `mix(0.5, 2.0, flowers_depth)` 当明暗。

### ③ 叶色：三档 theme 分支，都不采叶片贴图

```glsl
mode = (theme == 2 || theme == 4) ? 1
     : (theme == 1 && (C9 == 1 || C9 == 2 || C10 > 0.4)) ? 2
     : 0;
```

| mode | theme（推断） | 做法 |
| --- | --- | --- |
| 0 | 0 / 3 | `mix(canopy_color_1, canopy_color_2, hash(id*1128782))`，纯 `ColorLibrary`；theme 3 额外 `canopy_color_2 * 0.75` |
| 1 | 2 autumn / 4 winter | 随机 UV **采两次 `canopy_color`**，按 `mix(0.1, 0.5, hash(C5))` 混 |
| 2 | 1 flowery | 同 mode 1 但权重走 `smoothstep(0,1,hash(C5^1128782))`、第二次不 `*0.5`；`C9 == 2` 时 `uv.y = fract(uv.y) * 0.6` 锁在色板上 60% |

mode 1 的两次采样：

```glsl
uv_a = hash2(id^1128782, id^872) + uv * 0.01 + (wp.yz + wp.yx) * 0.0005;   // 近似常数色
uv_b = hash2(id^11282,  id^82 ) + uv * 0.20 + (wp.yz + wp.yx) * 0.05;      // 结果再 *0.5（暗）
```

**"随机 UV 采一张色板图"等于用纹理当随机取色器**，`uv * 0.01` 只留一点点卡片内渐变。
theme 判定的依据：theme 4 有积雪（`mix(color, vec3(0.7), smoothstep(0,1,clamp(N.y,0,1)))`）⇒ winter；
theme 2 与 4 共用同一段采样代码 ⇒ autumn；theme 1 走独立分支 ⇒ flowery。⚠️ theme 0 与 3 哪个是夏天未定。

### ④ G-buffer：只有一个 `uvec2`

```glsl
out uvec2 SV_TARGET0;
// .x = normal(8:8:8) | AO(8)          AO    = clamp(alpha.B - 1/256, 0, 1)
// .y = sqrt(color)(8:8:8) | height(8) height = max(leafH, flowerH)
```

`alpha.B` 减 `1/256` 是让 AO 能严格取到 0；delta 后 AO 字节为 0 的像素在 deferred 里会跳过法线抖动。

## 阴影变体

`f0adff76827e73ff` 与主变体是同一份源码的另一个编译特化。PS 为空 ⇒ **树影是实心四边形，没有叶隙**，
这就是 TG 树影总是软块状的原因。VS 砍掉 billboard / roll / 淡出 / 深度偏移，另有两处关键差异：

```glsl
vec3 p = mix(appear_pos, Vertex_Position, g) + Vertex_Normal;           // 沿卡片法线外扩 1 单位
if (dot(prim_normal_ws, light_dir_ws) >= 0.15) { gl_Position = NaN; }   // 主 pass 阈值是 0.3
```

外扩 1 单位是补偿主 pass 那 1.6 m 的深度凸起，让影子形状对得上。

## 树冠专用延迟光照

```text
Lo = albedo * (1/π) * wrap * sun * shadow                              (A) 直接漫反射
   + F * D * backlit_terms * sun * dist_fade * shadow * 1.5            (B) 光泽 + 逆光透射
   + mix(0.3, 1.0, cavity) * albedo * SH_irradiance(N) * 1.3           (C) 间接漫反射
   + SH_irradiance(R) * 0.04 * mix(cavity, 0.75*cavity+0.75, gi²) * 2  (D) 间接镜面
```

各项：

```glsl
wrap    = mix(0.2, 1.0, smoothstep(-0.2, 1.0, dot(N, L))) * 0.8;   // N·L=-0.2 处仍有 0.16，背光不死黑
F       = mix(0.2, 0.7, pow(1.0 - abs(dot(N, V)), 4.0));           // 掠射 0.7 / 正对 0.2
D       = 1.0 / (2.0*PI*PI * pow(1.0 - 0.5*sqr(dot(N', H)), 2.0)); // == D_GGX(α²=0.5)/π，很宽的软高光
backlit = max(0.0, -0.75 * dot(V, L));                             // 相机与太阳异侧 → 1
cavity  = clamp(exp(-(1.0 - 2.0*height) * dot(N, V)), 0.0, 1.0);   // height 当曲率/AO
dist_fade = mix(0.8, 0.3, smoothstep(50.0, 150.0, length(wp.xz))); // 远景树弱化
N'      = N + hash_sphere(AO_byte) * 0.5;                          // 空间稳定的伪随机粗糙度
```

四条值得单独记：

- **逆光旁路**：`backlit` 通过 `mix(wrap, 1.0, backlit)` 与 `mix(cavity_term, 1.0, backlit)`
  把 `N·L` 和厚度遮蔽**同时**旁路掉，只剩 `F` 的边缘项主导 —— 太阳在树背后时叶缘发亮就来自这里。
- **height 一通道三用**：深度浮雕、花的高度合成、`cavity`。厚（`height > 0.5`）恒亮，薄且正对相机变暗。
- **AO 字节兼作随机种子**：`hash(uint(AO_byte))` 生成球面方向 `* 0.5` 加到法线上，只用于 (B)(D)，
  逐帧稳定，不是噪声。
- **两个 pass 读的不是同一份东西**。同一个 `uvec2` G-buffer，高 16 位的拆法完全相反：

  | | `.x` 高 8 位 | `.y` 高 8 位 |
  | --- | --- | --- |
  | canopy pass | AO，兼法线抖动的随机种子 | height，跑深度浮雕与 `cavity` |
  | grass pass | 八面体压缩的第二法线 `x` | 同一条法线的 `y` |

  canopy pass 还逐像素做 `world_pos += view_ray · height · 1.6` 的位置还原 —— 这只对写过深度浮雕的
  树冠像素成立，对墙面会把位置推错；反过来 grass pass 把 canopy 的 AO/height 读成法线也是错的。
  两条互斥 ⇒ **两个 pass 必然按区域分开跑**。引擎侧有 `RenderPassApi::set_stencil_ref` /
  `cmd_set_stencil_compare_mask`，机制推测是 stencil，但 shader 里看不到。
- **不是 PBR**：canopy 这条没有 metallic / roughness / BRDF LUT，自己采 `shadow_map`（3 级联、6 抽样
  黄金角 `2.39996` 螺旋 PCF、`exp(-Δ*1024)` 的 ESM 风格软过渡，不是硬比较）。grass 那条反而更像通用
  主光照：有 `brdf_fg_lut`、`ssgi_tex`、`screen_shadows_tex`，甚至有跟草无关的 `highlight_active_hill`
  （地形编辑高亮）。

间接光是标准 L1 SH（系数 `0.886227 = π*Y₀`、`1.023327 = π*(2/3)*Y₁`）+ 一个从 L1 幅度推的伪 L2 项，
半分辨率 3（不足时补到 6）抽样、按 `exp2(-min(30, 80*|1 - d_half/d|))` 的深度相似度加权上采样。

⚠️ 读代码时会撞见 `dot(N, (wp - wp) + ...)` —— `(wp - wp)` 是 SPIRV-Cross 留下的常量 0，不是笔误。

## 其它叶类

| | 关键做法 |
| --- | --- |
| **藤蔓叶** `_ivy_instanced_ivy_leaf` | **每帧丢 1/4 像素**：`if ((frame_index & 3u) == (px&1) + (py&1)*2) discard;`，靠 TAA 平均出"薄"；阴影 pass 关掉这段。height 写死 `0x4D` = 0.302，AO 写死 255 |
| **灌木** `_nani_bush` | **`if (abs(dot(dir_cam_to_p, N)) > 0.5) discard;`** —— 正对相机的面片直接丢，只保留掠射的，永远看到蓬松边缘而不是平板。色彩按世界 `XZ + uv*0.2` 再 `*0.1` 采 `bush_color`，不是叶片贴图。AO `0x66` = 0.4，height `0xB3` = 0.702 |
| **背景树** `_clearing_instanced_background_tree` | 前向，无 G-buffer 无阴影无贴图：`albedo * (0.6/π) * mix(0.35, 0.9, smoothstep(-0.2, 0.8, N·L)) * sun`，再按 `smoothstep(0.07, 0.3, 0.0005/w)` 混天空色，最后 `* 1.2` |
| **落叶粒子** `_leaf_particles.cs` | `LeafParticle { vec3 position; float rotation; float scale; vec3 up_axis; }`，从 emitter（树位置）`±0.5` 重生，`scale = mix(0.6, 1.0, r) * 0.8`，`up_axis = normalize(rand(-1,1), 2, rand(-1,1))`，落地后每秒 `-0.05` 缩没 |

## 移植到 UE：已落地

2026-09-09 落地为 `M_TinyGladeCanopy` + `MI_TinyGladeCanopy`（都在 `/PCGPlugins/HouseTest/`）。
材质图在 `.uasset` 里读不出来，**可读源头是建图脚本** `Scripts/TinyGladeMakeCanopyMaterial.py`，
逐条取舍与踩过的坑在它的文件头；验收脚本是 `Scripts/TinyGladeShotCanopy.py`。

**能落地的前提是数据已经在网格里**（2026-09-09 实测，见「资产侧契约」）：
`central_tree_leafcards` 的 UV0 / UV1 / UV2 恰好就是卡片角码 / `prim_center.xy` / `prim_center.z`，
顶点法线逐卡一致（等价于 TG 的 `prim_normals`）。四条缺口因此全部有数据可用。

| 缺口 | 状态 | 落地方式 |
| --- | --- | --- |
| ① 深度浮雕 | **已落地**，符号与 TG 相反 | 只用 PDO：`DepthBulge · smoothstep(160, 320, PixelDepth) · (1 - height)`。UE 的 PDO 只能往远推，照抄 TG 的符号会得到凹陷 |
| ② 逆光旁路 | **只做到近似** | `MSM_TWO_SIDED_FOLIAGE` + SubsurfaceColor 承担透射；「逆光时旁路 `N·L` 与厚度遮蔽」在材质图里做不到，要改引擎或自定义 shading model |
| ③ 随机 UV 采色板 | **已落地** | Custom node 拿 `prim_center` 做逐卡 hash，随机 UV 采两次 `autumn_canopy`，按 `mix(0.1, 0.5, hash)` 混 |
| ④ 缩放淡出 | **已落地** | WPO 把顶点收向 `prim_center`，`fade²` 驱动；替代 TG 的 `gl_Position = NaN` 硬剔除（UE 材质里没有等价物） |

⚠️ **`DepthBulge` 默认 60 cm，不是 TG 的 160** —— 浮雕量要按**卡片尺寸**等比，不能照抄绝对值。
TG 的叶卡宽约 130 cm 配 1.6 m（比值 1.23），本项目的卡片 span 实测只有 40~54 cm，同比给出 ~60 cm。
用 160 出过一轮图：层次是出来了，但整棵树明显压暗一档。**换网格时这个值要跟着卡片尺寸重算。**

**验收口径**：三档对照出图（全开 / `DepthBulge=0` / `ConeCullEnable=0`），只在**树覆盖的像素**上算
差异率（天空要排除，否则默认关卡的体积云一动就是 57% 的假阳性）。实测树覆盖率 84%，
两条机制各约 80% 差异率。这条判据同时防「材质被静默换成默认材质」——那种情况下三张图逐位相同。

⚠️ 还没做的：镜头挖洞（速查表 ⑥）、实心阴影 + 法线外扩（⑧）、花的高度域合成、风摆动画。
前两条要动阴影 pass 与 `render_overrides` 那类全局开关，不是一张材质能覆盖的。

## 未确认

- `PlacementData.custom0` 的实际量级，决定 view-aligned 偏移是主导还是微调。只能抓帧。
- `glade_theme_id` 的 0 与 3 哪个是夏天。已定：4 = winter（有积雪分支）、2 = autumn、1 = flowery。
- `leaf_particle_draw` 复用了哪个 raster shader —— `tmp/shaders/` 里没有同名文件，
  贴图 `leaf_alpha.png` / `leaf_particle_colors_autumn|flowery.png` 存在但未定位到消费者。
- `render_overrides` 的 bit 1 在什么条件下置位（同时控制 VS 的近处剔除与 PS 的镜头挖洞）。
- 其余 42 个 G-buffer 生产者（墙、屋顶、地形、楼梯……）由哪个 pass 点亮。
  `_deferred_grass_light_sun_sky` 的绑定看着像通用主光照（`brdf_fg_lut` / `ssgi_tex` /
  `highlight_active_hill`），但名字是 grass；静态文件里定不了它究竟是主 pass 还是草特化，
  也定不了两个 pass 的分区机制。要抓帧看 pass 列表与 stencil 状态。
