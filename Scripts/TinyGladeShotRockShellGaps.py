# -*- coding: utf-8 -*-
"""岩壳缝隙出图（`Docs/TinyGlade/RockShell_Sparse.md`「七」的配图来源）。只改内存里的属性、不存盘。

三档，同机位（北面 / 西面各一近一中，路从 L_TerrainOpsDemo 土台的 +X 面爬上去，所以不拍东面）：
  asis    关卡原样（L_TerrainOpsDemo 的 Ground_Demo 带 09-06 实调覆盖，含 ChipAmount 35）
  chip0   只把 ChipAmount 归 0（TG 没有加性磕碰）
  cdo     `Ground_Tuning.md` 那 8 个实调覆盖换回 C++ 默认（胞腔尺寸保留关卡值，只比缝）

用法（真编辑器 + -ExecCmds，-ExecutePythonScript 跑完即退、tick 回调来不及触发）::

    set TG_GAP_LEVEL=/PCGPlugins/HouseTest/L_TerrainOpsDemo   (可选；别的关卡取台高最大的那座塑形物)
    set TG_GAP_TAG=terrain
    UnrealEditor.exe <uproject> -ExecCmds="py <本文件>" -unattended -nosplash -RenderOffscreen

产物：`Saved/TinyGladeShots/<tag>_<档>_<机位>.png`（`TG_GAP_OUT` 可改目录）。
捕获管线的坑（Lumen 覆盖、ViewState、逐帧预热）照 `TinyGladeShotRockShell.py` 文件头 ① ~ ⑨。
"""
import math
import os

import unreal

OUT_DIR = os.environ.get("TG_GAP_OUT", unreal.Paths.project_saved_dir() + "TinyGladeShots")
LEVEL = os.environ.get("TG_GAP_LEVEL", "/PCGPlugins/HouseTest/L_TerrainOpsDemo")
TAG = os.environ.get("TG_GAP_TAG", "gap")
W, H = 1600, 900
FIRST_TICK = 45
SETTLE = 8
WARMUP = int(os.environ.get("TG_SHOT_WARMUP", "96"))

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"ticks": 0, "handle": None, "plan": [], "world": None, "ground": None, "rt": None, "comp": None,
         "spawned": [], "step": 0, "phase": 0}

OVERRIDES = ["RockShellReliefFloor", "RockShellCellExpand", "RockShellSlopeLo", "RockShellSlopeHi",
             "RockShellRoadSink", "RockShellCellJitter", "RockShellChipAmount", "RockShellChipWavelength"]
LOGGED = ["RockShellCellSize", "RockShellBoundaryPoints", "RockShellSeedJitter", "RockShellClusterWarp",
          "RockShellSizeVariance", "RockShellCellRelief", "RockShellNoiseAmount", "RockShellNoiseWavelength",
          "RockShellBaseLift", "RockShellBaseSink", "RockShellSkirtTilt", "RockShellRiseMultiplier",
          "RockShellRiseExtend", "RockShellRiseNoiseAmount"] + OVERRIDES


def log(msg):
    unreal.log("GAPSHOT %s" % msg)


def look_at(frm, to):
    dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
    flat = math.hypot(dx, dy)
    return unreal.Rotator(roll=0.0, pitch=math.degrees(math.atan2(dz, max(flat, 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


def find(label):
    return next((a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == label), None)


def pool(ground):
    v = ground.get_rock_shell_pool_stats()
    return tuple(v) if v is not None else None


def set_props(values):
    g = STATE["ground"]
    for k, v in values.items():
        g.set_editor_property(k, v, notify_mode=unreal.PropertyAccessChangeNotifyMode.NEVER)
    g.rebuild_rock_shell()
    log("props %s -> pool %s" % (values, pool(g)))


def build():
    unreal.EditorLoadingAndSavingUtils.load_map(LEVEL)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    ground = find("Ground_Demo")
    shaper = find("Shaper_Mound")
    if not shaper:
        # 别的关卡没有 Shaper_Mound：取台高最大的那座（最可能有陡坡）
        cands = [a for a in ACTORS.get_all_level_actors() if isinstance(a, unreal.CSGroundShaperActor)]
        gz0 = ground.get_actor_location().z if ground else 0.0
        cands.sort(key=lambda a: -(a.get_actor_location().z - gz0))
        for a in cands:
            log("shaper candidate %s at %s maskSize=%s falloff=%s" % (a.get_actor_label(), a.get_actor_location(),
                a.get_editor_property("MaskSize"), a.get_editor_property("FalloffDistance")))
        shaper = cands[0] if cands else None
    if not ground:
        unreal.log_error("GAPSHOT FAILED: no Ground_Demo")
        return None
    STATE["ground"] = ground
    try:
        ground.set_editor_property("StairMesh", None)   # 带通知 ⇒ 石阶真的被收掉（不存盘）
    except Exception as exc:
        log("skip StairMesh: %s" % exc)

    cdo = unreal.get_default_object(ground.get_class())
    for k in LOGGED:
        try:
            log("param %-28s level=%-12s cdo=%s" % (k, ground.get_editor_property(k), cdo.get_editor_property(k)))
        except Exception as exc:
            log("param %s: %s" % (k, exc))
    STATE["asis"] = {k: ground.get_editor_property(k) for k in OVERRIDES}
    STATE["cdo"] = {k: cdo.get_editor_property(k) for k in OVERRIDES}
    ground.rebuild_rock_shell()
    log("pool (active, capacity, perCell) = %s  drawable='%s'" % (pool(ground), ground.get_rock_shell_undrawable_reason()))

    def gz(x, y):
        return ground.sample_height(unreal.Vector2D(x, y))

    if shaper:
        c = shaper.get_actor_location()
        radius = 0.5 * shaper.get_editor_property("MaskSize")
        falloff = shaper.get_editor_property("FalloffDistance")
    else:
        loc = ground.get_actor_location()
        c, radius, falloff = unreal.Vector(loc.x + 3200, loc.y + 3200, loc.z), 600.0, 800.0
    reach = radius + falloff
    mid_r = radius + 0.5 * falloff
    log("shaper centre=(%.0f, %.0f, %.0f) radius=%.0f falloff=%.0f" % (c.x, c.y, c.z, radius, falloff))

    # 路从土台的 +X 面爬上去（09-06 关卡内容），机位放在 −X（西）与 +Y（北）两面。
    def cams(dx, dy):
        side = unreal.Vector(dy, -dx, 0.0)   # 水平右手侧，给近景一点斜角
        aim = unreal.Vector(c.x + dx * mid_r, c.y + dy * mid_r, gz(c.x + dx * mid_r, c.y + dy * mid_r) + 30.0)
        nx, ny = c.x + dx * (reach + 380.0) + side.x * 220.0, c.y + dy * (reach + 380.0) + side.y * 220.0
        near = unreal.Vector(nx, ny, gz(nx, ny) + 180.0)
        mx, my = c.x + dx * (reach + 1300.0) + side.x * 500.0, c.y + dy * (reach + 1300.0) + side.y * 500.0
        mid = unreal.Vector(mx, my, gz(mx, my) + 520.0)
        return (near, look_at(near, aim)), (mid, look_at(mid, aim))
    WEST_NEAR, WEST_MID = cams(-1.0, 0.0)
    NORTH_NEAR, NORTH_MID = cams(0.0, 1.0)
    NEAR, MID = WEST_NEAR, WEST_MID
    cam_near = WEST_NEAR[0]

    STATE["plan"] = []
    for label, values in (("asis", STATE["asis"]), ("chip0", dict(STATE["asis"], RockShellChipAmount=0.0)),
                          ("cdo", STATE["cdo"])):
        STATE["plan"].append(("%s_west_near" % label, values, WEST_NEAR))
        STATE["plan"].append(("%s_west_mid" % label, None, WEST_MID))
        STATE["plan"].append(("%s_north_near" % label, None, NORTH_NEAR))
        STATE["plan"].append(("%s_north_mid" % label, None, NORTH_MID))

    cap_pp = unreal.PostProcessSettings()
    cap_pp.set_editor_property("override_dynamic_global_illumination_method", True)
    cap_pp.set_editor_property("dynamic_global_illumination_method", unreal.DynamicGlobalIlluminationMethod.LUMEN)
    cap_pp.set_editor_property("override_reflection_method", True)
    cap_pp.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)
    rt = unreal.RenderingLibrary.create_render_target2d(world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8,
                                                        unreal.LinearColor(0, 0, 0, 1), False)
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, cam_near, NEAR[1])
    comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
    comp.set_editor_property("texture_target", rt)
    comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    comp.set_editor_property("fov_angle", 55.0)
    comp.set_editor_property("capture_every_frame", False)
    comp.set_editor_property("capture_on_movement", False)
    comp.set_editor_property("post_process_settings", cap_pp)
    comp.set_editor_property("post_process_blend_weight", 1.0)
    comp.set_editor_property("always_persist_rendering_state", True)
    STATE["rt"], STATE["comp"] = rt, comp
    STATE["spawned"].append(cap)
    return world


def tick(delta):
    STATE["ticks"] += 1
    if STATE["ticks"] < FIRST_TICK:
        return
    step = STATE["step"]
    if step < len(STATE["plan"]):
        name, values, (loc, rot) = STATE["plan"][step]
        phase, STATE["phase"] = STATE["phase"], STATE["phase"] + 1
        if phase == 0:
            if values is not None:
                set_props(values)
            STATE["spawned"][0].set_actor_location_and_rotation(loc, rot, False, False)
        elif phase >= SETTLE:
            STATE["comp"].capture_scene()
            if phase >= SETTLE + WARMUP - 1:
                fname = "%s_%s.png" % (TAG, name)
                unreal.RenderingLibrary.export_render_target(STATE["world"], STATE["rt"], OUT_DIR, fname)
                log("shot %s" % fname)
                STATE["step"], STATE["phase"] = step + 1, 0
        return
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    for a in STATE["spawned"]:
        ACTORS.destroy_actor(a)
    log("DONE")
    unreal.SystemLibrary.quit_editor()


try:
    STATE["world"] = build()
except Exception:
    import traceback
    unreal.log_error("GAPSHOT FAILED in build():\n%s" % traceback.format_exc())
    STATE["world"] = None

if STATE["world"] is None:
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
