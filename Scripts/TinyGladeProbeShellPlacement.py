# -*- coding: utf-8 -*-
"""岩壳"长错地方"的定位探针：把塑形物 / 地面 / 活三角三者的世界坐标一次打全。

起因（2026-08-31）：`L_TerrainOpsDemo` 的画面上碎石只出现在地面矩形的一个角上，而
`demo_rock_shell` 的三条断言（skirt=1109 / plateau=0 / flat=0）**全绿** —— 那三条是
**相对塑形物自身位置**分类的，所以塑形物摆在哪它们都会绿，证明不了"壳在该在的地方"。

这个脚本回答的就是那一条：**活三角的世界坐标质心，离塑形物中心有多远。**

判据（自带阈值，红了会以非零退出码 + FAIL 行报出来）：
  · 活三角质心到塑形物中心的距离 <= Radius + FalloffDistance（壳应当抱着裙边环）
  · 活三角的世界 XY 包围盒必须落在地面矩形内（域外剔除生效）

跑法（编辑器不能开着）：
  UnrealEditor-Cmd.exe <uproject> -ExecutePythonScript="<本文件>" -unattended -nosplash -stdout -AbsLog=<log>
"""
import unreal

FAILS = []
PASSES = [0]


def check(label, ok, detail=""):
    if ok:
        PASSES[0] += 1
        unreal.log("[PASS] %s %s" % (label, detail))
    else:
        FAILS.append(label)
        unreal.log_error("[FAIL] %s %s" % (label, detail))


def actors():
    return unreal.EditorLevelLibrary.get_all_level_actors()


def find(label):
    for a in actors():
        if a and a.get_actor_label() == label:
            return a
    return None


def vec(v):
    return "(%.1f, %.1f, %.1f)" % (v.x, v.y, v.z)


unreal.EditorLoadingAndSavingUtils.load_map("/PCGPlugins/HouseTest/L_TerrainOpsDemo")

ground = find("Ground_Demo")
shaper = find("Shaper_Mound")
check("actors present", ground is not None and shaper is not None,
      "ground=%s shaper=%s" % (ground, shaper))
if not (ground and shaper):
    unreal.log("PROBE FAILED")
    raise SystemExit

# ---------------------------------------------------------------- 1) 三个 actor 的摆位
g_loc = ground.get_actor_location()
g_rot = ground.get_actor_rotation()
g_scale = ground.get_actor_scale3d()
s_loc = shaper.get_actor_location()

unreal.log("---- 摆位 ----")
unreal.log("ground  loc=%s rot=(%.2f, %.2f, %.2f) scale=%s"
           % (vec(g_loc), g_rot.roll, g_rot.pitch, g_rot.yaw, vec(g_scale)))
unreal.log("shaper  loc=%s" % vec(s_loc))

# ---------------------------------------------------------------- 2) 地面与岩壳的参数
def prop(obj, name, default=None):
    try:
        return obj.get_editor_property(name)
    except Exception:
        return default


cell = prop(ground, "CellSize")
nx = prop(ground, "NumVertsX")
ny = prop(ground, "NumVertsY")
slope_lo = prop(ground, "RockShellSlopeLo")
slope_hi = prop(ground, "RockShellSlopeHi")
cell_size = prop(ground, "RockShellCellSize")   # 程序化胞腔格距（2026-09-30 起取代 PatternScale）
rock_on = prop(ground, "bRockShell")

radius = 0.5 * (prop(shaper, "MaskSize") or 0.0)   # Radius 已删（09-29）：圆盘直径 = MaskSize
falloff = prop(shaper, "FalloffDistance")
lift = s_loc.z - g_loc.z   # LiftHeight 已删（09-29）：台顶 = actor 相对地面的高度
lift2 = prop(shaper, "SecondaryLiftScale")
noise = prop(shaper, "SkirtNoiseAmount")

unreal.log("---- 参数 ----")
unreal.log("ground  CellSize=%s NumVerts=%sx%s bRockShell=%s" % (cell, nx, ny, rock_on))
unreal.log("shell   SlopeLo=%s SlopeHi=%s CellSize=%s" % (slope_lo, slope_hi, cell_size))
unreal.log("shaper  MaskSize/2=%s Falloff=%s Top=%s SecondaryLiftScale=%s SkirtNoise=%s"
           % (radius, falloff, lift, lift2, noise))

# 解析上界：max|∇h| = Lift × 1.5 / Falloff（剖面的最陡处）。低于 SlopeLo 就一块石头都不长。
if lift is not None and falloff:
    max_grad = float(lift) * 1.5 / float(falloff)
    unreal.log("推算 max|grad| = Lift*1.5/Falloff = %.4f   (SlopeLo=%s)" % (max_grad, slope_lo))
    check("the mound is steep enough for the shell to appear at all",
          max_grad > float(slope_lo), "maxGrad=%.4f > SlopeLo=%.2f" % (max_grad, float(slope_lo)))

# ---------------------------------------------------------------- 3) 活三角的世界坐标
ground.set_editor_property("bRockShell", True)
shaper.rebuild_terrain()

res = ground.debug_read_rock_shell_sync()
if isinstance(res, tuple):
    total_verts, pos = int(res[0]), list(res[1])
else:
    total_verts, pos = int(res), []
tris = total_verts // 3
unreal.log("---- 回读 ----")
unreal.log("三角总数 %d，顶点 %d" % (tris, total_verts))

alive = []
for t in range(tris):
    p = pos[t * 3]
    if p.x != p.x or p.y != p.y or p.z != p.z:      # 第 0 个顶点是 NaN = 这个三角被关掉了
        continue
    alive.append((p.x, p.y, p.z))

check("some shell triangles are alive", len(alive) > 0, "alive=%d / %d" % (len(alive), tris))
if not alive:
    unreal.log("PROBE FAILED")
    raise SystemExit

xs = [a[0] for a in alive]
ys = [a[1] for a in alive]
cx, cy = sum(xs) / len(xs), sum(ys) / len(ys)
unreal.log("活三角 %d 个" % len(alive))
unreal.log("活三角世界 XY 包围盒  X[%.1f, %.1f]  Y[%.1f, %.1f]" % (min(xs), max(xs), min(ys), max(ys)))
unreal.log("活三角世界 XY 质心    (%.1f, %.1f)" % (cx, cy))

# ---- 关键对照：显式重建一次岩壳，看活三角会不会搬回塑形物身边 ----
# 若"重建后才对"，那就不是映射错，而是**壳没有在塑形物注册/移动之后重新位移过**（陈旧态）。
# 回归之所以全绿，正因为它在这一节之前跑了很多趟重建。
def alive_of(ground_actor):
    r = ground_actor.debug_read_rock_shell_sync()
    n, ps = (int(r[0]), list(r[1])) if isinstance(r, tuple) else (int(r), [])
    out = []
    for i in range(n // 3):
        q = ps[i * 3]
        if not (q.x != q.x or q.y != q.y or q.z != q.z):
            out.append((q.x, q.y, q.z))
    return out


def summarise(tag, lst):
    if not lst:
        unreal.log("  %-18s alive=0" % tag)
        return
    ax = sum(v[0] for v in lst) / len(lst)
    ay = sum(v[1] for v in lst) / len(lst)
    near = sum(1 for v in lst
               if ((v[0] - s_loc.x) ** 2 + (v[1] - s_loc.y) ** 2) ** 0.5 <= float(radius or 0) + float(falloff or 0))
    unreal.log("  %-18s alive=%-6d centroid=(%8.1f, %8.1f)  近塑形物 %d/%d" % (tag, len(lst), ax, ay, near, len(lst)))


unreal.log("---- 重建对照 ----")
summarise("load state", alive)
ground.rebuild_heights_from_shapers()
summarise("after heights", alive_of(ground))
ground.rebuild_rock_shell()
after = alive_of(ground)
summarise("after rebuild_shell", after)
if after:
    alive = after
    xs = [a[0] for a in alive]
    ys = [a[1] for a in alive]
    cx, cy = sum(xs) / len(xs), sum(ys) / len(ys)

# 逐三角的 Z 与位置抽样：能区分"被 Displace 位移过"和"基底几何从没被写过"。
zs = [a[2] for a in alive]
unreal.log("活三角 Z 区间 [%.2f, %.2f]，均值 %.2f" % (min(zs), max(zs), sum(zs) / len(zs)))
near_shaper = 0
for (ax, ay, az) in alive:
    if ((ax - s_loc.x) ** 2 + (ay - s_loc.y) ** 2) ** 0.5 <= float(radius or 0) + float(falloff or 0):
        near_shaper += 1
unreal.log("活三角中落在塑形物触及范围内的：%d / %d" % (near_shaper, len(alive)))
unreal.log("前 8 个活三角的第 0 顶点：")
for (ax, ay, az) in alive[:8]:
    unreal.log("    (%9.1f, %9.1f, %8.2f)  h_mirror=%.2f" % (ax, ay, az, ground.sample_height(unreal.Vector2D(ax, ay))))

# ---------------------------------------------------------------- 3b) 高度场到底在哪
# 这一步分开"场在哪"与"壳在哪"：两者不一致，错的就在壳的世界映射；一致则是关卡摆位。
unreal.log("---- 高度场采样（镜像，CPU 权威）----")
for label, x, y in (("shaper centre", s_loc.x, s_loc.y),
                    ("world origin", 0.0, 0.0),
                    ("live centroid", cx, cy),
                    ("shaper + radius", s_loc.x + float(radius or 0), s_loc.y),
                    ("centroid + radius", cx + float(radius or 0), cy)):
    h = ground.sample_height(unreal.Vector2D(x, y))
    unreal.log("  h(%-18s (%8.1f, %8.1f)) = %8.2f" % (label, x, y, h))

# ⚠️ 猎捕期这里曾有一条 "h(shaper) > h(liveCentroid)" 的断言 —— 修好之后它必然自相矛盾
# （壳就该长在土台上，两处高度当然相等）。留着会变成一条永远红的假门，已删。
h_shaper = ground.sample_height(unreal.Vector2D(s_loc.x, s_loc.y))
check("the mirror really carries the mound (otherwise everything below is vacuous)",
      h_shaper > 1.0, "h(shaper)=%.2f" % h_shaper)

# ---------------------------------------------------------------- 4) 判据
d = ((cx - s_loc.x) ** 2 + (cy - s_loc.y) ** 2) ** 0.5
reach = float(radius or 0.0) + float(falloff or 0.0)
unreal.log("质心到塑形物中心 = %.1f cm   (Radius + Falloff = %.1f)" % (d, reach))
check("the live shell hugs the shaper (its centroid is inside the skirt reach)",
      d <= reach, "dist=%.1f reach=%.1f shaper=(%.1f, %.1f) centroid=(%.1f, %.1f)"
      % (d, reach, s_loc.x, s_loc.y, cx, cy))

# 地面矩形：从地面 actor 的位置与格数推（与 GetWorldRect2D 同口径：以 actor 为中心）。
if cell and nx and ny:
    half_x = 0.5 * float(cell) * (int(nx) - 1)
    half_y = 0.5 * float(cell) * (int(ny) - 1)
    rmin = (g_loc.x - half_x, g_loc.y - half_y)
    rmax = (g_loc.x + half_x, g_loc.y + half_y)
    unreal.log("地面矩形（按 actor 中心推）X[%.1f, %.1f] Y[%.1f, %.1f]" % (rmin[0], rmax[0], rmin[1], rmax[1]))
    inside = (min(xs) >= rmin[0] - 1.0 and max(xs) <= rmax[0] + 1.0
              and min(ys) >= rmin[1] - 1.0 and max(ys) <= rmax[1] + 1.0)
    check("every live shell vertex stays inside the ground rect (domain cull works)", inside,
          "bbox X[%.1f,%.1f] Y[%.1f,%.1f]" % (min(xs), max(xs), min(ys), max(ys)))

unreal.log("passed=%d failed=%d" % (PASSES[0], len(FAILS)))
for f in FAILS:
    unreal.log_error("  failed: %s" % f)
unreal.log("PROBE FAILED" if FAILS else "PROBE OK")
