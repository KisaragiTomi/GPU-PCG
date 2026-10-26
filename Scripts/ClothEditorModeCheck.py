# -*- coding: utf-8 -*-
"""在**真编辑器**里（不进 PIE）验 `ACSClothActor`：放下去就带着一块布，并且在编辑器模式下一直模拟。

自动化测试（`PCGPlugins.ComputeShaderGenerator.ClothSim.*`）都是用例自己逐帧 `StepSimulation`，
走不到编辑器 world 的 tick；这一条走的是用户真正看到的那条路：放下 actor → `OnConstruction` 当场建布 →
编辑器 world 按 LEVELTICK_ViewportsOnly tick → `UCSClothSimComponent::TickComponent`（bTickInEditor）逐帧解算。

判据（日志里 `CLOTH EDITOR` 开头的行，末尾 `CLOTH EDITOR VERDICT OK / FAILED`）：
  A  spawn 回来、一个 tick 都还没过，布就已经建好（默认网格 65² 个粒子）
  B  只靠编辑器 tick，模拟帧数一直在涨
  C  布真的垂下来了：同一机位，刚放下时与几秒后各拍一张，画面变化足够大
  D  拖动 actor：钉边跟着走（网格发布的包围盒中心随 actor 平移）
  E  改网格参数（`GridResolution`）→ 当场重建，粒子数随之变化

⚠️ 编辑器窗口没焦点时，偏好设置 Performance →「Use Less CPU when in Background」（默认开）会给所有视口挂上
「Background Process」的 Realtime 覆盖，编辑器 world 退成 LEVELTICK_TimeOnly，任何 actor 都不 tick ——
脚本拉起的编辑器多半拿不到焦点。Python 改不了它（UEditorPerformanceSettings 没暴露给 Python，`set` 控制台
命令又会 SaveConfig 写进用户偏好），所以由 `RunClothEditorModeCheck.ps1` 用命令行 `-ini:EditorSettings:` 覆盖
关掉（它属于 config=EditorSettings），并在跑之前备份、跑完原样恢复引擎级 EditorSettings.ini 与本工程的
EditorPerProjectUserSettings.ini。

用法（必须真编辑器 + `-ExecCmds`，`-ExecutePythonScript` 跑完即退、tick 回调来不及触发）::

    .\\Scripts\\RunClothEditorModeCheck.ps1

产物：`Saved/ClothSimShots/editor_{placed,hanging,hanging_back}.png` 与 `editor_check.log`。
"""
import math
import os

import unreal

OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), "ClothSimShots")
TEMPLATE = "/Engine/Maps/Templates/Template_Default"
W, H = 1280, 720

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"handle": None, "world": None, "actor": None, "cloth": None, "rt": None, "cap": None,
         "phase": "warmup", "phase_ticks": 0, "frames0": 0, "ok": True, "shots": {}, "bounds0": None}

WARMUP_TICKS = 600         # 编辑器刚起来的那些帧可能还在建 UI / 编 shader：等到模拟帧开始走为止，最多这么久
SIM_TICKS = 240            # B/C：只靠编辑器 tick 跑这么久
DRAG_TICKS = 40            # D：分这么多 tick 把 actor 拖 300 cm


def fail(msg):
    STATE["ok"] = False
    unreal.log_error("CLOTH EDITOR !! " + msg)


def look_at(frm, to):
    """⚠️ unreal.Rotator 的参数序是 (roll, pitch, yaw)，只用关键字参数。"""
    dx, dy, dz = to.x - frm.x, to.y - frm.y, to.z - frm.z
    flat = math.hypot(dx, dy)
    return unreal.Rotator(roll=0.0, pitch=math.degrees(math.atan2(dz, max(flat, 1e-3))),
                          yaw=math.degrees(math.atan2(dy, dx)))


def capture(name):
    STATE["cap"].capture_component2d.capture_scene()
    unreal.RenderingLibrary.export_render_target(STATE["world"], STATE["rt"], OUT_DIR, "editor_%s.png" % name)
    STATE["shots"][name] = unreal.RenderingLibrary.read_render_target(STATE["world"], STATE["rt"], False)
    unreal.log("CLOTH EDITOR shot: editor_%s.png" % name)


def changed_ratio(a, b):
    if not a or not b or len(a) != len(b):
        return 0.0
    changed = total = 0
    for i in range(0, len(a), 7):
        pa, pb = a[i], b[i]
        total += 1
        if abs(pa.r - pb.r) + abs(pa.g - pb.g) + abs(pa.b - pb.b) > 24:
            changed += 1
    return changed / float(max(total, 1))


def build():
    unreal.EditorLoadingAndSavingUtils.new_map_from_template(TEMPLATE, False)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    STATE["world"] = world

    # 要验的就是「放下去就有布」：默认构造的 ACSClothActor，一个属性都不改。
    actor = ACTORS.spawn_actor_from_class(unreal.CSClothActor, unreal.Vector(0.0, 0.0, 250.0))
    if not actor:
        fail("spawn_actor_from_class(CSClothActor) returned None")
        return None
    cloth = actor.get_editor_property("cloth_component")
    STATE["actor"], STATE["cloth"] = actor, cloth

    n = cloth.get_particle_count()
    unreal.log("CLOTH EDITOR A right after spawn (no tick yet): ready=%s, %d particles" % (cloth.is_simulation_ready(), n))
    if not cloth.is_simulation_ready() or n != 65 * 65:
        fail("the placed actor has no cloth yet (ready=%s, %d particles, expected 65x65)" % (cloth.is_simulation_ready(), n))

    rt = unreal.RenderingLibrary.create_render_target2d(world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8)
    eye, aim = unreal.Vector(420.0, 260.0, 260.0), unreal.Vector(-40.0, 0.0, 170.0)
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, eye, look_at(eye, aim))
    comp = cap.capture_component2d
    comp.set_editor_property("texture_target", rt)
    comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    comp.set_editor_property("capture_every_frame", False)
    comp.set_editor_property("capture_on_movement", False)
    comp.set_editor_property("fov_angle", 60.0)
    STATE["rt"], STATE["cap"] = rt, cap
    capture("placed")
    STATE["frames0"] = cloth.get_simulated_frame_count()
    return world


def tick(delta):
    STATE["phase_ticks"] += 1
    phase = STATE["phase"]
    cloth = STATE["cloth"]

    if phase == "warmup":
        frames = cloth.get_simulated_frame_count() - STATE["frames0"]
        if frames > 0:
            unreal.log("CLOTH EDITOR B simulation started after %d editor ticks" % STATE["phase_ticks"])
            STATE["frames0"] = cloth.get_simulated_frame_count()
            STATE["phase"], STATE["phase_ticks"] = "simulate", 0
        elif STATE["phase_ticks"] > WARMUP_TICKS:
            fail("no frame simulated in %d editor ticks - the editor world never ticked the cloth component" % WARMUP_TICKS)
            STATE["phase"] = "done"
        return

    if phase == "simulate":
        if STATE["phase_ticks"] < SIM_TICKS:
            return
        frames = cloth.get_simulated_frame_count() - STATE["frames0"]
        unreal.log("CLOTH EDITOR B %d frames simulated during %d editor ticks (halted=%s)"
                   % (frames, SIM_TICKS, cloth.is_halted()))
        if frames < SIM_TICKS // 2:
            fail("only %d frames simulated in %d editor ticks" % (frames, SIM_TICKS))
        if cloth.is_halted():
            fail("the cloth halted")
        capture("hanging")
        # 再从背面拍一张：模板关卡的太阳在布后面，正面是背光的一片黑；背面（双面网格的反面）受光，
        # 顺带看反面的法线与绕序没问题。只出图，不进判据。
        cap = STATE["cap"]
        front = (cap.get_actor_location(), cap.get_actor_rotation())
        back_eye, back_aim = unreal.Vector(-560.0, -240.0, 240.0), unreal.Vector(-100.0, 0.0, 160.0)
        cap.set_actor_location_and_rotation(back_eye, look_at(back_eye, back_aim), False, False)
        capture("hanging_back")
        cap.set_actor_location_and_rotation(front[0], front[1], False, False)
        ratio = changed_ratio(STATE["shots"].get("placed"), STATE["shots"].get("hanging"))
        unreal.log("CLOTH EDITOR C placed-vs-hanging changed %.1f%% of sampled pixels" % (ratio * 100.0))
        if ratio < 0.03:
            fail("the shot barely changed (%.1f%%) - the cloth did not fall" % (ratio * 100.0))
        STATE["bounds0"] = STATE["actor"].get_cloth_mesh().get_world_bounds_approx()
        STATE["phase"], STATE["phase_ticks"] = "drag", 0
        return

    if phase == "drag":
        actor = STATE["actor"]
        if STATE["phase_ticks"] <= DRAG_TICKS:
            actor.set_actor_location(unreal.Vector(0.0, 300.0 * STATE["phase_ticks"] / DRAG_TICKS, 250.0), False, False)
            return
        if STATE["phase_ticks"] < DRAG_TICKS + 30:
            return   # 包围盒的重发走 EditMeshAsync，完成回调在之后的游戏线程 tick 上落地
        b0 = STATE["bounds0"]
        b1 = actor.get_cloth_mesh().get_world_bounds_approx()
        c0 = (b0.min + b0.max) * 0.5
        c1 = (b1.min + b1.max) * 0.5
        unreal.log("CLOTH EDITOR D bounds centre (%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f)"
                   % (c0.x, c0.y, c0.z, c1.x, c1.y, c1.z))
        if c1.y - c0.y < 150.0:
            fail("published bounds did not follow the dragged actor (dy = %.1f)" % (c1.y - c0.y))
        if cloth.is_halted():
            fail("the cloth halted while being dragged")
        actor.set_editor_property("grid_resolution", unreal.IntPoint(32, 32))
        STATE["phase"], STATE["phase_ticks"] = "rebuild", 0
        return

    if phase == "rebuild":
        if STATE["phase_ticks"] < 10:
            return
        n = cloth.get_particle_count()
        unreal.log("CLOTH EDITOR E after GridResolution=32x32: %d particles, ready=%s, halted=%s"
                   % (n, cloth.is_simulation_ready(), cloth.is_halted()))
        if n != 33 * 33:
            fail("changing GridResolution did not rebuild the cloth (%d particles, expected %d)" % (n, 33 * 33))
        STATE["phase"] = "done"
        return

    finish()


def finish():
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    for a in (STATE["actor"], STATE["cap"]):
        if a:
            ACTORS.destroy_actor(a)
    unreal.log("CLOTH EDITOR VERDICT %s" % ("OK" if STATE["ok"] else "FAILED"))
    unreal.SystemLibrary.quit_editor()


# 准备段整个包在 try 里：抛出去的话编辑器永远不退出，任务看着像挂死。
try:
    built = build()
except Exception:
    import traceback
    unreal.log_error("CLOTH EDITOR FAILED in build():\n%s" % traceback.format_exc())
    built = None

if built is None:
    unreal.log("CLOTH EDITOR VERDICT FAILED")
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
