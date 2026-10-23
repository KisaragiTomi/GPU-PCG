# -*- coding: utf-8 -*-
"""岩壳板缝纯黑：**先证明那些黑像素到底是什么**，再谈机制。

本项目已经有六条推断被实测推翻，所以这一步不做任何推理，只回答一个二选一：

  A **缝里什么都没画**（裙三角被 NaN 剔了 / 被背面剔除了）⇒ 看到的是缝后面的东西；
  B **缝里画了东西但它是黑的**（法线朝向 / 材质 / 光照）。

判法不靠印象：`SceneCapture` 的 `SCS_BASE_COLOR` / `SCS_NORMAL` 直接读 **G-buffer**，
**完全不过光照**。缝里若有几何，base color 就是岩壳的底色（灰）、normal 是一个单位向量；
若什么都没画，那两张图在缝里会与"整条岩壳关掉"的对照图逐像素一致 —— 也就是地面。
这个判据对"光照压黑"完全免疫，正是"精确 (0,0,0) 抬不动"要求的那一类证据。

同一趟还量两件**解析**事实（像素判据的对照组，不看图）：

  · 活三角里**盖 / 裙各有多少**。裙三角若被剔了，这个数会是 0 —— 直接判掉候选 A 的一半。
    分类不靠 aux 标记（Python 侧读不到），靠一条披挂后仍然成立的不变量：
    盖三角的三个顶点同属顶圈 ⇒ 沿法线的偏移**三顶点相同**（同胞腔同随机数）；
    裙三角跨两圈 ⇒ 偏移一正一负，离地高度差按 `CellRelief` 量级张开。
  · 缝的**几何宽度与深度**：相邻盖之间那条 V 形槽有多宽多深，决定它在屏幕上占多少像素。

⚠️ 出图前必须 `always_persist_rendering_state = True`（坑表：`USceneCaptureComponent`
默认没有 ViewState ⇒ PPV 里的曝光一条都不生效）。本脚本照抄 `TinyGladeShotSoftening.py`
的机位与捕获设置，只换 `capture_source`，两组图因此可以直接对上。

用法（真编辑器 + tick；`-ExecutePythonScript` 跑完即退，tick 回调根本不触发）::

    UnrealEditor.exe <uproject> -ExecCmds="py <本文件>" -nosplash -abslog=<独立日志>
"""
import math
import os

import unreal

PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"ticks": 0, "handle": None, "jobs": [], "spawned": [], "ground": None, "world": None}


def log(msg):
    unreal.log("SEAM %s" % msg)


def look_at(frm, to):
    """⚠️ unreal.Rotator 的参数序是 (roll, pitch, yaw)，只用关键字参数。"""
    dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
    flat = math.hypot(dx, dy)
    return unreal.Rotator(roll=0.0,
                          pitch=math.degrees(math.atan2(dz, max(flat, 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


def find_ground():
    for a in ACTORS.get_all_level_actors():
        name = a.get_class().get_name()
        if "Ground" in name and "Shaper" not in name:
            return a
    return None


# =============================================================================
# 解析证据：活三角里盖 / 裙各有多少，缝有多宽多深
# =============================================================================

def classify_shell(ground):
    """把回读到的顶点按三角分类。**不看图、不过光照**，纯几何。

    分类不变量（披挂之后仍然成立）：kernel 给顶圈的偏移是 `rand(cell) * CellRelief`、
    给底圈的是 `-CellRelief`，两者都沿地形法线。所以
      · 盖三角：三个顶点同圈同胞腔 ⇒ **离地高度三者相同**（差只剩表面噪声，≤ NoiseAmp）；
      · 裙三角：跨两圈 ⇒ 离地高度差至少一个 `CellRelief`（默认 30 cm）。
    阈值取 `CellRelief/2`，与噪声幅度（6 cm）差一个数量级，不会误判。
    """
    # UFUNCTION 的 `TArray<FVector>&` 是输出参数 ⇒ Python 侧不吃入参、返回 (count, array)。
    try:
        ret = ground.debug_read_rock_shell_sync()
    except Exception as exc:
        log("readback 不可用: %s" % exc)
        return
    positions = ret[1] if isinstance(ret, tuple) else ret
    log("readback vertices=%d (%d triangles)" % (len(positions), len(positions) // 3))

    relief = ground.get_editor_property("RockShellCellRelief")
    threshold = max(relief * 0.5, 1.0)

    alive_cap = alive_skirt = 0
    dz_cap_max = 0.0
    dz_skirt = []
    gap_widths = []
    for tri in range(len(positions) // 3):
        p0, p1, p2 = positions[tri * 3], positions[tri * 3 + 1], positions[tri * 3 + 2]
        if math.isnan(p0.x) or math.isnan(p1.x) or math.isnan(p2.x):
            continue
        # 离地高度：地形高度是解析场，Python 侧一次采样就够（只对活三角做，约一千个）。
        d = []
        for p in (p0, p1, p2):
            d.append(p.z - ground.sample_height(unreal.Vector2D(p.x, p.y)))
        spread = max(d) - min(d)
        if spread < threshold:
            alive_cap += 1
            dz_cap_max = max(dz_cap_max, max(d))
        else:
            alive_skirt += 1
            dz_skirt.append(spread)
            # 裙三角的水平跨度 ≈ LipOffset（缝的半宽）
            xs = [p.x for p in (p0, p1, p2)]
            ys = [p.y for p in (p0, p1, p2)]
            gap_widths.append(max(max(xs) - min(xs), max(ys) - min(ys)))
    alive = alive_cap + alive_skirt
    log("alive=%d  cap=%d  skirt=%d  (原件比例 盖 27566 : 裙 22032 = 55.6%% : 44.4%%)"
        % (alive, alive_cap, alive_skirt))
    if alive_skirt == 0:
        log("!! 活三角里一个裙三角都没有 —— 缝里确实什么几何都没生成")
    if dz_skirt:
        dz_skirt.sort()
        log("skirt 离地高度差: 中位 %.1f cm / 最大 %.1f cm；水平跨度中位 %.1f cm（LipOffset 量级）"
            % (dz_skirt[len(dz_skirt) // 2], dz_skirt[-1],
               sorted(gap_widths)[len(gap_widths) // 2]))
    log("cap 最高离地 %.1f cm（= CellRelief 上界 %.0f）" % (dz_cap_max, relief))


def log_shell_state(ground):
    # ⚠️ 坑 ⑩：调原因版。`is_rock_shell_drawable()` 在 UE Python 里只返回单一值，
    # 拆包成两个会直接 TypeError；就算没拆包，不可画时那个 `None` 也会被
    # `str()` 成一句像模像样的假原因。
    ok, reason = False, ""
    try:
        reason = str(ground.get_rock_shell_undrawable_reason())
        ok = (reason == "")
    except Exception as exc:
        reason = "查询失败 %s" % exc
    log("drawable=%s reason='%s' displaces=%s" %
        (ok, reason, ground.get_rock_shell_displace_count()))
    for name in ("RockShellSlopeLo", "RockShellSlopeHi", "RockShellCellRelief",
                 "RockShellCellJitter", "RockShellNoiseAmount", "RockShellCellSize", "RockShellBoundaryPoints",
                 "RockShellRoadSink", "RockShellRoadFade", "RockShellRoadBlurRadius"):
        log("param %-24s = %s" % (name, ground.get_editor_property(name)))
    mat = ground.get_editor_property("RockShellMaterial")
    log("material = %s" % (mat.get_path_name() if mat else "None"))


# =============================================================================
# 出图
# =============================================================================

def make_capture(world, loc, rot, source):
    rt = unreal.RenderingLibrary.create_render_target2d(
        world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8,
        unreal.LinearColor(0, 0, 0, 1), False)
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, loc, rot)
    comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
    comp.set_editor_property("texture_target", rt)
    comp.set_editor_property("capture_source", source)
    comp.set_editor_property("fov_angle", 55.0)
    comp.set_editor_property("capture_every_frame", False)
    comp.set_editor_property("capture_on_movement", False)
    # 坑 ⑤：离屏 capture 被引擎写死关掉 Lumen，只翻回 GI/反射两项，其余交给世界 PPV。
    pp = unreal.PostProcessSettings()
    pp.set_editor_property("override_dynamic_global_illumination_method", True)
    pp.set_editor_property("dynamic_global_illumination_method",
                           unreal.DynamicGlobalIlluminationMethod.LUMEN)
    pp.set_editor_property("override_reflection_method", True)
    pp.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)
    comp.set_editor_property("post_process_settings", pp)
    comp.set_editor_property("post_process_blend_weight", 1.0)
    # 坑 ⑧：没有 ViewState ⇒ 眼适应不跑 ⇒ PPV 里的曝光一条都不生效。
    comp.set_editor_property("always_persist_rendering_state", True)
    STATE["spawned"].append(cap)
    return comp, rt


def build():
    unreal.EditorLoadingAndSavingUtils.load_map("%s/L_TerrainOpsDemo" % PKG)
    world = unreal.EditorLevelLibrary.get_editor_world()

    ground = find_ground()
    shapers = [a for a in ACTORS.get_all_level_actors() if "Shaper" in a.get_class().get_name()]
    if not ground or not shapers:
        unreal.log_error("SEAM FAILED: ground=%s shapers=%d" % (bool(ground), len(shapers)))
        return None
    STATE["ground"] = ground
    hub = shapers[0].get_actor_location()

    # 机位与路都照抄 TinyGladeShotSoftening.py 的 backlit，这样两组图能直接对上。
    sun_yaw = 0.0
    for a in ACTORS.get_all_level_actors():
        if a.get_class().get_name() == "DirectionalLight":
            sun_yaw = a.get_actor_rotation().yaw
            break
    sx, sy = math.cos(math.radians(sun_yaw)), math.sin(math.radians(sun_yaw))

    ground.reset_paint()
    ground.begin_paint_stroke()
    for i in range(41):
        t = -1600.0 + i * 80.0
        x, y = hub.x + sx * t, hub.y + sy * t
        ground.apply_paint_stroke(unreal.Vector(x, y, ground.sample_height(unreal.Vector2D(x, y))))
    ground.end_paint_stroke()

    def at(along, side, up):
        x, y = hub.x + sx * along, hub.y + sy * along
        return unreal.Vector(x + side * -sy, y + side * sx,
                             ground.sample_height(unreal.Vector2D(x + side * -sy, y + side * sx)) + up)

    loc = at(2600.0, 0.0, 260.0)
    rot = look_at(loc, at(0.0, 0.0, -120.0))
    log("cam at (%.0f, %.0f, %.0f) sun_yaw=%.1f" % (loc.x, loc.y, loc.z, sun_yaw))

    log_shell_state(ground)
    classify_shell(ground)

    # 四张同机位：最终色（复现 14.60%）+ 两张 G-buffer（不过光照）+ 关壳对照。
    sources = [("final", "SCS_FINAL_COLOR_LDR"),
               ("basecolor", "SCS_BASE_COLOR"),
               ("normal", "SCS_NORMAL")]
    for tag, name in sources:
        src = getattr(unreal.SceneCaptureSource, name, None)
        if src is None:
            log("capture_source %s 不存在，跳过" % name)
            continue
        comp, rt = make_capture(world, loc, rot, src)
        STATE["jobs"].append(("seam_%s" % tag, comp, rt, None))

    # 关掉整条岩壳的对照组：缝里剩下的就是"地面本来长什么样"。
    for tag, name in (("off_final", "SCS_FINAL_COLOR_LDR"), ("off_basecolor", "SCS_BASE_COLOR")):
        src = getattr(unreal.SceneCaptureSource, name, None)
        if src is None:
            continue
        comp, rt = make_capture(world, loc, rot, src)
        STATE["jobs"].append(("seam_%s" % tag, comp, rt, "shell_off"))
    return world


FIRST_TICK = 60      # Lumen 的 surface cache 与天光实时捕获要几十帧才收敛
GAP = 12


def tick(delta):
    STATE["ticks"] += 1
    index = STATE["ticks"] - FIRST_TICK
    if index >= 0 and index // GAP < len(STATE["jobs"]):
        slot = index // GAP
        name, comp, rt, mode = STATE["jobs"][slot]
        if index % GAP == 2 and mode == "shell_off":
            # 只在第一张关壳图之前切一次，之后一直关着。
            if STATE["ground"].get_editor_property("bRockShell"):
                STATE["ground"].set_editor_property("bRockShell", False)
                STATE["ground"].rebuild_rock_shell()
                log("bRockShell -> False")
        if index % GAP == 8:
            comp.capture_scene()
            unreal.RenderingLibrary.export_render_target(STATE["world"], rt, OUT_DIR, "%s.png" % name)
            log("shot: %s.png" % name)
    if STATE["ticks"] < FIRST_TICK + GAP * len(STATE["jobs"]) + 12:
        return
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    if STATE["ground"]:
        STATE["ground"].set_editor_property("bRockShell", True)
        STATE["ground"].reset_paint()
    for a in STATE["spawned"]:
        ACTORS.destroy_actor(a)
    log("DONE")
    unreal.SystemLibrary.quit_editor()


# 坑 ⑥：准备段整个包在 try 里 —— 抛出去的话编辑器永远不退出，任务看着像挂死。
try:
    STATE["world"] = build()
except Exception as exc:
    import traceback
    unreal.log_error("SEAM FAILED in build(): %s\n%s" % (exc, traceback.format_exc()))
    STATE["world"] = None

if STATE["world"] is None:
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
