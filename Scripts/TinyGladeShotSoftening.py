# -*- coding: utf-8 -*-
"""「像不像 Tiny Glade」的改前/改后同机位对照图 + 可量化判据。

回答的是三件只有像素能答、readback 一个字都说不了的事，正好对应对照文档
`Docs/TinyGlade/Progress_ModuleComparison.md（卷四）` §5 归因表里排 2、3 两条与本轮的石阶标定：

  A  **背光面**（`backlit`）：土台朝背光那一侧。TG 的背光面**永远压不黑**
     （§3.3 `mix(0.6, 1.0, ao)`）而且**带地面草色**（§3.4 每个 RGB 通道各带一条 L1 方向）。
     判据：这一片的亮度分位数 + 色度，不是"看着亮了"。
  B  **阴影区**（`shadow`）：土台投在草地上的那片影子。同上，但它是**投影**不是朝向，
     两者在 UE 侧走的旋钮完全不同（一个是天光/GI 底，一个是阴影贴图 + AO），必须分开判读。
  C  **石阶踏面**（`tread` / `toe`）：踏面露不露得出来、块底有没有悬空。
     `toe` 是从坡底仰看整条石阶 —— **悬空量只有在这个角度才读得出来**，
     俯视图里被踏面自己挡住了。

**机位必须与太阳解耦**：方位角只取太阳的 **yaw**（改灯只改 pitch / 强度 / 色温，yaw 不动），
距离与高度是常数。否则一改光两张图就不是同机位，所有像素判据当场失效。

用法（tag 决定文件名前缀；一律跑两次，前后各一次）::

    set TG_SHOT_TAG=soft_before
    UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotSoftening.py"

产物：`Saved/TinyGladeShots/soft_<tag>_*.png`；离线判据用
`Scripts/TinyGladeShotSofteningStats.py`（PIL + numpy，编辑器外跑）。

--- 踩过的坑（逐条来自状态文件的坑表，全都真踩过）-------------------------------
 ① `unreal.Rotator` 的构造参数是 **(roll, pitch, yaw)**，一律用关键字参数。
    写错的症状：相机朝天，出图一张纯渐变，像"渲染没出来"。
 ② 必须用**真编辑器** `UnrealEditor.exe` 配 `-ExecCmds="py <脚本>"`。
    `-ExecutePythonScript` 跑完即退，tick 回调根本没机会触发。
 ③ 用离屏 `SceneCapture2D`，**不要** `HighResShot` —— 本工程起来是 4 分屏。
 ④ `create_render_target2d` 必须显式传 `RTF_RGBA8`（默认浮点，导出的 png 是 HDR 内容）。
 ⑤ **离屏 SceneCapture 被引擎写死关掉 Lumen**：`SceneCaptureRendering.cpp` 的
    `SetupViewFamilyForSceneCapture` 在 blend 完世界 PPV **之后**才写
    `DynamicGlobalIlluminationMethod = None`，所以 PPV 里的 Lumen 参数一条都不生效。
    翻回来的唯一办法是给捕获组件自己的 `post_process_settings` 再覆盖一次。
    ⚠️ 本脚本**只覆盖 GI/反射方法这两项**，曝光/AO/间接光一律让世界 PPV 说了算 ——
    否则这组图量的就是脚本里写死的那几个数，不是关卡真实的观感。
 ⑥ 准备阶段抛异常 ⇒ 编辑器**永不退出**（那条 Quit 再也执行不到）。准备段整个包在 try 里。
 ⑦ ⚠️ `L_TerrainOpsDemo` 的 GPU 实例石阶在离屏 capture 下**可能只画出一个固定子集**
    （状态文件「离屏 SceneCapture 画不出 GPU 实例」，真因仍开放）。石阶那两张因此是
    **辅证**：埋深标定的**主证据**是本脚本 `log_stair_geometry()` 打出来的解析量。
 ⑧ ⚠️ **本轮新揪出来的一条，与 ⑤ 同症状、不同根因**：`USceneCaptureComponent`
    默认没有 ViewState，于是眼适应整条链不跑，**PPV 里的曝光一条都不生效**。
    实测：EV100 从 0.5 改到 −1.0（2.83×），五张图的每一个分位数**逐位不变**。
    修法是 `always_persist_rendering_state = True`（具体行号与引擎依据写在那一行旁边）。
 ⑨ ⚠️ **⑤ 和 ⑧ 合起来会造出一个假故障：整张图里凡是没被太阳直射的 lit 表面都精确 (0,0,0)。**
    ⑧ 让捕获**真的有了 ViewState**，⑤ 又把 GI 翻回 Lumen —— 于是 Lumen 第一次真的跑了起来；
    而 Lumen 的最终聚集（屏幕探针 + 辐照缓存）**靠帧间历史**，`capture_scene()` 只抓一帧时
    间接光**恒为零**，天光一点都进不来。症状极具误导性：它看着像"几何上有个洞"或
    "天光坏了"，而且**任何曝光/AO/天光旋钮都抬不动它**（零乘什么都是零）。
    实测标定（同机位，`Scripts/TinyGladeShotSeamWarmup.py`，精确 (0,0,0) 占比）：
    连拍 1 次 **19.25%** → 2 次 9.43% → 4 次 4.56% → 8 次 0.007% → **16 次起 0.000%**；
    亮度均值到 32 次时已在 198 帧收敛值的 1% 以内。对照组：同样有 ViewState 但 GI 不翻回
    Lumen 时是 **0.00%** —— 所以这不是曝光、不是天光、不是几何。
    ⇒ 每个机位导出前必须先 `capture_scene()` 预热 `WARMUP_CAPTURES` 次（下面那个常数）。
    ⚠️ 别用"关掉 Lumen"来绕：工程级 `r.DynamicGlobalIlluminationMethod=1` 就是 Lumen，
    出图要量的是关卡真实的观感。（这一局两者的均值差 < 1%，但那是这个场景的巧合，不是保证。）
"""
import math
import os

import unreal

TAG = os.environ.get("TG_SHOT_TAG", "before")
PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"ticks": 0, "handle": None, "rigs": [], "spawned": [], "ground": None, "world": None}


def look_at(frm, to):
    """⚠️ unreal.Rotator 的参数序是 (roll, pitch, yaw)，只用关键字参数（坑 ①）。"""
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


def log_lighting():
    """把这一张图**实际**跑在什么光照下打进日志。改前/改后各一份，diff 就是这轮改了什么。"""
    for a in ACTORS.get_all_level_actors():
        cls = a.get_class().get_name()
        if cls == "DirectionalLight":
            c = a.get_editor_property("directional_light_component")
            r = a.get_actor_rotation()
            unreal.log("SOFT sun '%s' pitch=%.2f yaw=%.2f intensity=%.3f angle=%.4f temp=%s%.0f color=%s"
                       % (a.get_actor_label(), r.pitch, r.yaw,
                          c.get_editor_property("intensity"),
                          c.get_editor_property("light_source_angle"),
                          "on " if c.get_editor_property("use_temperature") else "off ",
                          c.get_editor_property("temperature"),
                          c.get_editor_property("light_color")))
        elif cls == "SkyLight":
            c = a.light_component
            unreal.log("SOFT skylight '%s' intensity=%.3f rtc=%s src=%s lowerBlack=%s lower=%s color=%s"
                       % (a.get_actor_label(), c.get_editor_property("intensity"),
                          c.get_editor_property("real_time_capture"),
                          c.get_editor_property("source_type"),
                          c.get_editor_property("lower_hemisphere_is_black"),
                          c.get_editor_property("lower_hemisphere_color"),
                          c.get_editor_property("light_color")))
        elif cls == "PostProcessVolume":
            pp = a.get_editor_property("settings")
            for name in ("auto_exposure_method", "auto_exposure_bias",
                         "auto_exposure_min_brightness", "auto_exposure_max_brightness",
                         "ambient_occlusion_intensity", "ambient_occlusion_power",
                         "lumen_skylight_leaking", "indirect_lighting_intensity",
                         "indirect_lighting_color", "lumen_diffuse_color_boost",
                         "expand_gamut", "tone_curve_amount", "film_slope", "film_toe",
                         "color_saturation", "color_contrast"):
                try:
                    unreal.log("SOFT ppv %-34s = %s" % (name, pp.get_editor_property(name)))
                except Exception as exc:
                    unreal.log_warning("SOFT ppv %s unreadable: %s" % (name, exc))


def log_stair_geometry(ground, shaper):
    """石阶埋深标定的**主证据**：解析量，不靠像素。

    裙边剖面 `S = W²(3−2W)`（W = 1 − skirt/Falloff）的最大斜率是 1.5，二次抬升再乘
    `1 + 1.5·k·√S`（k = SecondaryLiftScale），所以最陡处 `m = Lift/Falloff × 1.5334`。
    块底钉在等值线上、块心沿最陡方向**上坡**推 e，于是（沿最陡方向的一维剖面）：

      · 等值线水平间距   d = StairStepHeight / m     —— 也就是**可见踏面进深的上界**
      · 块前缘（下坡侧）悬空量 f = (X/2 − e)·m       —— **> 0 就是"支棱着像石墙"的那一条**
      · 块心露出高度     P = Z − e·m                 —— 属性注释里那条公式
      · 可见踏面         T = min(d, Z/m − e + X/2)   —— e 太大时踏面被地面吃掉
    """
    try:
        lift = shaper.get_editor_property("LiftHeight")
        falloff = shaper.get_editor_property("FalloffDistance")
        k = shaper.get_editor_property("SecondaryLiftScale")
        size = ground.get_editor_property("StairBlockSize")
        embed = ground.get_editor_property("StairEmbed")
        step = ground.get_editor_property("StairStepHeight")
    except Exception as exc:
        unreal.log_warning("SOFT stair geometry unavailable: %s" % exc)
        return
    # W = 0.5 是 6W(1−W) 的极值点；二次抬升在那里 S = 0.5。
    m = lift / max(falloff, 1e-3) * 1.5 * (1.0 + 1.5 * k * math.sqrt(0.5))
    d = step / max(m, 1e-6)
    f = (size.z * 0.0 + size.x * 0.5 - embed) * m
    p = size.z - embed * m
    t = min(d, size.z / max(m, 1e-6) - embed + size.x * 0.5)
    unreal.log("SOFT stairs lift=%.0f falloff=%.0f slope=%.4f block=(%.0f,%.0f,%.0f) embed=%.1f step=%.0f"
               % (lift, falloff, m, size.x, size.y, size.z, embed, step))
    unreal.log("SOFT stairs contour_spacing=%.2f  float_under_toe=%+.2f  proud_at_centre=%.2f  visible_tread=%.2f  rise/tread=%.3f"
               % (d, f, p, t, step / max(t, 1e-6)))


def build():
    unreal.EditorLoadingAndSavingUtils.load_map("%s/L_TerrainOpsDemo" % PKG)
    world = unreal.EditorLevelLibrary.get_editor_world()

    ground = find_ground()
    if not ground:
        unreal.log_error("SOFT FAILED: no ground actor")
        return None
    STATE["ground"] = ground

    shapers = [a for a in ACTORS.get_all_level_actors() if "Shaper" in a.get_class().get_name()]
    if not shapers:
        unreal.log_error("SOFT FAILED: no shaper")
        return None
    shaper = shapers[0]
    hub = shaper.get_actor_location()
    radius = shaper.get_editor_property("Radius")
    falloff = shaper.get_editor_property("FalloffDistance")

    log_lighting()
    log_stair_geometry(ground, shaper)

    # 方位角只取太阳 yaw（见文件头）。找不到灯就退回 0，出图仍然可比，只是不保证背光。
    sun_yaw = 0.0
    for a in ACTORS.get_all_level_actors():
        if a.get_class().get_name() == "DirectionalLight":
            sun_yaw = a.get_actor_rotation().yaw
            break
    sx, sy = math.cos(math.radians(sun_yaw)), math.sin(math.radians(sun_yaw))   # 光**去**的方向
    px, py = -sy, sx                                                            # 与之垂直

    # 路：沿最陡方向（= 太阳方向的水平投影，纯粹为了让石阶与背光面同框）穿过土台中心。
    # 一笔 3200 cm 覆盖 [−1400, −600] 与 [600, 1400] 两段裙边，石阶两侧都长。
    ground.reset_paint()
    ground.begin_paint_stroke()
    for i in range(41):
        t = -1600.0 + i * 80.0
        x, y = hub.x + sx * t, hub.y + sy * t
        ground.apply_paint_stroke(unreal.Vector(x, y, ground.sample_height(unreal.Vector2D(x, y))))
    ground.end_paint_stroke()

    top_z = ground.sample_height(unreal.Vector2D(hub.x, hub.y))
    unreal.log("SOFT hub=(%.0f, %.0f) top_z=%.1f radius=%.0f falloff=%.0f sun_yaw=%.1f"
               % (hub.x, hub.y, top_z, radius, falloff, sun_yaw))

    def at(along, side, up):
        """土台局部坐标：along 沿太阳水平方向（+ = 背光侧），side 垂直，up 相对该点地面。"""
        x, y = hub.x + sx * along + px * side, hub.y + sy * along + py * side
        return unreal.Vector(x, y, ground.sample_height(unreal.Vector2D(x, y)) + up)

    # 石阶最陡处：smoothstep 的极值在 skirt = 0.5·Falloff ⇒ 距中心 radius + 0.5·falloff。
    steep = radius + 0.5 * falloff

    cams = [
        # A 背光面：站在光**去**的那一侧回望，看到的全是背光面。
        ("backlit", at(2600.0, 0.0, 260.0), at(0.0, 0.0, -120.0)),
        # B 阴影区：高机位三四分，把土台投在草地上的那片影子整片收进画面。
        ("shadow", at(2400.0, 1900.0, 1100.0), at(1200.0, 0.0, -60.0)),
        # C 石阶踏面：贴脸三四分俯看，判"踏面露不露得出来"。
        ("tread", at(-steep + 220.0, 430.0, 270.0), at(-steep, 0.0, 40.0)),
        # C' 石阶趾部：坡底仰看，判"块底悬不悬空"—— 只有这个角度读得出来。
        ("toe", at(-(radius + falloff) - 260.0, 150.0, 130.0), at(-steep + 120.0, 0.0, 150.0)),
        # 全景：一张能一眼看出整体明度与色调的图。
        ("wide", at(3000.0, 2400.0, 1500.0), at(0.0, 0.0, 0.0)),
    ]

    # 坑 ⑤：只翻回 GI/反射方法两项，其余交给世界 PPV（否则量的是脚本不是关卡）。
    cap_pp = unreal.PostProcessSettings()
    cap_pp.set_editor_property("override_dynamic_global_illumination_method", True)
    cap_pp.set_editor_property("dynamic_global_illumination_method",
                               unreal.DynamicGlobalIlluminationMethod.LUMEN)
    cap_pp.set_editor_property("override_reflection_method", True)
    cap_pp.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)

    for name, loc, aim in cams:
        rt = unreal.RenderingLibrary.create_render_target2d(
            world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8,
            unreal.LinearColor(0, 0, 0, 1), False)
        cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, loc, look_at(loc, aim))
        comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
        comp.set_editor_property("texture_target", rt)
        comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
        comp.set_editor_property("fov_angle", 55.0)
        comp.set_editor_property("capture_every_frame", False)
        comp.set_editor_property("capture_on_movement", False)
        comp.set_editor_property("post_process_settings", cap_pp)
        comp.set_editor_property("post_process_blend_weight", 1.0)
        # 坑 ⑧（本轮新揪出来的，与坑 ⑤ 是**两条不同的坑**）：
        # `USceneCaptureComponent` 默认**没有 ViewState**，而没有 ViewState 时眼适应整条链
        # 都不跑——`PostProcessEyeAdaptation.cpp:1422-1425` 注释原文："UpdatePreExposure() was not
        # being called if no view state ... might impact legacy behavior of USceneCaptureComponent
        # that by default don't have a ViewState"。后果是 **PPV 里的曝光一条都不生效**：
        # 实测把 EV100 从 0.5 改到 −1.0（2.83×），五张图的每一个分位数都逐位不变；
        # 把曝光改写到捕获自己的 post_process_settings 上也一样不动。开了这一行之后
        # 同一个 EV 立刻生效（tread 机位 p50 0.172 → 0.299）。
        # ➔ 不开它的症状是“调了曝光但两张图一模一样”，与坑 ⑤ 的症状字面相同，
        #   但修法完全不同，别拿坑 ⑤ 的办法去修它。
        comp.set_editor_property("always_persist_rendering_state", True)
        # [名字, 组件, 渲染目标, 已连拍次数, 已导出]；预热计数见坑 ⑨。
        STATE["rigs"].append([name, comp, rt, 0, False])
        STATE["spawned"].append(cap)
        unreal.log("SOFT cam %-8s at (%.0f, %.0f, %.0f)" % (name, loc.x, loc.y, loc.z))
    return world


FIRST_TICK = 60      # 世界侧的收敛（天光实时捕获、Lumen 场景）要几十帧，早拍就是一张暗图
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


def tick(delta):
    STATE["ticks"] += 1
    if STATE["ticks"] < FIRST_TICK:
        return
    # 一帧只推进**一个**机位：几路同时连拍会互相抢帧，谁都攒不满自己的历史。
    for rig in STATE["rigs"]:
        name, comp, rt, shots, exported = rig
        if exported:
            continue
        comp.capture_scene()
        rig[3] = shots + 1
        if rig[3] >= WARMUP_CAPTURES:
            unreal.RenderingLibrary.export_render_target(STATE["world"], rt, OUT_DIR,
                                                         "soft_%s_%s.png" % (TAG, name))
            rig[4] = True
            unreal.log("SOFT shot: soft_%s_%s.png（预热 %d 帧）" % (TAG, name, rig[3]))
        return
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    if STATE["ground"]:
        STATE["ground"].reset_paint()
    for a in STATE["spawned"]:
        ACTORS.destroy_actor(a)
    unreal.log("SOFT DONE tag=%s" % TAG)
    unreal.SystemLibrary.quit_editor()


# 坑 ⑥：准备段整个包在 try 里 —— 抛出去的话编辑器永远不退出，任务看着像挂死。
try:
    STATE["world"] = build()
except Exception as exc:
    unreal.log_error("SOFT FAILED in build(): %s" % exc)
    STATE["world"] = None

if STATE["world"] is None:
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
