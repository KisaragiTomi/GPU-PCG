# Tiny Glade 楼梯逆向 · 附录 D：玩家绘制楼梯（§4.1）与门前踏步（§4.0）

> 目的：给 UE 侧「样条楼梯 actor」提供能直接照着写的 TG 做法。
> 起点是 [卷三 · 楼梯模块对照](Progress_ModuleComparison.md#vol-3)（§4.0–§4.3 四套的划分、§五 最小可行第一步、§七 资产实测），本文不复述它，只补**数据模型、常量、算法步骤**。
> 状态：初版完成（2026-09-16）。§3 拱/柱的触发高度仍未查清，见文末「未查到」。

## 证据等级

| 标注 | 含义 |
| --- | --- |
| `[PDB]` | `tiny_glade.pdb` 的符号名或类型记录（TPI 流的结构体字段/枚举变体） |
| `[ASM]` | `tiny-glade.exe` 的 dumpbin 反汇编；证据片段存 `evidence/stairs-playermade-20260916.asm` |
| `[SHADER]` | `D:/MyProject/Tiny Glade/tmp/shaders` 反编译 GLSL |
| `[ASSET]` | 资产实测（glb / json 顶点包围盒、三角数） |
| `[DOC]` | 既有文档（卷三、`MESH_GENERATION_ANALYSIS.md` 等） |
| 【推测】 | 由上述证据推理得出，推理链写在正文 |

单位约定：TG 源数据是**米**，本文换算成 **cm** 写；原始浮点常量在括号里给出十六进制。

## 速查（一句话结论）

| # | 问题 | 结论 | 详见 |
| --- | --- | --- | --- |
| 1 | 路径数据模型 | 节点 = 楼梯点装饰物（高度偏移 + 宽 + 栏杆 + 配色），边 = 无向 `GraphMap<DecoratorId, ()>`；每条边在 XZ 上走自然三次样条、每 ≤ 50 cm 采一点，高度在两端节点间线性插值；坡度 ≤ 0.25 平走道、≤ 2.5 楼梯、> 2.5 梯子 | §1 |
| 2 | 踏步 | 没有踏高/踏深参数：3D 曲线按 **50 cm 等弧长**取整重采样，相邻两点一级；每级是 ≥ 60 cm 高的实心块（`brick` 墙砖实例），沿宽度随机切 1–N 块，进深 ×1.13 互搭；宽 45–400 cm | §2 |
| 3 | 支撑 | 楼梯只投诉求，拱墙/柱/托架全由墙构造器用墙砖砌；拱数 ≈ 每 4 m 一拱；沿墙楼梯的托架判据 = 踏面下 117 cm 朝墙射 178 cm；**出拱/柱的离地阈值未查清** | §3 |
| 4 | 地形 / 墙 / 门 | 点高 ≥ 地面 − 10 cm、踏面 ≥ 地面 + 15 cm；穿墙开矩形洞（洞顶恒 10 m），`HoleOrigin = 4`；挂墙节点决定朝向、端部加厚与托架判定；与门无专门连接 | §4 |
| 5 | 栏杆 | `{None, Low, High, Wooden}` 存节点、按连通分量整体设置；石栏杆是砖、木栏杆是木板；开启时踏步让出 25 cm 并走半高块 + 地板网格分支 | §5 |
| 6 | 门前踏步 | 门底贴地（≤ 20 cm）且门外 106 cm 处下沉 10–150 cm 才出；`N = clamp(ceil(H/20), 2, 5)`，总外伸 56 cm、首排距门 28 cm、宽 = 门宽、与阳台二选一 | §6 |
| 7 | 撤销 | 节点走装饰物历史，图走「边集 diff + 哈希快照」，重放后下一帧重算；拖动动画来自 9/s 指数逼近 | §7 |
| 8 | 资产 | 全部砌体只用 `brick`；`stair_step` 是调试网格；梯子/木栏杆【推测】用 `wooden_plank`；`stairs_step/pebble` 属岩地台阶 | §8 |

---

## 1. 路径数据模型（StairsState 图）

### 1.0 先说一个坑：PDB 里没有 Rust 结构体字段

`tiny_glade.pdb` 的 TPI 流只有 6125 条类型记录，全是 C 依赖库（zstd / lcms …）的结构体 `[PDB]` —— Rust 侧是 line-tables-only 调试信息，**字段名、偏移一律查不到**。
本节的字段名来自两条旁证：exe 里 serde 派生的字符串（`struct X with N elements` + 紧随的字段名池、`VARIANTS` 胖指针数组）与反汇编里的偏移用法。字段**顺序**以 serde 声明序为准，内存偏移以反汇编为准。

### 1.1 节点 = 一个装饰物（Decorator），不是图自己的数据

| 事实 | 证据 |
| --- | --- |
| 楼梯点是 `DecoratorId`，编辑主体是 `EditSubject::StairPoint` | `[PDB]` `petgraph::graphmap::Edges<DecoratorId,(),Undirected>`；exe 串 `struct variant EditSubject::StairPoint with 1 element` |
| 节点的持久数据挂在装饰物上：`StairDecoratorInfo { width, ladder_color_id, railing, magic_color_id, supports_legacy_dont_use }`（serde 字段串）＋一个 f32 高度偏移（字段名串被去重进了别处的 `height`）。内存布局 `[ASM]`：`+0x00 f32 height`、`+0x04 f32 width`、`+0x08 u32 ladder_color_id`、`+0x0C u32 magic_color_id`、`+0x10 u8 railing` | exe 0x2c21944 字段池；`ui_change_stair_height`、`ui_focus_stairs` 14079E091、`set_decorator_color_id` 1422AF713/933/B63 |
| **节点高度 = 基面 + 偏移**：基面是地形双线性高度（`TerrainAttachment{pos_xz}`）或平屋顶 `PublicWallState::flat_roof_y`；偏移存 `StairDecoratorInfo+0x00`，UI 滑条范围 **0–10000 cm**（`Slider3d` 范围常量 `(0.0, 100.0)` 米） | `[ASM]` `ui_change_stair_height` 1407AE0F7 |
| 节点落点 `DecoratorDst` 有四种：`Wall(WallAttachment{coord, anchor})`（判别字节 0/1 复用为「在墙哪一侧」）、`Roof`=2、`Terrain(TerrainAttachment{pos_xz,…})`=3、`Stair(StairAttachment{graph_edge, uv, yaw_radians})`=4（给挂在楼梯上的灯笼/旗子用，楼梯点自己不会是 4，代码里是 `unreachable!`） | `[ASM]` `derive_stair_node_params` 跳表 0x142A8E448；exe 串 `WallAttachment coord anchor` / `StairAttachment graph_edge uv yaw_radians` / `TerrainAttachment pos_xz` |
| 栏杆类型 `StairRailingType = { None, Low, High, Wooden }`（4 个图标 `color_icon/railing_1..4`） | exe `VARIANTS` 数组 0x142acf110；`[PDB]` `country_core::resources::walls::decorator::StairRailingType` |
| 配色：`StairCustomization { stairs_color, ladder_color, railing_ty, magic_color }` | exe 串 `struct StairCustomization with 4 elements` |

`derive_stair_node_params` 把节点折成装配用的 `StairNodeParams` `[ASM]`：

```text
+0x00 u32  anchor 判别: 0 = Terrain, 1 = Roof, 2 = Wall
+0x04 f32  Terrain/Roof: 高度偏移（StairDecoratorInfo+0）
+0x08 ptr  Wall: 墙地址          +0x10 f32  Wall: yaw = DecoratorRotationXZ::from_xz_forward_vector(±墙切线)
+0x18 u8   度数（图里该节点的邻居数）
StairWallAnchor::wall_normal() = (-sin yaw, -cos yaw)
```

### 1.2 边与图

| 事实 | 证据 |
| --- | --- |
| `StairsState` 内核是 **无向 `petgraph::GraphMap<DecoratorId, (), Undirected>`**，边无权重 | `[PDB]` 泛型实参 |
| `StairGraphEdge(DecoratorId, DecoratorId)`（tuple struct，2 元素） | exe 串 `tuple struct StairGraphEdge with 2 elements`；`[ASM]` `impl$13::fmt` 用 `debug_tuple_field2_finish` |
| 除了边还有「端点 / 残桩」：`endpoint / try_set_endpoint / unset_endpoint / contains_edge_or_stub / iter_edges_and_stubs` —— 玩家正在往外拖的那一段是一条只有一端的 stub | `[PDB]` |
| 变更入口只有 `add_node / add_edge / remove_node / disconnect_edge`；持久化是 `StairsSnapshot{ graph?, checksum }`、`StairsHistoryDiff{ hash_before, hash_after, +2 }`、`StairsRemovedSupports{ edges }` | `[PDB]` + serde 串 |
| 续画：`continue_stairs` = `add_node` → `add_edge(endpoint, new)` → `try_set_endpoint(new)` | `[ASM]` 调用序列 |

### 1.3 从节点到几何：样条 → 密采样 → 分段类型

`generate_playermade_stair_requests`（系统）按连通链遍历图，**每条边**走下面这条链 `[ASM]`：

1. **XZ 平面的自然三次样条**：`utils::natural_cubic::Spline2D::from_control_points_with_tangents`（开链）/ `from_periodic_loop`（闭环），控制点 = 节点 XZ。端点切线【推测】来自 `AlignKind::outgoing_dir`（同一函数里调用了它，但没逐行追数据流）。高度**不进样条**，单独插值。
2. **密采样** `densely_sample_spline_segment(spline, seg_index)`：
   `L = t_next − t0`（段参数长；【推测】参数即弦长、单位米 —— `ceil(2·L)` 只有在米制下才是「每米 2 点」）；`n = max(ceil(2·L), 2)`；点距 `L/(n−1)` ⇒ **≤ 50 cm 一个点**；输出 `{ Vec<Vec2> 点, 起点切线, −终点切线 }`。
3. **分段类型** `determine_segment_type(xz[], h[])`：`s = |h_first − h_last| / |xz_first − xz_last|`（首尾弦）

   | 坡度 `s` | 返回 | 装配走哪条 |
   | --- | --- | --- |
   | `s ≤ 0.25`（≤ 14°） | 1 | 平走道：踏步点抬到「地形 + 15 cm」以上（见 §2） |
   | `0.25 < s ≤ 2.5` | 2 | 楼梯踏步 |
   | `s > 2.5`（> 68°） | 3 | **梯子** `construct_ladder` |

   `[ASM]` 14080C86B–14080C886（`__real@3e800000`=0.25，`__real@40200000`=2.5）。
4. 结果写进 `PlayermadeStairsAssemblyParams`（每条边一份）。已从 `length_ws` 与装配函数读出的字段 `[ASM]`：

   ```text
   +0x08/+0x10  Vec<Vec2>  xz        每个密采样点
   +0x40/+0x48  Vec<f32>   heights   与 xz 平行
   +0x58/+0x60  Vec<f32>   widths    每点宽度
   +0x70/+0x78  Vec<Vec2>  dirs      每点前进方向（算横向 perp 用）
   +0x88/+0x90  Vec<f32>   (可选) 每点系数 g —— 语义未查清（进 construct_stair_steps，见 §2.2）
   +0x130/+0x138 Vec<(u8 type, u8 flag, u16 count)>  同类型连续采样的分段表
   length_ws() = Σ |(xz,h)_i+1 − (xz,h)_i|   （3D 折线长）
   ```

**没有查到的限制**（试过的：`ui_focus_stairs` / `insert_point_ui` / `generate_playermade_stair_requests` 的浮点常量全表）：没有显式的最小/最大段长、转弯半径常量；坡度的唯一硬门槛就是上表 0.25 / 2.5 两刀（超过 2.5 不拒绝，改出梯子）。

## 2. 踏步生成

### 2.1 一句话：TG 没有「踏高 / 踏深」参数，踏步 = 3D 曲线按 50 cm 等弧长重采样

整条链上**找不到**任何「目标踏高」「目标踏深」常量；级数也不是按高差分级。做法是 `[ASM]`：

```text
1. 每点高度（compute_stair_assembly_params 的 fold 闭包 0x141292E30）
   h_lerp[i] = lerp(h_start, h_end, t[i])                    t = 沿边归一化参数（逐采样点）
   ground[i] = max(terrain(xz + perp·w/4), terrain(xz − perp·w/4))   左右各 1/4 宽处双线性采样
   height[i] = max(h_lerp[i], ground[i] − 10 cm)             ——线性插值，只允许比地面低 10 cm
   buried[i] = ground[i] > h_lerp[i]                          另存一个 BitVec
2. 宽度同理：width[i] = lerp(w_start, w_end, t[i])             （compute_stair_assembly_params 1408072C5）
3. 每个同类型分段（§1.3 的分段表）取出 3D 点 (x, height, z) → Curve<Vec3, CurveCoordSemantic3D>
   （累计长度用 √(dx²+dy²+dz²)，try_new_from_points 1401536F0）
4. Curve::try_resample(curve, 0.5, true)                      （playermade_stairs_assemble 14001A09A）
   N = max(round(L_3D / 0.5), 1)；输出 N+1 个 3D 等距点        （try_resample 140152B9B–140152C00）
   宽度、方向、g 按重采样点所在原段的 local t 线性插值
   平走道分段（type < 2）额外把 y 抬到 ≥ terrain + 15 cm
5. construct_stair_steps(点, 宽, 方向, g, …)：相邻两点 (a, b) = 一级踏步
```

⇒ **每级的 3D 斜边长恒为 `L_3D / round(L_3D/50 cm)` ≈ 50 cm**，踏高 = 50·sinθ、踏深 = 50·cosθ（θ 为坡角）。
坡度 0.25（14°）时约 12 × 49 cm，1.0（45°）时约 35 × 35 cm，2.5（68°）时约 46 × 19 cm；再陡就换成梯子。
余数不单独吸收 —— `round` 后把整段均分，每级略大或略小于 50 cm。

### 2.2 `construct_stair_steps`：一级踏步怎么变成砖 `[ASM]` 1407DA0B0

签名（Win64 栈参数按 `rbp+470h` 起逐个 8 字节）：

```text
construct_stair_steps(widths: &[f32], dirs: &[Vec2], points: &[Vec3], rng, brick_writer, wall_id: u32, _,
                      flags: u16, narrow_by_railing: bool, min_width: f32, terrain, g: &[f32], half_height: bool)
```

逐对 `(a, b)`：

| 量 | 规则 | 常量（cm） |
| --- | --- | --- |
| 踏面标高 `Y` | `max(a.y, b.y)`，且 `≥ terrain(mid) + 15` | 15 |
| 块高 `h` | `max(abs(b.y − a.y), 60)`；`half_height` 时 ×0.5 | **60** |
| 高端加厚 | 楼梯**较高那一端**水平 56 cm 以内的级（`k < floor(0.56 / d01)`），`h += 首级踏高`；若该端挂在墙上且宽 ≤ 150 cm（`flags` bit0 / bit8）则不加 | 56 / 150 |
| 块中心 | 踏面中点下移 `h/2` —— 每级是一块**从踏面往下 ≥ 60 cm 的实心块**，相邻级互相叠压，底面永远看不到 | — |
| 宽 `w` | `max(width − (narrow ? 25 : 0), min_width)`；`min_width` = 30 或 63 | 25 / 30 / 63 |
| 横向切砖 | 取 `w̄ = (w_a + w_b)/2`：`< 66` → 1 块；`66–120` → 随机 1 或 2 块；`≥ 120` → 砖长 `ℓ = smoothstep(rand)·lerp(70, 130, clamp((w̄−200)/200)) + 46`，块数 `ceil(w̄/ℓ)`；分界用 `random_splits(块数+1, max(0.3ℓ, 32)/w̄)`（近似均分 + 抖动，语义见 §6.4） | 66 / 120 / 46 / 70–130 / 32 |
| 砖进深 | 前后两点距离 × **1.13**（前后级互相搭 13%），再乘 `1 − rand·0.42·ḡ` | ×1.13 |
| 顶面磨损 | 写进 `sin_deform_y` 的四角 Y 偏移：`off(u) = rand·5 − (1 − abs(2u−1))·m`，`m = lerp(min(0.8Δy, lerp(5,15,τ)), min(0.8Δy, lerp(15,30,τ)), rand)`，`τ = clamp((w̄−200)/200)` ⇒ **踏面中间凹、两边高**；**首级与末级强制为 0**（平顶，好接地面/平台） | 5 / 5–15 / 15–30 |
| 跳过 | `half_height && ḡ < 0.1` 时整级不出砖（`half_height` 只在栏杆 ≠ None 分支为真，见 §5；`g` 语义未查清） | — |

**每块踏步砖都是墙砖实例** `[ASM]`：写进 `SparseInstanceBufferWriter<BrickTransformSsbo>`，96 字节布局与 `InstancedWallData` 一致，
`source_id = BrickSourceId::from_wall_id(…)`、`seed = 级序号 + 块序号`、`wallspace_x_range = (u0·w̄, u1·w̄)`、**`flags = 0x60`**（64 免编辑高亮 + 32 拱压扁/三平面 UV/免拱裁剪，位表见 `MESH_GENERATION_ANALYSIS.md` §1.7）。
⇒ 踏步用的是 600 顶点的 `brick` 单位盒，**不是** `stair_step` 网格（`stair_step` 的去向见 §8）。
仿射三列里哪一列承载横向片宽没有逐位追完【推测：沿路径 = 进深×1.13，竖直 = h，横向 = (u1−u0)·w̄】。

### 2.3 AlignKind / 宽度 gizmo / 平台 / 悬浮砖

| 项 | 结论 | 证据 |
| --- | --- | --- |
| `AlignKind` | 三变体枚举。0：带一个 Vec2 方向（`+4`），`position_offset(w) = dir·w`（有标志位时）；1：带两个 Vec2（`+4`、`+0xC`），`position_offset(w) = (d1 [+ d2])·w/2`，`outgoing_dir = d2`；2：无方向（`outgoing_dir` panic）。按 `(DecoratorId, DecoratorId) → AlignKind` 存进 IndexMap。**语义**【推测】：节点在墙上/墙角时把楼梯中线偏半个宽度贴墙（0 = 贴一面墙，1 = 墙角两面，2 = 自由） | `[ASM]` `AlignKind::position_offset` 140959F00、`outgoing_dir` 140959F50 |
| `find_best_neighbor_alignments` | 递归枚举每个节点的候选对齐，打分下限 `−10`（`__real@c1200000`），初值 `−FLT_MAX` | `[ASM]` 常量表 |
| 其余 gizmo | `height_all_widget`（整体升降，`ui_change_stair_height` 单点滑条 0–10000 cm）、`move_all_widget`（整体平移）、`rotate_single_node_stairs_widget`、`add_support_widget`（把被删的支撑加回来）、`stair_seg_add_point`（在边上插点，`insert_point_ui`） | `[ASM]` exe 串 |
| 宽度 | 节点字段 `StairDecoratorInfo.width`（`+0x04`），沿边线性插值（§2.1）。整体宽度 gizmo `width_all_widget`：`Slider3d` 增量范围 ±400 cm，结果 `clamp(w + Δ, 45, 400)` 写回**整个连通分量**每个节点；单节点宽度箭头 `stairs_width_{i}_arrow_{j}`：`Slider3d` 范围 `(50, 400)`。**默认宽度未查到**（在放置工具里，没追） | `[ASM]` `ui_focus_stairs` 14079E0E1、14079E329–14079E33B、1407A1A2A |
| 平台 `gen_stair_floors` | 与 `construct_stair_steps` 同一调用点先后出现；产物是 nani `Trimesh` 块（`StairsFloorMeshChunk`），常量 `−0.2`、`0.15` | `[ASM]` 14001A7D4；`[PDB]` |
| `StairsFloatingBrickParams` | `magic_support::playermade_stair_floating_bricks` 的输出（被删掉支撑后的悬浮砖），见 §3 | `[PDB]` |

## 3. 支撑结构（拱 / 墙 / 托架 / 柱 / 悬浮砖）

> 本节的**管线结构**是确凿的；「离地多高出拱 / 出柱」的**精确阈值没有逐行追完**（`construct_playermade_stairs_arches_n_walls` 4635 行，里面是 `ArchWalker` + 平滑混合），下面给出已确认的判据与未解读的常量池，MVP 请按 §9 的简化规则做。

### 3.1 总管线：楼梯只出「诉求」，砌法全在墙构造器 `[ASM]`

```text
playermade_stairs_assemble（每条边一份装配参数）
 ├─ 平走道/楼梯分段 → construct_stair_steps（§2.2）+ gen_stair_floors（平台三角网格块）
 ├─ 梯子分段       → construct_ladder
 ├─ split_segment_into_supported_and_unsupported_subsegments → 托架诉求
 ├─ 写 StairsArchSupportRequests / StairsBracketsRequests（【推测】其一是 14001C445 处 0xA8 字节的记录：3D 曲线 + 宽/方向数组 + wall_id + 边标识）
 ├─ 墙重叠 → 墙洞（§4.2）
 └─ gen_stairs_collider_and_rt_geo（碰撞 + 光追代理）
system_wall_constructor（另一个 crate，另一批系统）
 ├─ construct_playermade_stairs_arches_n_walls：preprocess_curve → ArchWalker::new/walk
 │     → WallConstructor::from_curve（**就是砌普通墙的那个构造器**）→ 墙砖实例
 │     → 在拱脚处 raycast 找落点 → StairsPillarProposals::add（柱子提案）
 ├─ construct_playermade_stair_brackets（托架石）
 ├─ process_playermade_stair_pillars_proposals → construct_playermade_stair_pillars_visuals
 │     （柱 = Rectangle2d 轮廓再走一次 WallConstructor::from_curve）
 └─ magic_support::playermade_stair_floating_bricks（被删支撑后的悬浮砖，带缓动动画）
```

**所有支撑物最终都是墙砖实例**（`BrickTransformSsbo`，`BrickSourceId::from_wall_id` + `with_transient_stair_edge_id`）；只有梯子与木栏杆走另一种实例缓冲（【推测】`WoodenPlankSsbo`，写入函数单态化哈希与砖不同）。
⇒ 在 UE 侧，「楼梯下的拱墙」= 「沿楼梯 XZ 路径砌一段带拱洞的墙」，可以直接复用房屋墙体的砌法，不需要新的几何原语。

### 3.2 托架（沿墙走的楼梯）：判据确凿 `[ASM]` 1407D94A0

仅对**挂墙节点**（`StairNodeAnchor::Wall`）所在分段执行，逐重采样点：

```text
origin = p − perp_toward_wall · (w/2)  ;  origin.y = p.y − 117 cm
hit    = RaycastWorld::raycast_simple(origin, dir = perp_toward_wall, max = 178 cm)
hit     → 该点「有墙可靠」：记 gap = |hit − origin|_xz − w/2（托架要伸出的长度）
no hit  → 该点「悬空」
连续同类点合成区间 → Vec<(start, end)> supported / unsupported + 每区间 gap 数组
```

托架石的尺寸常量池（`construct_playermade_stair_brackets`，未逐项归位）：`24, 12.6, 39, 52.5, 65, 75, 83.1, 88.9 cm`、`−56, −130 cm`，随机正交四元数 `utils::random_orthogonal_quat` 做 90° 翻转。

### 3.3 拱墙 / 柱：已确认的部分

| 事实 | 证据 |
| --- | --- |
| 拱墙不是网格，是 `WallConstructor::from_curve` 砌出来的砖墙；拱洞由 `ArchWalker` 沿曲线走出来 | `[ASM]` 1404993DF、14049877E、1404988BB |
| 拱脚在墙构造器里向下 `raycast_simple(max = FLT_MAX)` 找落点，命中类型 1/2（【推测】地形/墙体）分别 `+1 m` / `−1 m` 修正，未命中则采地形高度 | `[ASM]` 140498BA8–140498CA7 |
| 每个拱脚出一个 `StairsPillarProposals::add`（提案），`process_playermade_stair_pillars_proposals` 再裁决（去重/合并），柱子轮廓是 `Rectangle2d`，拱脚两端向内缩 `±7.6 cm`、沿切向偏 `31.5 cm` | `[ASM]` 140498DC9、1404989C8、140498ACD |
| **拱数**（`preprocess_curve`，模式标志 `[rbp+318h]` 为真的那一支）：支撑曲线长 `L`；两端带标志位（`flags` bit0 / bit8，【推测】= 该端挂墙/接实体）的端各先扣 160 cm，`n = max(round((L − 扣除) / 400 cm), 1)` ⇒ **一拱约 4 m**；带标志的端再按 `min(36.8 cm / L, 0.2)` 的比例内缩。另一支（模式标志为假）用比例 `min(0.45, 200 cm / L)` 与 `0.9 − 内缩比例` 切曲线，未读完 | `[ASM]` 140780F24–14078113F（`__real@404ccccd`=3.2、`3fcccccd`=1.6、`3e800000`=0.25、`3ebc6a7f`=0.368、`3e4ccccd`=0.2） |
| 用户删掉的支撑存 `StairsRemovedSupports{ edges }`，作为装配**输入**；被删处改出 `playermade_stair_floating_bricks`（悬浮砖，`EasingFunction::apply` + `sinf` 做浮动动画） | `[PDB]` + `[ASM]` 调用表 |
| 平台楼梯 §4.2 另有一套 `construct_platform_stairs`，本文不展开 | `[PDB]` |

**未查清**（给后来者的线索）：`arches_n_walls` 常量池 `−100 / −2.5 / −2 / −1.5 / −1 / −0.2 / 0.1 / 0.2 / 0.3 / 0.375 / 0.4 / 0.5 / 0.56 / 1 / 1.5 / 3 / 1e6`（米）。
读到的两处判断：`height − x > 0.4` 走一支、否则 `+0.3`（140498DFB）；拱底 `= max(h − 2.0, …)`（14049861A）。
猜测「踏面离地 > 40 cm 才考虑出拱、拱顶在踏面下 ~2 m 内」，**未验证，不要当规格用**。

## 4. 与地形 / 墙 / 门的交互

### 4.1 贴地

| 规则 | 常量（cm） | 证据 |
| --- | --- | --- |
| 节点高度 = 地形（或平屋顶）+ 玩家偏移（0–10000） | — | §1.1 |
| 每个密采样点的高度 `≥ max(左右 1/4 宽处地形) − 10`，并记 `buried` 位 | 10 | §2.1 |
| 平走道分段（坡度 ≤ 0.25）重采样点 `y ≥ 地形 + 15` | 15 | §2.1 |
| 每级踏面 `≥ 地形(mid) + 15`；每级块至少向下 60（天然插进地里，不需要单独的「接地」几何） | 15 / 60 | §2.2 |
| 节点挂在平屋顶上时基面用 `flat_roof_y`，墙洞按 `flat_roof_y − 30` 起算（§4.2） | 30 | `[ASM]` 14001D900 |

### 4.2 墙：两类交互

1. **挂墙节点**（`DecoratorDst::Wall`）：节点朝向 = 墙切线的法向（`from_xz_forward_vector`，按在墙哪一侧取正负），`StairWallAnchor{wall, yaw}`。挂墙且宽 ≤ 150 cm 的那一端，踏步不做「高端加厚」（§2.2）。托架判定只在挂墙分段做（§3.2）。
2. **穿墙开洞**：装配时用楼梯碰撞体 `RaycastWorld::iter_shape_overlaps` 找重叠的墙，对每面墙 `[ASM]` 14001D68C–14001DB24：
   - 取墙形状（矩形 `Rectangle2d::as_points3` 或圆 `Circle2d::as_points3`，高度 `flat_roof_y − 30 cm`）；
   - 按 `WallStyle::is_halftimbered` 选一个常量：半木墙 `76 cm`、其他 `66 cm`（`__real@3f428f5d` / `__real@3f28f5c3`）【推测：洞沿墙方向的半宽/余量】；
   - 每个洞记录 24 字节 `(wall_id, Aabb2{ min = (s0, y0), max = (s1, 10 m) })` —— **洞顶恒为 10 m**，即把楼梯经过处的墙从 `y0` 往上整段切开；`y0`【推测】= 该处踏面高度；
   - 存进 `StairsVisualState`，由 `playermade_stairs_add_wall_holes` 原样发布：`WallHoles::add(wall_id, &Aabb2, HoleOrigin = 4)`（1407D208E）。
   ⇒ 这个系统本身零计算（卷三 §二 的推测被确认）。

`StairsCanDetachFromWall` / `DetachedFromWall` 只是 onboarding 提示事件（`NotifyHintSystem*`），**未查到**独立的「脱墙」几何规则；从数据模型看，「脱墙」= 节点的 `DecoratorDst` 从 `Wall` 变成 `Terrain`。

### 4.3 门

门与玩家楼梯**没有专门的连接逻辑**（§1–§5 读过的 playermade 函数调用表里没有任何 door 相关函数）；楼梯接到门口时靠的是「挂墙节点 + 穿墙开洞」。门自己的台阶是 §6 那套独立系统。

## 5. 栏杆

| 事实 | 证据 |
| --- | --- |
| 类型 `StairRailingType = { None, Low, High, Wooden }`，存在节点 `StairDecoratorInfo.railing`；UI 命令 `SetStairRailingCmd`，图标 `StairRailingIcon` ×4 | §1.1 |
| 石栏杆（Low / High）= 墙砖实例：`assemble_railings` 沿楼梯 3D 曲线 `try_resample` 后按 `random_splits` 切块；拐角 `add_corner_stone_railing_piece`（常量池 `27 / −26.8 / 10 cm`，未逐项归位）；端头 `railings_tips` 向两侧 `raycast_simple_exclude_wall_id` 探测是否贴墙再决定收头 | `[ASM]` 调用表 + 常量 |
| 木栏杆 = `construct_wooden_railing`，走木板实例缓冲，常量 `15.7 / 20 / 30 / 103.6 / 160 cm` | `[ASM]` |
| 栏杆开启时踏步砖横向**让出 25 cm**：`construct_stair_steps` 的 `narrow_by_railing` 参数就是节点 `StairDecoratorInfo+0x10` 字节 | `[ASM]` 1400187B7 → 14001A868 |
| `playermade_stairs_assemble` 里有 3 个 `assemble_railings` 调用点（不同分支）；读过的代码里**没见到**按离地高度自动开关栏杆的判断，栏杆只由节点字段决定 | `[ASM]` `playermade_stairs_assemble` 调用表 |

`+0x10` = `railing` 已由写入点确认 `[ASM]`：`set_decorator_color_id` 里三段写入依次是 `+0x08 u32`（ladder_color_id）、`+0x10 u8`（railing）、`+0x0C u32`（magic_color_id），与 serde 声明序一致，且 `+0x10` 是唯一的字节写入（1422AF933）。

**栏杆 ≠ None 时装配走另一条分支** `[ASM]` 14001A7D4–14001A8A9：先 `gen_stair_floors`（平台三角网格块），再 `construct_stair_steps(narrow_by_railing = 1, half_height = 1)` —— 踏步砖让出 25 cm、块高减半。【推测】两侧的石栏杆墙承担了侧面，踏面由地板网格块兜底。

**配色与栏杆按连通分量整体设置**：每个 `Set*Cmd` 处理时先 `StairsState::get_all_connected_nodes`，再把值写进分量里每个节点 `[ASM]` 1422AF6BB 等。

## 6. §4.0 门前踏步

> 与「窗贴近墙底变成门」直接相关，本节写成可照抄的规格。全部 `[ASM]`。

### 6.1 谁触发、什么时候判

- 判定函数 `gothic_balcony_door::check_for_door_stairs`（1407F3340）。cottage 风格的门在 `generate_cottage_balcony_doors` 里调用它（1407F5221），gothic 风格把同一段逻辑**内联**进 `generate_gothic_balcony_doors`（1407F239E 起，同样的 1.5 / −0.8 常量）。
  前置条件：门的 `DecoratorDst::as_wall()` 第 `+0x21` 字节为 1（【推测】= 落地层的门；否则走阳台分支）。
- 判定通过 → `DecoratorBlueprint::add_door_stairs(params)`：把 32 字节参数装箱挂到蓝图 `+0x120`（`Option<Box<DoorStairsAssemblyParams>>`）。判定不通过 → 同一个函数继续去生成**阳台**（`CurveSlice::extend_right_until` 那一支）。**门前踏步与阳台二选一。**
- 每帧装配在 `WallAttachedDecoTracker<DoorStairsAssemblyParams>::with_diffs`（1412AF8F2）里对**变化了的**门调用 `construct_door_stairs`；参数逐字节比较（`Option<Box<…>>::eq`）就是差分守卫。

### 6.2 判定规则（`check_for_door_stairs`，门变换 = `Quat + translation`）

```text
ground0   = max(terrain(door.xz), 0)                                  // 0 = 【推测】水面标高
door_bot  = door.y − 0.5·(ext.max − ext.min)                           // 门洞底
wet       = mask130(door.xz) > 0  &&  door_bot ≤ 10 cm                 // 【推测】水边门：mask 是 GardenRaster 里的某张 130 m 顶视图
if !wet && ground0 ≤ door_bot − 20 cm        → 不出（门底离地 > 20 cm：这不是落地门）
front     = door.xz + rotate(quat, (0, 0, 106 cm)).xz                  // 门外 106 cm
if ground0 − terrain(front) ≤ 10 cm          → 不出（门外没有下坡）
g_min     = min(terrain(front), terrain(front ± perp·w/2))             // 门宽两侧各采一点
H         = ground0 − g_min
if H ≥ 150 cm && !wet                        → 不出（太高，交给别的结构）
params = { door.x, door.z, forward.xz(归一), top = ground0,
           bottom = wet ? −80 cm : g_min, sink = max(−80 cm − g_min, 0), width = 门宽 }
```

⚠️ 读法：TG 的门前踏步**不是**「门悬在半空就补台阶」，而是「门槛贴地（≤ 20 cm），但门外 1.06 m 内地面下沉 10–150 cm」。门悬空（门底高出地面 > 20 cm）时直接不出。

### 6.3 砌法（`construct_door_stairs`，14200C7F0）

调用点把**总外伸固定为 56 cm**（`mov dword ptr [rsp+30h],3F0F5C29h`，0.56）。

| 量 | 规则 | 常量（cm） |
| --- | --- | --- |
| 级数 `N` | `clamp(ceil(H / 20), 2, 5)` | 20 / 2–5 |
| 每级进深 | `56 / N`（N=2 → 28，N=5 → 11.2） | 56 |
| 第 j 级位置 | 门中心沿朝向外推 `28 + j·(56/N)` | 28 |
| 水平砖层 | 从 `top` 到 `bottom − sink` 均分成 `L = max(ceil((H+sink)/20), 2) − 1` 层，内部分界各抖 ±2 cm（`random_splits`，见 6.4） | 20 / ±2 |
| 金字塔轮廓 | 自上而下第 k 层铺 `min(k+1, N)` 排 ⇒ 顶层 1 排贴门，往下每层多外伸一排；实际可见级数 = `min(L, N)`（例：H = 50 → L = 2 层各 25、N = 3，只出 2 级，每排进深 18.7） | — |
| 横向切砖 | 每排沿门宽取 `max(ceil(w/45), 3)` 个分界点（含两端）⇒ ≥ 2 块，内部分界抖 ±9 cm，范围 `[−w/2, +w/2]` | 45 / ±9 |
| 单块尺寸 | 横向 = 片宽；竖向 = 层厚；进深 = `56/N + 10`（前后搭 10 cm） | +10 |
| 输出 | 同 §2.2：墙砖实例 `BrickTransformSsbo`，`BrickSourceId::from_wall_id(门所在墙)` | — |

⇒ 踏步宽度 = 门宽（`params.width`，就是门装饰物的宽度字段），**不加宽**。踏高 = 层厚 `(H+sink)/L`（≥ 20 cm，±2 cm 抖动），不是 `H/N`；轮廓由「每层多一排」自然形成。

### 6.4 `utils::random_splits(n, jitter)` 的真实语义（两处共用）

`[ASM]` 140C90050：输出 **n 个点**（`assert!(n >= 2)`）：`0`、`k/(n−1) + (rand − 0.5)·j`（k = 1…n−2）、`1`，其中 `j = min(0.495/(n−1), jitter)`。
即「n−1 段近似均分 + 内部分界随机抖动」，**不是**最小片宽约束。§2.2 与本节的块数都按「点数 − 1」算。

## 7. 撤销 / 编辑形态（简述）

UE 侧不照抄，只记形态 `[ASM]`（调用表）+ serde 串：

| 层 | 做法 |
| --- | --- |
| 节点 | 走通用装饰物历史：`DecoratorHistoryEdit = { Change, Place, Remove, Rotate }`，状态快照 `DecoratorHistoryState`（16 字段，含 `stair_width / railing / stairs_supports / ladder_color / magic_color`） |
| 图 | `StairsHistoryDiff{ hash_before, hash_after, +2 }`：`extract_diff_and_make_backup` 克隆 IndexMap 做备份，对比 `iter_edges_and_stubs` 得到边的增删集；`apply_diff` 只做 `GraphMap::add/remove_node/edge`；`restore_snapshot = apply_diff + calculate_hash` 校验；`reduce_diff` 排序去重合并相邻 diff |
| 重放 | `replay_stairs` 读 `StairsReplayCmd` 改图；它的调用表里没有任何几何函数 ⇒【推测】几何交给下一帧装配重算，卷三「快照 + diff + 重放」的推测与之相符 |
| 脏标记 | 三层：①`playermade_stairs_interpolate_and_compare`：高度/宽度用 `time_continuous_lerp_factor(速率 9/s)` 平滑逼近目标，差值 < 1e-4 视为稳定，不稳定的边继续算；②`playermade_stairs_assembly_propagate_dirty_flags`：经 `tabloid::Subscriber::take` 取墙变化（调用了 `PublicWalls::get` / `try_as_wall`）拼成 `DirtyTiles`，落在脏瓦片里的边标脏；③`invalidate_dirty_stair_artifacts`：删碰撞体与 `StairsVisualState` 里对应边的产物 |

⇒ 拖动时楼梯「长出来」的动画感来自①的指数逼近，不是插值关键帧。

## 8. 资产清单

TG 源 JSON 实测（`D:/MyProject/Tiny Glade/assets/meshes/`，米；TG 是 **Y 向上**）。UE 侧同名资产在 `Content/HouseTest/TinyGladeAsset/Meshes/<name>.uasset`（平铺，无子目录）。

| 网格 | 额外属性 | 顶点 / 三角 | TG 尺寸 cm（X × Y↑ × Z） | UE 轴（X × Y × Z↑） | 用途（证据） |
| --- | --- | --- | --- | --- | --- |
| `brick` | `is_bevel` | 600 / 300 | 100 × 100 × 100，居中 | 100 × 100 × 100 | **玩家楼梯踏步、门前踏步、石栏杆、托架、拱墙、柱、悬浮砖全部用它** `[ASM]` 墙砖实例 |
| `wooden_plank` / `_lod1` / `_straight` | `Vertex_UV, is_bevel` | 888/296、24/12、228/76 | 100 × 100 × 100，Y ∈ [0, 1]（底面在原点） | 100 × 100 × 100 | 【推测】梯子 `construct_ladder`、木栏杆 `construct_wooden_railing`（二者写同一种非砖实例缓冲） |
| `stair_step` | `Vertex_Color` | 448 / 196 | 63.7 × 35.4 × 133.1，Z ∈ [−1.4, 131.7] | 63.7 × 133.1 × 35.4 | **调试网格**：注册在 `crates/country-core/src/startup/debug.rs` 那张表里（与 `decorator_test*`、`cube_grey`、`debug_sphere_*` 同表，shader `solid_vertex_color`）。玩家楼梯管线不引用它 ⇒ 卷三 U5「原点偏在一端的用意」结案：不用管 |
| `stairs_step` | `bevel_offset` | 300 / 100 | 100 × 100 × 100，居中 | 100 × 100 × 100 | §4.3 岩地台阶（exe 串 `stairs_step_mesh/rocky_terrain/stairs/stairs_indirect.raster.hlsl`） |
| `stairs_pebble` | `bevel_offset` | 96 / 32 | 132.4 × 127.7 × 135.2 | 132.4 × 135.2 × 127.7 | §4.3 岩地台阶碎石 |
| `wooden_gate/ladder` | `Vertex_Color` | 152 / 228 | 85.0 × 741.2 × 145.0（Y ∈ [−730, 11]） | — | 同在 debug 表（名 `ladder`）；UE 侧 `wooden_gate_ladder` |
| `clutter/ladder` | `Vertex_Color` | 396 / 132 | 108.2 × 204.9 × 77.7 | — | 杂物摆件，与楼梯无关；UE 侧 `clutter_ladder` |
| 平台地板 | — | 运行时生成 | — | — | `gen_stair_floors` → nani `Trimesh` 块（`StairsFloorMeshChunk`），无资产 |

UE 轴换算按「glTF Y-up → UE Z-up、×100」只交换尺寸分量；导入时 Y 是否取反没有核对（尺寸不受影响）。

## 9. UE 落地建议（MVP）

> 标注：**〔TG〕** = 上文反汇编确认的做法，照抄；**〔UE〕** = 本文给的适配建议，可以改。

### 9.0 先改掉两个过期假设

1. 卷三 §五「踏步 = `n = round(|rise| / 18 cm)`、run = length / n」来自用户的 Godot 原型，**不是 TG 做法**。TG 是「3D 弧长 / 50 cm 取整」（§2.1），踏高随坡度连续变化。〔TG〕
2. 卷三 §五「起点是 `ACSHouseActor::BuildFramePlan`」已过期：`CSHouseFrame.h` 头注释写明那条路（样条 + 逐砖记录）已在裁决一删除，现在是解析 `FElement` + `CSHouseFrame::Scatter`。MVP 不建议一上来接 GPU 散布，见 9.3。〔UE〕

### 9.1 `ACSStairsActor : ACSTinyGlade`

组件：`USplineComponent* Path`（默认两点 `(0,0,0)`、`(400,0,200)`）＋ `UCSGpuInstancedMeshComponent* StepBricks`（base mesh = `brick`）。

| UPROPERTY | 默认 | 出处 |
| --- | --- | --- |
| `float StepDiagonal` | **50 cm** | 〔TG〕`Curve::try_resample(0.5)` |
| `float Width` | 150 cm，`ClampMin=45 ClampMax=400` | 〔TG〕clamp 45–400；默认值〔UE〕（TG 默认未查到） |
| `float MinBlockHeight` | **60 cm** | 〔TG〕`max(Δy, 0.6)` |
| `float TreadAboveGround` | **15 cm** | 〔TG〕`terrain + 0.15` |
| `float MaxSinkBelowGround` | **10 cm** | 〔TG〕`max(h, ground − 0.1)` |
| `float TreadOverlap` | **1.13** | 〔TG〕砖进深 ×1.13 |
| `float FlatSlope` / `float LadderSlope` | **0.25 / 2.5** | 〔TG〕`determine_segment_type`（MVP 超过 2.5 先报警不出梯子〔UE〕） |
| `float SplitSingleBelow` / `float SplitRandom2Below` | **66 / 120 cm** | 〔TG〕 |
| `float BrickLenBase` / `FVector2D BrickLenExtra` | **46 cm / (70, 130) cm** | 〔TG〕 |
| `bool bTreadWear` | true | 〔TG〕踏面中凹（§2.2 顶面磨损）；UE 无 `sin_deform_y`，MVP 可关〔UE〕 |
| `int32 Seed` | 0 | 〔TG〕按「边身份」`init_rng_u64(StairGraphEdgeIdentity)` 定随机数；UE 用 `HashCombine(Seed, 段序号)`〔UE〕 |
| `TSoftObjectPtr<ACSGroundActor> Ground` | 空 = 场景里找第一个 | 〔UE〕 |

`ReevaluateSite()`（基类 `OnConstruction` 已调用）= 生成计划 → `StepBricks->SetInstances(Transforms, /*bWorldSpace*/true)`。

### 9.2 纯函数与算法（放 `CSStairsPlan.h`，照 `CSHouseLogicTests` 的纪律单测）

```cpp
namespace CSStairs
{
struct FPlanParams { float StepDiagonal = 50, MinBlockHeight = 60, TreadAboveGround = 15, MaxSink = 10,
                     TreadOverlap = 1.13f, SplitSingleBelow = 66, SplitRandom2Below = 120,
                     BrickLenBase = 46; FVector2f BrickLenExtra{70, 130}; };
enum class ESegKind : uint8 { Flat, Stairs, Ladder };

// ① 每个样条段 i→i+1：XY 走样条、Z 在两端点间按 XY 弧长线性插值（〔TG〕高度不进样条）
void SamplePath(const USplineComponent& S, float MaxSpacing /*50*/, TFunctionRef<float(FVector2D)> Ground,
                float Width, float MaxSink, TArray<FVector>& OutPts, TArray<int32>& OutSegOfPt);
// ② 〔TG〕s = |Δz| / |Δxy|（段首尾弦）
ESegKind ClassifySegment(const FVector& A, const FVector& B, float FlatSlope, float LadderSlope);
// ③ 〔TG〕N = max(round(L3D / Diagonal), 1)，输出 N+1 个 3D 等弧长点
void ResampleEqualArc3D(TConstArrayView<FVector> Pts, float Diagonal, TArray<FVector>& Out);
// ④ 〔TG〕相邻两点 = 一级；输出砖的世界变换（brick 是 100 cm 居中立方体 ⇒ Scale = 尺寸 / 100）
void BuildStepBricks(TConstArrayView<FVector> Steps, float Width, const FPlanParams& P,
                     TFunctionRef<float(FVector2D)> Ground, uint32 Seed, TArray<FTransform>& Out);
}
```

`SamplePath` 逐点：`z = lerp(zA, zB, t)`；`ground = max(Ground(xy ± perp·W/4))`；`z = max(z, ground − MaxSink)`。〔TG〕

`BuildStepBricks` 对每对 `(a, b)`（k = 级序号）：

```text
d      = b.xy − a.xy;  fwd = normalize(d);  right = (−fwd.y, fwd.x)
top    = max(a.z, b.z, Ground(mid.xy) + TreadAboveGround)
h      = max(|b.z − a.z|, MinBlockHeight)                     // 〔TG〕
if k 位于高端 56 cm 水平范围内: h += |z1 − z0|（首级踏高）       // 〔TG〕可选
depth  = |d| · TreadOverlap
w̄ = Width;  count = w̄ < 66 ? 1 : w̄ < 120 ? rand{1,2} : ceil(w̄ / (smoothstep(rand)·lerp(70,130,τ) + 46))
splits = RandomSplits(count + 1, jitter = max(0.3ℓ, 32)/w̄)    // §6.4 语义，照抄
for j in 0..count-1:
   u0, u1 = splits[j], splits[j+1];  uc = (u0+u1)/2 − 0.5
   center = (mid.xy + right·uc·w̄, top − h/2)
   Out += FTransform(FRotationMatrix::MakeFromXZ(fwd3, Up), center, (depth, (u1−u0)·w̄, h) / 100)
```

踏面磨损（中间砖下沉 `(1 − |2u−1|)·m`）在 UE 没有逐角偏移通道，MVP 用「整块中心下沉 `(1 − |2uc|)·m·0.5`」近似〔UE〕；首末级不沉〔TG〕。

### 9.3 贴地、支撑、墙：MVP 取舍

| 项 | MVP 做法 | 为什么 |
| --- | --- | --- |
| 贴地 | `ACSGroundActor::SampleHeight(FVector2D)`（镜像双线性，零回读）喂 `Ground` 回调 | 〔UE〕与 TG 的 `TerrainHeightsData::sample_at_world_xz` 同语义 |
| 支撑 | **不做拱**。每块踏步块高改为 `max(h, top − ground(mid) + 10)`（整柱落地），离地高时视觉是实心台基 | 〔UE〕TG 的拱墙是 `ArchWalker + WallConstructor` 整套，阈值未查清（§3.3）；块本来就 ≥ 60 cm，低楼梯与 TG 观感一致 |
| 支撑 v2 | 离地 > 某阈值的连续段 → 沿楼梯 XY 路径出一段「带拱洞的墙」，复用房屋墙砖/门拱砖设施（TG 就是复用墙构造器） | 〔TG〕结构；阈值待补 |
| 托架 | 不做；v2 按 §3.2 射线判据（踏面下 117 cm 朝墙 178 cm）做 | 〔TG〕判据确凿 |
| 穿墙 | 不做；v2 用 `FCSWallOpening` 登记**矩形**洞：半宽 66 cm（半木墙 76 cm），从踏面挖到墙顶。TG **没有**斜洞（`Skew/AxisUS` 用不上） | 〔TG〕§4.2 |
| 栏杆 | 不做；v2 `Low/High` = 沿两侧边线的砖带（可复用 `CSHouseTrim` 的「离散块沿线累积」），同时踏步 `Width − 25` | 〔TG〕§5 |
| 梯子 | 坡度 > 2.5 的段先不出几何、在编辑器里报警 | 〔UE〕 |
| 撤销 | 用 UE 事务（样条组件自带），不做图状态 | 〔UE〕§7 |
| 刷新 | `SetInstances` 每次 rebuild 一次 flush；楼梯砖数百量级可接受。要零 flush 再迁到 `CSHouseFrame::Scatter` 的 `FElement` 路 | 〔UE〕 |

### 9.4 门前踏步（给「窗贴墙底变门」那条线）

建议做成**房子自己的纯函数**，不走楼梯 actor〔UE〕；规则全部照抄 §6〔TG〕：

```cpp
// 返回 false = 不出。Door* 全部世界空间 cm；Ground = SampleHeight
bool CSDoorSteps::Build(FVector2D DoorXY, FVector2D Outward, float DoorBottomZ, float DoorWidth,
                        TFunctionRef<float(FVector2D)> Ground, uint32 Seed, TArray<FTransform>& OutBricks);
//  ground0 = Ground(DoorXY);  if (ground0 <= DoorBottomZ − 20) return false;          // 门悬空：不出
//  front = DoorXY + Outward·106;  if (ground0 − Ground(front) <= 10) return false;     // 门外不下坡：不出
//  gmin = min(Ground(front), Ground(front ± perp·W/2));  H = ground0 − gmin;  if (H >= 150) return false;
//  N = clamp(ceil(H/20), 2, 5);  run = 56/N;  L = max(ceil(H/20), 2) − 1 层（RandomSplits 抖 ±2）
//  第 k 层（自上而下）铺 min(k+1, N) 排；第 j 排中心沿 Outward 距门 28 + j·run；块进深 run + 10
//  每排横向 max(ceil(W/45), 3) 个分界点（抖 ±9）；块尺寸 = (片宽, 层厚, run + 10)
```

⚠️ 「窗贴墙底变门」若产生的是**门底高出地面**的情况，TG 规则直接不出踏步（`ground0 ≤ door_bottom − 20`）。要不要为「悬空门」补台阶是本项目自己的裁决，TG 没有对位物。

## 附：搜索记录与未查到项

### 方法与中间产物

- 反汇编：`dumpbin /DISASM:NOBYTES` 全量流式过滤（三轮关键词 + 一轮「按被调函数反查调用者」），单函数用 `dumpbin /DISASM:BYTES /RANGE:` 精读并解析 RIP 相对引用（字符串 / 浮点 / 跳表）。脚本在会话 scratchpad，不入库。
- 字段名：PDB 无 Rust 类型 ⇒ 改从 exe 的 serde 字符串池与 `VARIANTS` 胖指针数组恢复（§1.0）。
- 证据片段：[`evidence/stairs-playermade-20260916.asm`](evidence/stairs-playermade-20260916.asm)（只留本文引用的函数）。

### 未查到 / 未查清

| 项 | 试过什么 | 下一步线索 |
| --- | --- | --- |
| 楼梯默认宽度 | `ui_focus_stairs` 只有 clamp（45–400），`continue_stairs` 复制端点 | 放置工具里 `DecoratorExtraInfo::Stair` 的构造点（搜 `From<StairDecoratorInfo>::from` 的调用者） |
| 拱 / 柱的触发高度（拱数已查到，见 §3.3） | 读了 `split_segment…`（托架判据确凿）、`preprocess_curve`（拱数）、`arches_n_walls` 的常量与两处比较 | 继续读 `arches_n_walls` 140498300–140498DDF 与 `ArchWalker::walk`（14048AB50）；【推测】`StairsArchSupportRequests` 在 `playermade_stairs_assemble` 14001C445 附近写入（0xA8 字节记录），另有 14001BA41 处 0xB8 字节记录 |
| `g` 数组（装配参数 `+0x88`）确切语义 | 只看到它进 `construct_stair_steps` 与「全 > 0」检查 | 找 `+0x88` 的写入点（`compute_stair_assembly_params` 的 from_iter 闭包 `h12718f9f…` / `h1df2cfdd…`） |
| `AlignKind` 三变体的名字 | 无 Debug/serde 串 | 只能靠行为；§2.3 已给行为 |
| 梯子几何 | 只取了常量池（59.3 / 22.2 / 16 / 40 / 65 cm）与缓冲类型 | 读 `construct_ladder` 1407DB3E0 |
| 平台 `gen_stair_floors` 何时出 | 确认在「栏杆 ≠ None」分支调用；平走道分段是否也出未确认 | 读 14001CFFC 那个调用点的前置条件 |
| `StairsHistoryEdit` 变体名 | 猜词搜 `VARIANTS` 未命中 | 读其 `deserialize::visit_enum` |
