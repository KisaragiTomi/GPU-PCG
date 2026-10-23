# TinyGlade 窗变门逆向（附录 E）

> 2026-09-16 · 只读调研 · 证据等级：`[PDB]` 符号表 / `[ASM]` dumpbin 反汇编 / `[ASSET]` glb 实测 / `[DOC]` 既有文档 / 【推测】
> TG 单位是米；本文一律换算成 cm。反汇编摘录见 `evidence/window-to-door-20260916.asm`。
> 状态：✅ 7 个问题均已答完；未坐实项列在文末附录。门前踏步的**几何构造**不在本文范围（归楼梯报告）。

## 0. 结论速览

| # | 问题 | 结论（cm） | 等级 |
| --- | --- | --- | --- |
| 1 | 触发判据 | 拖拽/放置时每帧调 `snap_balcony_door`：**窗碰撞盒底 `y − H_win/2` 低于宿主墙墙脚 `ground(u)`（阈值 0）⇒ 落地门**；另有「别的墙平顶阳台」「玩家楼梯面」两路吸附。每帧派生只读存档字段、不判高度。鼠标模式无滞回；手柄模式窗不会被拖成门。 | [ASM] |
| 2 | 吸附 | 落地门心 = `max(ground,0) + H_door/2 + 5`（门碰撞盒底离墙脚 5）；`BalconySnapResult = {platform: WallId, y: f32, tag: u8}`，tag 0 阳台 / 1 落地 / 2 楼梯 / 3 无 / 4 太高。拖离即 tag 3 ⇒ 下一帧回窗。墙变形时落地门按同式重贴地。 | [ASM] |
| 3 | 类型/档位 | 存档无「门」类型：`CottageWindow/GothicWindow` 的派生子类型 `*WallWindow ↔ *BalconyDoor`，真源是 `anchor.platform.is_valid()`；rank 1..3 原样对应（`max_rank`=3 仅窗类）。门宽高取**碰撞网格**包围盒：cottage 262.5 高 × 114.8/146.5/172.3 宽；gothic 315.6/332.1/354.7 × 147.6/177.9/250.6。两种风格都有门；`ArrowSlit` 不变门。 | [ASM][ASSET] |
| 4 | Balcony / 踏步 | "BalconyDoor" 是 TG 所有门的统称；阳台门不出踏步/栏杆。**落地门**：`check_for_door_stairs` 要求门底离地 < 20（或水边门底 ≤ 10），门前 106 处比门下低 > 10，且（非水边）落差 < 150 ⇒ 出踏步；否则若墙底高出地形 > 15 ⇒ 出 85 高栏杆 `*_rails`。 | [ASM] |
| 5 | 墙洞 | cottage 门洞与窗洞是**同一种矩形记录** `{u,y,w,h,type=0}`，门洞底 = 墙脚 +5、高 262.5；gothic 门是**三块台阶矩形**（手写常量）。门不调 `add_lintels` / `flowerbed_xforms`，无窗台、无玻璃；门扇资产自带门框。 | [ASM] |
| 6 | 约束 | 门不能上角（`can_be_on_corner` 不含 4/9）；距角 `< W/2 + 46`（半木墙 20）不合法，与窗同规则。门顶 `≥ 墙顶`（宿主平顶再 −79）⇒ 校验时剔除（cottage 落地门要墙高 > 267.5）。无门专属间距规则。 | [ASM] |
| 7 | 挂件 | `clutter_version ≥ 4` 时按 seed：25% `door_bell`（门心局部 −25, +74, +10）、25% `door_krans`（0, +50, +20）、50% 无。 | [ASM][ASSET] |

全部常量（TG 米 → cm）：**5**（门底离墙脚）· **10**（复核容差）· **20**（阳台探针外伸 / 踏步离地闸 / 半木墙角距）· **40**（阳台平面容差、门底高出地板上限、宿主墙底下限）· **46**（距角）· **50 / 100**（楼梯射线外伸与起点抬高 / 射线长）· **79**（平顶地板 = `max_y − 79`）· **106 / 150 / 80**（踏步前探距离 / 最大落差 / 水边下探）· **15**（栏杆判据）· **31.5**（抓取参考点外伸）· 挂件偏移 **(−25,74,10)** / **(0,50,20)** · 平顶判据 **坡度 ≤ 0.1**。

## 1. 触发判据

### 1.1 在哪里判：**只在拖拽/放置交互里判**，每帧派生不判【确凿】

| 位置 | 做什么 | 证据 |
| --- | --- | --- |
| `ui_move_decorator` / `ui_place_decorator` → `calculate_decorator_interaction_intent` → **`snap_balcony_door`** | 每帧按光标位置算吸附；成功则把 `platform` / `is_bottom_door` / 吸附后的 y 写进 intent，松手随 op 落盘 | `[ASM]` 0x14127E774（唯一 UI 调用点，`dl=0`、第 13 个参数 `false`） |
| `derive_decorator_subtype(s)` | **只看 `platform.is_valid()`**，不看高度（§3.2） | `[ASM]` 0x140814277 |
| `cull_oob_decorators::is_decorator_location_valid` | 对已是门的记录复跑 `snap_balcony_door(dl=1)`，结果为「太高」就归档剔除（§6.2） | `[ASM]` 0x14081090D |
| `move_decorators_following_anchors` | 墙变形时，`is_bottom_door` 的门**重新贴地**（同一个 y 公式） | `[ASM]` 0x14081D2EA |

⇒ 「变门」是**交互期决定、存档字段承载、每帧派生只读**的三段式。

### 1.2 判据：`snap_balcony_door` 的三路吸附（按优先级）【确凿】

符号：`y` = 光标命中墙面点的世界高度（抓取偏移已把它对到装饰物中心，见 §2.3）；
`H_win` / `H_door` = 同 rank 的 `*WallWindow` / `*BalconyDoor` 碰撞盒高度（§3.4）；
`fwd` = 装饰物朝外的 XZ 方向。常量 `[ASM]` 均为单精度米，已换算 cm。

| 优先级 | 结果 tag | 条件 | 吸附后门中心 y | `platform` |
| --- | --- | --- | --- | --- |
| ① 阳台 | 0 | 遍历**其他墙**的平屋顶 R（`Roof::is_flat` = 坡度参数 ≤ 0.1；跳过宿主墙自己的顶）：<br>`floor = R.max_y − 79`（与 `PublicWallState::flat_roof_y` 同式）；<br>`y > floor` **且** `floor ≥ max(host.bottom_y − 40, y − H_door/2 − 40)`；<br>**且** 平面上：矩形顶按 `contains_point_padding(pad=0.8)` 即**每边外扩 40**，窗位与 `窗位+20·fwd` 都在内；圆形顶 `sd(窗位) < 40` 且 `sd(窗位+20·fwd) < 0` | `max(host.bottom_y, floor) + H_door/2` | R 的墙 |
| ② 落地 | 1 | 以上都不成立，且 `ground(u) > y − H_win/2`<br>（`ground(u)` = 宿主墙墙脚折线在 u 处插值高度，再 `max(·,0)`） | `max(ground + H_door/2 + 5, H_door/2)` | 宿主墙自己 |
| ③ 楼梯 | 2 | 从 `(窗位 + 50·fwd, y − H_win/2 + 50)` 竖直向下射线，长 100，只查楼梯子世界（flag `0x1000`，命中种类 `0x0D` = `Stair`，§附） | `max(hit.y + H_door/2 + 5, H_door/2)` | 宿主墙自己 |
| — | 3 | 都没命中 ⇒ **保持窗**（`platform` 置无效、`is_bottom_door=false`、y 用光标值） | — | 无 |
| — | 4 | 仅 `dl=1`（校验路径）：吸附成功后 `y + H_door/2 ≥ wall_top(u) + (宿主墙自带平顶 ? −79 : 0)` ⇒ 门顶戳出墙顶 | — | — |

关键摘录（落地分支，`dl=0` 路径）：

```asm
14127F862: movss xmm11,[rdx+rax*4+4]   ; 墙脚折线下一点 .y
14127F86E: mulss xmm11,[rsp+64h]        ; lerp(t)
14127F87A: xorps xmm0,xmm0
14127F87D: maxss xmm11,xmm0             ; ground = max(ground, 0)
14127F9B3: mulss xmm12,[__real@bf000000]; −0.5 · H_win
14127F9BC: addss xmm12,[rsp+34h]        ; win_bottom = y − H_win/2
14127F9C3: ucomiss xmm11,xmm12
14127F9CB: jbe   14127F8A7              ; ground ≤ win_bottom → 去射线找楼梯
14127F9D1: addss xmm11,xmm13            ; ground + H_door/2
14127F9D6: addss xmm11,[__real@3d4ccccd]; + 0.05 m
14127F9E3: maxss xmm0,xmm11             ; max(H_door/2, ·)
14127F9FD: mov   bl,1                   ; tag = 1（落地门）
```

### 1.3 比的是什么、阈值多少

**比的是「窗碰撞盒底」对「墙脚地面」**：`y − H_win/2 < ground(u)` ⇒ 变门【确凿】。不是比窗心、不是比窗顶，
也**没有额外的 cm 容差**——阈值就是 0，只是半高取自碰撞盒（`(y_max − y_min)/2`，**假定原点在盒心**）。
按 §3.5 实测：

| 窗 | `H_win` | 光标 y 低于 `ground + …` 即变门 | 变门后门心 / 门底 |
| --- | --- | --- | --- |
| cottage 1x1 / 2x1 | 160 | **80** | 门心 `ground+136.25`；网格底（−130）落在 `ground+6.25` |
| cottage 3x1 | 170 | **85** | 同上 |
| gothic 1x1 / 2x1 / 3x1 | 224.1 / 265.6 / 319.9 | **112.05 / 132.8 / 159.95** | 门心 `ground + (157.8 / 166.05 / 177.35) + 5`；gothic 门碰撞盒盒心偏上 9.8 / 9.05 / 12.35 cm，门底实落 `ground + 14.8 / 14.05 / 17.35`（cottage 为 6.25）——代码按「原点在盒心」算半高的副作用 |

### 1.4 滞回：**鼠标模式下没有**【确凿】

`calculate_decorator_interaction_intent` 采纳吸附结果的门槛 `[ASM]` 0x14127E7A4–E7ED：

```text
doorish = anchor.platform.is_valid() || anchor.is_bottom_door || !ActiveInputMode::is_controller()
采纳 = doorish && tag ∈ {0,1,2}
新 is_bottom_door = doorish && tag == 1
```

两个调用方都传 `!is_controller()`（`[ASM]` 0x1407A6B8C / 0x1407AC451：`call is_controller; xor al,1; mov [rsp+68h],al`）。
⇒ **鼠标/键盘：每帧只看当前光标，进门、出门同一条判据，无状态滞回**。
手柄：窗永远不会被拖成门；已经是门的，吸附成功就保持门，失败（tag 3）就退回窗。

⚠️ 几何上有「隐性回差」：变门时门心被抬到 `ground + H_door/2 + 5`（cottage 136.25），而判据看光标 y 是否 `< ground + H_win/2`（80）。
抓取偏移把光标对到门心（§2.3），所以**抓起一扇已落地的门、原地不动时，按代码它会立即回成窗**，直到往下拖过 80 cm 线。
未在游戏内验证，标【推测】。

## 2. 吸附行为与 `BalconySnapResult`

### 2.1 `BalconySnapResult` 布局（16 字节）【确凿】

`snap_balcony_door` 出口 `[ASM]` 0x14127FCE1–FCEC：

| 偏移 | 类型 | 含义 |
| --- | --- | --- |
| +0x00 | `WallId` | 立足平台：阳台 = 平顶那面墙；落地/楼梯 = 宿主墙自己 |
| +0x08 | f32 | 吸附后的门中心世界 y（米） |
| +0x0C | u8 tag | 0 阳台 · 1 落地 · 2 楼梯 · 3 无（非窗类型也回 3）· 4 太高（仅校验路径） |

变体名 PDB 里没有字符串（非 Debug 类型），上面的语义名是按分支行为起的。

### 2.2 变门时位置确实被吸到墙底【确凿】

- 落地门：门中心 `= max(ground(u),0) + H_door/2 + 5 cm`，即**门碰撞盒底比墙脚高 5 cm**；
  u 不变（沿墙位置跟光标），只改 y。
- 墙变形后 `move_decorators_following_anchors` 对 `is_bottom_door` 的门**用同一公式重贴地**
  （`[ASM]` 0x14081D3DE–D43E：`max(lerp,0) + (y_max−y_min)·0.5 + 0.05`，再 `max(·, H/2)`），
  并回写 `anchor_ws.y`、`coord.y` 与 `wall_height_relative_y = y − max_y`。普通窗则按 `wall_height_relative_y` 跟着墙顶走。
- 阳台门：门心 = `max(host.bottom_y, floor) + H_door/2`（**没有** +5 cm）。

`host.bottom_y` = `PublicWallState+0x280`：`construct_elevation_supports::rectangle_pillars` 拿它和地形高比来决定立柱、
`shape_placement_params` 拿它当墙底 y `[ASM]` 0x14124FE0D / 0x140980391 ⇒ **墙底抬升高度**（墙可以悬空，下面补柱子）。
`ground(u)` 取自墙底 3D 折线（`PublicWallState` 里有两条：吸附用 +0x90，重贴地/栏杆用 +0x40；两条的区别未坐实）。

### 2.3 拖离怎么变回窗【确凿】

每帧重算；tag 3 时 intent 写 `platform = 无效`、`is_bottom_door = false`、y = 光标 y ⇒ 下一帧派生为 `*WallWindow`。
抓取参考点 = `coord(u,y)` 的世界点 + 朝外 31.5 cm（`try_get_grab_offset_ss_if_valid`，`__real@3ea147ae`）`[ASM]` 0x141277F7F，
所以光标 y 基本就是装饰物中心 y。

## 3. 类型 / 档位映射

### 3.1 存档里**没有「门」这个类型**——门是窗的派生子类型【确凿】

`DecoratorType`（玩家放置、可序列化的类型）的变体名取自 serde 字符串表 `[ASM]`（exe `.rdata` 0x142ACED41 附近），
判别值由 `is_window` / `max_rank` / `spawns_clutter` / `can_place_on_*` 五个小函数的位运算反推 `[ASM]`
（`Clutter{ty}` 是带数据变体，占 0x00–0x37，其余变体按声明序落在 niche 0x38 起）：

| 值 | `DecoratorType` | `is_window` | `max_rank` | `spawns_clutter` | 可放地形 |
| --- | --- | --- | --- | --- | --- |
| 0x38 | `CottageWindow` | ✅ | **3** | ✅ | ❌ |
| 0x39 | `GothicWindow` | ✅ | **3** | ✅ | ❌ |
| 0x3A | `ArrowSlit` | ✅ | **3** | ❌ | ❌ |
| 0x3B | `Chimney` | ❌ | 1 | ❌ | ❌ |
| 0x3C | `Lantern` | ❌ | 1 | ❌ | ✅ |
| 0x3D | `Flag` | ❌ | 1 | ❌ | ✅ |
| 0x00–0x37 | `Clutter { ty }` | ❌ | 1 | ❌ | ✅ |
| 0x3F | `Stairs` | ❌ | 1 | ❌ | ✅ |

```asm
; DecoratorType::is_window         ; DecoratorType::max_rank
add  cl,0C8h  ; t-0x38             add  cl,0C8h
cmp  cl,3                          xor  eax,eax
setb al       ; t∈{38,39,3A}       cmp  cl,3 ; setb al
ret                                lea  rax,[rax*2+1]   ; 窗类 3，其余 1
```

`DecoratorSubtype`（每帧派生、**不存档**）的 26 个变体名取自 `impl Debug for DecoratorSubtype` 的
长度表 0x142AE9EB0 + 相对偏移表 0x142AE9F80 `[ASM]`：

```text
 0 CottageWallWindow   1 CottageHalfDormer   2 CottageDormer   3 CottageCornerWindow
 4 CottageBalconyDoor  5 CottageTrapDoor     6 GothicWallWindow 7 GothicHalfDormer
 8 GothicDormer        9 GothicBalconyDoor  10 GothicTrapDoor  11 FlagWall  12 FlagCorner
13 FlagRoof 14 FlagFlatRoof 15 FlagTerrain 16 LanternWall 17 LanternTerrain 18 LanternFlatRoof
19 ChimneyWall 20 ChimneyVentPipe 21 ChimneyRoof 22 ArrowSlitWall 23 ArrowSlitHalfDormer 24 Clutter 25 Stairs
```

⇒ **「窗变门」在 TG 里不是换类型，而是同一个 `CottageWindow` / `GothicWindow` 记录的派生子类型从
`*WallWindow` 变成 `*BalconyDoor`**。rank（`DecoratorRank { x }`，1..3）原样保留 ⇒ 窗 rank *n* ↔ 门 rank *n* 一一对应。
`ArrowSlit` 永远变不成门（它只派生 `ArrowSlitWall` / `ArrowSlitHalfDormer`）。

### 3.2 派生规则 `derive_decorator_subtype`【确凿】

`[ASM]` 0x140813D00，按 `DecoratorType` 跳表 0x142A8F990、再按 `DecoratorDst` 跳表 0x142A8F9D0 分派。
窗类 + 挂墙（`WallAttachment`）分支是**纯字段判断，不看高度**：

```text
if anchor.corner.is_attached()      → Cottage: CottageCornerWindow(3) | Gothic: GothicWallWindow(6)
elif anchor.platform.is_valid()     → Cottage: CottageBalconyDoor(4)  | Gothic: GothicBalconyDoor(9)   ← 门
elif is_half_dormer(...)            → CottageHalfDormer(1)            | GothicHalfDormer(7)
else                                → CottageWallWindow(0)            | GothicWallWindow(6)
挂屋顶（RoofAttachment）             → Roof::is_flat ? *TrapDoor(5/10) : *Dormer(2/8)
```

```asm
140814265: movzx ecx,byte ptr [rsp+58h]      ; anchor.corner
14081426A: call  WallCornerAttachment::is_attached
140814271: jne   140814124                   ; → 转角窗
140814277: lea   rcx,[rsp+40h]               ; anchor.platform (WallId)
14081427C: call  WallId::is_valid
140814283: je    140814297                   ; → 半老虎窗 / 普通窗
140814285: xor   eax,eax
140814287: cmp   byte ptr [r15+46h],38h      ; CottageWindow?
14081428C: setne al
14081428F: lea   eax,[rax+rax*4]
140814292: add   eax,4                       ; Cottage→4 BalconyDoor, Gothic→9 GothicBalconyDoor
```

⇒ 门/窗的分界**由存档字段 `WallAttachmentAnchor.platform` 是否有效决定**；这个字段只在拖拽交互
（`snap_balcony_door`，见 §1/§2）里被写。每帧的 `derive_decorator_subtypes` 只读字段、不重判高度。

### 3.3 `WallAttachment` 存档结构【确凿】

`WallAttachment { coord, anchor: WallAttachmentAnchor }`。字段名取自 serde 字符串 `[ASM]`
（0x142C7B183：`struct WallAttachmentAnchor with 6 elements`），偏移取自
`impl Serialize for WallAttachmentAnchor`（0x1416FA690，按字段名长度 22/9/6/8/14/4 对位）与
`derive_decorator_subtype` 的读取 `[ASM]`：

| `WallAttachment` 偏移 | 字段 | 类型 | 说明 |
| --- | --- | --- | --- |
| +0x00 | `coord` | `WallSpaceCoord{u: f32, y: f32}` | u = 沿墙弧长；y = **中心世界高度**（门生成器 `movshdup` 取它喂 `xvy`，0x1407F45CD） |
| +0x08 | `anchor.platform` | `WallId`（u64，**0 = 无**，serde 为 0 时省略） | **门的立足平台**；无效 = 窗 |
| +0x10 | `anchor.wall_height_relative_y` | f32 | `coord.y − wall_top(u)`（`decorator_on_wall_height_changed` 0x14081BA67）；墙变高时普通挂件据此跟墙顶走 |
| +0x14 | `anchor.anchor_ws` | Vec3 | 世界锚点 |
| +0x20 | `anchor.corner` | `WallCornerAttachment` | 转角挂接 |
| +0x21 | `anchor.is_bottom_door` | bool | **落地门**（吸附到墙脚/地面那一类，见 §2） |
| +0x22 | `anchor.side` | `WallAttachmentSide{Inner,Outer}` | 同时承载 `DecoratorDst` 的 niche（2=Roof,3=Terrain,4=Stair） |

### 3.4 门的宽高来自**碰撞网格包围盒**【确凿】

`DecoratorMeshLibrary` 以 `DecoratorLibKey(subtype, rank)` 为键，值里带 `load_collision_as_bbx`
算出的 32 字节盒 `[ASM]`：`{cos,sin @0 | center_xz @8 | size_x @0x10 | size_z @0x14 | y_min @0x18 | y_max @0x1C}`。
`snap_balcony_door` 查两次：`H_door = y_max−y_min`（键 `(*BalconyDoor, rank)`）、
`H_win`（键 `(*WallWindow, rank)`）。对应网格名（exe 字符串）：
`decorators/balcony_door_rank{1,2,3}_collision`、`decorators/balcony_door_gothic_rank{1,2,3}_collision`。
实测尺寸见 §3.5。

### 3.5 资产实测（glb POSITION 包围盒，TG 米 → cm，Y 向上，Z 朝墙外）【ASSET】

| 网格 | 宽 X | 高 Y | 深 Z | Y 范围 |
| --- | --- | --- | --- | --- |
| `balcony_door_rank1` / `2` / `3`（门扇+门框，渲染） | 120 / 150 / 180 | 262.5 | 31.6 | [−130, +132.5] |
| `balcony_door_rank1/2/3_collision`（**判据用**） | 114.8 / 146.5 / 172.3 | **262.5** | 47.3 | [−130, +132.5] |
| `balcony_door_rank1/2/3_rails` | 160 / 190 / 220 | 85 | 65 | [−100, −15]（rank3 [−95, −10]） |
| `balcony_door_gothic_rank1/2/3` | 112.6 / 140.2 / 204.6 | 294.5 / 312.9 / 329.4 | 29.6 | [−148, +146.5] / [−157.2, +155.7] / [−165.3, +164.1] |
| `balcony_door_gothic_rank1/2/3_collision`（**判据用**，含尖拱帽） | 147.6 / 177.9 / 250.6 | **315.6 / 332.1 / 354.7** | 20 | [−148, +167.6] / [−157, +175.1] / [−165, +189.7] |
| `balcony_door_gothic_rank1/2/3_hat` | 147.6 / 177.9 / 248.8 | 126.3 / 142.6 / 165.4 | 70 | [41.5, 167.8] / [32.3, 174.9] / [24.2, 189.6] |
| `gothic_balcony_door_rank1/2/3_rails` | 152.6 / 180.2 / 244.6 | 85 | 65 | [−118, −33] / [−127, −42] / [−135, −50] |
| `window_cottage_1x1/2x1/3x1_collision`（**判据用**） | 78 / 143.5 / 213 | **160 / 160 / 170** | 8.5 / 9 / 57.6 | [−80, 80] / [−80, 80] / [−85, 85] |
| `window_gothic_1x1/2x1/3x1_collision`（**判据用**） | 69.5 / 122.5 / 161.6 | **224.1 / 265.6 / 319.9** | 34 | [−97.3, 126.8] / [−95.3, 170.3] / [−95.3, 224.6] |
| `clutter/door_bell` | 24.9 | 39.9 | 23.3 | [−28.8, 11.2] |
| `clutter/door_krans`（另有 `door_krans_autumn`） | 80.5 | 82.7 | 33.3 | [−39.5, 43.1] |
| （对照）`decorators/door`：交互代理盒，非真门 | 120 | 250 | 75 | [−125, 125] |

- 门宽**随 rank 变**、门高**不随 rank 变**（cottage 恒 262.5）；gothic 门随 rank 长高（尖拱）。
- 门与窗 rank 一一对应，**尺寸不相关**：cottage 窗 78/143.5/213 宽 → 门 120/150/180 宽（渲染），窗宽 213 的 3x1 变门后反而变窄。
- 栏杆网格原点即门中心：Y 在门心下 100→15 cm、Z 从墙面向外 0→65 cm ⇒ 栏杆顶离门底约 115 cm（cottage）。
- `balcony_door_single`（150×262.5）在 exe 字符串里**未被 `cottage_balcony_door.rs` 引用**，疑为遗留资产【推测】。

## 4. "Balcony" 的含义：栏杆与门前踏步判据

### 4.1 "Balcony door" 就是 TG 里**所有门**的统称【确凿】

`*BalconyDoor` 覆盖 §1.2 的三种立足：阳台（别的墙的平顶）、落地（墙脚）、楼梯面。名字来自最初的「通往平顶阳台的门」，
落地门只是 `platform = 宿主墙自己` 的特例。两个生成器 `generate_cottage_balcony_doors` / `generate_gothic_balcony_doors`
对每扇门按下面顺序出件 `[ASM]`：

| 步 | cottage | gothic | 条件 |
| --- | --- | --- | --- |
| 1 门扇 | `decorators/balcony_door_rank{n}`（subset `nani_balcony_door`） | `decorators/balcony_door_gothic_rank{n}`（同 subset） | 总是 |
| 2 帽 | — | `decorators/balcony_door_gothic_rank{n}_hat`（`nani_gothic_window_bricks`，`get_gothic_hat_bones` 三骨点掰弯） | 总是 |
| 3 墙洞 | 向 blueprint 推一条 `{coord(u,y), 宽=碰撞盒 size_x, 高=碰撞盒 H, type=0}` | 按 rank 推**三条**手写矩形（台阶状尖拱） | 总是（§5） |
| 4 挂件 | `add_door_autoclutter`（§7） | 同一个函数 | `clutter_version ≥ 4` |
| 5 踏步 | `check_for_door_stairs` → `add_door_stairs` | 同一套逻辑（内联） | **仅 `is_bottom_door`** |
| 6 栏杆 | `decorators/balcony_door_rank{n}_rails`（`nani_solid_vertex_color_window`） | `decorators/gothic_balcony_door_rank{n}_rails` | **仅 `is_bottom_door` 且没出踏步**，且下式成立 |

网格名表：cottage 0x142A8BC20（门扇相对偏移表）/ 0x142A8BC2C（栏杆），gothic 0x142A8B5F8（栏杆）`[ASM]`。

⇒ **阳台门（`platform` 是别的墙）既不出踏步也不出栏杆**；平顶本身带女儿墙/城垛。栏杆与踏步都是给**落地门**兜底的。

### 4.2 栏杆判据【确凿】

`[ASM]` 0x1407F5234–F5387（gothic 0x1407F24AA–F25FB 同式）：

```text
base_y  = lerp(墙底折线 A(+0x40).y, u)
terrain = TerrainHeights.bilinear_sample(lerp(A.xz, u))
出栏杆 ⇔ is_bottom_door && !stairs_added && base_y > terrain + 15 cm
```

即墙脚悬空（墙底被抬起、下面是柱子/悬崖/水）超过 15 cm、又没法铺踏步时，门口加一道 85 cm 高的栏杆。

### 4.3 门前踏步判据 `check_for_door_stairs`【确凿】（几何构造不归本文）

`[ASM]` 0x1407F3340。输入：门变换（四元数 + 中心）、门碰撞盒、`TerrainHeights`、`WaterRaster`（系统签名里的 `world_raster_defs::WaterRaster`）。

```text
door_bottom = pos.y − H_door/2
h0 = max(terrain(pos.xz), 0)
water = WaterRaster.sample(pos.xz, default=130) > 0
入口闸：  (water && door_bottom ≤ 10 cm，世界高度，海平面 = 0) → 水边模式
       或 h0 > door_bottom − 20 cm                          → 地面模式（门底离地 < 20 cm）
       否则 → 不出（门悬在地面 20 cm 以上，交给 §4.2 栏杆）
前方采样：P1 = pos + rot·(0, 0, 106 cm)；h1 = terrain(P1)
       h0 − h1 ≤ 10 cm → 不出（门前不下坡）
       hL/hR = terrain(P1 ∓ rot·(W/2, 0, 0))，W = 碰撞盒宽；h_min = min(h1, hL, hR)
       非水边 且 h0 − h_min ≥ 150 cm → 不出（太陡）
出踏步：DoorStairsAssemblyParams { pos.xz, fwd.xz(归一化), max_y = h0,
        min_y = 水边 ? −80 cm : h_min, bottom_padding = max(−80 cm − h_min, 0), width = W }
```

`DoorStairsAssemblyParams` 的 Debug 字段名串（0x142C28B6D）：`max_y` `min_y` `bottom_padding` `width`（与 position/orientation 共用串）`[ASM]`。
结果写进 `DecoratorBlueprint+0x120`（`add_door_stairs` 分配 32 字节）。
「水边」用的栅格是 `WaterRaster`：两个门生成器的系统签名 `[PDB]` 恰为
`ActiveSession · PublicWalls · DecoratorStorage · DerivedDecoratorInfoResources · DecoratorBlueprints · TerrainHeightsData · WaterRaster`。

## 5. 墙上的洞：门洞 vs 窗洞

### 5.1 洞记录完全同构，只差尺寸与中心【确凿】

两边往 `DecoratorBlueprint+0xC8`（Vec，20 字节/条）推的是同一种记录 `[ASM]`：

| 字段 | 窗 `generate_cottage_wall_windows` 0x1407ED631 | 门 `generate_cottage_balcony_doors` 0x1407F4B99 |
| --- | --- | --- |
| +0x00 中心 | `WallAttachment.coord (u, y)` | 同左（y 已被吸附改写） |
| +0x08 宽 | 窗碰撞盒 `size_x` | 门碰撞盒 `size_x` |
| +0x0C 高 | 窗碰撞盒 `y_max − y_min` | 门碰撞盒 `y_max − y_min` |
| +0x10 type | `0` | `0` |

⇒ TG 的 cottage 门洞**就是一个矩形墙洞**：没有「贴地」专用洞型，也没有拱；它之所以贴地，只是因为门心被吸到了
`ground + H_door/2 + 5`。cottage 落地门的洞：**底 = ground + 5 cm，顶 = ground + 267.5 cm，宽 114.8 / 146.5 / 172.3**。

**gothic 门不用碰撞盒，而是按 rank 推三块手写矩形叠成台阶状尖拱** `[ASM]` 0x1407F16CF–F194B（常量以门心 y 为原点，cm）：

| rank | 洞 1（主体）中心 Δy / 宽 / 高 | 洞 2 | 洞 3 | 洞底（相对门心） |
| --- | --- | --- | --- | --- |
| 1 | −20 / 113 / 256 | +114 / 81 / 40 | +134 / 41 / 40 | **−148**（= 网格底） |
| 2 | −41 / 140 / 232 | +98 / 131 / 48 | +134 / 80 / 32 | **−157** |
| 3 | −33 / 204 / 264 | +115 / 162 / 39 | +142 / 100 / 46 | **−165** |

主洞宽 = 渲染网格宽（112.6/140.2/204.6），洞底与网格底逐 cm 对齐；台阶角由 `*_hat` 盖住。
因为吸附按碰撞盒半高算，gothic 落地门的洞底实落 `ground + 14.8 / 14.05 / 17.35`。

### 5.2 门不出过梁、不出窗台、不出花槽【确凿（否定式）】

| 调用 | 窗 | 门（cottage / gothic） |
| --- | --- | --- |
| `cottage_wall_window::add_lintels`（过梁 `setdressing_window_lintel`） | ✅ 0x1407ED822 | ❌ 两个门生成器的全部 call 里都没有 |
| `cottage_wall_window::flowerbed_xforms`（窗下花槽） | ✅ 0x1407EDF23 / EE427 | ❌ |
| `DecoratorExtraInfo::as_window_mut`（窗的额外数据） | ✅ 0x1407ED1EE | ❌ |
| 门扇网格自带门框 | — | ✅（`balcony_door_rank*` 120×262.5×31.6，含框） |

窗台 `setdressing_window_sill` 在这两个 `.rs` 的字符串区都不出现，窗那侧由窗网格/花槽体系提供，门侧没有任何对应件 `[ASM]`（字符串区 0x142A8BA00–BC40 枚举）。
与 `[DOC] House_Window.md`「门那边全是挂件、没有门框件」吻合，并补一条：**门扇资产本身就是门框+门扇一体件**。

## 6. 约束：转角 / 墙高 / 间距

### 6.1 门不能上转角【确凿】

`DecoratorSubtype::can_be_on_corner` `[ASM]` 0x140B25B50：`bt 0x91008, subtype` ⇒ 位 3/12/16/19 =
`CottageCornerWindow` / `FlagCorner` / `LanternWall` / `ChimneyWall`。**`CottageBalconyDoor`(4)、`GothicBalconyDoor`(9) 都不在里面**。
且 §3.2 的派生顺序是**先判转角**：挂在转角上的 cottage 窗永远派生成转角窗，gothic 窗派生成普通窗，都不会成为门。

**距角规则与窗完全相同**（`should_snap_to_corner` 0x140811900 / `is_decorator_location_valid` 0x140810935 起）：
矩形墙上，`|u − u_corner| < W/2 + 46 cm`（半木结构墙 `is_halftimbered` 为 20 cm）即判「贴角」——
可上角的子类型吸附到角，不可上角的（门、普通窗）校验返回 code 5（带角序号），不被当作合法位置。

```asm
14081192A: call  DecoratorType::should_offer_snap_to_corner   ; 除 Stairs 外都 true
140811956: movss xmm9,[__real@3eeb851f]      ; 0.46 m
140811965: movss xmm9,[__real@3e4ccccd]      ; 0.20 m（halftimbered）
140811997: mulss xmm8,[__real@3f000000]      ; W · 0.5
1408119A0: addss xmm8,xmm9
1408119A5: ucomiss xmm8,xmm0                  ; xmm0 = |u − u_corner|
1408119A9: seta  al
```

### 6.2 最小墙高：**只在校验路径**上约束【确凿】

`is_decorator_location_valid` 对子类型 4/9 调 `snap_balcony_door(dl=1, flag=1)`（0x14081090D），tag 4 ⇒ 返回 code 6，
`cull_oob_decorators` 只保留 code 0x0B（合法），其余一律从存储移走、录历史时交 `DecoratorArchivist::archive_storage_decorators`
归档（0x14080E3DB，墙改回来可由 `restore_archived_decorators` 恢复）⇒ **门被剔除（可恢复）**：

```text
太高 ⇔ y + H_door/2 ≥ wall_top(u) + (宿主墙本身是平顶 ? −79 cm : 0)
```

落地 cottage 门（`y = ground + 136.25`）⇒ 墙高须 **> 267.5 cm**（平顶墙 > 346.5 cm）；
gothic 门须 > 320.6 / 337.1 / 359.7 cm。UI 拖拽路径（`dl=0`）不查这一条；拖拽时是否另有红色提示 **未查到**。
校验路径的落地判据改用门高：`ground > (y − H_door/2) − 10 cm`（`__real@bdcccccd`，0x14127F890），给已吸附的门留 10 cm 余量。

### 6.3 门与门/窗的间距：未见门专属规则【确凿（否定式）】

交互期统一走 `collision_detection::intersect_wall_decorators` + `merge_proposal_wall_decorators`（按碰撞盒相交 / 合并提议），
代码里没有按 `BalconyDoor` 分支的间距常量；门只是碰撞盒更高更宽。
「门上开窗」在 TG 里同样会被碰撞盒相交挡住（门碰撞盒 262.5 高，几乎占满一层墙）【推测】。

## 7. 门的挂件 `add_door_autoclutter`【确凿】

`[ASM]` 0x1407F5E70（cottage 生成器里内联一份，gothic 生成器直接 call）：

`DecoratorInfo` 的 serde 字段表是 `dst ty is_preview rank color_id seed clutter_version extra`（串 0x142ACEF8D）；
单字节字段 +0x44 rank、+0x46 ty 已由 §3 坐实，+0x45 只剩 `clutter_version`（`seed` 在 +0x2C，喂 `init_rng`）。

```text
if DecoratorInfo[+0x45] (clutter_version) < 4 → 不出
r = hash(DecoratorInfo.seed) & 3
r == 0 → "doorbell"（autoclutter kind 3），门局部偏移 (x −25, y +74, z +10) cm
r == 1 → "krans"   （kind 4 | DragState 标记），门局部偏移 (x 0,  y +50, z +20) cm
r == 2,3 → 不出
```

- 局部坐标以**门中心**（门网格原点）为原点、随门旋转（X 沿墙、Y 上、Z 朝外）；cottage 门网格底在原点下 130 cm，
  故门铃挂在门槛上约 204 cm、偏门侧 25 cm；花环在门槛上约 180 cm、居中、外挑 20 cm。
- 名字串是 autoclutter 键（`calculate_hash("doorbell")` / `calculate_hash("krans")`），资产为
  `clutter/door_bell.glb`（24.9×39.9×23.3）与 `clutter/door_krans.glb`（80.5×82.7×33.3，另有 `door_krans_autumn`）`[ASSET]`。
- 每扇门至多一件，概率各 25%；窗那边的花槽/花盆是另一套（`flowerbed_xforms`），门不出。

## 8. 给 UE 实现者的建议

> 读的是 2026-09-16 当时的源码（只读）：`ACSWindowMarker` 已有 `DoorMesh` / `DoorHatMesh` / `DoorSnapHeight=40`、
> `FCSWallAnchor::bDoorForm`、`CSHouse_ResolveDoorForm`（`CSHouseProfile.h:1387`）、`CSHouse_QueryOpening` 对 `bDoorForm` 放过 `MinSillZ`。
> 下面按「TG 确凿做法」与「适配建议」分开写；墙空间 Z 以墙基为 0（本项目墙基是平的，TG 的 `ground(u)` 在这里恒 0）。

### 8.1 判据的纯函数形式（TG 原式）

```cpp
// TG 确凿：拖拽路径（snap_balcony_door, dl=0）——恒按「窗」的碰撞盒底判，阈值 0
bool TG_ShouldSnapBottomDoor_Drag(float CenterZ, float WinH)       // CenterZ = 光标命中点（= 本体中心）
{ return CenterZ - WinH * 0.5f < 0.0f; }                            // ground(u) > y − H_win/2

// TG 确凿：已落盘门的复核（is_decorator_location_valid, dl=1）——按「门」的碰撞盒底判，留 10 cm
bool TG_StillBottomDoor_Validate(float DoorCenterZ, float DoorH)
{ return DoorCenterZ - DoorH * 0.5f - 10.0f < 0.0f; }

// TG 确凿：吸附后的门中心、洞
float TG_DoorCenterZ(float DoorH) { return FMath::Max(DoorH * 0.5f + 5.0f, DoorH * 0.5f); }  // = H/2 + 5
// 洞：Z0 = 5，Z1 = 5 + DoorH，宽 = 门碰撞盒宽（不是渲染网格宽）

// TG 确凿：门太高 ⇒ 剔除（不是拒放；拖拽期不查）
bool TG_DoorTooTall(float DoorCenterZ, float DoorH, float WallTopZ, bool bHostHasFlatRoof)
{ return DoorCenterZ + DoorH * 0.5f >= WallTopZ + (bHostHasFlatRoof ? -79.0f : 0.0f); }
```

| 量 | TG 默认值 | 出处 |
| --- | --- | --- |
| 进门阈值（窗盒底 − 墙基） | **0 cm**（cottage 窗心 < 80 / 3x1 < 85；gothic < 112.05 / 132.8 / 159.95） | `[ASM]` 0x14127F9B3–F9CB |
| 门底离墙基 | **5 cm** | `__real@3d4ccccd`，0x14127F9D6 / 0x14081D404 |
| 复核容差 | **10 cm** | `__real@bdcccccd`，0x14127F890 |
| 门高 / 门宽（碰撞盒） | 262.5 × 114.8 / 146.5 / 172.3（cottage）；315.6 / 332.1 / 354.7 × 147.6 / 177.9 / 250.6（gothic） | `[ASSET]` §3.5 |
| 距角 | 洞心距角点 < W/2 + 46（半木墙 20）即不合法 | `[ASM]` 0x140811956 |
| 阳台地板 | 平顶墙 `max_y − 79`；窗心须高于地板、门底须 ≤ 地板 + 40；平面容差 40 | `[ASM]` §1.2 ① |
| 楼梯射线 | 墙外 50、窗底 +50 起向下 100 | `[ASM]` §1.2 ③ |

### 8.2 与当前实现的差异（逐条标注性质）

| # | 当前 UE | TG | 性质 / 建议 |
| --- | --- | --- | --- |
| 1 | `CSHouse_ResolveDoorForm`：**按当前形态**的盒底 `< DoorSnapHeight(40)` | 拖拽恒按**窗**盒底 `< 0`；已落盘的门按**门**盒底 `− 10 < 0` 复核 | **适配，保留**。TG 那条「抓起门会立刻回成窗」（§1.4）在本项目的「解析不动点」约束下不可接受；按当前形态判正好对应 TG 的双公式（拖拽看窗、复核看门）。40 是为填 `WindowMinSillZ` 死区的本项目常量，TG 没有窗台下限；想逐位对齐 TG 就把两者同时降到 0。 |
| 2 | 门形态 `SillZ = 0`（洞底 = 墙基） | 洞底 = 墙基 **+5 cm**，门网格底再高 1.25 | **建议改成 5**（`Z0 = 5`，锚点 `SillZ` 记 5 或在 `MakeDemand` 里 +5）。纯常量，不影响谓词。若墙基那条砖带/勒脚在 5 cm 处露缝难看，再退回 0 并记为适配。 |
| 3 | 洞宽高取 `DoorMesh`（渲染网格 `balcony_door_rank1`）包围盒 = **120 × 262.5** | cottage：洞取**碰撞网格**包围盒 = **114.8 × 262.5**（rank2 146.5、rank3 172.3）；gothic：三块台阶矩形（§5.1 表） | **建议改**：UE 没导入 `*_collision`，cottage 按 rank 查表（或渲染宽 −5.2 / −3.5 / −7.7）。用 120 的话门框外缘恰好压在洞缘上，盖不住切口（窗那边 78×160 碰撞与渲染同宽，所以窗没暴露）。gothic 若只挖一个矩形会把尖拱两肩挖空、帽件盖不全——要么照 TG 叠三块，要么用本项目已有的 `ECSOpeningShape::Arch` 剪影（适配）。 |
| 4 | `CSHouse_QueryOpening` 对门形态照判 `AboveEave`：`Z1 > WallHeight − LintelBand` | 只判门顶 ≥ 墙顶（平顶墙再 −79） | **风险**：默认 `WallHeight=300`、`LintelBand=40` ⇒ 门顶上限 260，**TG 的 262.5 门在默认墙上会被拒**。建议门形态改判 `Z1 < WallHeight`（TG 原式），或确认 demo 墙高 ≥ 307.5。 |
| 5 | `CornerMargin = 60`（洞缘距外角点） | 洞缘距角 < 46（半木 20）不合法 | 本项目更严，**保留**（适配）。 |
| 6 | `RefreshExtraPieceLayout` 把门网格包围盒中心压到洞心 | TG 门网格**原点**在门心 y；cottage 渲染盒心比原点高 1.25，gothic 低 0.75 / 0.75 / 0.6 | 差 ≤ 1.25 cm，**可忽略**；若按 #2 改成 Z0 = 5，门底与 TG 差 1.25。 |
| 7 | 门形态隐藏 `OpeningMesh/LintelMesh/SillMesh/GlassMesh` | 门不出过梁、窗台、花槽、玻璃（§5.2） | **一致**。 |
| 8 | 门挂件靠子蓝图 `DoorForm` 标签 | TG：`clutter_version ≥ 4` 时 25% 门铃 / 25% 花环 / 50% 无，按装饰物 seed 取 | 建议：`door_bell` 局部 (沿墙 −25, 上 +74, 外 +10)、`door_krans` (0, +50, +20)，**以门心为原点**；二选一按 `MarkerId` 哈希 `& 3`。 |
| 9 | 无 | 落地门且未出踏步、墙基高出地形 > 15 cm ⇒ 出 `balcony_door_rank{n}_rails` / `gothic_balcony_door_rank{n}_rails` | 待做；UE 墙基平、地形在 `HeightOffset` 下时才会触发。踏步判据见 §4.3（归楼梯那份报告实现）。 |
| 10 | 无 | 阳台门（别的墙平顶）、楼梯门 | 本项目没有平顶步道 / 玩家楼梯前**不做**；做时 `platform` 应作为锚点字段（TG 形态真源是 `platform.is_valid()`，`is_bottom_door` 只区分落地子型）。 |

### 8.3 变门时网格件对照（TG 确凿）

| 本项目槽位 | 窗形态（cottage 1x1） | 门形态 cottage rank *n* | 门形态 gothic rank *n* |
| --- | --- | --- | --- |
| 定洞件 | `decorators_window_cottage_1x1`（78×160） | `balcony_door_rank{n}`（门扇+门框一体） | `balcony_door_gothic_rank{n}` |
| 盖顶件 | `setdressing_window_lintel` | **无** | `balcony_door_gothic_rank{n}_hat`（与门同原点） |
| 窗台 | `setdressing_window_sill` | **无** | **无** |
| 玻璃 | `window_cottage_1x1_glass` | **无** | **无** |
| 栏杆（条件件） | — | `balcony_door_rank{n}_rails` | `gothic_balcony_door_rank{n}_rails` |
| 挂件（随机） | 花槽体系 | `door_bell` / `door_krans`（`door_krans_autumn`） | 同左 |

rank 对应：窗 1x1/2x1/3x1 ↔ 门 rank1/2/3（同一个 `DecoratorRank`）。`ArrowSlit` 类不变门（本项目 `bCanBecomeDoor=false` 已对位）。

## 附：辅助证据与未查到项

### 附 1 射线命中种类（§1.2 ③ 用到）【确凿】

`convert_raycast_hit_to_decorator_dst` 跳表 0x142C24118 `[ASM]`：种类 `0`→`WallAttachment`、`2`→`RoofAttachment`、
`0x0B`→`TerrainAttachment`（需 `can_place_on_terrain`）、**`0x0D`→`StairAttachment{graph_edge, uv}`**（需 `can_place_on_stair_top`），其余不可放；
`Rapier::raycast_simple` 未命中写 `0x11` `[ASM]` 0x1408CE3D2。种类→子世界表 0x142AA4415 里 `0x0D → 12`，
所以 `snap_balcony_door` 传的过滤位 `0x1000 = 1<<12` 只打玩家楼梯。命中结果布局：`+0x18 hit.xy`、`+0x20 hit.z`、`+0x24 toi`
（0x1408CE3C1–CB），`snap` 读的 `[rsp+5Ch]` = `+0x1C` = `hit.y`。

### 附 2 试过但没拿到 / 没坐实的

| 项 | 试过 | 结果 |
| --- | --- | --- |
| `BalconySnapResult` 变体名 | PDB 只有类型名；exe 串里搜 `BalconySnap*`、Debug 表 | **未查到**（无 Debug impl），tag 语义按分支行为命名 |
| `DecoratorType` 的 Debug | 反汇编标签 `DecoratorType$u20$as$u20$core..fmt..Debug` 0 命中 | 改用 serde 变体串 + 位运算反推，已坐实 |
| `impl$4::dims_wall_space`、`find_closest_norm_curve_t`、`is_wall_decorator_location_valid` | 反汇编标签 0 命中 | 已内联，未单独分析（不影响判据） |
| `PublicWallState` +0x40 与 +0x90 两条墙底折线的区别 | 找 serde / Debug 字段表（`PublicWallState` 无 serde） | **未坐实**；本项目墙基平，二者都退化为墙基 |
| 拖拽时「门太高」是否有 UI 提示 | `ui_move_decorator` / `ui_place_decorator` 调用表里没有 `is_decorator_location_valid` | **未查到**；只确认剔除发生在 `cull_oob_decorators` |
| 「抓起落地门会立即回窗」 | 按 `try_get_grab_offset_ss_if_valid` + 判据推演 | 【推测】，未进游戏验证 |
| `balcony_door_single` 用途 | cottage 门生成器字符串区 0x142A8BA00–BC40 | 未被引用，疑为遗留 |
| 门前踏步几何（`construct_door_stairs`） | — | 按分工**未分析** |

### 附 3 工具与复现

- 反汇编：`dumpbin /DISASM:NOBYTES tiny-glade.exe | python grab2.py 标签关键词 函数体关键词 out.asm`（按标签或函数体子串流式保留，单次约 15 s，共跑 6 次）。
- 常量/串：PE 节表把 VA 映射到文件偏移后直接读（`.rdata` 0x142925000 起）；Debug 变体名 = 长度表 + 相对偏移表。
- 资产：`extracted/meshes/**.glb` 的 POSITION accessor min/max（TG 米 ×100）。
