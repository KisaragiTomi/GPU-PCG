# -*- coding: utf-8 -*-
"""披挂岩壳（链 B）的出图 + **像素判据**。

要回答的是三件只有像素能答的事（readback 断言对它们一个字都说不了）：

  A  **陡坡上真的长出岩壁**：`IsRockShellDrawable()` 只证明"组件/网格/流/材质都在"，
     它证明不了槽位池里的三角有没有真的变成屏幕上的颜色。所以这里拍**同一机位、
     只切 `bRockShell`** 的两张，逐像素比 —— 差异像素率就是"它画出来了"的硬证据。
  B  **平地上没有**：台顶与羽化之外应当一块石头都没有（裁决四：裙边高度是坡度的副产品，
     平地自动塌成零高度）。
  C  **画路前后的对照**：路上的壳是**沉下去**不是隐藏（裁决五）。两张同机位对照图里
     石头应当是"慢慢埋进土里"，而不是整块整块地消失。

用法（tag 决定文件名前缀）::

    set TG_SHOT_TAG=v1
    UnrealEditor.exe <project> -ExecCmds="py .../TinyGladeShotRockShell.py"

产物：`Saved/TinyGladeShots/rockshell_<tag>_*.png`，判据打在日志的 `ROCKSHELL PIXELS` 行。

--- 踩过的坑（全部来自状态文件的坑表）-----------------------------------------
 ① `unreal.Rotator` 的构造参数是 **(roll, pitch, yaw)**，一律用关键字参数。
    写错的症状：相机朝天，出图一张纯渐变，像"渲染没出来"。
 ② 必须用**真编辑器** `UnrealEditor.exe` 配 `-ExecCmds="py <脚本>"`。
    `-ExecutePythonScript` 跑完即退，tick 回调根本没机会触发。
 ③ 用离屏 `SceneCapture2D`，**不要** `HighResShot` —— 本工程起来是 4 分屏。
 ④ `create_render_target2d` 必须显式传 `RTF_RGBA8`（默认浮点，导出的 png 是 HDR 内容）。
 ⑤ **离屏 SceneCapture 被引擎写死关掉 Lumen**：唯一的翻回办法是给捕获组件自己的
    `post_process_settings` 再覆盖一次（它在引擎那两行之后才 apply）。
 ⑥ 准备阶段抛异常 ⇒ 编辑器**永不退出**（那条 Quit 再也执行不到）。准备段整个包在 try 里。
 ⑦ GPU 实例石阶在离屏 capture 下画不对（真因仍未定位）。本脚本关掉石阶再拍 ——
    岩壳不是实例化路径（它是 `UCSMeshRenderComponent`，与地面同一条路），不受这条影响。
 ⑧ ⚠️ **`USceneCaptureComponent` 默认没有 ViewState**，于是眼适应整条链不跑，
    **PPV 里的曝光一条都不生效**（实测把 EV100 改 2.83 倍，五张图每个分位数逐位不变）。
    与坑 ⑤ 症状字面相同、根因完全不同，别拿坑 ⑤ 的办法去修它。
 ⑨ ⚠️ **修了 ⑧ 才会触发 ⑨，两条必须一起加。** 有了 ViewState，坑 ⑤ 翻回来的 Lumen 才
    第一次真的跑起来；而 Lumen 的最终聚集靠**帧间历史**，`capture_scene()` 只抓一帧 ⇒
    间接光恒为零 ⇒ 没被太阳直射的 lit 表面全部精确 (0,0,0)，看着像"几何有洞"。
    修法：每个机位导出前 `capture_scene()` 预热 `WARMUP_CAPTURES` 次，**一帧一次**。
"""
import math
import os

import unreal

TAG = os.environ.get("TG_SHOT_TAG", "v1")
PKG = "/PCGPlugins/HouseTest"
OUT_DIR = unreal.Paths.project_saved_dir() + "TinyGladeShots"
W, H = 1600, 900

# 像素判据的采样格。逐像素读 144 万次太慢，48 × 27 = 1,296 个点足以给出稳定的差异率。
GRID_X, GRID_Y = 48, 27
# 单通道差 > 12/255 才算"这个像素变了" —— 压掉 TAA/曝光的抖动，留下真正的几何变化。
PIXEL_DELTA = 12
# 精确 (0,0,0) 的容许上限，与 `TinyGladeShotSofteningStats.py` 的 `ZERO_FAIL` 同一个数。
ZERO_FAIL = 0.005
# 壳的近景单独放宽（2026-08-31，壳改 TG 形态之后）：胞腔间的裂缝是**几何真值** ——
# 相邻胞腔的浮高差（`CellRelief` 逐胞腔随机）把缝拉成全遮蔽的深槽，槽底间接光精确为零，
# 实测 0.772%（10/1296），且只出现在这一张（其余五张 ≤ 0.077% ⇒ 预热本身是好的）。
# 这条阈值的**本职**是抓坑 ⑨（预热失效 ⇒ 整片未直射面全零，历史上是两位数百分比），
# 2% 对那个失效模式仍然一眼红；别拿它去卡裂缝 —— 消裂缝黑的正式旋钮是
# `RockShellSkirtTilt`（裙圈外撇斜壁互相交叉，2026-08-31 用户方案），那是观感决策，
# 不该由出图脚本的阈值倒逼。
# overhead 同限：正俯视能看进个别槽底（边缘磕碰 `RockShellChipAmount` 让盖缘高低错落，
# 实测 0.54% = 7 个采样点，全在缝线上）—— 同一类几何真黑，不是预热失效。
CREVICE_ZERO_FAIL = {"wall_shell_on": 0.02, "overhead": 0.02}

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {"ticks": 0, "handle": None, "plan": [], "world": None, "ground": None,
         "samples": {}, "rt": None, "comp": None, "spawned": [], "step": 0, "phase": 0,
         "shaper": None}


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
    """演示关卡的属性名随版本漂移过；缺了就跳过，别让准备段抛异常（坑 ⑥）。"""
    try:
        obj.set_editor_property(prop, value)
        return True
    except Exception as exc:
        unreal.log_warning("ROCKSHELL skip %s: %s" % (prop, exc))
        return False


def set_shell(on):
    STATE["ground"].set_editor_property("bRockShell", on)
    unreal.log("ROCKSHELL bRockShell=%s" % on)


def paint_road():
    g, c, reach = STATE["ground"], STATE["centre"], STATE["reach"]
    g.reset_paint()
    g.begin_paint_stroke()
    for i in range(33):
        y = c.y - reach - 200.0 + i * (2.0 * (reach + 200.0) / 32.0)
        g.apply_paint_stroke(unreal.Vector(c.x, y, g.sample_height(unreal.Vector2D(c.x, y))))
    g.end_paint_stroke()
    unreal.log("ROCKSHELL road painted across the mound")


def build():
    unreal.EditorLoadingAndSavingUtils.load_map("%s/L_TerrainOpsDemo" % PKG)
    world = unreal.EditorLevelLibrary.get_editor_world()

    ground = find("Ground_Demo")
    shaper = find("Shaper_Mound")
    if not (ground and shaper):
        unreal.log_error("ROCKSHELL FAILED: no Ground_Demo / Shaper_Mound")
        return None
    STATE["ground"] = ground

    # 坑 ⑦：石阶那条路在离屏 capture 下画不对，且与本次判读无关，关掉再拍。
    # 这里原来还要关第二条（塑形物自持的旧路 `StepMeshes`）并在收尾写回 —— 旧路已随
    # 2026-08-30「裁决一」第二步整条删除，那个属性不存在了，两处一并去掉。
    try_set(ground, "StairMesh", None)
    STATE["shaper"] = shaper
    ground.set_editor_property("bRockShell", True)
    mat = ground.get_editor_property("RockShellMaterial")
    unreal.log("ROCKSHELL material=%s" % (mat.get_name() if mat else "<NONE — 会用引擎默认材质画成一片灰>"))
    ground.reset_paint()
    shaper.call_method("RebuildTerrain")

    c = shaper.get_actor_location()
    radius = 0.5 * shaper.get_editor_property("MaskSize")   # 塑形物的 Radius 已删（09-29）：圆盘直径 = MaskSize，设了 mask 时是近似
    falloff = shaper.get_editor_property("FalloffDistance")
    reach = radius + falloff
    STATE["centre"], STATE["reach"] = c, reach

    # 机位一律从土台参数反算，不手调 —— 手调的机位在土台一动之后就全废了。
    def ground_at(x, y):
        return ground.sample_height(unreal.Vector2D(x, y))

    mid_r = radius + 0.5 * falloff          # 最陡的那一圈（smoothstep 的拐点）
    aim_wall = unreal.Vector(c.x + mid_r, c.y, ground_at(c.x + mid_r, c.y) + 60.0)
    cam_wall = unreal.Vector(c.x + reach + 900.0, c.y, ground_at(c.x + reach + 900.0, c.y) + 320.0)

    top_z = ground_at(c.x, c.y)
    cam_top = unreal.Vector(c.x, c.y, top_z + 4200.0)

    # 平地：站在羽化之外，平视扫过一片纯平地（这张里**不该有任何石头**）。
    flat_x = c.x + reach + 2500.0
    cam_flat = unreal.Vector(flat_x, c.y - 1500.0, ground_at(flat_x, c.y - 1500.0) + 180.0)
    aim_flat = unreal.Vector(flat_x, c.y + 1500.0, ground_at(flat_x, c.y + 1500.0) + 120.0)

    # 路：3/4 侧视看路爬上坡面那一段。
    cam_road = unreal.Vector(c.x + reach + 700.0, c.y - reach - 700.0,
                             ground_at(c.x + reach + 700.0, c.y - reach - 700.0) + 700.0)
    aim_road = unreal.Vector(c.x, c.y - mid_r, ground_at(c.x, c.y - mid_r) + 60.0)

    CAM_WALL = (cam_wall, look_at(cam_wall, aim_wall))
    CAM_TOP = (cam_top, unreal.Rotator(roll=0.0, pitch=-90.0, yaw=0.0))
    CAM_FLAT = (cam_flat, look_at(cam_flat, aim_flat))
    CAM_ROAD = (cam_road, look_at(cam_road, aim_road))

    # (名字, 拍之前要做的事, 机位)。A 的两张必须**同机位、只切开关**，否则差异率没有意义。
    STATE["plan"] = [
        ("wall_shell_off", lambda: set_shell(False), CAM_WALL),
        ("wall_shell_on", lambda: set_shell(True), CAM_WALL),
        ("overhead", lambda: None, CAM_TOP),
        ("flat_none", lambda: None, CAM_FLAT),
        ("road_before", lambda: None, CAM_ROAD),
        ("road_after", paint_road, CAM_ROAD),
    ]

    # 坑 ⑤：把捕获自己那份 PP 覆盖建起来，Lumen 才会回到出图里。
    cap_pp = unreal.PostProcessSettings()
    cap_pp.set_editor_property("override_dynamic_global_illumination_method", True)
    cap_pp.set_editor_property("dynamic_global_illumination_method",
                               unreal.DynamicGlobalIlluminationMethod.LUMEN)
    cap_pp.set_editor_property("override_reflection_method", True)
    cap_pp.set_editor_property("reflection_method", unreal.ReflectionMethod.LUMEN)

    rt = unreal.RenderingLibrary.create_render_target2d(
        world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8,
        unreal.LinearColor(0, 0, 0, 1), False)
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, cam_wall, CAM_WALL[1])
    comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
    comp.set_editor_property("texture_target", rt)
    comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
    comp.set_editor_property("fov_angle", 55.0)
    comp.set_editor_property("capture_every_frame", False)
    comp.set_editor_property("capture_on_movement", False)
    comp.set_editor_property("post_process_settings", cap_pp)
    comp.set_editor_property("post_process_blend_weight", 1.0)
    # 坑 ⑧：没有 ViewState ⇒ 眼适应整条链不跑 ⇒ **PPV 里的曝光一条都不生效**
    # （实测把 EV100 改 2.83 倍，五张图每个分位数逐位不变）。与坑 ⑤ 同症状、不同根因。
    # ⚠️ 开了这一行就会立刻继承坑 ⑨，必须同时有下面那段预热，只加这一行会把图弄得更糟。
    comp.set_editor_property("always_persist_rendering_state", True)
    STATE["rt"], STATE["comp"] = rt, comp
    STATE["spawned"].append(cap)
    return world


def sample_pixels():
    """采样格上的 RGB。逐像素读 144 万次太慢，48 × 27 已足够给出稳定的差异率。"""
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
    """精确 (0,0,0) 占比 —— 坑 ⑨ 有没有被预热掉的**指纹**。

    修好之前，凡是没被太阳直射的 lit 表面都精确为零；修好之后这个数应当趋近 0。
    它同时也是差异率的**校正项**：两张图里同一块地方都是精确零，`diff_ratio` 会把它读成
    "没变"，于是所有差异率都被系统性低估 —— 这就是本轮阈值必须重新标定的原因。
    """
    return sum(1 for p in samples if p[0] == 0.0 and p[1] == 0.0 and p[2] == 0.0) / float(len(samples))


FIRST_TICK = 45      # Lumen 的 surface cache / 天光实时捕获要几十帧才收敛，早拍就是一张暗图
SETTLE = 6           # 动作（切开关 / 画路 / 挪相机）之后留给重建与组件重注册的帧
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


def report():
    s = STATE["samples"]
    ok = True
    for name in sorted(s):
        z = zero_ratio(s[name])
        unreal.log("ROCKSHELL PIXELS %-16s zero=%.3f%%" % (name, z * 100.0))
        # 坑 ⑨ 的门：预热到位之后没有哪一张该剩下大片精确零。0.5% 与
        # `TinyGladeShotSofteningStats.py` 的 `ZERO_FAIL` 同源（那边实测 16 次起就是 0.000%）。
        # 近景那张走 `CREVICE_ZERO_FAIL`（裂缝槽底的几何真黑，见常量处注释）。
        if z > CREVICE_ZERO_FAIL.get(name, ZERO_FAIL):
            unreal.log_error("ROCKSHELL !! %s 有 %.3f%% 的像素精确 (0,0,0) —— Lumen 预热没起作用（坑 ⑨）"
                             % (name, z * 100.0))
            ok = False
    if "wall_shell_off" in s and "wall_shell_on" in s:
        ratio, changed = diff_ratio(s["wall_shell_off"], s["wall_shell_on"])
        unreal.log("ROCKSHELL PIXELS wall on-vs-off: %d/%d changed (%.1f%%)"
                   % (changed, GRID_X * GRID_Y, ratio * 100.0))
        # A：同机位只切 bRockShell，画面必须真的变。
        # ⚠️ **阈值随管线修复（坑 ⑧+⑨）重新标定过**：修之前没被直射的表面在两张图里
        # **都**精确 (0,0,0)，`diff_ratio` 把它们读成"没变"，差异率被系统性低估到 54%；
        # 预热到位之后实测 **57.9%**（750/1296）。
        # 标定依据：两次独立的编辑器启动给出**逐位相同**的 750/1296 ⇒ 这条判据的运行间
        # 噪声为 0（⚠️ 别把这句推广到所有脚本 —— 裙边噪声那条摆到 12%，见那边的注释）。
        # 阈值只需要为"合法的观感调整"留余量。取实测值的**一半**（29%）：
        # 它的物理含义是"岩壳在屏幕上的覆盖面至少还剩一半"。旧的 3% 在岩壳掉了 95% 时
        # 照样绿，等于没有门。
        if ratio < 0.29:
            unreal.log_error("ROCKSHELL !! 切换 bRockShell 几乎没有改变画面 —— 岩壳没有被画出来")
            ok = False
    if "road_before" in s and "road_after" in s:
        ratio, changed = diff_ratio(s["road_before"], s["road_after"])
        unreal.log("ROCKSHELL PIXELS road before-vs-after: %d/%d changed (%.1f%%)"
                   % (changed, GRID_X * GRID_Y, ratio * 100.0))
        # C：画路之后画面必须变（石头沉下去 + 地面换成土路材质），但**不该整片消失**。
        # 同上重新标定：实测 **5.5%**（71/1296），两次独立启动逐位相同。取一半 = 2.75%。
        # 路只占这张 3/4 侧视图的一条窄带，所以这个数天然比 A 小一个量级 ——
        # 不能拿 A 的阈值套过来，也不该为了好看往上抬。
        if ratio < 0.0275:
            unreal.log_error("ROCKSHELL !! 画路前后画面几乎没变 —— 下沉没有发生")
            ok = False
    unreal.log("ROCKSHELL PIXEL VERDICT %s" % ("OK" if ok else "FAILED"))


def tick(delta):
    STATE["ticks"] += 1
    if STATE["ticks"] < FIRST_TICK:
        return
    step = STATE["step"]
    if step < len(STATE["plan"]):
        name, action, (loc, rot) = STATE["plan"][step]
        phase, STATE["phase"] = STATE["phase"], STATE["phase"] + 1
        if phase == 0:
            action()
            STATE["spawned"][0].set_actor_location_and_rotation(loc, rot, False, False)
        elif phase >= SETTLE:
            # 坑 ⑨：一帧只打一次 capture_scene()，Lumen 的帧间历史才会真的往前走。
            # 挪过相机 / 切过开关之后历史等于清零，所以**每个机位都要重新预热满**。
            STATE["comp"].capture_scene()
            if phase >= SETTLE + WARMUP_CAPTURES - 1:
                unreal.RenderingLibrary.export_render_target(
                    STATE["world"], STATE["rt"], OUT_DIR, "rockshell_%s_%s.png" % (TAG, name))
                STATE["samples"][name] = sample_pixels()
                unreal.log("ROCKSHELL shot: rockshell_%s_%s.png（预热 %d 帧）"
                           % (TAG, name, WARMUP_CAPTURES))
                STATE["step"], STATE["phase"] = step + 1, 0
        return

    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    report()
    if STATE["ground"]:
        STATE["ground"].reset_paint()
    for a in STATE["spawned"]:
        ACTORS.destroy_actor(a)
    unreal.log("ROCKSHELL DONE tag=%s" % TAG)
    unreal.SystemLibrary.quit_editor()


# 坑 ⑥：准备段整个包在 try 里 —— 抛出去的话编辑器永远不退出，任务看着像挂死。
try:
    STATE["world"] = build()
except Exception:
    import traceback
    unreal.log_error("ROCKSHELL FAILED in build():\n%s" % traceback.format_exc())
    STATE["world"] = None

if STATE["world"] is None:
    unreal.SystemLibrary.quit_editor()
else:
    STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
