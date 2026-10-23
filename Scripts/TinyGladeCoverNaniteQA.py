# -*- coding: utf-8 -*-
"""
地被改走 Nanite 之后的出图验收（2026-09-29，见 Docs/TinyGlade/Ground_Cover.md）。**不存盘**。

回答三个只有像素答得了的问题：

  A  **花不是默认材质**：与 15:20 那张基准同机位（`user_cam`），画面里另放两株对照薰衣草 ——
     左边普通 StaticMeshComponent + `MI_TG_Lavender`，右边网格自己的 `MI_TG_VertexColor`。
     统计紫色像素占比；默认材质是中性灰，占比接近 0（M_TG_Grass 的花分支编译失败时就是这样，2026-09-29 踩过）。
  B  **花在成片**：俯视 40 m 见方（`top`）。
  C  **WPO 在交界外彻底停了**：同一个低机位隔一段风场时间各拍一张**场景深度**（`wpo_depth_t0/t1`），
     按深度（= 离相机多远）分带统计"深度变了的像素"占比。几何在动的地方深度才会变 —— 法线风只改明暗、不改深度，
     所以交界（草近处窗口的边，约 60 m）之外这个占比应当 ≈ 0，交界以内（尤其 40 m 以内满幅区）明显大于 0。
     两张之间把四个地被 MI 的 `TimeOffset` 推 0.6 并多等几十帧，风场相位肯定不同。

用法（离屏、不弹窗；跑完自己退出）::

    UnrealEditor.exe <uproject> /PCGPlugins/HouseTest/L_HouseGroundDemo -ExecCmds="py <本文件>" -RenderOffscreen -nosplash -unattended -AbsLog=<log>

产物：`Saved/CoverNaniteQA_20260929/*.png`；判据打在日志的 `COVERQA` 行，最后一行 `COVERQA VERDICT OK/FAILED`。
坑表同 `TinyGladeShotGrassLod.py`（离屏 SceneCapture 要自己的 PP 覆盖开 Lumen、预热帧、无头要钉地被机位）。
"""
import colorsys
import math
import os

import unreal

OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), "CoverNaniteQA_20260929")
W, H = 1219, 775            # 与编辑器视口截图同尺寸，方便和基准对着看
DW, DH = 640, 360           # 深度图小一点：整张读回 Python 做差
USER_CAM = (unreal.Vector(1511.195241, 323.474552, 653.816527), unreal.Rotator(pitch=-18.383470, yaw=-327.379218, roll=0.0))
FIRST_TICK = 45
SETTLE = 6
WARMUP = 96
MI_NAMES = ("MI_TG_Grass", "MI_TG_GrassFar", "MI_TG_LowpolyFlower", "MI_TG_Lavender")

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
MEL = unreal.MaterialEditingLibrary
STATE = {"ticks": 0, "step": 0, "phase": 0, "plan": [], "spawned": [], "depth": {}, "ok": True, "time_offsets": None}


def log(msg):
    unreal.log("COVERQA %s" % msg)


def fail(msg):
    STATE["ok"] = False
    unreal.log_error("COVERQA !! %s" % msg)


def find_ground():
    for actor in ACTORS.get_all_level_actors():
        name = actor.get_class().get_name()
        if "Ground" in name and "Shaper" not in name:
            return actor
    return None


def lumen_pp():
    pp = unreal.PostProcessSettings()
    pp.set_editor_property("override_dynamic_global_illumination_method", True)
    pp.set_editor_property("dynamic_global_illumination_method", unreal.DynamicGlobalIlluminationMethod.LUMEN)
    pp.set_editor_property("override_reflection_method", True)
    pp.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)
    return pp


def make_capture(world, width, height, fmt, source, fov):
    rt = unreal.RenderingLibrary.create_render_target2d(world, width, height, fmt, unreal.LinearColor(0, 0, 0, 1), False)
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, USER_CAM[0], USER_CAM[1])
    comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
    comp.set_editor_property("texture_target", rt)
    comp.set_editor_property("capture_source", source)
    comp.set_editor_property("fov_angle", fov)
    comp.set_editor_property("capture_every_frame", False)
    comp.set_editor_property("capture_on_movement", False)
    comp.set_editor_property("post_process_settings", lumen_pp())
    comp.set_editor_property("post_process_blend_weight", 1.0)
    comp.set_editor_property("always_persist_rendering_state", True)
    STATE["spawned"].append(cap)
    return cap, comp, rt


def spawn_control(mesh, material, loc):
    actor = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, loc)
    comp = actor.get_component_by_class(unreal.StaticMeshComponent)
    comp.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
    comp.set_static_mesh(mesh)
    comp.set_material(0, material)
    actor.set_actor_scale3d(unreal.Vector(0.6, 0.6, 0.6))
    STATE["spawned"].append(actor)


def shift_time_offset(delta):
    mats = [unreal.EditorAssetLibrary.load_asset("/PCGPlugins/HouseTest/TinyGladeAsset/Materials/%s" % n) for n in MI_NAMES]
    if STATE["time_offsets"] is None:
        STATE["time_offsets"] = [MEL.get_material_instance_scalar_parameter_value(m, "TimeOffset") for m in mats]
    for m, base in zip(mats, STATE["time_offsets"]):
        MEL.set_material_instance_scalar_parameter_value(m, "TimeOffset", base + delta)   # 返回值不可信，下面回读
        MEL.update_material_instance(m)
    log("TimeOffset -> %s" % [round(MEL.get_material_instance_scalar_parameter_value(m, "TimeOffset"), 3) for m in mats])


def build():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    if not world or world.get_name() != "L_HouseGroundDemo":
        unreal.EditorLoadingAndSavingUtils.load_map("/PCGPlugins/HouseTest/L_HouseGroundDemo")
        world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    STATE["world"] = world
    ground = find_ground()
    if not ground:
        fail("关卡里没有地面")
        return None
    STATE["ground"] = ground
    loc, rot = USER_CAM
    # 无头没有视口：近处窗口与 Nanite 距离带都钉到机位上（SetGroundCoverViewOverride 也推给了每个地被组件）。
    ground.call_method("SetGroundCoverViewOverride", (loc,))

    # 对照薰衣草：视线前方 9 m、左右各 2.5 m（Z 跟地面 actor，花本身有 +25 的偏移，这里给个大概即可）。
    base = loc + rot.get_forward_vector() * 900.0
    base.z = ground.get_actor_location().z + 25.0
    right = rot.get_right_vector()
    lav = unreal.EditorAssetLibrary.load_asset("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/garden_flower_01_lavender")
    spawn_control(lav, unreal.EditorAssetLibrary.load_asset("/PCGPlugins/HouseTest/TinyGladeAsset/Materials/MI_TG_Lavender"), base - right * 250.0)
    spawn_control(lav, unreal.EditorAssetLibrary.load_asset("/PCGPlugins/HouseTest/TinyGladeAsset/Materials/MI_TG_VertexColor"), base + right * 250.0)

    STATE["color"] = make_capture(world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8, unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR, 90.0)
    # RTF_RGBA32F 而不是 R32F：read_render_target_raw 读不了单通道浮点（第一轮就是空数组），R 通道 = 场景深度（cm）。
    STATE["depthcap"] = make_capture(world, DW, DH, unreal.TextureRenderTargetFormat.RTF_RGBA32F, unreal.SceneCaptureSource.SCS_SCENE_DEPTH, 70.0)
    # 深度那台只拍地面这一个 actor（地被组件与它们的 Nanite 替身都挂在它下面）：地面外的水面、树、房子
    # 自己会动（水波、树叶风），混进来就分不清"交界外还在动"的是不是草花。
    depth_comp = STATE["depthcap"][1]
    depth_comp.set_editor_property("primitive_render_mode", unreal.SceneCapturePrimitiveRenderMode.PRM_USE_SHOW_ONLY_LIST)
    # ShowOnlyActors 从 Python 写会报 "cannot be edited on templates"，走组件自带的函数（收的是此刻地面身上的图元组件，
    # 地被组件与 Nanite 替身在关卡加载时就建好了，钉机位也不会换组件）。
    depth_comp.show_only_actor_components(ground)
    log("深度捕获只拍地面 %s 的图元组件" % ground.get_actor_label())

    # 顺着机位的视线方向往地面里挪（机位在地面西南角附近，往反方向挪会拍到地面外的水）。
    ahead = rot.get_forward_vector()
    top = (unreal.Vector(loc.x + ahead.x * 2500.0, loc.y + ahead.y * 2500.0, 3500.0), unreal.Rotator(pitch=-89.9, yaw=rot.yaw, roll=0.0))
    wide = (unreal.Vector(loc.x, loc.y, 260.0), unreal.Rotator(pitch=-4.0, yaw=rot.yaw, roll=0.0))
    STATE["wide"] = wide
    STATE["plan"] = [
        ("user_cam", "color", USER_CAM, None),
        ("top", "color", top, None),
        ("wpo_color", "color", wide, None),
        ("wpo_depth_t0", "depth", wide, lambda: shift_time_offset(0.0)),
        ("wpo_depth_t1", "depth", wide, lambda: shift_time_offset(0.6)),
    ]
    return world


def as_list(result):
    """UE Python 对"bool 返回值 + 引用出参"的函数返回 (bool, 数组) 元组；只要数组那一项。"""
    if isinstance(result, tuple):
        for item in result:
            if not isinstance(item, bool):
                return list(item)
        return []
    return list(result) if result is not None else []


def purple_share(pixels):
    purple = grey = 0
    for c in pixels:
        h, s, v = colorsys.rgb_to_hsv(c.r / 255.0, c.g / 255.0, c.b / 255.0)
        if s > 0.18 and 0.66 < h < 0.86 and v > 0.2:
            purple += 1
        elif s < 0.08 and v > 0.45:
            grey += 1
    n = float(max(len(pixels), 1))
    return purple / n, grey / n


def report_user_cam():
    _, _, rt = STATE["color"]
    pixels = as_list(unreal.RenderingLibrary.read_render_target(STATE["world"], rt, False))
    purple, grey = purple_share(pixels)
    log("A user_cam 紫色像素 %.3f、中性灰亮像素 %.3f（%d 像素）" % (purple, grey, len(pixels)))
    if purple < 0.005:
        fail("画面里几乎没有紫色 —— 薰衣草八成还是默认材质（检查日志里的 'Failed to compile Material Instance'）")


def report_wpo():
    t0, t1 = STATE["depth"].get("wpo_depth_t0"), STATE["depth"].get("wpo_depth_t1")
    if not t0 or not t1 or len(t0) != len(t1):
        fail("深度图没拿到（t0 %d / t1 %d 个像素）" % (len(t0 or []), len(t1 or [])))
        return
    bands = [(0, 1500), (1500, 3000), (3000, 4000), (4000, 5500), (5500, 6500), (6500, 9000), (9000, 20000)]
    stats = {b: [0, 0] for b in bands}
    for a, b in zip(t0, t1):
        d = a.r
        if d <= 0.0 or d > 20000.0:
            continue
        for lo, hi in bands:
            if lo <= d < hi:
                stats[(lo, hi)][1] += 1
                if abs(b.r - a.r) > 1.0:
                    stats[(lo, hi)][0] += 1
                break
    # 变了的像素画成一张 PPM（按距离带着色）方便人眼看是哪里在动：<40 m 绿、40–65 m 黄、>65 m 红。
    try:
        ppm = bytearray(("P6 %d %d 255" % (DW, DH)).encode("ascii") + bytes((10,)))
        for a, b in zip(t0, t1):
            d = a.r
            moved = 0.0 < d <= 20000.0 and abs(b.r - a.r) > 1.0
            if not moved:
                ppm += bytes((20, 20, 20))
            elif d < 4000.0:
                ppm += bytes((40, 200, 40))
            elif d < 6500.0:
                ppm += bytes((230, 210, 40))
            else:
                ppm += bytes((240, 40, 40))
        with open(os.path.join(OUT_DIR, "wpo_depth_moved.ppm"), "wb") as f:
            f.write(bytes(ppm))
    except Exception as error:
        log("写 PPM 失败：%s" % error)
    for (lo, hi), (moved, n) in zip(stats.keys(), stats.values()):
        log("C 深度 %4.0f-%4.0f m：%6d 像素，深度变了（>1 cm）%.3f" % (lo / 100.0, hi / 100.0, n, moved / float(max(n, 1))))
    near = stats[(0, 1500)]
    near_share = near[0] / float(max(near[1], 1))
    # 交界外"真的在动"只数**两张里都落在 65 m 外**的像素：前景（交界以内还在摆的花草）扫过远景时，远处那个像素的深度会从
    # 70 m 跳成 50 m —— 那是遮挡在变，不是远处在动（第一版按单张深度分带，把这种像素算成了交界外在动，报了 3% 的假阳性）。
    both_far = both_far_moved = 0
    for a, b in zip(t0, t1):
        if 6500.0 <= a.r <= 20000.0 and 6500.0 <= b.r <= 20000.0:
            both_far += 1
            if abs(b.r - a.r) > 1.0:
                both_far_moved += 1
    far_share = both_far_moved / float(max(both_far, 1))
    log("C 近处（<15 m）变了 %.3f；两张都在交界外（>65 m）的 %d 像素里变了 %d（%.4f）"
        % (near_share, both_far, both_far_moved, far_share))
    if near_share < 0.02:
        fail("近处深度几乎没变：风没动起来（或者两张时间相同），这组对比没有区分度")
    if far_share > 0.005:
        fail("交界外还有几何在动（%.2f%% 的像素）：WPO 没在交界处停下" % (far_share * 100.0))
    # 原始深度落盘（float32，t0 在前 t1 在后，行优先 DW×DH），离线还想换判据不必再开编辑器。
    try:
        import struct
        with open(os.path.join(OUT_DIR, "wpo_depth_t0_t1.f32"), "wb") as f:
            f.write(struct.pack("<%df" % len(t0), *[a.r for a in t0]))
            f.write(struct.pack("<%df" % len(t1), *[b.r for b in t1]))
    except Exception as error:
        log("写深度失败：%s" % error)


def tick(delta):
    STATE["ticks"] += 1
    if STATE["ticks"] < FIRST_TICK:
        return
    step = STATE["step"]
    if step < len(STATE["plan"]):
        name, kind, (loc, rot), action = STATE["plan"][step]
        cap, comp, rt = STATE["color"] if kind == "color" else STATE["depthcap"]
        phase, STATE["phase"] = STATE["phase"], STATE["phase"] + 1
        if phase == 0:
            if action:
                action()
            cap.set_actor_location_and_rotation(loc, rot, False, False)
        elif phase >= SETTLE:
            comp.capture_scene()
            if phase >= SETTLE + WARMUP - 1:
                if kind == "color":
                    unreal.RenderingLibrary.export_render_target(STATE["world"], rt, OUT_DIR, "%s.png" % name)
                    if name == "user_cam":
                        report_user_cam()
                else:
                    STATE["depth"][name] = as_list(unreal.RenderingLibrary.read_render_target_raw(STATE["world"], rt, False))
                log("shot %s" % name)
                STATE["step"], STATE["phase"] = step + 1, 0
        return

    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    try:
        report_wpo()
    finally:
        shift_time_offset(0.0)
        STATE["ground"].call_method("ClearGroundCoverViewOverride")
        for actor in STATE["spawned"]:
            ACTORS.destroy_actor(actor)
        log("VERDICT %s" % ("OK" if STATE["ok"] else "FAILED"))
        unreal.SystemLibrary.quit_editor()


os.makedirs(OUT_DIR, exist_ok=True)
try:
    world = build()
except Exception:
    import traceback
    unreal.log_error("COVERQA FAILED in build():\n%s" % traceback.format_exc())
    world = None
if world is None:
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
