# -*- coding: utf-8 -*-
"""
给两张演示关卡补上 PostProcessVolume（无限范围）+ ExponentialHeightFog，并把太阳的
半影宽度调到 TG 的量级。

**为什么需要这个脚本**：`Docs/TinyGlade/Progress_ModuleComparison.md（卷四）` §7.2 实测 —— 两张演示关卡
零 PostProcessVolume、零 ExponentialHeightFog，跑的全是 UE 出厂默认（默认色调曲线、
默认 bloom、默认暗角、零 grading）。同一批 albedo 丢进出厂默认管线不会得到 TG 那张图，
差的不是美术资产，是光照与后处理（同文 §5 归因表：≈60% 光照算法 + 25% 后处理 + 15% 资产）。

**这里逼近的是 TG "柔和"的三个来源**（前两条是反编译 GLSL 实证，第三条是推断，见对照文档 §5）：

  ① 三级联 PCSS 软阴影（§3.2）—— penumbra 随遮挡体距离放大。
     ⚠️ **VSM 被拍板关掉之后，UE 侧这一条今天是断的**：`LightSourceAngle` 只被
     `r.Shadow.FilterMethod=1`（PCSS，引擎自标 experimental）那条分支读，默认的 Uniform PCF
     一个字都不读它。值仍然设着（PCSS 一开就生效），但**开不开是全工程渲染决策，本脚本不碰**。
  ② **AO 被硬夹在 `mix(0.6, 1.0, ao)`（§3.3）—— 阴影区永远压不黑。**
     ⚠️ 对照文档给的两个 UE 对位物本轮核下来都不成立（详见 `setup_ppv` 里 ② 那段的实证）。
     真正同义的是 **`LumenSkylightLeaking`**（引擎原话："keep indoor areas from going fully
     black"）+ **天光的下半球纯色**，本脚本的重点因此挪到了这两处。
  ③ 彩色 L1 间接光被 `indirect_light_tint` 上色、饱和度 ×1.17、增益 ×1.2（§3.4）。
     UE 没有"逐 RGB 通道各一条 L1 方向"，最接近的一手是**天光下半球色 = 地面草色**
     （见 `setup_skylight`）。

**刻意不做的事**：不堆 bloom / 色差 / 暗角。TG 确实有这三样（§4 ①②④），但它的柔和不是
滤镜给的；靠滤镜"像"会把画面糊掉却仍然缺影子。本脚本反而把 UE 出厂那圈暗角**关掉**。
局部曝光（`r.DefaultFeature.LocalExposure.*`，本工程已设 0.8）同样不动 —— TG 里没有对位物。

**曝光：本轮改口了**。原先这里写"留给引擎默认"，理由是"猜一个绝对 EV 猜错就是全黑或全白"。
改前的像素实测把这条理由推翻了：五个机位**一个过曝像素都没有**（p99 只有 0.35~0.61），
是整体欠曝，不是没法定标；而 EV 有闭式解（见 `setup_ppv` ⓪）。直方图自适应还有两个硬伤：
每个机位曝到不同 EV ⇒ 同机位对照失效；抬完天光它自动降回去 ⇒ 抵消本轮全部改动。
TG 那条"阴影均值驱动曝光"（§4③）UE 确实没有对位物、也确实不硬造，但**"固定曝光"这一半
本来就该抄**。

用法（两张关卡都会被改写并保存）：

  UnrealEditor-Cmd.exe <project> \\
      -ExecutePythonScript=".../TinyGladeSetupLighting.py" -unattended -nopause -nosplash

按日志断言判定：末行 `LIGHTING OK` / `LIGHTING FAILED`。
"""
import unreal

PKG = "/PCGPlugins/HouseTest"
LEVELS = ["%s/L_HouseGroundDemo" % PKG, "%s/L_TerrainOpsDemo" % PKG]

# 演示关卡里认它们作"我们放的那一份"，重复跑幂等（不会每跑一次多长一个体积）。
PPV_LABEL = "TG_PostProcess"
FOG_LABEL = "TG_HeightFog"

FAILS = []
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def setp(obj, name, value, why):
    """设一个属性并把"它对位 TG 的哪一条"打进日志。

    ⚠️ UE Python 里属性名拼错是**抛异常**不是静默忽略，而一个体积里几十个属性只要中间挂一个
       后面全都不会被设 —— 症状是"脚本跑完了但只有前几项生效"。这里逐个包起来，
       末尾统一报，一次跑就能把所有拼错的名字全找出来。
    """
    try:
        obj.set_editor_property(name, value)
        unreal.log("      %-46s = %-30s | %s" % (name, value, why))
    except Exception as exc:
        FAILS.append("%s.%s (%s)" % (type(obj).__name__, name, exc))
        unreal.log_error("      SET FAILED %s: %s" % (name, exc))


def find_by_label(label):
    return next((a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == label), None)


def ensure(cls, label, location):
    """按标签找回上一次跑留下的那一个；没有才新建。"""
    existing = find_by_label(label)
    if existing:
        unreal.log("    reuse %s" % label)
        return existing
    actor = ACTORS.spawn_actor_from_class(cls, location, unreal.Rotator(roll=0.0, pitch=0.0, yaw=0.0))
    actor.set_actor_label(label)
    unreal.log("    spawned %s" % label)
    return actor


# -----------------------------------------------------------------------------
# PostProcessVolume
# -----------------------------------------------------------------------------
def setup_ppv():
    ppv = ensure(unreal.PostProcessVolume, PPV_LABEL, unreal.Vector(0.0, 0.0, 0.0))
    # 无限范围：演示关卡里相机会跑到很远（TerrainOpsDemo 的全景机位在 1500+ cm 外），
    # 带边界的体积在那里直接失效，症状是"近处调好了、退一步全变回默认"。
    setp(ppv, "unbound", True, "无限范围 —— 演示关卡没有玩家出生点，相机哪都去")
    setp(ppv, "enabled", True, "")
    setp(ppv, "blend_weight", 1.0, "")
    setp(ppv, "priority", 0.0, "")

    pp = ppv.get_editor_property("settings")

    # ---- ⓪ 曝光：钉死，不用直方图自适应 ----
    #
    # 为什么改口（原先这里刻意"留给引擎默认"）：改前实测五个机位全部欠曝 ——
    # 全图均值 0.10~0.18、p99 只有 0.35~0.61（**一个像素都没有过曝**），而暗部占比高到 47%。
    # 直方图自适应是"跟着画面内容跑"的，于是两件事同时发生：① 每个机位曝到不同的 EV，
    # 改前/改后的同机位对照失去意义；② 抬完天光它会自动把曝光降回去，抵消掉这一轮的改动。
    #
    # 对位 TG（§4③）：TG 的曝光**不是**直方图自适应，是一个固定值再乘
    # `mix(1.0, 0.57, smoothstep(0.3, 1, shadow_mask_mean))`。UE 没有"阴影均值驱动曝光"的
    # 对位物（硬造不如不造），但"固定曝光"这一半是可以照抄的，而且比自适应更接近 TG。
    #
    # ⚠️ 单位：`r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange=True`（本工程已开）
    #    把 Min/MaxBrightness 从 cd/m² 换成 **EV100**（PostProcessEyeAdaptation.cpp:530-534）。
    #    Min == Max 时引擎按 Manual 处理、立即锁定（同文件 :606 注释原文
    #    "if we don't have a valid range ... then force it like Manual"）。
    #
    # 取值是**实测定标**的，不是拍脑袋（纯解析算只能给个起点）：
    #   · 解析起点：LuminanceMax = 0.78/LensAttenuation(0.78) = 1 ⇒ 场景亮度 L 送进色调映射器
    #     的值就是 `L / 2^EV`。太阳 6 lux、pitch −42° ⇒ 水平踏面 L = 6·sin42°·0.5/π ≈ 0.64。
    #   · 定标：以**改前那套直方图自适应实际曝到的亮度为基准**（它虽然不可复现，
    #     但亮度档本身是合理的）：EV = −1.0 拍出来比它暗一档多（全景机位 p50 0.242
    #     vs 0.435），再降一档到 **−2.0** 刚好对齐（p50 0.441、p90 0.511），而两张石阶
    #     机位的 p99 仍然只到 0.86、过曝像素占比 ≤ 0.01%。
    #     钉在这一档的好处：后面抬阴影底的改动就**不会被自适应吃掉**，
    #     改前/改后两张图的总亮度又恰好相等 ⇒ 像素判据量的是光照，不是曝光。
    #   · 为什么不改灯强而改曝光：曝光一钉死，太阳绝对值就只剩“太阳 : 天光”一个自由度了。
    #     两边一起抬等于什么都没发生，只会把数字搞得不可复现。
    #   ⚠️ 定标前先踩了一个坑：离屏 SceneCapture 默认没有 ViewState ⇒ 眼适应整条链不跑，
    #     PPV 里的曝光怎么改都一模一样（详情与修法写在
    #     `TinyGladeShotSoftening.py` 头注释的坑 ⑧）。不先修那一条，这里怎么定标都是假的。
    setp(pp, "override_auto_exposure_bias", True, "")
    setp(pp, "auto_exposure_bias", 0.0, "锚点全部交给下面的 EV100，别再叠一层偏置")
    setp(pp, "override_auto_exposure_min_brightness", True, "")
    setp(pp, "auto_exposure_min_brightness", -2.0, "EV100。Min == Max ⇒ 锁死曝光（见上面的依据）")
    setp(pp, "override_auto_exposure_max_brightness", True, "")
    setp(pp, "auto_exposure_max_brightness", -2.0, "同上，必须与 Min 逐位相等")

    # ---- ② AO 的最暗值抬起来（TG `mix(0.6, 1.0, ao)`，对照文档 §3.3）----
    #
    # ⚠️ **对照文档 §3.3 给的两个对位物，在引擎里逐行核对下来都不成立**（本轮实证，见交付说明）：
    #   · `r.AmbientOcclusion.Intensity` 这个 CVar **在 5.7 里根本不存在**（全树 0 命中）。
    #   · `AmbientOcclusionIntensity` 的语义是 "how much it affects the **non direct** lighting
    #     after base pass"（Scene.h:2118），而 TG 夹的是**直接光**（`sun *= mix(0.6,1,ao)`）。
    #     更要命的是本工程走 Lumen 屏幕探针：`DiffuseIndirectComposite.usf:381-386` 那段
    #     `FinalAmbientOcclusion` 被 `#if DIM_APPLY_DIFFUSE_INDIRECT != ..._SCREEN_PROBE_GATHER`
    #     整个跳过，AO 改从 `ShortRangeAOTexture` 走（:182-195）⇒ **这三行在主路径上是空操作**。
    # 留着它们，是因为离屏 SceneCapture 那条路可能落在非 Lumen 分支上（引擎写死关 Lumen），
    # 那时它们才是唯一起作用的一档。**别再把它们当成 TG 那条夹子的对位物。**
    setp(pp, "override_ambient_occlusion_intensity", True, "")
    setp(pp, "ambient_occlusion_intensity", 0.4, "仅非 Lumen 回退路：1-0.4 = 间接光最暗 0.6")
    setp(pp, "override_ambient_occlusion_power", True, "")
    setp(pp, "ambient_occlusion_power", 1.0, "出厂 2.0 是把暗处再压一次幂，与 TG「压不黑」相反")
    setp(pp, "override_ambient_occlusion_radius", True, "")
    setp(pp, "ambient_occlusion_radius", 200.0, "cm。房子墙高 ~600，取 1/3 让墙根有一圈软暗")
    setp(pp, "override_ambient_occlusion_static_fraction", True, "")
    setp(pp, "ambient_occlusion_static_fraction", 0.0, "本工程 r.AllowStaticLighting=False，没有静态光可压")

    # **这三条才是 TG「阴影区永远压不黑」在 UE 侧真正的对位物。**
    # 引擎自己的措辞就是这个意思（Scene.h:1752）：`LumenSkylightLeaking` 是
    # "an art direction knob (non-physically based) to keep indoor areas from going fully black"
    # —— 一个无条件的天光底，与 TG 把 AO 夹在 0.6 是同一件事、同一个动机。
    # 0.12 → 0.20 的依据是改前的像素测量：背光面机位 47.5% 的像素在近黑档、最暗 20% 的
    # 平均饱和度 **0.000**（纯黑连色相都没有）。0.12 显然不够。
    setp(pp, "override_lumen_skylight_leaking", True, "")
    setp(pp, "lumen_skylight_leaking", 0.20, "TG §3.3 的下限：20% 天光无条件漏进被遮挡处")
    # TG 的下限是**一个常数** 0.6，与距离无关。UE 这条按距离渐入（Scene.h:1760 原文
    # "Smaller values make the skylight leaking flatter, while larger values create an
    # Ambient Occlusion effect"）—— 要对位 TG 就得取**小**值，让它平，而不是又变成一层 AO。
    setp(pp, "override_lumen_full_skylight_leaking_distance", True, "")
    setp(pp, "lumen_full_skylight_leaking_distance", 50.0, "cm，取小 = 平底，对位 TG 的常数 0.6")
    # 漏进来的那份天光也上色 —— 对位 §3.4「背光面吃到地面草色」的那一半（另一半在天光的
    # 下半球色，见 setup_skylight）。
    setp(pp, "override_lumen_skylight_leaking_tint", True, "")
    setp(pp, "lumen_skylight_leaking_tint", unreal.LinearColor(0.86, 1.0, 0.72, 1.0),
         "TG §3.4：漏光带草地的绿，阴影区才「有颜色」而不是「有灰」")

    # ---- ③ 间接光染色 + 增益（TG `indirect_light_tint` / 饱和 ×1.17 / 增益 ×1.2，§3.4）----
    # TG 的机制是"每个 RGB 通道各带一条 L1 方向"，所以背光面会吃到地面草色的偏移；
    # UE 侧没有对位物，能做的是把弹射光整体往暖绿推一点 + 提增益，让阴影区**有颜色**而不是灰。
    setp(pp, "override_indirect_lighting_color", True, "")
    setp(pp, "indirect_lighting_color", unreal.LinearColor(1.0, 0.97, 0.86, 1.0),
         "TG §3.4 `indirect_light_tint` —— 暖绿偏移，对位草地弹射")
    setp(pp, "override_indirect_lighting_intensity", True, "")
    setp(pp, "indirect_lighting_intensity", 1.2, "TG §3.4 的增益 ×1.2，逐字对位")
    setp(pp, "override_lumen_diffuse_color_boost", True, "")
    setp(pp, "lumen_diffuse_color_boost", 1.15,
         "TG §3.4 的饱和度 ×1.17 —— UE 没有「GI 饱和度」旋钮，这条走 pow(DiffuseColor,1/boost)，方向一致")

    # ---- ④ Tony McMapface：**UE 5.7 没有对位物，这里只做局部近似，别当成移植** ----
    #
    # 本轮把引擎全树核了一遍：`AgX` / `Tony` / `McMapface` 在 `Engine/Shaders` 与
    # `Engine/Source/Runtime` 里**零命中**；唯一的 filmic 曲线是 ACES 系的 `FilmToneMap`
    # （`TonemapCommon.ush:105`），"Uncharted / HP / Legacy / ACES" 那几档只是**同一条曲线的
    # 参数预设**（同文件 :68-98），不是另一条变换。`r.TonemapperOutputDevice` /
    # `r.TonemapperFilm` 都不存在（`r.Tonemapper.*` 只有 `Sharpen` 一个）；`OutputDevice`
    # 是 sRGB/Rec709/PQ 这类**显示编码**选择器，不是 look。OCIO 是插件里的
    # `FSceneViewExtension`，`Scene.h` / `PostProcessVolume.h` 对它零引用 ⇒ 不是 PPV 属性。
    # ⇒ **结论：没有。** 真要 Tony 只能烘一张 33³ 的 `ColorGradingLUT` 塞进 PPV，
    #    那是引进一个外部资产，超出本轮范围，只报告不做。
    #
    # 能诚实照抄的只有 Tony 的**一个**性质——"高光去饱和向白"——UE 有一档一等旋钮：
    # 分段色彩校正的高光饱和度。这不是同一个变换，只是同方向的一小步，注释里说清楚。
    setp(pp, "override_expand_gamut", True, "")
    setp(pp, "expand_gamut", 0.0, "TG §4⑥ Tony 保色相；UE 出厂 ExpandGamut=1 把高饱和往色域外推，正好反向")
    setp(pp, "override_color_saturation_highlights", True, "")
    setp(pp, "color_saturation_highlights", unreal.Vector4(0.88, 0.88, 0.88, 1.0),
         "Tony「高光去饱和向白」的**部分**对位；不是同一条变换，只是同方向")
    setp(pp, "override_color_correction_highlights_min", True, "")
    setp(pp, "color_correction_highlights_min", 0.45, "上面那档从哪儿开始算高光")

    # ---- 刻意压掉的廉价效果 ----
    # TG 确实有 bloom（§4②，但只有 3% 且极宽）与暗角（§4④）。堆它们不会带来 TG 的柔和，
    # 只会盖住"没有影子"这个真问题。这里只把 UE 出厂那圈明显的辉光/暗角收掉。
    setp(pp, "override_bloom_intensity", True, "")
    setp(pp, "bloom_intensity", 0.2, "TG §4② 是 3% 且极宽；这里只是把出厂 0.675 那圈收掉")
    setp(pp, "override_vignette_intensity", True, "")
    setp(pp, "vignette_intensity", 0.0, "出厂 0.4。柔和不从暗角来，关掉免得混淆判读")
    setp(pp, "override_scene_fringe_intensity", True, "")
    setp(pp, "scene_fringe_intensity", 0.0, "同上，色差不参与")

    ppv.set_editor_property("settings", pp)
    return ppv


# -----------------------------------------------------------------------------
# ExponentialHeightFog
# -----------------------------------------------------------------------------
def setup_fog():
    fog = ensure(unreal.ExponentialHeightFog, FOG_LABEL, unreal.Vector(0.0, 0.0, 0.0))
    comp = fog.get_editor_property("component")

    # 演示关卡最远的机位在 ~2500 cm 外，地面 32 m 见方。密度按这个尺度取：近处（门拱、石阶）
    # 一点都不糊，远景才开始淡下去。密度再大一档就会把整张图罩上一层灰，那不是 TG 的样子 ——
    # TG 的远景是**景深**糊掉的（§4，12 个 dof shader），不是雾罩的。
    setp(comp, "fog_density", 0.008, "出厂 0.02。演示关卡才 32 m，出厂密度会把全景罩灰")
    setp(comp, "fog_height_falloff", 0.15, "越小过渡越高越软")
    setp(comp, "start_distance", 1500.0, "cm —— 近处零雾，门拱/石阶的判读不受影响")
    setp(comp, "fog_max_opacity", 0.9, "留一点点通透，远山不糊成纯色")
    setp(comp, "directional_inscattering_exponent", 16.0, "对着太阳那圈暖光的收束")
    # ⚠️ 属性名是 bEnableVolumetricFog（ExponentialHeightFogComponent.h:128），不是 "volumetric_fog" ——
    #    后者在 UE 5.7 里抛 "Failed to find property"，而不是静默忽略。
    setp(comp, "enable_volumetric_fog", False,
         "TG 的 `_apply_volumetrics` 是另一条 pass，这里不抄：体积雾会吃掉 SceneCapture 的可判读性")
    return fog


# -----------------------------------------------------------------------------
# ① 半影：平行光的 LightSourceAngle
# -----------------------------------------------------------------------------
def setup_sun():
    suns = [a for a in ACTORS.get_all_level_actors() if isinstance(a, unreal.DirectionalLight)]
    if not suns:
        unreal.log_warning("    no DirectionalLight in this level; skipping the penumbra knob")
        return None
    for sun in suns:
        comp = sun.get_editor_property("directional_light_component")
        unreal.log("    sun '%s'" % sun.get_actor_label())
        # ⚠️ **VSM 关掉之后这一档已经不起作用了，本轮实证**（原注释写的是 VSM 的 SMRT）：
        #    `r.Shadow.FilterMethod` 默认 0 = Uniform PCF，`LightSourceAngle` 只在
        #    `ShadowRendering.cpp:546` 那条 `== 1`（PCSS）分支里被读
        #    （`ShadowRendering.h:1735` 是全项目唯一的 CSM 侧读点），其余读点是 Lumen 直接光 /
        #    距离场阴影 / 胶囊阴影 / 光追降噪，本工程一条都没走。
        #    ⇒ 值留着（PCSS 一开就立刻生效，且它是 TG §3.2 三级联 PCSS 的**精确**对位物），
        #      但把 `r.Shadow.FilterMethod=1` 写进 `DefaultEngine.ini` 是**全工程渲染决策**，
        #      与刚被拍板的 VSM 同一档，agent 不擅自改 —— 只报告。
        setp(comp, "light_source_angle", 1.75,
             "TG §3.2 PCSS 的对位物；⚠️ 需 r.Shadow.FilterMethod=1 才生效（未擅自开）")
        setp(comp, "cast_shadows", True, "没有影子的话上面几条全都看不出来")
        # ⬇ **曝光钉死之后，两张关卡的太阳必须同一个强度**：一个 EV100 不可能同时对上
        #    两个不同亮度的场景，否则一张关卡正常、另一张全黑或全白，
        #    而两者都“脚本跑成功了”。 6 lux 就是 setup_ppv ⑩ 那条 EV 反算所依据的值。
        setp(comp, "intensity", 6.0, "lux。两张关卡必须同值 —— 曝光已钉死，一个 EV 要同时对两边")
        # 太阳角度与强度（本轮判断，依据写在这里）：
        #   · **pitch / yaw 不动**（−42° / 140°）。−42° 让明暗终结线正好扫过土台的球面，
        #     形体读得最清楚，投影长度约 1.1 倍物高 —— 再低会把半个场景推进阴影里，
        #     再高影子短到读不出体积。而且改前/改后要同机位判读，机位是从 yaw 反算的，
        #     动 yaw 等于把两组图变成不同机位，像素判据当场作废。
        #   · **强度也不动**（6 lux）。曝光已经钉死（见 setup_ppv ⓪），太阳的绝对值就只剩
        #     "太阳 : 天光"这一个自由度了，而本轮要压的正是这个比 —— 所以钱花在**抬天光**上
        #     （setup_skylight），不是抬太阳。抬太阳只会把两边一起抬、比值不变。
    return suns[0]


# -----------------------------------------------------------------------------
# ③ 天光：阴影区的**底**与**颜色**都在这里
# -----------------------------------------------------------------------------
def setup_skylight():
    """TG §3.4「彩色 L1 间接光」在 UE 侧最实的一手。

    TG 的机制是**每个 RGB 通道各带一条 L1 方向**，所以墙的背光面会吃到地面草色的偏移方向 ——
    这正是它的阴影区"有颜色"而不是"有灰"的机械原因。UE 的天光只有一份 SH，没有逐通道方向，
    但它有一个**下半球纯色**开关，语义恰好就是"从地面方向来的那一份环境光是什么颜色"。

    ⚠️ 改前实测：两张关卡的天光都是 `bLowerHemisphereIsBlack = true` +
       `LowerHemisphereColor = (0,0,0)`（引擎出厂默认，`SkyLightComponent.cpp:306/314`）——
       **凡是朝下、朝侧下的方向，环境光贡献精确为零**。这就是"背光面纯黑"最后那一半根因
       （前一半是 `real_time_capture=False`，已在 `TinyGladeMakeStoneMaterial.py` 里修掉）。
       改前背光面机位最暗 20% 像素的平均饱和度是 **0.000** —— 连色相都没有，正是这条的指纹。

    ⚠️ 属性名有坑：`bLowerHemisphereIsBlack` 的 DisplayName 是 "Lower Hemisphere Is **Solid
       Color**"，它不是"要不要黑"，是"下半球要不要换成 LowerHemisphereColor"。所以**保持 True**、
       只改颜色，才是"给下半球一份草色"；关掉它反而会去用实时捕获里那片本来就没内容的下半球。
       实时捕获下同样生效：`ReflectionEnvironmentRealTimeCapture.cpp:970-987` 有一趟专门的
       `FApplyLowerHemisphereColorPS` 把它盖上去。
    """
    skies = [a for a in ACTORS.get_all_level_actors() if isinstance(a, unreal.SkyLight)]
    if not skies:
        unreal.log_warning("    no SkyLight in this level; the shadow floor will stay at zero")
        return None
    for sky in skies:
        comp = sky.light_component
        unreal.log("    skylight '%s'" % sky.get_actor_label())
        # 实时捕获：地形与房子一直在变，静态立方图从没捕获过就是一张黑图（今天踩过）。
        setp(comp, "real_time_capture", True, "静态立方图没捕获过 = 一张黑图，Details 面板还看不出来")
        # 太阳 6 lux 对天光 1.0 ⇒ 明暗比太硬。TG 的天空是**被刻意提亮提饱和**过的
        # （§3.8：`sky_low_color × 1.5`，再 `mix(lum, sky, 1.25)`），它的阴影区亮度就是这么来的。
        setp(comp, "intensity", 2.6, "TG §3.8 天空是 ×1.5 + 饱和 ×1.25；抬的是「太阳:天光」这个比")
        setp(comp, "lower_hemisphere_is_black", True,
             "字面是「下半球用纯色」不是「下半球是黑的」—— 要留 True 才轮得到下面这行")
        setp(comp, "lower_hemisphere_color", unreal.LinearColor(0.10, 0.14, 0.055, 1.0),
             "TG §3.4：从地面方向来的那份环境光 = 草色。这是背光面「有颜色」的来源")
    return skies[0]


# -----------------------------------------------------------------------------
for level in LEVELS:
    unreal.log("========== %s ==========" % level)
    unreal.EditorLoadingAndSavingUtils.load_map(level)
    setup_ppv()
    setup_fog()
    setup_sun()
    setup_skylight()
    unreal.EditorLoadingAndSavingUtils.save_current_level()
    unreal.log("  saved")

if FAILS:
    for f in FAILS:
        unreal.log_error("FAIL %s" % f)
    unreal.log_error("LIGHTING FAILED (%d property/properties)" % len(FAILS))
else:
    unreal.log("LIGHTING OK")
