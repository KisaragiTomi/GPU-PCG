# -*- coding: utf-8 -*-
"""岩壳最外圈：`Noise` 该不该也乘 `Rock` —— **只出对照图 + 量化，不下结论**。

争议点在 `Shaders/Private/CSGroundRockShell.usf` 的这一行::

    Pos += N * ((Relief * Rock + Noise) * (1 - Road) - RoadSink * Road);

`bAlive` 的硬阈正好是 `SlopeLo`，而 `Rock = smoothstep(SlopeLo, SlopeHi, |∇h|)` 在 `SlopeLo`
处**恰好为 0** ⇒ 最外圈活三角的 `Relief` 被乘没了（顶盖与裙塌到同一个面 = 地面本身），
而 `Noise` 仍是满幅 ±`NoiseAmp` ⇒ 那一圈是**贴着地面上下各半**。
数学上这是个缺陷；但它也可能正是想要的「石头渐渐没入草地」—— TG 的岩壁边缘本来就该化开，
一乘 `Rock` 反而可能得到一圈过于干净的边。**两种都拍出来，交给人看。**

本脚本**不改任何东西**：变体由外部改 `.usf` 那一行再跑第二遍决定，tag 区分两组产物。

用法（两遍，中间改 usf，拍完还原）::

    set TG_SHOT_TAG=noiseraw          # 现状
    UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotRockNoise.py"
    # 把那一行改成 ((Relief + Noise) * Rock) 之后再跑一遍
    set TG_SHOT_TAG=noisexrock
    UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotRockNoise.py"

产物：`Saved/TinyGladeShots/rocknoise_<tag>_*.png` + `rocknoise_<tag>_samples.json`
（采样格 + 逐顶点统计，供 `TinyGladeShotRockNoiseStats.py` 跨两次运行比对）。

--- 机位为什么是这三个 ---------------------------------------------------------------
争议只发生在**最外圈那一环**（`Rock ≈ 0` 而 `Noise` 满幅），所以三个机位全部围着那一环转，
半径由 `sample_height` 现场反算，不手调 —— 手调的机位在土台参数一动之后就全废了：

  `edge_face`   贴脸：眼睛与外沿环齐高、视线近乎水平。分辨"边缘是化开还是一刀切"。
  `mid`         中景：整条岩壁带 + 外沿 + 草地一起入画，看整体读数而不是局部。
  `low_graze`   低机位掠射：眼高只有 25 cm，视线擦着地面看过去 ——
                **垂直方向的几厘米在掠射下会被放大成明显的天际线缺口**，
                "半沉半浮"能不能看出来就靠这一张。
  `edge_45`     45° 俯看外沿环：贴脸看不全的那一圈的分布形态。

--- 踩过的坑（全部照抄状态文件的坑表，一条都别省）------------------------------------
 ① `unreal.Rotator` 的参数序是 **(roll, pitch, yaw)**，一律关键字参数。
 ② 必须**真编辑器** + `-ExecCmds="py <脚本>"`；`-ExecutePythonScript` 跑完即退，tick 不触发。
 ③ 离屏 `SceneCapture2D`，不要 `HighResShot`（本工程起来是 4 分屏）。
 ④ `create_render_target2d` 显式传 `RTF_RGBA8`（默认浮点 ⇒ png 是 HDR 内容）。
 ⑤ 离屏 capture 被引擎写死关掉 Lumen，只能用捕获组件自己的 `post_process_settings` 覆盖回来。
 ⑥ 准备段抛异常 ⇒ 编辑器**永不退出**，整段包在 try 里。
 ⑦ GPU 实例石阶在离屏 capture 下画不对，关掉再拍（岩壳是网格路，不受影响）。
 ⑧ `always_persist_rendering_state = True`，否则 PPV 里的曝光一条都不生效。
 ⑨ 有了 ⑧ 才会触发 ⑨：Lumen 靠帧间历史，每个机位导出前必须**一帧一次**预热 32 次。
    ⑧⑨ 缺一不可 —— 只加 ⑧ 会让间接光恒为零，图比不加还糟。
 ⑩ 脚本一律调 `get_rock_shell_undrawable_reason()`，**不要**调 `is_rock_shell_drawable()`：
    后者不可画时返回 `None`，`str(None) == "None"` 会伪造出一句像模像样的原因。
"""
import json
import math
import os

import unreal

TAG = os.environ.get("TG_SHOT_TAG", "noiseraw")
PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900

GRID_X, GRID_Y = 48, 27
ZERO_FAIL = 0.005            # 坑 ⑨ 的门，与 TinyGladeShotSofteningStats.py 的 ZERO_FAIL 同源
FIRST_TICK = 45
SETTLE = 6
# 坑 ⑨ 的执行面。**128 不是保守取整，32 是真的不够**（2026-08-31 重标定）。
# 两档收敛得分开看（整表与重跑方式在 `TinyGladeShotSeamWarmup.py` 的文件头）：
#   ① 「间接光不恒零」：zero% 在 **16 次**就到 0.000% —— 当初取 32 的全部依据，
#      而它只能证明"不是黑图"；
#   ② 「画面不再变」：相对 w384 的差异点占比 16 次 45.293% / **32 次 0.540%** /
#      64 次起 0.000%（192 次那 0.077% = 1/1296，是采样噪声底）。
#      另一个更难收的场景（上一轮的拖动态）：w32 差 2.04%、w128 差 0.015%。
# ⇒ 32 会给每一条像素判据搭上 0.5%~2% 的**系统误差**，与最薄那条门
#   （zero > 0.5%）同量级；取 128 = 收敛平台往里一整个倍频。
#   代价实测：一次 capture_scene() ≈ 35 ms（1081 次 / 38 s）⇒ 每张图多约 3.4 s。
# ⚠️ 必须**一帧一次**：同一帧连打 128 次不推进帧间历史，等于没预热。
# ⚠️ 环境变量只给**纯记录图**降档（set TG_SHOT_WARMUP=32），带门的图一律 128。
WARMUP_CAPTURES = int(os.environ.get("TG_SHOT_WARMUP", "128"))

# 法向垂距剖面的半径分箱宽度 cm。
# ⚠️ **不抽样，全量扫**：49,598 个三角里只有 1,112 个活着，抽样步长哪怕取 37 也只落到个位数个
# 外沿顶点（第一次跑就是这么翻的，n=4）。NaN 过滤是纯 Python 循环（14.9 万次，约 1 秒），
# 高度/梯度采样只对活顶点做（3,336 个 × 5 次）。
RADIUS_BIN = 100

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"ticks": 0, "handle": None, "plan": [], "world": None, "ground": None,
         "samples": {}, "rt": None, "comp": None, "spawned": [], "step": 0, "phase": 0,
         "stats": {}}


def look_at(frm, to):
    """⚠️ unreal.Rotator 的参数序是 (roll, pitch, yaw)，只用关键字参数（坑 ①）。"""
    dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
    flat = math.hypot(dx, dy)
    return unreal.Rotator(roll=0.0,
                          pitch=math.degrees(math.atan2(dz, max(flat, 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


def find(label):
    return next((a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == label), None)


def try_set(obj, prop, value):
    try:
        obj.set_editor_property(prop, value)
        return True
    except Exception as exc:
        unreal.log_warning("ROCKNOISE skip %s: %s" % (prop, exc))
        return False


def height_at(ground, x, y):
    return ground.sample_height(unreal.Vector2D(x, y))


def grad_mag(ground, x, y, d):
    """与 kernel 的 `CSRockShell_GradientXY` 同形的中心差分（差分跨距 = 地面格距）。

    不用解析导数：高度场是"全部塑形物重叠取 max"，接缝处不可导 —— 而外沿环正是这条判据
    唯一起作用的地方，解析导数会在那里给出错的陡峭度。
    """
    dx = height_at(ground, x + d, y) - height_at(ground, x - d, y)
    dy = height_at(ground, x, y + d) - height_at(ground, x, y - d)
    return math.hypot(dx, dy) / (2.0 * d)


def solve_edge_radius(ground, c, slope_lo, cell, r_hi):
    """外沿环半径：|∇h| 从上往下**穿过 `SlopeLo`** 的那个半径（Rock 恰好为 0 的地方）。

    从最陡的一圈往外扫而不是从中心往外扫：剖面是 smoothstep，`|∇h| = SlopeLo` 有**两个**解
    （内侧靠台顶、外侧靠平地），争议中的"渐渐没入草地"是外侧那一个。
    """
    step = max(cell * 0.5, 25.0)
    samples = []
    r = step
    while r <= r_hi:
        samples.append((r, grad_mag(ground, c.x + r, c.y, cell)))
        r += step
    if not samples:
        return r_hi * 0.5, r_hi * 0.5, 0.0
    r_max, g_max = max(samples, key=lambda s: s[1])
    r_out = r_max
    for r, g in samples:
        if r > r_max and g < slope_lo:
            r_out = r
            break
    return r_out, r_max, g_max


def build():
    unreal.EditorLoadingAndSavingUtils.load_map("%s/L_TerrainOpsDemo" % PKG)
    world = unreal.EditorLevelLibrary.get_editor_world()

    ground = find("Ground_Demo")
    shaper = find("Shaper_Mound")
    if not (ground and shaper):
        unreal.log_error("ROCKNOISE FAILED: no Ground_Demo / Shaper_Mound")
        return None
    STATE["ground"] = ground

    # 坑 ⑦：石阶那条实例路在离屏 capture 下画不对，且与本次判读无关。
    try_set(ground, "StairMesh", None)
    ground.set_editor_property("bRockShell", True)
    ground.reset_paint()                    # 这一组对照里**不画路** —— 路是另一条效果，别混进来
    shaper.call_method("RebuildTerrain")

    # 坑 ⑩：只调 get_rock_shell_undrawable_reason()。is_rock_shell_drawable() 不可画时返回
    # None，str(None) == "None" 会伪造出一句像模像样的原因，看着像"材质缺了"。
    why = str(ground.get_rock_shell_undrawable_reason())
    unreal.log("ROCKNOISE drawable-reason=%r（空串 = 可画）" % why)
    mat = ground.get_editor_property("RockShellMaterial")
    unreal.log("ROCKNOISE material=%s" % (mat.get_name() if mat else "<NONE>"))

    c = shaper.get_actor_location()
    radius = 0.5 * shaper.get_editor_property("MaskSize")   # 塑形物的 Radius 已删（09-29）：圆盘直径 = MaskSize，设了 mask 时是近似
    falloff = shaper.get_editor_property("FalloffDistance")
    reach = radius + falloff
    cell = ground.get_editor_property("CellSize")
    slope_lo = ground.get_editor_property("RockShellSlopeLo")
    slope_hi = ground.get_editor_property("RockShellSlopeHi")
    noise_amp = ground.get_editor_property("RockShellNoiseAmount")
    relief = ground.get_editor_property("RockShellCellRelief")

    r_out, r_max, g_max = solve_edge_radius(ground, c, slope_lo, cell, reach + 1200.0)
    STATE.update({"centre": c, "reach": reach, "cell": cell, "slope_lo": slope_lo,
                  "slope_hi": slope_hi, "r_out": r_out, "r_max": r_max})
    unreal.log("ROCKNOISE geom: radius=%.0f falloff=%.0f reach=%.0f cell=%.0f | "
               "SlopeLo=%.2f SlopeHi=%.2f NoiseAmp=%.1f Relief=%.1f | "
               "max|grad|=%.3f @ r=%.0f，外沿环（Rock→0）r=%.0f"
               % (radius, falloff, reach, cell, slope_lo, slope_hi, noise_amp, relief,
                  g_max, r_max, r_out))

    def gz(x, y):
        return height_at(ground, x, y)

    # 争议只发生在**外沿那一环**，四个机位全部围着它转。
    #
    # 瞄点取 `r_out + 130` 而不是环内 —— 两版都翻在这上面，记清楚为什么：
    # `r_out` 是**宏观**坡度跌破 `SlopeLo` 的半径（这里 1200），但壳的活三角一直铺到 1400，
    # 因为地面还叠着**裙边噪声**，宏观平地上照样有局部超阈的小坡（实测 max|∇h| = 2.02，
    # 而纯 smoothstep 剖面的解析上界只有 Lift×1.5/Falloff = 1.31）。
    # ⇒ 真正的"石头没入草地"发生在 r ≈ 1250..1400 的一片**散落**里，不是一条干净的环。
    # 第一版瞄 `r_out − 120`、第二版瞄 `r_out − 200`，两次都瞄在坡面中段，出来一整屏岩壁。
    ax, ay = c.x + r_out + 130.0, c.y
    za = gz(ax, ay)
    # 站位在瞄点再外 570 cm 的平地上 —— 够近能看清单个胞腔，又不至于插进几何里。
    sx, sy = c.x + r_out + 700.0, c.y
    sz = gz(sx, sy)
    unreal.log("ROCKNOISE cams: 瞄点 r=%.0f z=%.0f；站位 r=%.0f z=%.0f"
               % (r_out + 130.0, za, r_out + 700.0, sz))

    # ① 贴脸：570 cm 外、眼高 130 cm 微俯 —— 分辨"边缘是化开还是一刀切"。
    cam_face = unreal.Vector(sx, sy, sz + 130.0)
    aim_face = unreal.Vector(ax, ay, za + 25.0)

    # ② 中景：整条岩壁带 + 外沿 + 草地一起入画（与 TinyGladeShotRockShell 的 wall 机位同源）。
    mid_r = radius + 0.5 * falloff
    cam_mid = unreal.Vector(c.x + reach + 900.0, c.y, gz(c.x + reach + 900.0, c.y) + 320.0)
    aim_mid = unreal.Vector(c.x + mid_r, c.y, gz(c.x + mid_r, c.y) + 60.0)

    # ③ 低机位掠射：眼高只有 22 cm，视线擦着草面看过去 —— **垂直方向的几厘米在掠射下会被
    #    放大成天际线上的缺口/凸起**，"半沉半浮"能不能看出来就靠这一张。
    cam_low = unreal.Vector(sx, sy, sz + 22.0)
    aim_low = unreal.Vector(ax, ay, za + 30.0)

    # ④ 45° 俯看外沿：贴脸装不下的那一片散落的分布形态（第一轮里这张的信噪比最高）。
    cam_45 = unreal.Vector(ax + 640.0, ay - 230.0, za + 520.0)
    aim_45 = unreal.Vector(ax - 60.0, ay, za + 10.0)

    STATE["plan"] = [
        ("edge_face", (cam_face, look_at(cam_face, aim_face)), 50.0),
        ("mid", (cam_mid, look_at(cam_mid, aim_mid)), 55.0),
        ("low_graze", (cam_low, look_at(cam_low, aim_low)), 42.0),
        ("edge_45", (cam_45, look_at(cam_45, aim_45)), 48.0),
    ]

    # 坑 ⑤：捕获自己那份 PP 覆盖，Lumen 才回到出图里。
    cap_pp = unreal.PostProcessSettings()
    cap_pp.set_editor_property("override_dynamic_global_illumination_method", True)
    cap_pp.set_editor_property("dynamic_global_illumination_method",
                               unreal.DynamicGlobalIlluminationMethod.LUMEN)
    cap_pp.set_editor_property("override_reflection_method", True)
    cap_pp.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)

    rt = unreal.RenderingLibrary.create_render_target2d(
        world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8,
        unreal.LinearColor(0, 0, 0, 1), False)
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, cam_mid, STATE["plan"][1][1][1])
    comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
    comp.set_editor_property("texture_target", rt)
    comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    comp.set_editor_property("capture_every_frame", False)
    comp.set_editor_property("capture_on_movement", False)
    comp.set_editor_property("post_process_settings", cap_pp)
    comp.set_editor_property("post_process_blend_weight", 1.0)
    comp.set_editor_property("always_persist_rendering_state", True)   # 坑 ⑧（必须配 ⑨）
    STATE["rt"], STATE["comp"] = rt, comp
    STATE["spawned"].append(cap)
    return world


def sample_pixels():
    world, rt = STATE["world"], STATE["rt"]
    out = []
    for gy in range(GRID_Y):
        y = int((gy + 0.5) * H / GRID_Y)
        for gx in range(GRID_X):
            x = int((gx + 0.5) * W / GRID_X)
            col = unreal.RenderingLibrary.read_render_target_pixel(world, rt, x, y)
            out.append((col.r, col.g, col.b))
    return out


def zero_ratio(samples):
    return sum(1 for p in samples if p[0] == 0.0 and p[1] == 0.0 and p[2] == 0.0) / float(len(samples))


def vertex_stats():
    """回读活顶点，落两样东西：**逐槽位置**（供跨运行逐顶点比对）+ 按半径分箱的法向垂距。

    争议的物理内容就一句话：最外圈（`Rock ≈ 0`）的活顶点是不是**贴着地面上下各半**。

    ⚠️ **不要在单次运行里靠梯度把"外沿环"框出来** —— 试过，是死路：位移是 `Pos += N * d`，
    `N` 有水平分量，顶点 XY 跟着跑了最多几十厘米；而外沿的过渡带在半径上只有 1 m 宽，
    于是"在位移后的位置上量到的梯度"根本不是 kernel 当初算 `Rock` 用的那个，
    分带被系统性涂抹（实测：本该 ±6 cm 的外沿带量出 ±39 cm）。

    **真正干净的量在两次运行之间**：`bAlive` 只看坡度、与 `Noise` 无关，图案与种子又都没变
    ⇒ 两次运行的**槽位一一对应**，逐槽相减得到的正是被改掉的那一项本身
    （`ΔPos = −Noise·(1−Rock)·N`），一次分带都不需要。所以这里只负责**如实落盘**，
    比对交给 `TinyGladeShotRockNoiseStats.py`。

    单次运行里仍留一份按**半径**分箱的法向垂距剖面（半径是位移动不了多少的量，
    比梯度稳），只当"这一趟的壳长什么样"的存档，不拿它下结论。
    一阶垂距 = `(z − h(xy)) / sqrt(1 + |∇h|²)`；直接用 `z − h(xy)` 量到的是
    "垂距 + 水平漂移 × 坡度"，在 2.0 的坡上后一项就有几十厘米。

    ⚠️ `DebugReadRockShellSync` 是阻塞回读，**只给诊断与测试**，一个字节都不许进交互路径。
    """
    ground = STATE["ground"]
    res = ground.debug_read_rock_shell_sync()
    positions = res[-1] if isinstance(res, (tuple, list)) else res
    total = len(positions)
    cell = STATE["cell"]
    c, r_max = STATE["centre"], STATE["r_max"]

    live_tris = []
    live_pos = []
    perp_by_bin = {}
    # ⚠️ **死活是逐三角判的，判据只在第 0 个顶点的 x 上**（kernel 为省写入量只写那一个 NaN）。
    # 逐顶点判 NaN 会把死三角的第 1/2 个顶点算成"活的" —— 它们根本没被这趟 dispatch 写过，
    # 留着建网格时那份静止姿态（Z = 包围盒底），于是垂距里混进 −32 m 这种数
    # （第一次跑就是这么翻的：14.9 万个槽里"活"了 10 万个，而实测活三角只有 1,112 个）。
    for t in range(total // 3):
        base = t * 3
        if math.isnan(positions[base].x):
            continue                      # 整个三角出局
        live_tris.append(t)
        for k in range(3):
            p = positions[base + k]
            live_pos.append([p.x, p.y, p.z])
            g = grad_mag(ground, p.x, p.y, cell)
            perp = (p.z - height_at(ground, p.x, p.y)) / math.sqrt(1.0 + g * g)
            r = math.hypot(p.x - c.x, p.y - c.y)
            perp_by_bin.setdefault(int(math.floor(r / RADIUS_BIN)) * RADIUS_BIN, []).append(perp)

    def describe(vals):
        if not vals:
            return {"n": 0}
        vals = sorted(vals)
        n = len(vals)
        below = sum(1 for v in vals if v < 0.0)
        return {"n": n, "min": vals[0], "max": vals[-1],
                "mean": sum(vals) / n, "median": vals[n // 2],
                "below_pct": 100.0 * below / n, "span": vals[-1] - vals[0]}

    profile = {str(k): describe(v) for k, v in perp_by_bin.items()}
    unreal.log("ROCKNOISE VERTS 顶点槽 %d，活 %d（= %d 个活三角）；最陡环 r=%.0f"
               % (total, len(live_pos), len(live_tris), r_max))
    for k in sorted(perp_by_bin):
        d = describe(perp_by_bin[k])
        unreal.log("ROCKNOISE PROFILE r=%5d..%-5d n=%4d  法向垂距 cm: min %+7.2f / 中位 %+7.2f"
                   " / max %+7.2f  幅度 %6.2f  地面以下 %5.1f%%"
                   % (k, k + RADIUS_BIN, d["n"], d["min"], d["median"], d["max"],
                      d["span"], d["below_pct"]))
    return {"slots": total, "live_tris": live_tris, "live_pos": live_pos, "profile": profile}


def report():
    s = STATE["samples"]
    ok = True
    for name in sorted(s):
        z = zero_ratio(s[name])
        unreal.log("ROCKNOISE PIXELS %-12s zero=%.3f%%" % (name, z * 100.0))
        if z > ZERO_FAIL:
            unreal.log_error("ROCKNOISE !! %s 有 %.3f%% 的像素精确 (0,0,0) —— Lumen 预热没起作用（坑 ⑨）"
                             % (name, z * 100.0))
            ok = False

    path = os.path.join(OUT_DIR, "rocknoise_%s_samples.json" % TAG)
    payload = {"tag": TAG,
               "grid": [GRID_X, GRID_Y],
               "geom": {k: STATE.get(k) for k in ("reach", "cell", "slope_lo", "slope_hi",
                                                  "r_out", "r_max")},
               "verts": STATE["stats"],
               "pixels": {k: [list(p) for p in v] for k, v in s.items()}}
    try:
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(payload, fh)
        unreal.log("ROCKNOISE samples -> %s" % path)
    except Exception as exc:
        unreal.log_error("ROCKNOISE 采样落盘失败: %s" % exc)
        ok = False
    unreal.log("ROCKNOISE VERDICT %s" % ("OK" if ok else "FAILED"))


def tick(delta):
    STATE["ticks"] += 1
    if STATE["ticks"] < FIRST_TICK:
        return
    step = STATE["step"]
    if step < len(STATE["plan"]):
        name, (loc, rot), fov = STATE["plan"][step]
        phase, STATE["phase"] = STATE["phase"], STATE["phase"] + 1
        if phase == 0:
            STATE["comp"].set_editor_property("fov_angle", fov)
            STATE["spawned"][0].set_actor_location_and_rotation(loc, rot, False, False)
        elif phase >= SETTLE:
            # 坑 ⑨：一帧只打一次，Lumen 的帧间历史才会往前走；挪过相机之后历史等于清零，
            # 所以**每个机位都要重新预热满**。
            STATE["comp"].capture_scene()
            if phase >= SETTLE + WARMUP_CAPTURES - 1:
                unreal.RenderingLibrary.export_render_target(
                    STATE["world"], STATE["rt"], OUT_DIR, "rocknoise_%s_%s.png" % (TAG, name))
                STATE["samples"][name] = sample_pixels()
                unreal.log("ROCKNOISE shot: rocknoise_%s_%s.png（预热 %d 帧）"
                           % (TAG, name, WARMUP_CAPTURES))
                STATE["step"], STATE["phase"] = step + 1, 0
        return

    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    try:
        STATE["stats"] = vertex_stats()
    except Exception:
        import traceback
        unreal.log_error("ROCKNOISE vertex_stats 失败:\n%s" % traceback.format_exc())
    report()
    for a in STATE["spawned"]:
        ACTORS.destroy_actor(a)
    unreal.log("ROCKNOISE DONE tag=%s" % TAG)
    unreal.SystemLibrary.quit_editor()


# 坑 ⑥：准备段整个包在 try 里 —— 抛出去的话编辑器永远不退出，任务看着像挂死。
try:
    STATE["world"] = build()
except Exception:
    import traceback
    unreal.log_error("ROCKNOISE FAILED in build():\n%s" % traceback.format_exc())
    STATE["world"] = None

if STATE["world"] is None:
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
