# -*- coding: utf-8 -*-
"""把演示关卡的地面尺寸与摆位调到各自需要的档，并把摆位挪进新范围。

地面的网格从 actor 位置往 +X/+Y 铺（WorldToGrid 用 (WorldXY − Origin)/Cell），
所以 N 格 × 50 cm 的地面占 (0,0)–(N·50, N·50)，中心在 N·25。
摆位不跟着挪的话房子整个掉出地面，症状是"落座回 0、门全关、石阶一级都不长"，
而每一条看起来都像别的 bug。

**两张关卡的地面不再同尺寸**，理由是它们量的东西不同：

| 关卡 | 地面 | 为什么 |
| --- | --- | --- |
| `L_HouseGroundDemo` | 64 格 = 32 m | 量的是房子（footprint 600×400）、落座、门拱、砖缝。32 m 足够两栋并排，越小越快 |
| `L_TerrainOpsDemo` | **256 格 = 128 m** | 量的是地形链：塑形物剖面、石阶、**披挂岩壳** |

⚠️ **地形演示关卡为什么必须放大到 128 m**（2026-08-30，链 B 动工时）：

1. 岩壳的碎裂图案是 Tiny Glade 原件，胞腔 **5.53 m**、tile 136.5 m，**不可平铺也不宜缩放**
   （tile 实测不是周期的；缩放会丢掉「与 TG 同绝对密度」这个唯一的观感锚点）。
2. 5.53 m 的胞腔让计划 D9「密度与尺度」的塑形物尺度警告从建议变成**硬要求**：
   原来的 `Radius=300` / `Falloff=400` 裙边只有 4 m 宽，装不下一个胞腔 ⇒ **一块碎石都不会出现**。
   改成 `Radius=600` / `Falloff=800`（计划推荐的出路 1），裙边 8 m。
3. 但影响半径随之从 700 涨到 **1400** cm。旧的 32 m 地面上：单座勉强放得下（中心 1600，
   波及 200..3000），而回归 `demo_gpu_stairs` 那段要在 `c.x + 间距` 再摆**第二座**，
   第二座的裙边会伸出地面矩形 —— 地面边缘因此变成一圈台高的悬崖，石阶与岩壳都会长在那儿，
   把"两座交汇处"那段断言测的东西弄脏。
4. 128 m 同时正好是岩壳图案 136.5 m tile 能盖住的最大地面（每边富余 4.25 m），
   也是计划 D9「密度与尺度」表里写的本项目口径（`NumCells 256 × CellSize 50`）。

`L_HouseGroundDemo` 一格没动 —— 它那 26 条断言与 4 条零阻塞断言全部照旧。
"""
import unreal

PKG = "/PCGPlugins/HouseTest"
CELL_SIZE = 50.0

HOUSE_CELLS = 64                                  # 32 m
HOUSE_CENTRE = HOUSE_CELLS * CELL_SIZE * 0.5      # 1600

TERRAIN_CELLS = 256                               # 128 m，见上面第 4 条
TERRAIN_CENTRE = TERRAIN_CELLS * CELL_SIZE * 0.5  # 6400

# 塑形物：岩壳的 5.53 m 胞腔要求裙边装得下（计划 D9 出路 1）。影响半径 = 600 + 800 = 1400。
SHAPER_RADIUS = 600.0
SHAPER_FALLOFF = 800.0

# ⚠️ **台高必须跟着羽化一起抬** —— 这一条计划里没写，是链 B 动工时实测出来的：
#
#   裙边剖面是 smoothstep，它的最大斜率是 1.5，所以 max|∇h| = 台高 × 1.5 / Falloff（台高 = actor 相对地面的高度）。
#   岩壳的坡度软阈照抄 TG：smoothstep(0.75, 1.25, |∇h|)。
#   把 Falloff 从 400 拉到 800（为了装下 5.53 m 的胞腔）之后，台高还是 300 的话
#   max|∇h| = 300 × 1.5 / 800 = **0.5625**，**整个土台都在 0.75 阈下** ⇒ 一块碎石都不会长
#   （实测：只有裙边噪声碰巧抄过阈的那几处出了 178 个三角，画面上是零星几块）。
#
#   反解：要让最陡处达到软阈上端 1.25（mask 满值），台高 ≥ 1.25 × 800 / 1.5 = 667。
#   取 700：max|∇h| = 1.3125，陡坡带拿满 mask，而台顶与盘外仍然是平的（坡度 0）。
#
#   这正是计划自己那句话的量化版："1.5 m 的小土包本来就不该长岩壁，TG 里也只有大悬崖长"。
#   链条是：**胞腔尺寸 → 羽化宽度 → 坡度 → 台高**，计划只写了第一环。
SHAPER_LIFT = 700.0

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def find(label):
    return next((a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == label), None)


def resize_ground(cells):
    g = find("Ground_Demo")
    if not g:
        unreal.log_error("RESIZE: no Ground_Demo")
        return None
    g.set_editor_property("NumCellsX", cells)
    g.set_editor_property("NumCellsY", cells)
    g.set_editor_property("CellSize", CELL_SIZE)
    g.call_method("RebuildGroundMesh")
    unreal.log("RESIZE ground -> %d x %d @ %.0f cm = %.0f m" % (cells, cells, CELL_SIZE, cells * CELL_SIZE / 100.0))
    return g


def move(label, x, y):
    a = find(label)
    if not a:
        return
    z = a.get_actor_location().z
    a.set_actor_location(unreal.Vector(x, y, z), False, False)
    unreal.log("RESIZE moved %-16s -> (%.0f, %.0f)" % (label, x, y))


# ---- L_HouseGroundDemo（不变）----
unreal.EditorLoadingAndSavingUtils.load_map("%s/L_HouseGroundDemo" % PKG)
resize_ground(HOUSE_CELLS)
# 两栋房子并排放在地面中带，footprint 600×400，彼此与边界都留出余量。
move("House_Road", 1100.0, HOUSE_CENTRE)
move("House_Pillar", 2200.0, HOUSE_CENTRE)
move("TG_Sun", HOUSE_CENTRE, HOUSE_CENTRE)
move("TG_SkyLight", HOUSE_CENTRE, HOUSE_CENTRE)
move("TG_Start", 1100.0, HOUSE_CENTRE - 900.0)
for label in ("House_Road", "House_Pillar"):
    a = find(label)
    if a:
        a.call_method("RebuildHouse")
unreal.EditorLoadingAndSavingUtils.save_current_level()
unreal.log("RESIZE L_HouseGroundDemo saved")

# ---- L_TerrainOpsDemo ----
unreal.EditorLoadingAndSavingUtils.load_map("%s/L_TerrainOpsDemo" % PKG)
resize_ground(TERRAIN_CELLS)
# 影响半径 600 + 800 = 1400，摆中心（6400）离边界还有 5000，第二座摆在 +1600 也放得下。
move("Shaper_Mound", TERRAIN_CENTRE, TERRAIN_CENTRE)
move("TG_Sun", TERRAIN_CENTRE, TERRAIN_CENTRE)
move("TG_SkyLight", TERRAIN_CENTRE, TERRAIN_CENTRE)
move("TG_Start", TERRAIN_CENTRE, TERRAIN_CENTRE - 2400.0)
s = find("Shaper_Mound")
if s:
    # ⚠️ 垂直归位不能省：2026-09-29 起台顶 = actor 相对地面的高度（ComputeTopHeight = Z − 地面Z），
    # 基准态的 Z 就是"地面 Z + SHAPER_LIFT"。"塑形物上下拖动"实验会把拖过的 Z 存进关卡，
    # 不归位的话回归的地形断言全红，看起来像高度场坏了。
    g = find("Ground_Demo")
    gz = g.get_actor_location().z if g else 0.0
    loc = s.get_actor_location()
    if abs(loc.z - (gz + SHAPER_LIFT)) > 0.01:
        unreal.log("RESIZE Shaper_Mound z %.0f -> %.0f (vertical reset)" % (loc.z, gz + SHAPER_LIFT))
        s.set_actor_location(unreal.Vector(loc.x, loc.y, gz + SHAPER_LIFT), False, False)
    s.set_editor_property("MaskSize", 2.0 * SHAPER_RADIUS)
    s.set_editor_property("FalloffDistance", SHAPER_FALLOFF)
    s.call_method("RebuildTerrain")
    unreal.log("RESIZE Shaper_Mound -> Radius %.0f / Falloff %.0f / Lift %.0f  (max slope %.3f)"
               % (SHAPER_RADIUS, SHAPER_FALLOFF, SHAPER_LIFT, SHAPER_LIFT * 1.5 / SHAPER_FALLOFF))
unreal.EditorLoadingAndSavingUtils.save_current_level()
unreal.log("RESIZE L_TerrainOpsDemo saved")
unreal.log("RESIZE DONE")
