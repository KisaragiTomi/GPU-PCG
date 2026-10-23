# TinyGlade 屋脊长度与山墙逆向（附录 F）

> 2026-10-02 · 只读调研 · 证据等级：`[ASM]` dumpbin 反汇编 + PDB 符号 / `[DATA]` exe 数据段（serde 表、跳转表）/ 【推测】
> TG 单位是米；本文正文一律换算成 cm，地址是 `tiny-glade.exe` 内的 VA。反汇编摘录见 [`evidence/roof-ridge-gable-20261002.asm`](evidence/roof-ridge-gable-20261002.asm)（21 段，文件头有读法速查）；`assemble_rectangular_roof` 全文在 [`evidence/roof-tiles-assemble-20260929.asm`](evidence/roof-tiles-assemble-20260929.asm)。
> 状态：✅ 用户 2026-10-02 提的三件事（脊长可调、脊长 = 房长时四坡变双坡、端墙变三角形）在 TG 侧都有确凿对位；未坐实项列在 §8。本项目的落法见 §6 与 [`Roof_TerraceAndRidge.md`](Roof_TerraceAndRidge.md#脊长可调与双坡山墙2026-10-02)。

## 0. 结论速览

| # | 问题 | 结论（cm） | 等级 |
| --- | --- | --- | --- |
| 1 | 脊长是不是参数 | 是存盘参数 `RoofShapeParams::ridge_length_01`（`Roof` + 0x34）。脊长 = 10 + r·(顺脊边长 − 10)；另一维恒 10。只对矩形成立，圆形恒是一个尖 | [ASM] |
| 2 | 怎么调 | 屋脊两端各一个抓手，沿脊向拖：`len = 2·|P − C| − 起拖偏移`（关于脊中点对称），再 `r = clamp((len − 10) / (边长 − 10), 0, 1)`。没有吸附、死区或滞回 | [ASM] |
| 3 | 何时变双坡 | `Roof::is_gable` ⇔ `ridge_length_01 == 1.0` 且 `height_01 > 0.1`。**精确相等**，1.0 只来自上一行那个 clamp 的饱和 | [ASM] |
| 4 | 双坡时屋面怎么变 | 两端的坡面不发瓦（`gable_sides` = 边号 `dir`、`dir \| 2`）；檐口矩形与脊矩形顺脊改成同长 ⇒ 两侧坡面是矩形。四条角线盖板照发，退化成两端的封边板 | [ASM] |
| 5 | 端墙怎么变三角形 | 屋顶不封端，是**墙**升上去：山墙边逐点墙顶 += c + H·(1 − s / W)^k。砖层按行中线逐层收短成台阶，角石砌到抬高的角点，抹灰栅格把顶线以上的顶点标成墙外 | [ASM] |
| 6 | 两种山墙 | `GableRoofStyle`：0 = `Stepped`、1 = `Overhang`。**不是玩家选项**：恒等于墙体是不是半木构。砖石 / 灰泥墙 = Stepped：屋面每头缩进 26.5、墙比屋面线高 46；半木构 = Overhang：屋面每头挑出 26.5 + 檐口出挑、墙贴屋面线 | [ASM][DATA] |
| 7 | 新建房子的默认脊长 | `ridge_length_01_from_rectangle_ratio(a, b)`：长边 ≥ 750 且长宽比 ≥ 1.5 的矩形**默认就是双坡**；脊向沿长边（平局归 X） | [ASM] |
| 8 | 换脊向 | 点击脊中点处的抓手，只写 `ridge_dir`，`ridge_length_01` 原值套到另一条边上 | [ASM] |
| 9 | 塔尖 | 只有脊矩形长宽比 < 1.01（没有脊）才放一根；有脊的四坡与双坡都没有 | [ASM] |

## 1. 数据模型

`Roof`（0x58 字节，serde 字段表在 `.rdata` 142C8F938）：

| 偏移 | 字段 | 说明 |
| --- | --- | --- |
| +0x08 | `wall_shape` 的种类 | 1 = 矩形；+0x0C 朝向 (cos, sin)、+0x14 中心、+0x1C 尺寸 |
| +0x24 | `tip_offset_01` | 矩形屋顶不吃它：`assemble_rectangular_roof` 与 `to_world_space` 都把它乘 0（1421AEFF5、1408E2822） |
| +0x2C | `profile` | 剖面指数 k = lerp(0.5, 1.5, profile)；默认 0.5 ⇒ k = 1（直线） |
| +0x30 | `height_01` | `Roof::height` = lerp(40, 1200, height_01)；≤ 0.1 是平顶 |
| +0x34 | `ridge_length_01` | 本文主角 |
| +0x38 | `eave_01` | 檐口出挑 `eave_length_ws`，默认 0.2 ⇒ 约 25.6 |
| +0x3C | `ridge_dir` | 0 = 脊沿局部 X，1 = 沿局部 Y |
| +0x4C | `pivot_y` | = 墙的 `max_y`（墙顶） |
| +0x54 | `gable_style` | 0 = `Stepped`，1 = `Overhang`（VARIANTS 表 142C8F798） |

矩形的角与边号（`Rectangle2d::as_points2` 140C98B0E）：角 0 = (−x, −y)、1 = (−x, +y)、2 = (+x, +y)、3 = (+x, −y)，边 i = 角 i → 角 i+1。于是 `gable_sides` = (`dir`, `dir | 2`)（1408E33E0）：`dir = 0` 时两条山墙边是边 0 / 2（长 = `size.y`），正是垂直于脊的那一对。

## 2. 脊长参数

### 2.1 参数 ↔ 脊长

```text
RoofRidgeDims::calc(r, dir, sx, sy)          1408E2700
    顺脊那一维 = (1 − r)·10 + r·顺脊边长          # r = 0 → 10 cm（一个尖），r = 1 → 与房子一样长
    另一维     = 10
    非矩形     = (10, 10)                         # RoofRidgeDims::from 1408E2690

inverse_ridge_01_param(len, dir, sx, sy)     1408E30F0
    r = clamp((len − 10) / (顺脊边长 − 10), 0, 1)   # 1408E3127 maxss / 1408E3152 minss
```

`to_world_space`（1408E2750）：`max(dims) ≤ 10` 给一个点（尖），否则给两个端点 = 矩形中心 ∓ dims / 2，高度 `pivot_y + Roof::height`。**脊恒以矩形中心为中点。**

### 2.2 山墙判据

```text
Roof::is_gable        1408E33B0     ridge_length_01 == 1.0 (cmpeqps) && height_01 > 0.1
Roof::gable_sides     1408E33E0     is_gable ? Some((dir, dir | 2)) : None
```

r 从 0.999… 到 1.0 是**几何上的不连续**：r → 1⁻ 时端坡面的水平进深下限是 (28 + eave) / 2，不会变竖直；到恰好 1.0 才整体切到山墙分支。

### 2.3 新建矩形房子时的默认值

`country_core::…::rectangle::create_rectangle`（140A93DF7–140A9416A，三个辅助函数被内联）写进 `CreateRoofCmd`：

```text
tip_offset_01   = (0, 0)                                    140A9413A
profile         = 0.5                                       140A94143
eave_01         = 0.2                                       140A94159
ridge_dir       = (size.y > size.x)                         140A94161     # 沿长边，平局归 X
ridge_length_01 = ridge_length_01_from_rectangle_ratio(size.x, size.y)   # 1408E3B60
    s = smoothstep01((max(a, b) − 225) / 525)
    r = smoothstep01((max(a/b, b/a) + 1.5·s − 1.5) / 1.5)
```

按公式代入（双精度算的样例，不是从 exe 读的数）：

| 尺寸 cm | `ridge_length_01` | 默认形态 |
| --- | --- | --- |
| 300 × 300、400 × 400 | 0 | 攒尖（有塔尖） |
| 600 × 600 | 0.453 | 四坡 |
| 800 × 800 | 0.741 | 四坡 |
| 600 × 400 | 0.898 | 四坡 |
| 800 × 600 | 0.966 | 四坡 |
| 800 × 500、1000 × 600、300 × 100 | 1.0 | **双坡 + 山墙** |

`update_roof_spatial`（1421A7710）在墙形变化时只同步 `wall_shape` / `pivot_y` / `gable_style`，**不重算**这三个默认值 —— 它们只在创建那一刻取一次，之后脊长是比例、跟着房子伸缩。

## 3. 屋面

### 3.1 0 ≤ r < 1：檐口矩形与脊矩形之间的放样面

`get_roof_expanded_top_and_bottom`（140A880C0）：檐口矩形 = 墙矩形 + 28 + `eave_length_ws`（两轴整边加量，即每侧 14 + eave / 2）；脊矩形 = `RoofRidgeDims`（用未外扩的墙尺寸）。`assemble_rectangular_roof`（1421AEEF0）的面 i 第 j 排 = 两个矩形对应边之间按 t 线性插值，高度 = `Roof::height`·t^k。

四个面共用同一条剖面与同一个高度，水平进深不同 ⇒ **两端坡面与两侧坡面的坡度不同**：两侧进深 = 檐口半宽 − 5，两端进深 = 檐口半长 − 脊长 / 2。每面各自按自己的坡长取排数（`max(ceil(坡长 / 43.75), 2)`，1421B0F37）。

### 3.2 r == 1：山墙分支

头部的改写（1421AF05F–1421AF110）：

```text
c = (gable_style == 0) ? 81 : 25             1421AF07E / 1421AF0BE
d = c + eave_length_ws                       1421AF0AA
if gable_style == 0 { d = −d }               1421AF0F2
檐口矩形.顺脊长 += d                          1421AF0FD
脊矩形.顺脊长    = 同一个值                    1421AF101     # 上下同长 ⇒ 两端的面是竖直的
```

代入檐口矩形原长（墙 + 28 + eave）：

| 样式 | 屋面顺脊总长 | 每一头相对墙矩形 |
| --- | --- | --- |
| `Stepped`（0） | 墙长 − 53 | **缩进 26.5**（与 eave 无关） |
| `Overhang`（1） | 墙长 + 53 + 2·eave | **挑出 26.5 + eave** |

其余几处：

| 项 | 读数 | 地址 |
| --- | --- | --- |
| 发哪些坡面 | 面号等于 `gable_sides` 任一个就 `continue`，只剩两侧的矩形坡面 | 1421B0B96–1421B0BB1 |
| 四条角线盖板 | **不跳过**（循环体里没有任何山墙判断）。檐口角与脊角顺脊坐标相同 ⇒ 四条线退化成屋面两端从檐口爬到脊的边线，那四排 25 宽的窄板成了**封边板** | 1421AF1AC–1421AFBF6 |
| 逐瓦裁剪面 | 不跳过；两端的裁剪面退化成垂直于脊的竖直面，在屋面端头向内 7.5 | 1421B0C66–1421B0E99 |
| 水平脊盖板的两端 | 端点先取未改写的 ±墙长 / 2，再：`Overhang` 两头各 +56，`Stepped` 两头各 −28 | 1421AFE93–1421AFF59 |
| 屋顶端头由什么封 | 这个函数什么都不发，封端的是墙（§4） | 1421B1FE5 起只剩释放与返回 |

### 3.3 塔尖

`place_roof_spires` 的闭包（1419EE9E0）：`max(d.x / d.y, d.y / d.x) < 1.01` 才放（d = `RoofRidgeDims`）。只有 r ≈ 0 的攒尖顶与圆顶有一根塔尖；有脊的屋顶一根都没有，山墙没有任何特殊处理。

## 4. 山墙墙体

### 4.1 墙顶：`init_wall_height_and_arch_segment_height`（14078B550）

房子的墙顶本来是一个常数 `max_y`。矩形 + 有屋顶 + `is_gable` 时（14078BC9A–14078C424），对落在两条山墙边上的每个路径点：

```text
墙顶[i] += c + H · (1 − s / W)^k            14078C1AB–14078C1FF
    s = 该点到山墙中线的水平距离
    H = Roof::height()
    k = lerp(0.5, 1.5, profile)
    c = 半木构 ? 0 : 46                      14078C19A–14078C1A3
    W = 半跨 + 28 + (半木构 ? 0 : (2·profile − 1)·eave)     14078BFB2–14078BFF5
max_y_gable = max_y + H + 46                14078C404–14078C424
```

- W > 半跨，所以墙角处没落回 0：角点也被抬高（Stepped 至少抬 46）。四个角是共享路径点 ⇒ 檐墙在离角不到一个采样段（约 50）内有一小段回落。
- 路径点沿每条边约 50 一个、边的中点必是采样点（`crashy_resample_remap_per_segment_2` 14146BB70，矩形强制奇数点）⇒ 山墙尖的高度是精确的。
- **墙顶线不是屋面的精确平移**，只是同指数的近似：墙用的 W 与屋面檐口半宽（半跨 + 14 + eave / 2）不是同一个数。

### 4.2 砖层

| 步骤 | 读数 | 地址 |
| --- | --- | --- |
| 行高带 | 山墙时多切一带 `[max_y, 最高墙顶]`，檐口高度处必有一条行界 | `from_curve` 140769B7D–140769C8E |
| 行怎么收口 | 一行只存在于「线性插值的墙顶 ≥ 该行中线」的区间 ⇒ 逐层收短成台阶 | `trim_rows` 14123EEF0、`split_heightmap_by_threshold` 141FE0750 |
| 行端吸附 | 山墙 0.2 块砖 = 9.2；非山墙 0.5 块 = 23（一块砖 46） | 14123F249–14123F278 |
| 不足一块砖的行段 | 山墙**加宽**成约一块砖（新宽 = 旧宽 / 4 + 0.75 砖）；非山墙丢弃 | 14123F323–14123F3B7 |
| 行端抖动 | 山墙时关掉（`jitter = 0`）⇒ 台阶边是干净的 | `construct_walls` 140758FE5 |
| 越线的砖 | `Stepped` 整砖不裁；半木构给越线砖打 `0xC` 标志并带上三点墙顶线，用途【推测】是逐砖顶裁 | 14076C1C3–14076C386 |
| 角石 | 砌到被抬高的角点；`max_y` 以上宽 56，以下 92 ± 16 | `add_wall_corners` 141215A91–141215F03 |

### 4.3 抹灰

`split_wall_into_sides`（14075DC60）：山墙边的那一面是一直到屋脊高度的矩形栅格（40 一格），只多一个 `gable` 标志。`generate_plaster_mesh_and_entities`（1412269B0）在山墙面上复用同一份逐点墙顶（1412274C7–1412274F6）：高于墙顶线的顶点标 `outside`（1412278F0–141227908），边缘距离按「墙顶线 − 46」算（1412279C8–1412279E5）。⇒ 抹灰的上边界比墙顶线低 46：`Stepped` 时正好落在名义屋面线上。

## 5. 交互

### 5.1 脊长抓手：`ui_edit_roof_ridge_length`

`create_ui_widget`（1421AA170）给每栋矩形坡屋顶建 **2 个**抓手，各在脊端点沿脊向外：

| 量 | 值 | 地址 |
| --- | --- | --- |
| 触发盒中心 | 端点 + 外向 × 75 | 1421AA44D |
| 触发盒尺寸 | 100 × 100 × 150（长轴沿脊）⇒ 从端点起向外伸 150 | 1421AA58F |
| 箭头网格 | `brush_arrow_flat_single_foward`（原文如此拼写），缩放 = 2（悬停 / 拖拽 × 1.2），位置 = 端点 + 外向 × 20 | 1421A9EF0–1421AA08C |
| 何时可见 | 未聚焦时只有光标压到隐形盒子上才显示；选中那栋房（`Editing(Wall \| Roof)`）时两只常显 | 1421A8210、1421AA78E |
| 不出抓手 | 平顶（`height_01 ≤ 0.1`）、非矩形 | 1421AA211–1421AA223 |

拖拽（`do_ui_edit_roof_ridge_length` 1421A8C50）：

```text
C = 两个脊端点的中点；P = 光标投到「过 C、沿该抓手外向」的射线上（参数钳在 ≥ 0）
起拖帧：  offset = 2·|P0 − C| − 当前脊长          1421A963C–1421A9689     # 不发命令
拖拽中：  len = 2·|P − C| − offset                1421A9794–1421A97C1
          r   = inverse_ridge_01_param(len, …)
          EditRoofCmd { ridge_length_01: Set(r) }  1421A97ED–1421A9854     # 其余字段全不动
```

- 关于脊中点对称：拖一头，另一头镜像移动。
- 没有吸附阈值、死区或滞回；`len ≥ 边长` ⇒ r 恰好 1.0 ⇒ 山墙。
- 高度抓手与脊长抓手互不影响（各自只写自己的字段）。

### 5.2 `edit_roof`（1421AB7C0）在脊长变化时做什么

写 `ridge_length_01` 前后各取一次 `is_gable`（1421ABD97 / 1421ABDDB）：翻转时发 `RecalculateGableRoofWallHeights` 与音效 0x0A（变成山墙）/ 0x0B（离开）。该事件被 `publish_public_wall_state` 原样翻译成 `OnWallChanged` → 重算墙顶 → 重砌砖（140A8B8AD–140A8B9C8）。保持山墙但改了剖面 / 高度 / 檐口也重算。

### 5.3 换脊向

`ui_edit_roof_ridge_dir`：点击脊中点处的 100 × 100 × min(脊长, 200) 盒子（单尖顶没有）。`switch_roof_ridge_direction`（141A3AA2B–141A3AAF7）**只写 `ridge_dir`**，`ridge_length_01` 不动 —— r = 1 的山墙换向后仍是山墙；高度、檐口也不动。

## 6. 本项目的落法

| TG | 本项目 | 说明 |
| --- | --- | --- |
| `ridge_length_01` | `ACSHouseActor::RoofRidgeLength01` | `< 0` = 自动（本项目原有的等坡度四坡，旧存档都是它）；`[0, 1]` 是 TG 的口径 |
| `ridge_dir` | `RoofRidgeAxis` | 只在显式脊长时读；自动时脊恒沿长轴 |
| `gable_style` | `GableRoofStyle` | 默认 `Stepped` |
| `RoofRidgeDims::calc` / `inverse_ridge_01_param` / 默认公式 | `CSHouseRoof_RidgeLengthFrom01` / `_Ridge01FromLength` / `_DefaultRidgeLength01` | 逐式照抄 |
| 放样面 | `FCSRoofDesc::EdgeSteep`（逐边坡度倍率）；`CSHouseRoof_ApplyRidge` 一处换算 | 端坡面倍率 = (半跨 + 外挑) / (半长 + 外挑 − 脊长 / 2) |
| 山墙分支 | 倍率 ≤ 0 的边 = 山墙边；`FCSRoofDesc::GableOverhang` = −26.5（Stepped）或 26.5 + eave（Overhang） | `CSHouseRoof_GableOverhang` |
| 封边板 | 骨架弧的 `bVerge`，照铺斜脊窄板 | `CSHouseTile::BuildPlan` |
| 墙顶抬高 | `CSHouseRoof_BuildGableProfile` → `ACSHouseActor::BuildGableWalls`；房体三角汤、砖层、角石共用 | Stepped 再加 46 |
| 砖层收口 | `CSHouseBrickWall::BuildGableCourses`：行中线判据 + 0.2 砖吸附 + 不足一块加宽 | 同 `trim_rows`；开了 `bBrickWallEnabled` 时整面山墙都砌 |
| 灰泥只抹到「墙顶线 − 46」 | `FCSHouseGableWall::PlasterDrop`：阶梯山墙的墙板只做到屋面线；上面那一圈由同一个发射器只出露在灰泥外的砖（`bTrimGable`） | 默认墙是单层墙板，灰泥下面的砖不出 |
| 脊长抓手 | `ACSHouseRidgeHandleActor` × 2 → `ACSHouseActor::PushRidgeLength` | 位移 δ ⇒ 脊长 + 2δ |
| 换脊向 | `SwitchRoofRidgeDirection`（细节面板按钮） | 只换轴，比例原样 |
| 新建默认值 | `ResetRoofRidgeToTinyGladeDefault`（按钮） | 不是默认行为，见下 |

有意与 TG 不同的地方：

| 项 | TG | 本项目 | 原因 |
| --- | --- | --- | --- |
| 默认脊长 | 新建时按长宽比公式取 | 自动 = 等坡度四坡（脊长 = 长边 − 短边） | 2026-08-31 以来的存量房子与关卡不能变样；要 TG 默认值按一下按钮 |
| 山墙样式 | 墙体样式的派生量 | 可选属性 | 本项目没有半木构墙；默认值取 TG 给砖石墙的那一种 |
| 剖面 | `H·t^k`，k 可调 | 只有直线（k = 1 = TG 默认 `profile` 0.5） | `ProfileCurvature` 全库未实现（计划 D4 开放项） |
| 山墙顶线 | 同指数近似（§4.1） | 与屋面同一个求值器逐点求值 | 墙与屋面不会一个陡一个缓 |
| 檐口外扩 | 每侧 14 + eave / 2 | 每侧 `RoofOverhang`；折回 eave = 2·(外挑 − 14) | 本项目没有 eave 这个量 |
| 逐瓦裁剪面 | 角线竖直面向内 7.5 | 两面等高线，不内缩 | 09-30 已定的斜脊收口口径 |
| 水平脊盖板的两端 | 墙端 +56（悬挑）/ −28（阶梯） | 屋面端头：+26.5 + eave / −26.5 | 脊线取自骨架，与屋面同长；盖板段长另有 1.4 倍搭接 |
| 灰泥的上界 | 栅格顶点的软边（边缘距离）| 墙板几何硬停在屋面线，露砖是实例砖 | 本项目的墙是单层墙板 + 砖实例 |
| 排数 | 每面 `ceil(坡长 / 43.75)`，排线按归一化剖面弧长等分 | 每面 `round(坡长 / 排距)` 等分 | 09-29 已定：沿用本项目的等分排线 |
| 塔尖 | 有脊就没有 | 四坡保留尖顶，双坡不立 | 尖顶是 08-31 用户要的；双坡照 TG |
| 抓手显隐 | 悬停才出 / 选中常显、悬停放大 1.2、音效 | 进拉尺寸模式常显，无放大与音效 | 与其余抓手一致 |
| 拖到头的容差 | 无（山墙状态下按住不动可能掉出 1 ulp） | 双精度换算，原地不动恒为 1 | `CSHouseRoof_Ridge01FromLength` |

## 7. 常量表

| hex | 值（m） | 用处 | 地址 |
| --- | --- | --- | --- |
| `3dcccccd` | 0.1 | 脊长下限、脊矩形的另一维、平 / 坡分界 | 1408E26A7、140A8812E、1408E33C6 |
| `3f4f5c29` | 0.81 | `Stepped` 的顺脊修正 c | 1421AF07E |
| `3e800000` | 0.25 | `Overhang` 的顺脊修正 c；角线盖板宽 | 1421AF0BE、1421AFAE0 |
| `3e8f5c29` | 0.28 | 檐口矩形固定外扩；山墙轮廓半宽的外扩 | 140A88107、14078BFF5 |
| `3f0f5c29` | 0.56 | 山墙脊盖板端点修正（`Stepped` 再乘 −0.5） | 1421AFF05、1421AFF3D |
| `bd99999a` | −0.075 | 逐瓦裁剪面的内缩 | 1421B0E39 |
| `3eeb851f` | 0.46 | `Stepped` 山墙高出屋面线；`max_y_gable` 的加量；一块砖的标称长度 | 14078C1A3、14078C404 |
| `3e4ccccd` | 0.2 | 山墙行端吸附（× 砖长）；箭头离端点；默认 `eave_01` | 14123F249、1421A9FD1、140A94159 |
| `3f400000` | 0.75 | 抓手触发盒中心离端点 | 1421AA44D |
| `3f800000` / `3fc00000` | 1.0 / 1.5 | 触发盒尺寸 | 1421AA45E、1421AA466 |
| `c0100000` / `40a80000` | −2.25 / 5.25 | 默认脊长公式里的尺寸项 | 140A93E46、140A939AC |
| `3f8147ae` | 1.01 | 塔尖：脊矩形长宽比阈值 | 1419EEA62 |
| `3ee00000` | 0.4375 | 排距基数（逐面按坡长取排数） | 1421B0F37 |

## 8. 没能确定的

- 砖实例标志位 `0xC` 在着色器里做什么（§4.2 的「逐砖顶裁」是推测），抹灰 `outside` / 边缘距离的后续用法。
- 屋面相对 `pivot_y` 的真实偏移（瓦厚、剖面端点）没读，所以「山墙顶与瓦面之间的净空」只有墙侧公式，没有一个数。
- `PlayRoofSound` 0x0A / 0x0B 与 `GableAppear` / `GableDisappear` 的对应是按语义与字符串相邻推的。
- `profile < 0.5` 且檐口大时 §4.1 的 W 可能小于半跨（`powf` 负底数），没看到防护，参数域是否到得了未验证。
- 改矩形尺寸的系统（`do_ui_edit_rectangle_dims` / `finish_edit_rectangle`）是否重算屋顶参数没读；只确认了 `update_roof_spatial` 不重算。
- 半木构山墙三角区内木梁的排布（`half_timbering` 只读了问到的两处）；本项目没有半木构墙，不影响落地。
- TG 山墙状态下「按住抓手不动」会不会掉出山墙：反汇编里没有容差，按 f32 运算顺序离线模拟约 45% 的摆放会得到 r < 1，**没有运行时观察**。
