# 藤蔓在障碍处的转向（2026-09-12）

本轮处理一条画面缺陷：`CSHouseVine::BuildPlan` 的墙面游走在**遇到墙洞或墙角时会出现巨大的折线夹角** ——
相邻两段之间一次折过 100° 以上，视觉上藤在障碍处"折断"式急拐。病因不在参数取值上，而在算法形态：
旧代码在障碍处**直接改写倾角**（`Angle = -Angle` / `Angle = 0`），而倾角是相对竖直方向的**绝对**偏角，
取反一次相邻段的夹角就正好是 `2·|Angle|`，`MaxLean` 默认 1.15 rad ⇒ 上界 **2.30 rad ≈ 131.8°**。
单测实测的基线是 **100.4°**（见 §五）。

修正依据来自 Tiny Glade 本体的反汇编：**TG 的藤蔓里没有任何"镜像/归零"式的离散改向**。
下面先给取证，再给改动与验证。

## 一、取证方法

本机没有 Ghidra/IDA，用 MSVC 的 `dumpbin` 够用 —— PDB 就在 exe 旁边，能解 Rust mangled 名：

```powershell
# 反汇编 58 MB 的 exe 上 GB，必须流式过滤、不落全量磁盘（实测一次 15 秒左右）
& "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.43.34808/bin/Hostx64/x64/dumpbin.exe" `
    /DISASM:NOBYTES "D:/MyProject/Tiny Glade/tiny-glade.exe" |
  python grab_funcs.py ivy_direction_proposer ivy_grower intersect_ivy_growth `
      closest_segment_index get_wall_double_sided_normal check_for_wall_jump `
      wall_coord spawn_ivy is_point_still_valid remove_ivy_under_windows
```

`grab_funcs.py` 是十几行的流式过滤器：顶格且以 `:` 结尾的行是函数标签，按关键词只保留命中的函数体。
捞出来的证据存 [`evidence/ivy-growth-turning-20260912.asm`](evidence/ivy-growth-turning-20260912.asm)（9 个函数、3203 行）。

**两条读法上的坑**（都踩过）：

- 浮点常量是 `__real@<hex>` 的**单精度**，而且 **TG 的单位是米** ——
  `struct.unpack('<f', struct.pack('<I', 0x3e570a3e))` → 0.21 m = 21 cm。不换算整条推理会歪一个数量级。
- `check_for_wall_jump` 只以 `closure_env$2` 的形式出现在一个 `sort_by` 的单态化里，
  函数本体被**内联**进了 `ivy_grower`。搜不到 ≠ 不存在。

### 涉及的符号

| 符号 | 作用 |
| --- | --- |
| `country_core::resources::ivy::ivy_direction_proposer::IvyDirectionProposer::get_direction` | **方向发生器**：位置 + 藤 id → 单位 Vec2（沿墙, 向上） |
| `<IvyDirectionProposer as Default>::default` | 两层 `bracket_noise::FastNoise` 的配置（种子 / 倍频 / 增益） |
| `country_core::systems::ivy::ivy_grower::ivy_grower` | 每帧推进一段的主体，内联了 `check_for_wall_jump` |
| `ivy_grower::intersect_ivy_growth_w_wall_segment` | 一步是否穿过**别的墙**的平面足迹；命中则把落点贴到那面墙的外皮上 |
| `ivy_grower::closest_segment_index` / `get_wall_double_sided_normal` | 最近墙段查询 / 逐点帧法线 |
| `country_core::resources::ivy::wall_coord::WallCoord::into_world_space` | `(沿墙弧长, 高度)` → 世界（与本项目的 `FStrandPoint::WallSZ` 同构） |
| `country_core::systems::ivy::ivy_pruner::is_point_still_valid` | **事后裁剪**：点离墙太远 / 出墙端 / 落进洞 ⇒ 删 |
| `system_decorator::remove_ivy_under_windows` → `ivy_pruner::remove_ivy_at_location` | 窗下的藤由另一条系统整批删掉 |

## 二、TG 到底怎么转向

### 2.1 方向不是"累加的随机增量"，而是**位置的平滑函数**

`get_direction(&self, pos: Vec3, ivy_id: u64) -> Vec2` 的反汇编（`0x141234590`）翻译回来是：

```rust
// 常量全部来自 __real@ 单精度池，单位米
let p = pos / 2.8;                                      // 40333333 = 2.8

// ① 横向分量：一层噪声 × 一个随高度增长的权重
let n0 = self.noise_a.get_noise3d(p.x - 23.0, p.y - 40.0, p.z - 32.5);
let w  = (3.0 * p.y).clamp(1.0, 8.0);                   // 40400000 / 3f800000 / 41000000
let along = w * n0;

// ② 纵向分量：另取两处噪声，其中一处只当"向上偏置"用
let n1 = self.noise_a.get_noise3d(p.x + 12.03, p.y + 67.0, p.z + 785.0);
let n2 = self.noise_a.get_noise3d(p.x - 12.03, p.y -  0.82, p.z - 785.0);
let up = n1 + (0.35 * (1.0 - n2) + 0.60 * n2);          // = n1 + lerp(0.35, 0.60, n2)

let base = vec2(along, up).normalize();

// ③ 再整体转一个角：角度也来自噪声，采样点带一个**逐藤**的随机偏移
let mut rng = PerchanceContext::new(ivy_id * 2 + 1);    // lea rax,[rdi*2+1]
let q = vec3(p.x - 24.0 - 100.0 * rng.uniform_f32(),
             p.y +  7.0 + 100.0 * rng.uniform_f32(),
             p.z +  5.5 - 100.0 * rng.uniform_f32());
let n3 = self.noise_b.get_noise3d(q.x, q.y, q.z);
let t  = (n3 + 0.5).clamp(0.0, 1.0);                    // (2·n3 + 1) · 0.5
let deg = 80.0 * t - 80.0 * (1.0 - t);                  // 42a00000 = 80 ⇒ θ = 160°·n3
let rad = deg * 0.017453292 * 0.5;                      // 3c8efa35 = π/180；× 0.5 = 半角
// sinf/cosf 之后是 glam 的 Quat{0, 0, sin, cos} * Vec3 sandwich ⇒ 绕 Z 转 deg
(Quat::from_rotation_z(deg.to_radians()) * base.extend(0.0)).xy().normalize()
```

`default`（`0x1412343B0`）里两层噪声的配置：

| | 种子 | 倍频 | 增益 | 另两个 f32 字段 |
| --- | --- | --- | --- | --- |
| `noise_a` | 13 | 2 | 2.0 | 2.0 / 1.0 |
| `noise_b` | 103 | 3 | 1.0 | 1.3 / 2.0 |

两者都由 `FastNoise::seeded(45)` 造出来后再 `set_seed`。

**这一段是本轮的全部要点**：TG 的方向是 `dir(pos, ivy_id)` —— **位置的连续函数**，
不是"上一段的方向 + 一次独立随机"。步长只有 **0.21 m**（见 2.2），而噪声的基频波长是 **2.8 m**，
一步只走过 **7.5%** 个波长，所以**相邻段的方向天然接近**，折角小是结构性的、不是调出来的。

另外两条顺带读出来的形态事实（本轮**没有**移植，见 §六）：

- `w = clamp(3·h/2.8, 1, 8)`：横向摆幅**随高度增长** —— 贴地那一米只有 1、到 7.47 m 以上才吃满 8。
  这正是 TG 的藤"下面收、上面散开"的来源。
- `up = n1 + lerp(0.35, 0.60, n2)`：纵向分量带一个**恒为正**的偏置 ⇒ 整体向上，但允许局部往下绕。

### 2.2 一步的几何与三道闸

`ivy_grower` 的一次推进（`0x14094B4CC` 起）：

```rust
let dir = proposer.get_direction(last.pos, ivy_id);
let u   = wall.curve.get_approx_u_from_pos(last.pos.xz);
let arc = wall.length * u;

let next = WallCoord {
    along:  arc + sign * dir.x * 0.21,      // 3e570a3e = 0.21 m = 21 cm
    height: last.height + dir.y * 0.21,     // ⇒ dir.x = 沿墙、dir.y = 向上
};
if next.along < 0.0 || next.along > wall.length { return; }    // 出墙端 ⇒ 本帧不长
let cap = if wall.has_coping { wall.height + 0.20 }            // 3e4ccccd
          else if wall.segments < 2 { wall.height - 0.79 - 0.10 }   // bf4a3d71 / bdcccccd
          else { wall.height - 0.46 };                              // beeb851f
if next.height > cap { return; }                               // 顶到屋面 ⇒ 本帧不长
```

随后三道闸：

- **洞**（`0x14094B7C6`）：`PrevWallHoles::wall_holes(wall_id)` 取这面墙的洞表，逐个把 `Aabb2` 外扩
  `__xmm@3f8000003dcccccdbf800000bdcccccd`，按内存序是 `(−0.1, −1.0, +0.1, +1.0)`，
  即**沿墙 10 cm、竖直 1.0 m**；`Aabb2::contains` 命中就 `ivy.blocked = true` 并跳出。
- **别的墙**（`0x14094B8F3`）：`WallSpatialHash::query_with_segment` 沿这一步查候选墙段，
  `intersect_ivy_growth_w_wall_segment` 逐个求交（把墙当成沿墙方向半长 `len/2`、法向半宽 **0.315 m**
  的 AABB ⇒ 墙厚 **0.63 m**，`3f2147ae`），按距离排序取最近的一个，
  把落点**贴到被撞那面墙的外皮上**（`hit ± normal·0.315`，取离藤当前位置近的那一侧）。
- **长度理智闸**（`0x14094BF26`）：`|next_ws − last_ws| < 0.42 m` 才 `add_point`，
  `0.42 = 2 × 0.21` —— 新点不许离上一个点超过两个步长。跨墙换参数化时靠它兜住。

### 2.3 结论：三条

1. **没有任何镜像 / 归零 / 反弹。** 9 个函数里一次都没有出现"把方向取反"或"把方向按到某个固定值"。
   方向只有两个来源：`get_direction`（连续场）和撞上别的墙时的"贴到被撞墙的外皮上"
   —— 后者改的是**位置**（步长随之变短），方向一个字不改。
2. **没有显式的转向速率上限**，但有一条**等价的结构性上限**：方向是波长 2.8 m 的噪声场的函数，
   而步长 0.21 m。把基频那一层单独拿出来算（幅值 1 的正弦走过 0.075 个波长，最大变化
   `2·sin(π·0.075) = 0.235`，而 `θ = 160°·n3`）⇒ 单靠基频，一步最多转 **160° × 0.235 ≈ 37.6°**；
   若只算 `t` 的半程映射则是 18.8°。更高的倍频（`noise_b` 是 3 octave、增益 1.0）会把它抬上去，
   所以这个数是**下界不是上界** —— 本报告**不冒充**"TG 的转向上限就是 X 度"，
   只主张**量级在几十度、且连续**。
3. **障碍处不转向，直接停。** 洞的处理是 `blocked = true`；这个标志每帧都会让 `ivy_grower` 跳过这根藤
   （`0x14094AE63`），只有当这面墙的洞集合变化时才清零重试（`0x14094AE96`）。
   窗下的藤更是由 `remove_ivy_under_windows` → `remove_ivy_at_location` **整批删掉**。
   `ivy_pruner::is_point_still_valid` 用的是更紧的一档：离墙 > **0.365 m** 删、
   出墙端（`±0.01/len` 的归一化容差）删、落在**外扩 0.1 m** 的洞 AABB 里删。
   **TG 有本钱这么做**，但理由**不是**"起点撒在整面墙上" —— 那条推论 2026-09-12 已被反汇编
   `ivy_spawner` 推翻，见 §2.4。真正的理由是**播种是连续的**。

### 2.4 起点全在墙脚（2026-09-12 更正）

`country_core::systems::ivy::ivy_spawner::ivy_spawner`（`0x14094CF90`，反汇编存
`evidence/ivy-seeding-20260912.asm`）：

```rust
// 入口闸：GladeSettings 里的一个 bool，不是输入动作  (cmp byte ptr [rax+1Ch],1 ; jne ret)
// 预算闸：现存藤的总点数 + 19 < 0x9C40 = 40000
let u   = ...;                                  // 沿墙的参数
let p2  = curve.get_coord_at_u(u);              // movsd = 8 字节 ⇒ **Vec2**，墙曲线是水平的
let p3  = p2.x0y();                             // SwizzlerX0Y ⇒ Vec3(x, 0, z) —— **高度是字面的 0**
if noise.get_noise(p3.x, p3.z) <= threshold { continue; }   // 沿墙疏密 = 一道 2D 噪声门
// 每次事件播 usize_less_than(3) + 2 = 2..4 根；每根点数预算 20..70（0x46，被剩余全局预算钳）
storage.spawn_ivy(&p_a, &p_b, 0.0, ..., 20, hi);
```

三条更正：

- **TG 没有半墙上起的藤**，起点全在墙脚一条线上 —— 与本项目**相同**。墙曲线是水平的这一点由
  生长器独立印证（`get_approx_u_from_pos(last.pos.xz)`）。沿墙是**一维**散布 + 二维噪声门，
  不是墙面上的二维散布。
- **"自动播种"这条成立**：入口是 `GladeSettings` 的 bool，不是输入动作。
  （`CursorTerrainRaycast` / `ActionSetStates` 确实在读集里，用途未查，**不主张**它们做什么。）
- **它"有本钱停"的真因是连续累积**：每帧播 2–4 根、每根 20–70 点、吃满 40000 点全局预算才停手；
  `blocked` 还会在这面墙的洞集合变化时清零重试（`0x14094AE96`）⇒ 一根撞门洞停了，噪声门会继续
  在墙脚别处播新的。本项目是提交时**一次性确定性**生成整套藤 —— 停一根就是永久少一根。

## 三、改动

### 3.1 立场：抄"连续转向"，不抄"撞上就停"

"撞上就停"在本项目里试过并已作废（`CSHouseVine.cpp` 旧注释记着实测数）：
演示房子一开六个拱，两面长墙的藤当场从 **319 段掉到 132 段**，拱之间的墙面成片秃掉。
原因是 §2.4 那条 —— 两边的起点**都**在墙脚（落地拱 `Z0 = 0` 正好压在根上），但 TG 是
**连续播种 + 全局点预算**，停一根会有新的补上；本项目是提交时一次性确定性生成，停一根就是
永久少一根。
所以本轮**只**把"离散改写倾角"换成"预算内转向"，保留"绕得过去就绕"这条与 TG 不同的策略，
并把它写成代码注释里的一条明示偏差。

### 3.2 一段之内只许转 `MaxTurn`

新参数 `FParams::MaxTurn`（`CSHouseVine.h`）与 `ACSHouseActor::VineMaxTurn`，默认 **0.70 rad ≈ 40.1°**。
取值不是拍的，是在 `VineRootEscapesHoles` 的场景（6 m 墙 + 三个 150 宽的落地拱，
拱之间含 12 cm 外扩只剩 20 cm 的窄道）上扫出来的：

| `MaxTurn` | 0.40 | 0.55 | **0.70** | 0.85 | 1.00 | 1.15 | 1.50 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 保留段数比 | 0.380 | 0.495 | **0.598** | 0.596 | 0.626 | 0.616 | 0.606 |
| 藤平均爬到（cm） | 214 | 234 | **288** | 287 | 289 | 293 | 285 |

无洞对照组的平均高度是 **288–291 cm**（墙高 300）。**0.70 是"带洞的墙与无洞的墙爬得一样高"的第一个值**
—— 也就是第一个不再有藤在窄道里提前收尾的预算。再往上只多出几个百分点的段数（那是折角换来的"皱"，
不是覆盖面），而折角上界跟着涨。

两侧边界：

- **不小于 `Wander`（0.55）**：游走本身就是每段 ±0.55 的转向，预算比它小会把**无障碍处**的形状
  也一起改掉（0.40 那一列就是：连高度都掉到 214）。取得比 `Wander` 大时，无障碍段与旧代码逐位相同。
- **不大于 TG 的量级**：TG 步长 0.21 m、本项目 0.26 m，按"单位弧长的转向量"折算，
  §2.3 那个 18.8°–37.6° 的区间对应本项目 23°–46°。40.1° 落在里面。

### 3.3 障碍处：在预算内**扫**一个能过去的倾角，而不是改写倾角

旧代码在墙角 `Angle = -Angle`、在洞里先 `Angle = -Angle` 再 `Angle = 0`。新代码把这三处全删掉，
改成一条统一的**两层扫描**：

1. **内层扫倾角**：候选全部落在 `[上一段倾角 − MaxTurn, 上一段倾角 + MaxTurn] ∩ [±MaxLean]` 之内，
   以游走给出的目标倾角为圆心、按 `|偏移|` 递增往两侧扫（步距 = 预算的 1/4，共 9 个候选），
   取**第一个既不出墙端、也不落进洞、也不越墙顶**的。
2. **外层收步长**：整步扫完一圈还没有一条路，就把这一步收到 1/2、再收到 1/4 重扫
   （`CSHouseVine_StepFracs = {1.0, 0.5, 0.25}`）。

几条性质：

- 第 0 个候选恒等于目标倾角 ⇒ **无障碍时逐位等于旧行为**。
- 因为候选集本身被预算夹住，"相邻段夹角 ≤ `MaxTurn`"是**构造性**的，不靠调参。
- "取能过去的最小转向"在几何上就是**沿障碍边缘滑行**：藤贴着洞缘/墙角绕过去，而不是弹开。
- **外层为什么必需**：预算 0.70 而 `MaxLean` 1.15 ⇒ 一段之内**换不了横移的方向**。
  藤贴着洞缘、还带着 1.0 rad 朝洞里的倾角时，九个候选全都仍然朝洞里走，整根当场收尾。
  收短一步等于给倾角多争一两段的时间转过来，而**倾角一个字不改** ⇒ 转向上限逐位不受影响。
  实测：只有内层时保留率 0.485，加上外层 0.495，再把预算标到 0.70 才到 0.598。
- 外层是 TG 同构的：`intersect_ivy_growth_w_wall_segment` 撞上别的墙时并**不**改方向，
  它把落点挪到被撞那面墙的外皮上（`hit ± normal·0.315 m`）—— 也就是**步长随障碍变短**；
  TG 另有 `|新点 − 上一点| < 0.42 m = 2 × 步长` 的理智闸，说明它本来就容忍变长的步。
- 跨墙（TG 的 `check_for_wall_jump`）照旧：掷中就从相邻墙的对应端进去、倾角不变；
  掷不中则这个候选算"不通过"，继续往回扫 —— 这才是原来那句 `Angle = -Angle` 的替代。
- 两层都扫遍还没有路才 `break`。这一条与 TG 的 `blocked = true` 同义，只是门槛比它高得多。

### 3.4 逐位可复现没有变

新算法的随机源仍然只有 `IdentityHash(起点墙, 藤号, 段号, 佐料, 种子)`：

- 游走用 `11u`、跨墙掷用 `53u`，**两者的取值与旧代码逐字一致**；
- 扫描本身**不掷随机**，扫描顺序只由候选下标决定；
- 跨墙掷提到了扫描循环之外（旧代码只在撞墙角时才掷）。`Hash01` 是纯函数、没有流式状态，
  所以提前掷不改变任何取值；留在循环里反而会让"掷不掷"取决于扫到第几档 —— 那才是自指。

于是 `SpawnTime` 的键（根身份，不含位置）一个字都没动，"拖动不重播生长"这条性质自然保住。
`Vine.TurnRate` 里有一条逐位复现断言守着（同一输入两次 `BuildPlan`，折线的每个 `(S, Z)` 与
`EdgeIndex` 逐 bit 相同，刻意不给容差）。

## 四、改到哪儿了

| 文件 | 位置 | 改动 |
| --- | --- | --- |
| `Source/ComputeShaderGenerator/Public/CSHouseVine.h` | `FParams`，第 82–113、132–136 行 | 新增 `MaxTurn`（默认 0.70，附标定表）；`MaxLean` 与 `JumpChance` 的注释改成实际行为 |
| `Source/ComputeShaderGenerator/Private/CSHouseVine.cpp` | 匿名 namespace，第 22–61 行 | 新增 `CSHouseVine_TurnProbes`、`CSHouseVine_StepFracs`、`CSHouseVine_ProbeAngle` |
| 同上 | `BuildPlan` 每段推进，第 259–364 行 | 删掉两处 `Angle = -Angle` 与一处 `Angle = 0`，换成两层扫描 |
| `Source/ComputeShaderGenerator/Public/CSHouseActor.h` | 第 1064–1086 行 | 新增 `VineMaxTurn`（默认 0.70） |
| `Source/ComputeShaderGenerator/Private/CSHouseActor.cpp` | 第 2809、2846 行 | 喂 `Params.MaxTurn`，并把它加进幂等哈希 |
| `Source/ComputeShaderGenerator/Private/Tests/CSHouseVineTests.cpp` | 第 434–463 行 | `VineRootEscapesHoles` 补"藤数 + 平均高度"的 AddInfo，并更新那条已经过时的 0.663 |
| 同上 | 第 943–1110 行 | 新增 (16) `Vine.TurnRate` |

## 五、验证

### 编译

```text
Build.bat UETest574Editor Win64 Development -Project=... -WaitMutex            → Result: Succeeded
Build.bat ... -SingleFile=".../Private/CSHouseVine.cpp"                        → Result: Succeeded
Build.bat ... -SingleFile=".../Private/CSHouseActor.cpp"                       → Result: Succeeded
Build.bat ... -SingleFile=".../Private/Tests/CSHouseVineTests.cpp"             → Result: Succeeded
```

⚠️ 本插件是 unity 构建，漏 include 在全量构建里**一声不吭**，所以三个改过的 TU 都单独过了一遍
`-SingleFile`（三条都无 warning）。

### 单测

`Automation RunTests PCGPlugins` → **`Result={Success}` × 128、`Result={Fail}` × 2**。

新加的那一条（`PCGPlugins.TinyGladeHouse.Vine.TurnRate`）通过，量出来的数：

| | 相邻段对 | 最大转折 |
| --- | --- | --- |
| **旧算法**（同一场景，只把 `CSHouseVine.cpp` 换回 HEAD） | 53 | **100.4°**，其中 10 对折过 31.5° |
| 新算法，默认 `MaxTurn = 0.70` | 131 | **34.5°**（上界 40.1°） |
| 新算法，`MaxTurn = 0.15` | — | **8.6°**（= 0.15 rad，说明闸真的接上了） |
| 新算法，预算放开到 `2·MaxLean + 0.1 = 2.40` | — | **63.1°**（反向门：场景真的有大转向的需求） |

`VineRootEscapesHoles`：保留率 **0.598**（58 / 97 段，门限 0.45），藤数 5 / 7，
**平均爬到 288 / 288 cm**（墙高 300）—— 段数比旧算法的 0.663 低 6.5 个百分点，而**高度持平**，
掉的是"皱"不是"覆盖"。那条测试里已经把这两个数一起 AddInfo 出来，并把过时的 0.663 换成了 0.598。

### 两条**既存**失败（不是本轮引入）

```text
PCGPlugins.TinyGladeHouse.Vine.StrandMatchesRecords
  Expected '折线点与枝记录的世界位置逐点相同' to be 0, but it was 85.
PCGPlugins.TinyGladeHouse.Vine.TubePath
  Expected '逐点半径落在 [0.55R, R] 内' to be 0, but it was 56.
```

**已用实验证明与本轮无关**：把 `CSHouseVine.cpp` 单独换回 `HEAD` 版本重新构建，
这两条报出**一模一样的 85 和 56**（`TurnRate` 则在同一次跑里报 100.4°）。两条都是 2026-09-06
给 `PackTubePath` 加"梢部收尖 + 梢部收紧"时没跟着更新的旧断言：

- `TubePath`：`TipTaperMin` 默认 0.06，末点的半径必然是 `0.55 × 0.06 × R`，
  而断言的下界是 `0.55R` ⇒ **对任何非空规划恒假**，与游走算法无关。
- `StrandMatchesRecords`：光标按 `PointBase += S.Points.Num()` 推进，
  而 `PackTubePath` 每根藤会多排**两个收尖点** ⇒ 从第二根藤起整体错位 2 个下标。
  错位量正好是 `Branch.Num() − (第一根的段数)`，实测 `99 − 14 = 85`，与报出来的数逐位吻合。

两条都在 `PackTubePath` 的语义上、不在本轮的改动面内，所以**本轮没有动它们**。

## 六、本轮没有做的事

下面三条都是 §二读出来的**确凿**事实，但移植它们会把每一根藤的形状都挪一遍，
与"消除障碍处的大折角"是两件事，所以单独留着：

- **方向改成 `(S, Z)` 上的平滑噪声场**（TG 的做法本身）。`(S, Z)` 是墙面参数坐标、拖房子时不变，
  所以它与"身份里不含位置"那条纪律**不冲突**，是可行的；收益是转向上限变成噪声场的 Lipschitz
  常数、连 `MaxTurn` 和 `StepFracs` 都不必要。代价是全体藤重排。
- **横向摆幅随高度增长**（`clamp(3·h/2.8, 1, 8)`）。这一条顺带能解掉 §3.3 那个"一段之内换不了
  横移方向"的根因 —— TG 的藤贴地那一米本来就近乎竖直，根本走不到大倾角撞洞缘的局面。
- **纵向的恒正偏置**（`lerp(0.35, 0.60, n2)`）。

另外两条是**明示的偏差**，不是待办：

- 洞的处理本项目是"绕"而 TG 是"停"（理由见 §3.1）。
- 洞的竖直外扩本项目是 0（只有 `HoleClearance` 12 cm 的等距外偏移）而 TG 在生长期用了 1.0 m ——
  TG 那 1 米是为"只测下一个点就停"配的余量，绕的策略不需要它，加上去反而会把拱肩上的藤吃光
  （拱肩那条自由带本来只有 5–50 cm 宽，见 `IsInsideOpening` 的注释）。
