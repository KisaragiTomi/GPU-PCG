# -*- coding: utf-8 -*-
"""Nanite 截面 HLOD 的流程演示：在测试关卡里按分镜渲一段 PNG 序列（字幕与编码交给 NaniteCutHLODVideoEncode.py）。

分镜（30 fps）：
  1 intro     环绕看源物体群
  2 box       画出收集盒（临时的 M_Bound 立方体）
  3 cut       BuildHLOD（不剔除）→ 切到 HLOD 显示
  4 wire      线框 X 光：同一份截面，剔除关 → 开，封闭区域里的三角消失
  5 bake      BakeHLOD → 显示烘焙出的 Nanite 静态网格
  6 compare   切换距离外，源 ↔ 烘焙 HLOD 交替
  7 closeup   推近到设计距离以内，露出图集分辨率的极限
  8 outro     收尾定格

用法::

    UnrealEditor-Cmd.exe <uproject> /PCGPlugins/NaniteCutHLOD/L_NaniteCutHLODTest \
        -ExecCmds="py <本文件>" -unattended -nosplash -stdout -AbsLog=<log>

产物：Saved/NaniteCutHLODVideo/frames/frame_%05d.png 与 timeline.json（每个分镜的帧区间 + 字幕 + 实测数字）。
关卡不存盘：临时 actor 用完销毁，烘焙资产只标脏。
"""
import json
import math
import os
import time
import traceback

import unreal

LEVEL = "/PCGPlugins/NaniteCutHLOD/L_NaniteCutHLODTest"
HLOD_LABEL = "NCH_HLOD"
SOURCE_PREFIX = "NCH_Obj_"
OUT_DIR = os.path.join(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir()), "NaniteCutHLODVideo")
FRAME_DIR = os.path.join(OUT_DIR, "frames")

W, H = 1600, 900
FPS = 30
BOUND_MATERIAL = "/PCGPlugins/MeshBoolean/M_Bound.M_Bound"
CUBE_MESH = "/Engine/BasicShapes/Cube.Cube"

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
STATE = {"handle": None, "gen": None, "done": False, "busy": False, "spawned": [], "frame": 0, "shots": [], "facts": {}}


def log(msg):
    unreal.log("NCH VIDEO " + str(msg))


def world():
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()


def look_at(frm, to):
    """坑 ①：Rotator 的位置参数序是 (roll, pitch, yaw)，只用关键字参数。"""
    dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
    return unreal.Rotator(roll=0.0, pitch=math.degrees(math.atan2(dz, max(math.hypot(dx, dy), 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


def ease(t):
    return t * t * (3.0 - 2.0 * t)


def union_bounds(actors):
    lo = [float("inf")] * 3
    hi = [float("-inf")] * 3
    for a in actors:
        origin, extent = a.get_actor_bounds(only_colliding_components=False)
        for i, (o, e) in enumerate(zip((origin.x, origin.y, origin.z), (extent.x, extent.y, extent.z))):
            lo[i] = min(lo[i], o - e)
            hi[i] = max(hi[i], o + e)
    return lo, hi


def make_capture(name, fov, wire):
    # 坑 ④：必须显式 RTF_RGBA8，默认浮点导出的 png 是 HDR 内容
    rt = unreal.RenderingLibrary.create_render_target2d(
        world(), W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8, unreal.LinearColor(0, 0, 0, 1), False)
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, unreal.Vector(0, 0, 0), unreal.Rotator())
    cap.set_actor_label("NCH_VideoCapture_" + name)
    STATE["spawned"].append(cap)
    comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
    comp.set_editor_property("texture_target", rt)
    comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    comp.set_editor_property("fov_angle", fov)
    comp.set_editor_property("capture_every_frame", False)
    comp.set_editor_property("capture_on_movement", False)
    comp.set_editor_property("always_persist_rendering_state", True)   # 坑 ⑧：没有 ViewState 就没有眼适应与 TSR 历史
    # 与静帧对照脚本同口径：GPU 截面没有 Lumen 卡片也没有距离场，开着间接光两组本来就不同。
    pp = unreal.PostProcessSettings()
    pp.set_editor_property("override_dynamic_global_illumination_method", True)
    pp.set_editor_property("dynamic_global_illumination_method", unreal.DynamicGlobalIlluminationMethod.NONE)
    pp.set_editor_property("override_reflection_method", True)
    pp.set_editor_property("reflection_method", unreal.ReflectionMethod.SCREEN_SPACE)
    comp.set_editor_property("post_process_settings", pp)
    comp.set_editor_property("post_process_blend_weight", 1.0)
    if wire:
        flag = unreal.EngineShowFlagsSetting()
        flag.set_editor_property("show_flag_name", "Wireframe")
        flag.set_editor_property("enabled", True)
        comp.set_editor_property("show_flag_settings", [flag])
    return {"comp": comp, "rt": rt, "actor": cap}


def destroy(capture):
    if capture and capture["actor"] in STATE["spawned"]:
        STATE["spawned"].remove(capture["actor"])
        ACTORS.destroy_actor(capture["actor"])


def place(capture, pos, target):
    capture["comp"].get_owner().set_actor_location_and_rotation(pos, look_at(pos, target), False, False)


def orbit(center, radius, elevation_deg, azimuth_deg, height_bias=0.0):
    e, a = math.radians(elevation_deg), math.radians(azimuth_deg)
    return unreal.Vector(center.x + radius * math.cos(e) * math.cos(a),
                         center.y + radius * math.cos(e) * math.sin(a),
                         center.z + radius * math.sin(e) + height_bias)


def shoot(capture, frames):
    """渲 frames 帧：每帧捕获一次并导出，帧号全局递增。"""
    for _ in range(frames):
        capture["comp"].capture_scene()
        yield
        unreal.RenderingLibrary.export_render_target(world(), capture["rt"], FRAME_DIR, "frame_%05d.png" % STATE["frame"])
        STATE["frame"] += 1


def shot(name, caption, sub=""):
    start = STATE["frame"]
    STATE["shots"].append({"name": name, "start": start, "caption": caption, "sub": sub})
    return start


def close_shot():
    STATE["shots"][-1]["end"] = STATE["frame"]
    s = STATE["shots"][-1]
    log("分镜 %s: 帧 %d-%d" % (s["name"], s["start"], s["end"] - 1))


def run():
    for name in os.listdir(FRAME_DIR) if os.path.isdir(FRAME_DIR) else []:
        if name.endswith(".png"):
            os.remove(os.path.join(FRAME_DIR, name))
    os.makedirs(FRAME_DIR, exist_ok=True)

    if LEVEL not in world().get_path_name():
        LEVELS.load_level(LEVEL)
    # 命令行带地图时，第一帧 actor 还没登记完 —— 等到找得着为止。
    # 按类找而不是按标签：标签会在类改动重实例化时丢（2026-09-29 就遇到 NCH_HLOD 变回默认名）。
    hlod_class = getattr(unreal, "CSNaniteCutHLODActor", None)
    hlod, actors = None, []
    for _ in range(300):
        actors = ACTORS.get_all_level_actors()
        if hlod_class is not None:
            hlod = next((a for a in actors if isinstance(a, hlod_class)), None)
        if hlod is None:
            hlod = next((a for a in actors if a.get_actor_label() == HLOD_LABEL), None)
        if hlod is not None:
            log("HLOD actor: %s（标签 %s）" % (hlod.get_name(), hlod.get_actor_label()))
            break
        yield
    if hlod is None:
        raise RuntimeError("找不到 %s（关卡 %s，%d 个 actor）" % (HLOD_LABEL, world().get_path_name(), len(actors)))
    for a in [x for x in actors if x.get_actor_label().startswith("NCH_TempCapture_")]:
        ACTORS.destroy_actor(a)
    objects = [a for a in actors if a.get_actor_label().startswith(SOURCE_PREFIX)]
    lo, hi = union_bounds(objects)
    center = unreal.Vector((lo[0] + hi[0]) * 0.5, (lo[1] + hi[1]) * 0.5, lo[2] + (hi[2] - lo[2]) * 0.35)
    house = next((a for a in objects if a.get_actor_label() == SOURCE_PREFIX + "House"), None)
    house_center = house.get_actor_location() if house else center

    lit = make_capture("lit", 75.0, False)
    hlod.show_sources()
    for _ in range(90):        # 让 TSR / 眼适应先收敛，再开始出帧
        lit["comp"].capture_scene()
        yield

    # ---- 1 intro：环绕看源
    shot("intro", "Nanite 截面 HLOD", "源：%d 个物体，全精度 378,362 三角" % len(objects))
    for i in range(120):
        t = i / 119.0
        place(lit, orbit(center, 17000 - 2000 * ease(t), 20 - 4 * ease(t), 200 + 50 * t), center)
        for _ in shoot(lit, 1):
            yield
    close_shot()

    # ---- 2 box：画出收集盒
    box_extent = hlod.get_editor_property("gather_box").get_unscaled_box_extent()
    bound = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, hlod.get_actor_location(), unreal.Rotator())
    STATE["spawned"].append(bound)
    bound.set_actor_label("NCH_VideoBoundBox")
    smc = bound.static_mesh_component
    smc.set_mobility(unreal.ComponentMobility.MOVABLE)
    smc.set_editor_property("static_mesh", unreal.load_asset(CUBE_MESH))
    smc.set_material(0, unreal.load_asset(BOUND_MATERIAL))
    smc.set_editor_property("relative_scale3d", unreal.Vector(box_extent.x / 50.0, box_extent.y / 50.0, box_extent.z / 50.0))
    smc.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
    shot("box", "收集盒", "包围盒中心落在盒内的静态网格进 HLOD；可用 PickTag / ExcludeTag 再筛")
    for i in range(75):
        t = i / 74.0
        place(lit, orbit(center, 15000, 16 + 6 * ease(t), 250 + 40 * t), center)
        for _ in shoot(lit, 1):
            yield
    close_shot()
    ACTORS.destroy_actor(bound)
    STATE["spawned"].remove(bound)

    # ---- 3 cut：BuildHLOD（先不剔除）
    hlod.set_editor_property("cull_hidden", False)
    t0 = time.time()
    hlod.build_hlod()
    result = hlod.get_editor_property("last_result")
    cut_tris = result.get_editor_property("written_triangles")
    STATE["facts"]["cut_triangles"] = cut_tris
    STATE["facts"]["cut_seconds"] = round(time.time() - t0, 2)
    hlod.show_hlod()
    shot("cut", "BuildHLOD：取 Nanite cluster 截面",
         "按 80 m 切换距离反推 CutError = 8.33 cm，GPU 遍历层级 → %s 三角" % format(cut_tris, ","))
    for i in range(150):
        t = i / 149.0
        place(lit, orbit(center, 15000 - 3000 * ease(t), 22 - 6 * ease(t), 290 - 60 * t), center)
        for _ in shoot(lit, 1):
            yield
    close_shot()

    # ---- 4 wire：剔除关 → 开
    wire = make_capture("wire", 60.0, True)
    wire_pos = unreal.Vector(house_center.x - 2100, house_center.y - 2100, house_center.z + 1500)
    place(wire, wire_pos, house_center)
    for _ in range(45):
        wire["comp"].capture_scene()
        yield
    shot("wire_off", "剔除前：线框 X 光", "封闭后屋与隧道深处的物体都还在截面里")
    for _ in shoot(wire, 75):
        yield
    close_shot()

    hlod.set_editor_property("cull_hidden", True)
    t0 = time.time()
    hlod.build_hlod()
    cull = hlod.get_editor_property("last_cull_result")
    STATE["facts"]["cull_after"] = cull.get_editor_property("triangles_after")
    STATE["facts"]["cull_views"] = cull.get_editor_property("num_views")
    STATE["facts"]["cull_seconds"] = round(time.time() - t0, 2)
    for _ in range(30):
        wire["comp"].capture_scene()
        yield
    shot("wire_on", "外部可见性剔除",
         "包围球外 %d 个视点软件光栅，从来不是最近面的三角删掉 → %s 三角"
         % (STATE["facts"]["cull_views"], format(STATE["facts"]["cull_after"], ",")))
    for _ in shoot(wire, 90):
        yield
    close_shot()
    destroy(wire)

    # ---- 5 bake
    t0 = time.time()
    hlod.bake_hlod()
    baked = hlod.get_editor_property("last_baked_mesh")
    STATE["facts"]["bake_seconds"] = round(hlod.get_editor_property("last_bake_seconds"), 2)
    STATE["facts"]["bake_triangles"] = hlod.get_editor_property("last_bake_triangles")
    STATE["facts"]["bake_mesh"] = baked.get_path_name() if baked else None
    log("bake %.1f s -> %s" % (time.time() - t0, STATE["facts"]["bake_mesh"]))
    hlod.show_hlod()
    for _ in range(120):       # 新材质要现编 shader，先空跑让它上屏
        lit["comp"].capture_scene()
        yield
    shot("bake", "BakeHLOD：烘成带贴图的 Nanite 静态网格",
         "重分 UV → 用源材质烘 2048 的 BaseColor / Normal / Roughness → %s 三角，%.1f s"
         % (format(STATE["facts"]["bake_triangles"], ","), STATE["facts"]["bake_seconds"]))
    for i in range(120):
        t = i / 119.0
        place(lit, orbit(center, 12000, 16, 230 + 25 * ease(t)), center)
        for _ in shoot(lit, 1):
            yield
    close_shot()

    # ---- 6 compare：切换距离外交替
    shot("compare", "切换距离（80 m）外：源 ↔ 烘焙 HLOD", "每秒交替一次，肉眼分不出")
    for i in range(240):
        t = i / 239.0
        if i % 30 == 0:
            (hlod.show_sources if (i // 30) % 2 == 0 else hlod.show_hlod)()
        place(lit, orbit(center, 11000, 14, 255 + 30 * t), center)
        for _ in shoot(lit, 1):
            yield
    close_shot()

    # ---- 7 closeup：推近到设计距离以内
    shot("closeup", "推近到设计距离以内", "图集约 3.5 cm/纹素，近看会糊 —— HLOD 只在切换距离之外用")
    for i in range(150):
        t = i / 149.0
        if i % 30 == 0:
            (hlod.show_sources if (i // 30) % 2 == 0 else hlod.show_hlod)()
        place(lit, orbit(house_center, 6000 - 4200 * ease(t), 12, 232), house_center)
        for _ in shoot(lit, 1):
            yield
    close_shot()

    # ---- 8 outro
    hlod.show_hlod()
    shot("outro", "全精度 378,362 → 截面 %s → 剔除后 %s"
         % (format(STATE["facts"]["cut_triangles"], ","), format(STATE["facts"]["cull_after"], ",")),
         "烘焙 %.1f s；对照统一关 Lumen 与距离场 AO" % STATE["facts"]["bake_seconds"])
    for i in range(90):
        place(lit, orbit(center, 13000, 18, 262 + 8 * (i / 89.0)), center)
        for _ in shoot(lit, 1):
            yield
    close_shot()

    destroy(lit)
    with open(os.path.join(OUT_DIR, "timeline.json"), "w", encoding="utf-8") as f:
        json.dump({"fps": FPS, "width": W, "height": H, "frames": STATE["frame"],
                   "shots": STATE["shots"], "facts": STATE["facts"]}, f, ensure_ascii=False, indent=2)
    log("共 %d 帧 -> %s" % (STATE["frame"], FRAME_DIR))


def finish(ok):
    if STATE["done"]:
        return
    STATE["done"] = True
    if STATE["handle"] is not None:
        unreal.unregister_slate_post_tick_callback(STATE["handle"])
    for a in list(STATE["spawned"]):
        try:
            ACTORS.destroy_actor(a)
        except Exception:
            pass
    log("DONE" if ok else "FAILED")
    unreal.SystemLibrary.quit_editor()


def tick(_delta):
    # 重入保护：BakeHLOD 里材质烘焙的进度框会泵 Slate tick。
    if STATE["done"] or STATE["busy"]:
        return
    STATE["busy"] = True
    try:
        next(STATE["gen"])
    except StopIteration:
        finish(True)
    except Exception:
        unreal.log_error("NCH VIDEO exception\n" + traceback.format_exc())
        finish(False)
    finally:
        STATE["busy"] = False


try:
    os.makedirs(FRAME_DIR, exist_ok=True)
    STATE["gen"] = run()
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
except Exception:
    unreal.log_error("NCH VIDEO exception\n" + traceback.format_exc())
    finish(False)
