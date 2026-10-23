# -*- coding: utf-8 -*-
"""岩壳盖 / 裙分开着色的**像素判据** + 出图。

单测 `RockShell.CapSkirtSurvivesBake` 已经证明通道里两种值都在、且烘成 StaticMesh 之后还在。
它证明不了的只有一件事：**那两种值有没有变成屏幕上不同的颜色**。
`bIsCapTri` 被打了却没人读整整一轮，正是因为"数据在"和"画面上看得见"之间没有任何一条断言 ——
这个脚本就是补那一条。

判据形状照抄 `TinyGladeShotRockShell.py` 的 A（同机位只切一个开关，差异率就是硬证据）::

    on  = 关卡现状（MI_rocky_terrain 把 RockShellCapSkirt 覆盖成 1）
    off = 同一张 MI 的 MID，把 RockShellCapSkirt 按回 0（= 改动前的样子）

⚠️ **对照组走 MID 而不是改资产**：改资产会把"被测对象"本身改掉，下次跑就测不到东西了；
而且那正是留给"故意破坏世界侧"实验的手（把 `MI_rocky_terrain` 的参数按成 0 再跑本脚本，
门必须报红）。

--- 阈值怎么来的 -------------------------------------------------------------------
先量真值再取一半，物理含义 =「裙在屏幕上的覆盖面至少还剩一半」。**不是**取一个能绿的小数。
实测值写在 `THRESH` 旁边。

--- 踩过的坑（照抄状态文件的坑表）-----------------------------------------------------
 ① `unreal.Rotator` 参数序是 (roll, pitch, yaw)，一律关键字参数。
 ② 必须真编辑器 + `-ExecCmds="py <脚本>"`（`-ExecutePythonScript` 跑完即退，tick 不触发）。
 ③ 离屏 `SceneCapture2D`，不要 `HighResShot`。 ④ `create_render_target2d` 显式 `RTF_RGBA8`。
 ⑤ 离屏 capture 被引擎写死关掉 Lumen，只能用捕获组件自己的 `post_process_settings` 覆盖回来。
 ⑥ 准备段整个包在 try 里，否则编辑器永不退出。 ⑦ 石阶那条实例路在离屏 capture 下画不对，关掉。
 ⑧ `always_persist_rendering_state = True`（否则 PPV 曝光一条都不生效）。
 ⑨ ⑧ 会立刻带出 ⑨：每个机位导出前**一帧一次**预热 32 次，缺一不可。
 ⑩ 一律调 `get_rock_shell_undrawable_reason()`，**不要** `is_rock_shell_drawable()` ——
    后者不可画时返回 `None`，`str(None) == "None"` 会伪造出一句像模像样的原因。
"""
import math
import os

import unreal

TAG = os.environ.get("TG_SHOT_TAG", "v1")
PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
SHELL_MI = "/PCGPlugins/HouseTest/TinyGladeAsset/Materials/MI_rocky_terrain"
PARAM = "RockShellCapSkirt"

W, H = 1600, 900
# ⚠️ 采样格比别的脚本密一倍：裙的竖直高度 = 局部坡度 × 19.5 cm，在屏幕上是**几像素宽的窄带**，
# 48 × 27 的格距是 33 px —— 第一次标定时 wall 那张两图 md5 明明不同，采样格却一个点都没落在
# 裙上，差异率读成 0.0%。96 × 54（格距 17 px）才接得住这个尺度的信号。
GRID_X, GRID_Y = 96, 54
PIXEL_DELTA = 12
ZERO_FAIL = 0.005
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

# 阈值 = **实测值的一半**（物理含义：裙在屏幕上的覆盖面至少还剩一半）。
# 每个机位一份，不互相套 —— 裙的屏幕占比与"视线和坡面的夹角"强相关，实测跨了一个量级。
#
# 标定实测（96×54 采样格，两次独立启动 5.69% / 5.67%，运行间噪声约 0.02 个百分点）：
#   band  5.67%   close  2.04%   over  0.00%   wall  0.00%
# 噪声地板用同一批全分辨率对照量过：`over` 与 `wall` 两张的 `>12/255` 占比分别是 0.00% / 0.01%
# ⇒ 2.8% 的门离地板有两个量级，不是"擦着噪声过"的假门。
MEASURED = {"band": 0.0567, "close": 0.0204}
THRESH = {"band": 0.0283, "close": 0.0102}
# ⚠️ `over`（正俯视）**故意不设门**：从正上方看，裙那圈沿法线的壁正好**接近侧视**，
# 全分辨率实测最大通道差只有 **28/255**，而且整张图 52.8% 的像素都有微小差异 ——
# 那是**间接光**（裙暗了，弹到盖上的光就少了），不是裙本身。信号与背景分不开，
# 给它设门只会得到一条永远绿的门。它留下来只作看图用。

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"ticks": 0, "handle": None, "plan": [], "world": None, "ground": None,
         "samples": {}, "rt": None, "comp": None, "spawned": [], "step": 0, "phase": 0,
         "on_mat": None, "off_mat": None}


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
        unreal.log_warning("CAPSKIRT skip %s: %s" % (prop, exc))
        return False


def use_material(mat):
    """换岩壳材质并重披挂。`RockShellInputHash` 把材质指针算进去了，所以这一步会真的重建。"""
    STATE["ground"].set_editor_property("RockShellMaterial", mat)
    STATE["ground"].call_method("RebuildRockShell")
    unreal.log("CAPSKIRT material -> %s" % (mat.get_name() if mat else "<NONE>"))


def build():
    unreal.EditorLoadingAndSavingUtils.load_map("%s/L_TerrainOpsDemo" % PKG)
    world = unreal.EditorLevelLibrary.get_editor_world()

    ground = find("Ground_Demo")
    shaper = find("Shaper_Mound")
    if not (ground and shaper):
        unreal.log_error("CAPSKIRT FAILED: no Ground_Demo / Shaper_Mound")
        return None
    STATE["ground"] = ground

    try_set(ground, "StairMesh", None)          # 坑 ⑦
    ground.set_editor_property("bRockShell", True)
    ground.reset_paint()                        # 不画路：路会把壳整片压下去，与本次判读无关
    shaper.call_method("RebuildTerrain")

    on_mat = ground.get_editor_property("RockShellMaterial")
    if on_mat is None:
        on_mat = unreal.EditorAssetLibrary.load_asset(SHELL_MI)
        ground.set_editor_property("RockShellMaterial", on_mat)
    STATE["on_mat"] = on_mat

    # 对照组：同一张 MI 的 MID，只把那一个参数按回 0。**不动资产**（见文件头）。
    off_mat = unreal.MaterialLibrary.create_dynamic_material_instance(world, on_mat)
    off_mat.set_scalar_parameter_value(PARAM, 0.0)
    STATE["off_mat"] = off_mat

    on_value = unreal.MaterialEditingLibrary.get_material_instance_scalar_parameter_value(on_mat, PARAM) \
        if isinstance(on_mat, unreal.MaterialInstanceConstant) else float("nan")
    unreal.log("CAPSKIRT %s：关卡材质 %s = %.3f；对照组 MID = 0.000"
               % (PARAM, on_mat.get_name(), on_value))
    if not (on_value > 0.5):
        # 参数没开的话两组图会完全一样，差异率≈0 —— 门会报红，但报红的原因值得先说清楚。
        unreal.log_error("CAPSKIRT !! 关卡材质的 %s = %.3f（不是 1）—— 先跑 "
                         "Scripts/TinyGladeMakeRockShellShading.py" % (PARAM, on_value))

    why = str(ground.get_rock_shell_undrawable_reason())   # 坑 ⑩
    unreal.log("CAPSKIRT drawable-reason=%r（空串 = 可画）" % why)

    c = shaper.get_actor_location()
    radius = 0.5 * shaper.get_editor_property("MaskSize")   # 塑形物的 Radius 已删（09-29）：圆盘直径 = MaskSize，设了 mask 时是近似
    falloff = shaper.get_editor_property("FalloffDistance")
    reach = radius + falloff

    def gz(x, y):
        return ground.sample_height(unreal.Vector2D(x, y))

    # 两个机位都对着**最陡的那一圈**（smoothstep 的拐点）—— 裙的竖直高度 = 局部坡度 × 19.5 cm，
    # 所以只有陡坡上裙才有面积可看；平地上裙自动塌成零高度，拍它等于拍一条线。
    mid_r = radius + 0.5 * falloff
    cam_wall = unreal.Vector(c.x + reach + 500.0, c.y, gz(c.x + reach + 500.0, c.y) + 420.0)
    aim_wall = unreal.Vector(c.x + mid_r, c.y, gz(c.x + mid_r, c.y) + 80.0)
    cam_band = unreal.Vector(c.x + reach + 250.0, c.y - reach - 250.0,
                             gz(c.x + reach + 250.0, c.y - reach - 250.0) + 900.0)
    aim_band = unreal.Vector(c.x + mid_r * 0.7, c.y - mid_r * 0.7,
                             gz(c.x + mid_r * 0.7, c.y - mid_r * 0.7) + 60.0)

    # 贴脸：站在陡坡外 350 cm、比瞄点高 150 cm 微俯。裙是**沿地形法线**的那圈 60 cm 高的壁
    # （盖浮 0..30、裙底沉 30），离得越近、俯角越接近法线的垂面，它在屏幕上占得越多。
    ax, ay = c.x + mid_r + 350.0, c.y
    aim_close = unreal.Vector(c.x + mid_r, c.y, gz(c.x + mid_r, c.y) + 40.0)
    cam_close = unreal.Vector(ax, ay, gz(ax, ay) + 260.0)

    # 俯视陡坡带：裙在**平面投影**里占 13.4%（原件实测：盖是内缩孤岛 86.6%、裙精确填缝 13.4%），
    # 所以视线越接近坡面法线，裙的屏幕占比越接近那个上界。这一张的信噪比最高。
    ov_r = mid_r
    cam_over = unreal.Vector(c.x + ov_r, c.y, gz(c.x + ov_r, c.y) + 1500.0)
    rot_over = unreal.Rotator(roll=0.0, pitch=-90.0, yaw=0.0)

    # ⚠️ `wall` 机位**被取消**，不是忘了：它站在坡脚平视，视线几乎贴着坡面 ——
    # 裙底沿法线沉 30 cm 本来就埋在地面下（画面里胞腔之间露的是**草**不是裙壁），
    # 剩下那点裙在屏幕上只有几像素宽。实测 0/1296 与 0/5184 各一次，两次都是**精确 0** ——
    # 一条看不见信号的门比没有门更糟，它会永远绿着。
    CAMS = [
        ("over", (cam_over, rot_over), 45.0),
        ("band", (cam_band, look_at(cam_band, aim_band)), 55.0),
        ("close", (cam_close, look_at(cam_close, aim_close)), 60.0),
    ]
    STATE["cams"] = [name for name, _, _ in CAMS]
    STATE["plan"] = []
    for name, view, fov in CAMS:
        STATE["plan"].append(("%s_on" % name, lambda: use_material(STATE["on_mat"]), view, fov))
        STATE["plan"].append(("%s_off" % name, lambda: use_material(STATE["off_mat"]), view, fov))

    cap_pp = unreal.PostProcessSettings()       # 坑 ⑤
    cap_pp.set_editor_property("override_dynamic_global_illumination_method", True)
    cap_pp.set_editor_property("dynamic_global_illumination_method",
                               unreal.DynamicGlobalIlluminationMethod.LUMEN)
    cap_pp.set_editor_property("override_reflection_method", True)
    cap_pp.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)

    rt = unreal.RenderingLibrary.create_render_target2d(
        world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8,
        unreal.LinearColor(0, 0, 0, 1), False)
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, cam_over, STATE["plan"][0][2][1])
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


def diff_ratio(a, b):
    changed = sum(1 for pa, pb in zip(a, b)
                  if max(abs(pa[0] - pb[0]), abs(pa[1] - pb[1]), abs(pa[2] - pb[2])) > PIXEL_DELTA)
    return changed / float(len(a)), changed


def zero_ratio(samples):
    return sum(1 for p in samples if p[0] == 0.0 and p[1] == 0.0 and p[2] == 0.0) / float(len(samples))


def report():
    s = STATE["samples"]
    ok = True
    for name in sorted(s):
        z = zero_ratio(s[name])
        unreal.log("CAPSKIRT PIXELS %-10s zero=%.3f%%" % (name, z * 100.0))
        if z > ZERO_FAIL:
            unreal.log_error("CAPSKIRT !! %s 有 %.3f%% 的像素精确 (0,0,0) —— Lumen 预热没起作用（坑 ⑨）"
                             % (name, z * 100.0))
            ok = False

    for cam in STATE.get("cams", []):
        a, b = s.get("%s_on" % cam), s.get("%s_off" % cam)
        if not (a and b):
            continue
        ratio, changed = diff_ratio(a, b)
        gate = THRESH.get(cam)
        unreal.log("CAPSKIRT PIXELS %-6s on-vs-off: %d/%d changed (%.2f%%)，门 %s"
                   % (cam, changed, GRID_X * GRID_Y, ratio * 100.0,
                      ("%.2f%%" % (gate * 100.0)) if gate is not None else "<未标定>"))
        if gate is None:
            # 未标定的机位只报数不判 —— 编一个阈值出来比没有门更糟（它会一直绿）。
            unreal.log_warning("CAPSKIRT %s 还没有阈值，本轮只出数（标定：取实测值的一半写进 THRESH）" % cam)
            continue
        if ratio < gate:
            unreal.log_error("CAPSKIRT !! %s：切 %s 几乎没有改变画面 —— 盖/裙没有被分开着色"
                             % (cam, PARAM))
            ok = False
    unreal.log("CAPSKIRT PIXEL VERDICT %s" % ("OK" if ok else "FAILED"))


def tick(delta):
    STATE["ticks"] += 1
    if STATE["ticks"] < FIRST_TICK:
        return
    step = STATE["step"]
    if step < len(STATE["plan"]):
        name, action, (loc, rot), fov = STATE["plan"][step]
        phase, STATE["phase"] = STATE["phase"], STATE["phase"] + 1
        if phase == 0:
            action()
            STATE["comp"].set_editor_property("fov_angle", fov)
            STATE["spawned"][0].set_actor_location_and_rotation(loc, rot, False, False)
        elif phase >= SETTLE:
            STATE["comp"].capture_scene()       # 坑 ⑨：一帧一次
            if phase >= SETTLE + WARMUP_CAPTURES - 1:
                unreal.RenderingLibrary.export_render_target(
                    STATE["world"], STATE["rt"], OUT_DIR, "capskirt_%s_%s.png" % (TAG, name))
                STATE["samples"][name] = sample_pixels()
                unreal.log("CAPSKIRT shot: capskirt_%s_%s.png（预热 %d 帧）"
                           % (TAG, name, WARMUP_CAPTURES))
                STATE["step"], STATE["phase"] = step + 1, 0
        return

    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    report()
    # 材质换回关卡原本那张，别把 MID 留在关卡里（MID 不能存盘，留着下次开图就是空材质）。
    use_material(STATE["on_mat"])
    for a in STATE["spawned"]:
        ACTORS.destroy_actor(a)
    unreal.log("CAPSKIRT DONE tag=%s" % TAG)
    unreal.SystemLibrary.quit_editor()


try:                                            # 坑 ⑥
    STATE["world"] = build()
except Exception:
    import traceback
    unreal.log_error("CAPSKIRT FAILED in build():\n%s" % traceback.format_exc())
    STATE["world"] = None

if STATE["world"] is None:
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
