# -*- coding: utf-8 -*-
"""
地被（草 + 花）改走 Nanite，WPO 按相机距离在交界前淡出（2026-09-29 用户："让花草都设置为随着屏幕远离 WPO 降低，
直至交界处时彻底消失，并且 disable WPO，只留下法线变化。让这些内容均使用 nanite"；追问后定为「只在最后一段减弱」、
「花 Nanite + CSM 影子副本」）。

做三件事，**幂等**（重跑只补缺的，不重复加节点）：

 1. 四张网格开 Nanite：`SM_TG_GrassBlade` / `SM_TG_GrassClump` / `lowpoly_flower` / `garden_flower_01_lavender`。
    组件判据就是资产的 Nanite 开关（`UCSGpuInstancedMeshComponent::IsNaniteRenderPath`），不需要别的设置。
    花另把回退网格设成全精度、保留 Allow CPU Access：花的影子由一份**只投影**的经典实例代理画
    （`bClassicShadowOnNanitePath`，进 CSM 保住那份软度），它读的正是回退 LOD 的顶点。
 2. `M_TG_Grass`（草与三种花共用的母材质）：
    · WPO × 距离淡出 `Fade = 1 − saturate((|P − Cam| − Start) / (End − Start))`；Start / End 取 custom primitive data
      [0] / [1]（`ACSGroundActor` 按草的近 / 远交界与 `CoverWPOFadeLength` 写）。没写（End = 0）= 不淡出，
      别处直接挂这张材质的静态网格照旧。
    · 法线 = `normalize(N + WindDirection × (TGWindBend × NormalWindStrength × (1 − Fade)))` —— 与 `M_TG_GrassFar`
      的 `TG Far Normal Wind` 同一条式子：淡到交界时近处与远处的法线逐位相同，交界之外"只留下法线变化"。
    · `MaxWorldPositionOffsetDisplacement`（Nanite 按它放宽簇包围盒，不设的话风一吹边缘的簇会被错剔）+ Nanite 用途。
 3. `M_TG_GrassFar`：Nanite 用途（它本来就没有 WPO）。

⚠️ `M_TG_Grass` 用户手改过：改之前的原件在 PCGPlugins 的 git 里（`Content/` 是入库的），出了问题从那里取回，不另存副本。
⚠️ 在**用户开着的编辑器**里跑（MCPUnreal execute_script）：无头进程另开一份编辑器时，存包会与用户的编辑器抢文件锁。
"""
import unreal

ASSET = "/PCGPlugins/HouseTest/TinyGladeAsset"
MESH_DIR = "%s/Meshes" % ASSET
MAT_DIR = "%s/Materials" % ASSET

NEAR_MAT = "%s/M_TG_Grass" % MAT_DIR
FAR_MAT = "%s/M_TG_GrassFar" % MAT_DIR

# (网格, 回退网格要不要全精度) —— 花的影子副本读回退 LOD，所以花要全精度；草不投影，回退网格用默认。
MESHES = [
    ("%s/SM_TG_GrassBlade" % MESH_DIR, False),
    ("%s/SM_TG_GrassClump" % MESH_DIR, False),
    ("%s/lowpoly_flower" % MESH_DIR, True),
    ("%s/garden_flower_01_lavender" % MESH_DIR, True),
]

# 与 M_TG_GrassFar 的默认值相同（TinyGladeMakeGrassClump.py 的 NORMAL_WIND_STRENGTH）：交界两侧法线一致。
NORMAL_WIND_STRENGTH = 0.35
# Nanite 的 WPO 包围盒余量（cm）。草的风位移常数 10 cm、再乘弯曲幅度（≤ 2）与高度，50–60 cm 的叶子顶多挪 30 cm 出头。
MAX_WPO_DISPLACEMENT = 60.0
# custom primitive data 下标：与 ACSGroundActor::CoverWPOFadeStartIndex / CoverWPOFadeEndIndex 一致。
CPD_FADE_START = 0
CPD_FADE_END = 1

FADE_DESC = "TG WPO Distance Fade"
NEAR_NORMAL_DESC = "TG Near Normal Wind"

FADE_HLSL = """// 地被 WPO 按相机距离淡出（2026-09-29）：[S, E] 里线性淡到 0。
// E <= 0 = 组件没给 custom primitive data（不是地被、或旧关卡）⇒ 不淡出，照旧满幅。
if (E <= 0.0) return 1.0;
float Dist = length(P - C);
return 1.0 - saturate((Dist - S) / max(E - S, 1.0));
"""

NEAR_NORMAL_HLSL = """// 近处的法线风：WPO 淡出多少，法线就接过多少 —— 与 M_TG_GrassFar 的 TG Far Normal Wind 同一条式子，
// 淡到交界（F = 0）时两档逐位相同；贴着相机（F = 1）时与改动前一样，只有 WPO。
float3 Nw = N + D * (B * S * (1.0 - F));
return normalize(Nw);
"""

eal = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary
FAILED = []


def log(msg):
    unreal.log("COVERNANITE %s" % msg)


def fail(msg):
    FAILED.append(msg)
    unreal.log_error("COVERNANITE FAILED: %s" % msg)


# ---------------------------------------------------------------------------
# 1) 网格开 Nanite
# ---------------------------------------------------------------------------

def enable_nanite(path, full_fallback):
    mesh = eal.load_asset(path)
    if not mesh:
        fail("%s 不存在" % path)
        return
    settings = mesh.get_editor_property("nanite_settings")
    changed = False
    if not settings.get_editor_property("enabled"):
        settings.set_editor_property("enabled", True)
        changed = True
    if full_fallback:
        # 回退网格 = 影子副本画的那份：全精度（小网格本来就不减面，这里只是把意图写死）。
        for name, value in (("fallback_target", getattr(unreal.NaniteFallbackTarget, "RELATIVE_ERROR", None)),
                            ("fallback_relative_error", 0.0)):
            if value is None:
                continue
            try:
                if settings.get_editor_property(name) != value:
                    settings.set_editor_property(name, value)
                    changed = True
            except Exception as error:   # 属性名随引擎版本变过；缺了只是回退网格用默认精度
                unreal.log_warning("COVERNANITE %s: 设不了 %s（%s）" % (mesh.get_name(), name, error))
    if changed:
        # 走 set_editor_property ⇒ PostEditChangeProperty ⇒ 资产重建；地被组件听 OnPostMeshBuild 自己换路。
        mesh.set_editor_property("nanite_settings", settings)
        eal.save_loaded_asset(mesh)
    cpu = mesh.get_editor_property("allow_cpu_access")
    log("%-28s Nanite=%s 回退全精度=%s CPU 访问=%s%s" % (
        mesh.get_name(), mesh.get_editor_property("nanite_settings").get_editor_property("enabled"),
        full_fallback, cpu, "（已改并保存）" if changed else ""))
    if full_fallback and not cpu:
        # 编辑器里读得到回退 LOD，打包版读不到 ⇒ 影子副本没东西画（主通道不受影响）。
        unreal.log_warning("COVERNANITE %s 没开 Allow CPU Access：打包版里花的影子副本会读不到顶点" % mesh.get_name())


# ---------------------------------------------------------------------------
# 2) M_TG_Grass：WPO 距离淡出 + 近处法线风
# ---------------------------------------------------------------------------

def walk(mat, node, seen):
    if node is None or node in seen:
        return
    seen.append(node)
    for one in mel.get_inputs_for_material_expression(mat, node):
        walk(mat, one, seen)


def all_expressions(mat):
    """材质里现有的全部表达式（从每个输出属性往回走一遍，孤儿节点不算）。"""
    seen = []
    for prop in (unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET, unreal.MaterialProperty.MP_NORMAL,
                 unreal.MaterialProperty.MP_BASE_COLOR, unreal.MaterialProperty.MP_ROUGHNESS,
                 unreal.MaterialProperty.MP_OPACITY_MASK, unreal.MaterialProperty.MP_EMISSIVE_COLOR):
        walk(mat, mel.get_material_property_input_node(mat, prop), seen)
    return seen


def custom_by_desc(nodes, desc):
    for node in nodes:
        if node.get_class().get_name() == "MaterialExpressionCustom" and str(node.get_editor_property("description")) == desc:
            return node
    return None


def make_custom(mat, desc, code, inputs, output_type, x, y):
    custom = mel.create_material_expression(mat, unreal.MaterialExpressionCustom, x, y)
    custom.set_editor_property("description", desc)
    custom.set_editor_property("code", code)
    custom.set_editor_property("output_type", output_type)
    pins = []
    for name in inputs:
        pin = unreal.CustomInput()
        pin.set_editor_property("input_name", name)
        pins.append(pin)
    custom.set_editor_property("inputs", pins)
    return custom


def cpd_scalar(mat, name, index, x, y):
    param = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, x, y)
    param.set_editor_property("parameter_name", name)
    param.set_editor_property("default_value", 0.0)
    param.set_editor_property("group", "WPO Fade")
    param.set_editor_property("bUseCustomPrimitiveData", True)
    param.set_editor_property("PrimitiveDataIndex", index)
    return param


def vs_to_ps(mat, node, x, y):
    """把一个在顶点着色器里算的量经 VertexInterpolator 送到像素着色器。

    风场 `TG Wind Bend` 在花的分支（`HeightAsBladeT = True`）读 `PreSkinnedPosition`，那只在顶点着色器里有。
    直接接进 Normal（像素着色器）⇒ 花的两个 MI 编译失败、**静默换成默认材质**（2026-09-29 实测：日志
    "(Node PreSkinnedPosition) ... not available in the Pixel shader"，花整片灰白）；草走 UV 那一支所以没事，最难看出来。
    Nanite 5.7 支持顶点插值器（NaniteShading.cpp 的 bMaterialHasProgrammableVertexUVs）。
    """
    interp = mel.create_material_expression(mat, unreal.MaterialExpressionVertexInterpolator, x, y)
    if not mel.connect_material_expressions(node, "", interp, ""):   # 单输入节点的针脚名是空串
        fail("接不上 VertexInterpolator 的输入")
    return interp


def repair_bend_interpolator(mat, nodes):
    """旧版本脚本建出来的图：`TG Near Normal Wind` 的 B 直接接风场。补一个 VertexInterpolator（幂等）。"""
    near = custom_by_desc(nodes, NEAR_NORMAL_DESC)
    if near is None:
        return
    inputs = mel.get_inputs_for_material_expression(mat, near)   # 与 Custom 节点的输入同序：N, D, B, S, F
    if len(inputs) < 3 or inputs[2] is None:
        fail("%s 的 B 输入是空的" % NEAR_NORMAL_DESC)
        return
    if inputs[2].get_class().get_name() == "MaterialExpressionVertexInterpolator":
        log("%s 的 B 已经经过 VertexInterpolator" % NEAR_NORMAL_DESC)
        return
    connect(vs_to_ps(mat, inputs[2], -500, 1000), near, "B")
    log("修：%s 的 B 改经 VertexInterpolator（风场在顶点着色器里算，像素着色器读插值）" % NEAR_NORMAL_DESC)


def connect(src, dst, pin):
    if not mel.connect_material_expressions(src, "", dst, pin):
        fail("接不上 %s 的输入 %s" % (dst.get_editor_property("description") if hasattr(dst, "get_editor_property") else dst, pin))


def set_usage(mat, usage_name, flag):
    usage = getattr(unreal.MaterialUsage, usage_name, None)
    if usage is None:
        fail("这个引擎的 Python 里没有 MaterialUsage.%s" % usage_name)
        return
    mel.set_material_usage(mat, usage)   # Python 侧只回一个 bool（C++ 的 bNeedsRecompile 出参没带出来），回读才是真话
    if not mat.get_editor_property(flag):
        fail("%s 的 %s 没勾上" % (mat.get_name(), flag))


def patch_near_material():
    mat = eal.load_asset(NEAR_MAT)
    if not mat:
        fail("%s 不存在" % NEAR_MAT)
        return

    nodes = all_expressions(mat)
    if custom_by_desc(nodes, FADE_DESC):
        log("%s 已有 %s —— 图不再改（幂等）" % (mat.get_name(), FADE_DESC))
    else:
        wpo_top = mel.get_material_property_input_node(mat, unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
        normal_top = mel.get_material_property_input_node(mat, unreal.MaterialProperty.MP_NORMAL)
        if wpo_top is None or normal_top is None:
            fail("M_TG_Grass 的 WPO 或 Normal 是空的 —— 结构变了，脚本要跟着改")
            return
        subtree = []
        walk(mat, wpo_top, subtree)
        bends = [n for n in subtree if n.get_class().get_name() == "MaterialExpressionCustom"
                 and "wind" in str(n.get_editor_property("description")).lower()]
        dirs = [n for n in subtree if n.get_class().get_name() == "MaterialExpressionVectorParameter"
                and str(n.get_editor_property("parameter_name")).lower() == "winddirection"]
        if not bends or not dirs:
            fail("WPO 子树里没找到风场（TG Wind Bend）/ 风向（WindDirection）")
            return
        bend, wind_dir = bends[0], dirs[0]
        log("WPO 子树 %d 个节点；风场 %s，风向 %s；Normal 顶节点 %s"
            % (len(subtree), bend.get_editor_property("description"), wind_dir.get_editor_property("parameter_name"),
               normal_top.get_class().get_name()))

        start = cpd_scalar(mat, "WPOFadeStart", CPD_FADE_START, -900, 900)
        end = cpd_scalar(mat, "WPOFadeEnd", CPD_FADE_END, -900, 1000)
        world_pos = mel.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, -900, 700)
        try:
            world_pos.set_editor_property("world_position_shader_offset",
                                          unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
        except Exception as error:
            unreal.log_warning("COVERNANITE WorldPosition 设不了排除偏移（%s），用默认" % error)
        camera = mel.create_material_expression(mat, unreal.MaterialExpressionCameraPositionWS, -900, 800)

        fade = make_custom(mat, FADE_DESC, FADE_HLSL, ("P", "C", "S", "E"), unreal.CustomMaterialOutputType.CMOT_FLOAT1, -650, 800)
        for src, pin in ((world_pos, "P"), (camera, "C"), (start, "S"), (end, "E")):
            connect(src, fade, pin)

        faded = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -350, 600)
        connect(wpo_top, faded, "A")
        connect(fade, faded, "B")
        if not mel.connect_material_property(faded, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET):
            fail("接不上 WPO")

        strength = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -650, 1100)
        strength.set_editor_property("parameter_name", "NormalWindStrength")
        strength.set_editor_property("default_value", NORMAL_WIND_STRENGTH)
        strength.set_editor_property("group", "Wind")

        near_normal = make_custom(mat, NEAR_NORMAL_DESC, NEAR_NORMAL_HLSL, ("N", "D", "B", "S", "F"),
                                  unreal.CustomMaterialOutputType.CMOT_FLOAT3, -350, 900)
        for src, pin in ((normal_top, "N"), (wind_dir, "D"), (vs_to_ps(mat, bend, -500, 1000), "B"), (strength, "S"), (fade, "F")):
            connect(src, near_normal, pin)
        if not mel.connect_material_property(near_normal, "", unreal.MaterialProperty.MP_NORMAL):
            fail("接不上 Normal")
        log("图已改：WPO × %s、Normal ← %s" % (FADE_DESC, NEAR_NORMAL_DESC))
    repair_bend_interpolator(mat, all_expressions(mat))

    if abs(mat.get_editor_property("max_world_position_offset_displacement") - MAX_WPO_DISPLACEMENT) > 1e-3:
        mat.set_editor_property("max_world_position_offset_displacement", MAX_WPO_DISPLACEMENT)
    set_usage(mat, "MATUSAGE_INSTANCED_STATIC_MESHES", "bUsedWithInstancedStaticMeshes")
    set_usage(mat, "MATUSAGE_NANITE", "bUsedWithNanite")
    mel.recompile_material(mat)
    eal.save_loaded_asset(mat)
    stats = mel.get_statistics(mat)
    log("%s：vs=%d ps=%d 指令，MaxWPO=%.0f cm，Nanite=%s" % (
        mat.get_name(), stats.num_vertex_shader_instructions, stats.num_pixel_shader_instructions,
        mat.get_editor_property("max_world_position_offset_displacement"), mat.get_editor_property("bUsedWithNanite")))


def patch_far_material():
    mat = eal.load_asset(FAR_MAT)
    if not mat:
        fail("%s 不存在（先跑 TinyGladeMakeGrassClump.py）" % FAR_MAT)
        return
    set_usage(mat, "MATUSAGE_INSTANCED_STATIC_MESHES", "bUsedWithInstancedStaticMeshes")
    set_usage(mat, "MATUSAGE_NANITE", "bUsedWithNanite")
    mel.recompile_material(mat)
    eal.save_loaded_asset(mat)
    log("%s：Nanite=%s" % (mat.get_name(), mat.get_editor_property("bUsedWithNanite")))


def main():
    patch_near_material()
    patch_far_material()
    for path, full_fallback in MESHES:
        enable_nanite(path, full_fallback)
    log("FAILED=%d %s" % (len(FAILED), "; ".join(FAILED)))
    log("DONE")


main()
