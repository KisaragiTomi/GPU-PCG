# -*- coding: utf-8 -*-
"""把岩壳的**顶盖**与**裙**分开着色：`M_TG_Texture` 加一条按顶点色 R 的裙压暗支路。

--- 依据：为什么是"裙偏暗"，不是别的 ----------------------------------------------
1. **这一位本来就是给材质的。** `Docs/TinyGlade/Ground_Shaper.md`「对照 Tiny Glade」实测记着：
   TG 烘焙件里的 `Triangle.is_top`「**位移着色器里从没被读**，只原样搬运，
   **是给光栅化/材质用的**」。本项目的 `bIsCapTri`（bit 26）是它的对位物，打了但一直没人读 ——
   拿它分开着色不是新发明，是把 TG 原本的用途接上。
2. **裙是岩壁面，盖是石头顶面。** 同一节实测：盖三顶点全在顶圈；裙**每一个都跨两圈**，
   竖直高度 = 局部坡度 × 19.5 cm ——「平地自动塌成零高度，陡坡自动拉成岩壁」。
   即裙就是那圈近乎竖直的断面，盖是朝天的顶面。
3. **压暗补的是壳"结构上不可能有"的那份自阴影。** 同文档：「它不是实体，是贴在地形上的
   一层壳」——没有底、不封闭。近乎竖直的裙面因此既没有体积去遮挡自己，
   也没有几何厚度让 Lumen 算出接触暗部；而朝天的盖能拿到整个上半球的天光。
   缺的正是这份方向性差异，用顶点色补它是最省的做法。
4. **不动粗糙度**：裁决六写明「当前阶段验收面收窄：只要求法线正确 + 颜色贴图正确，
   **粗糙度 / 金属度 / AO / 次表面一律不追**」。所以这里只改 BaseColor 的明度，
   少一个没人验收的活动件。

--- 为什么改得起 `M_TG_Texture`（459 个 MI 挂在下面）--------------------------------
整条支路**挂在一个默认 0 的标量参数 `RockShellCapSkirt` 后面**：

    factor = lerp(1, RockShellSkirtDarken, (1 − VertexColor.R) × RockShellCapSkirt)
    BaseColor = <原来接在 BaseColor 上的那一支> × factor

`RockShellCapSkirt = 0` ⇒ `factor ≡ 1` ⇒ **与改动前逐位相同**，与那张网格有没有顶点色、
顶点色是什么值全都无关。只有 `MI_rocky_terrain` 把它覆盖成 1。
⇒ 另外 458 个 MI 一个像素都不会变，**不需要逐个去核对它们的顶点色**
（clutter 那批网格的 `COLOR_0` 是有内容的，真按顶点色无条件压暗会把它们一起弄暗）。

⚠️ `TinyGladeMakeDecorMaterial.py` 的文件头还写着「`M_TG_Texture` 不要改」——
**那句已被裁决六作废**（2026-08-30 用户追加指令「帮我把材质都改为默认光照」，
`M_TG_Texture` 本体被翻成 `MSM_DefaultLit`，状态文件同日记了「『只许 gallery 用』那句随之作废」）。
本脚本没有去改那个文件头（它是另一位 agent 的产物），只在这里记一笔。

--- 还剩两张 unlit 的 TG 母材质：**已核实不在交付路径，故意保留** -------------------
`M_TG_TextureMasked`（20 个 MI）与 `M_TG_Glass` 至今仍是 `MSM_UNLIT`。
**这不是裁决六的遗漏，是核实之后的保留**（2026-08-31 复核）：两者的**唯一**消费者是
`Content/HouseTest/TinyGladeAsset/Maps/TinyGladeGallery.umap` 那张看图关卡 —— 990 处组件材质覆盖里
**没有一处**落在实例组件上，也没有任何 C++ / 蓝图 / 生成路径引用它们。
裁决六的字面约束是「**任何交付路径上**的材质都不许是 `MSM_Unlit`」，看图关卡不在交付路径上，
与 `M_SimpleBrush` / `M_Bound` / `M_TransformTest` / `M_Color` 那四张调试材质同一档
（可视化材质受光反而是错的）。

⇒ **下一轮扫材质纪律时不要把它们当遗漏去改。** 真要改的触发条件只有一个：
它们中的某一张开始被交付路径上的东西引用（挂到 `UCSGpuInstancedMeshComponent` /
`UCSMeshRenderComponent`，或被任何 `Scripts/TinyGladeMake*.py` 写进 CDO）——
那一刻它就进了交付路径，才轮到按 `M_TG_Texture` 那四步翻 lit。

--- 通道从哪来 ---------------------------------------------------------------------
`CSRockShell::BuildMesh` 把 `bIsCapTri` 写进岩壳网格的**顶点色 R**（1 = 盖 / 0 = 裙），
通道字典在 `Source/ComputeShaderGenerator/Public/CSGroundRockShell.h`。
走顶点色而不是 aux 槽 33 是硬要求：aux 流是 `VET_None`，既进不了顶点工厂也进不了
`SaveToStaticMesh`（裁决六 ②「顶点色通道字典与多组 UV 必须随网格保住」）。

用法::

    UnrealEditor-Cmd.exe <uproject> -ExecutePythonScript="<本文件>" -unattended -nopause -abslog=<日志>

日志自诊断：最后一行是 `ROCKSHELLMAT OK` 或 `ROCKSHELLMAT FAILED`。可重跑（幂等）。
"""
import unreal

MASTER = "/PCGPlugins/HouseTest/TinyGladeAsset/Materials/M_TG_Texture"
SHELL_MI = "/PCGPlugins/HouseTest/TinyGladeAsset/Materials/MI_rocky_terrain"

PARAM_AMOUNT = "RockShellCapSkirt"     # 0 = 整条支路关掉（默认）；1 = 全额
PARAM_DARKEN = "RockShellSkirtDarken"  # 裙的 BaseColor 乘数
GROUP = "Rock Shell"

# 0.55 = 裙比盖暗约一档。取值依据是"补上壳结构上不可能有的自阴影"，不是调出来的好看数：
# 竖直面在均匀上半球天光下拿到的辐照度约是水平朝天面的一半（cos 加权的半球积分之比），
# 0.55 就落在那个量级上，留了一点余量避免读成纯黑缝。
DARKEN_DEFAULT = 0.55

MEL = unreal.MaterialEditingLibrary
FAILS = []


def check(label, ok, detail=""):
    if ok:
        unreal.log("[PASS] %s %s" % (label, detail))
    else:
        FAILS.append(label)
        unreal.log_error("[FAIL] %s %s" % (label, detail))


def make_scalar(mat, name, default, x, y, lo=0.0, hi=1.0):
    node = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, x, y)
    node.set_editor_property("parameter_name", name)
    node.set_editor_property("default_value", default)
    node.set_editor_property("group", GROUP)
    try:
        node.set_editor_property("slider_min", lo)
        node.set_editor_property("slider_max", hi)
    except Exception:
        pass
    return node


def build_graph(mat):
    """在 BaseColor 那一支和 BaseColor 之间插一个乘数。不重写原来那一支。"""
    base_prop = unreal.MaterialProperty.MP_BASE_COLOR
    base_node = MEL.get_material_property_input_node(mat, base_prop)
    base_out = MEL.get_material_property_input_node_output_name(mat, base_prop)
    unreal.log("ROCKSHELLMAT BaseColor 现接：%s 的输出 %r"
               % (base_node.get_class().get_name() if base_node else "<空>", base_out))
    # 空的话就不猜：接一个凭空的常数只会把整库刷成纯色，而且不报错。
    if base_node is None:
        check("BaseColor 上原本接着东西（否则无从插入乘数）", False)
        return False

    vc = MEL.create_material_expression(mat, unreal.MaterialExpressionVertexColor, -900, 400)
    inv = MEL.create_material_expression(mat, unreal.MaterialExpressionOneMinus, -680, 400)
    MEL.connect_material_expressions(vc, "R", inv, "")

    amount = make_scalar(mat, PARAM_AMOUNT, 0.0, -900, 540)
    mask = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -480, 440)
    MEL.connect_material_expressions(inv, "", mask, "A")
    MEL.connect_material_expressions(amount, "", mask, "B")

    one = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -480, 300)
    one.set_editor_property("r", 1.0)
    darken = make_scalar(mat, PARAM_DARKEN, DARKEN_DEFAULT, -480, 360)

    factor = MEL.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, -280, 360)
    MEL.connect_material_expressions(one, "", factor, "A")
    MEL.connect_material_expressions(darken, "", factor, "B")
    MEL.connect_material_expressions(mask, "", factor, "Alpha")

    tint = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -120, 120)
    MEL.connect_material_expressions(base_node, base_out, tint, "A")
    MEL.connect_material_expressions(factor, "", tint, "B")
    MEL.connect_material_property(tint, "", base_prop)
    return True


def main():
    mat = unreal.EditorAssetLibrary.load_asset(MASTER)
    check("母材质存在", isinstance(mat, unreal.Material), MASTER)
    if not isinstance(mat, unreal.Material):
        return

    # 纪律：交付路径上的材质一律不许 unlit（裁决六）。这里顺手守一道 ——
    # 本脚本会重编译母材质，万一哪天有人在别处把 shading model 改回去，这条会当场报红。
    sm = mat.get_editor_property("shading_model")
    check("母材质是 DefaultLit（裁决六：交付路径不许 unlit）",
          sm == unreal.MaterialShadingModel.MSM_DEFAULT_LIT, str(sm))

    existing = [str(n) for n in MEL.get_scalar_parameter_names(mat)]
    if PARAM_AMOUNT in existing:
        unreal.log("ROCKSHELLMAT 支路已在（标量参数 %s 存在），跳过建图。" % PARAM_AMOUNT)
    elif not build_graph(mat):
        return
    else:
        MEL.recompile_material(mat)
        unreal.EditorAssetLibrary.save_loaded_asset(mat)

    # **中性性判据**：默认值必须是 0，否则 459 个 MI 会一起被改。这一条比"图连对了"更重要 ——
    # 连错了看得见，默认值不是 0 只会让整个提取资产库悄悄变暗一档。
    amount_default = MEL.get_material_default_scalar_parameter_value(mat, PARAM_AMOUNT)
    darken_default = MEL.get_material_default_scalar_parameter_value(mat, PARAM_DARKEN)
    check("母材质默认 %s = 0（其余 MI 逐位不变）" % PARAM_AMOUNT,
          abs(amount_default) < 1e-6, "= %.4f" % amount_default)
    check("母材质默认 %s ≈ %.2f" % (PARAM_DARKEN, DARKEN_DEFAULT),
          abs(darken_default - DARKEN_DEFAULT) < 1e-4, "= %.4f" % darken_default)

    mi = unreal.EditorAssetLibrary.load_asset(SHELL_MI)
    check("岩壳材质实例存在", isinstance(mi, unreal.MaterialInstanceConstant), SHELL_MI)
    if not isinstance(mi, unreal.MaterialInstanceConstant):
        return
    MEL.set_material_instance_scalar_parameter_value(mi, PARAM_AMOUNT, 1.0)
    MEL.update_material_instance(mi)
    unreal.EditorAssetLibrary.save_loaded_asset(mi)
    mi_amount = MEL.get_material_instance_scalar_parameter_value(mi, PARAM_AMOUNT)
    check("MI_rocky_terrain 把 %s 覆盖成 1" % PARAM_AMOUNT, abs(mi_amount - 1.0) < 1e-6,
          "= %.4f" % mi_amount)

    # 只有岩壳那一个 MI 该覆盖它。别的 MI 覆盖了就是有人顺手抄了参数，画面上看不出来。
    overriding = []
    for child in MEL.get_child_instances(mat):
        inst = unreal.EditorAssetLibrary.load_asset(child.export_text() if hasattr(child, "export_text") else str(child))
        if isinstance(inst, unreal.MaterialInstanceConstant):
            try:
                if abs(MEL.get_material_instance_scalar_parameter_value(inst, PARAM_AMOUNT)) > 1e-6:
                    overriding.append(inst.get_name())
            except Exception:
                pass
    unreal.log("ROCKSHELLMAT 子实例 %d 个，其中把 %s 开起来的：%s"
               % (len(MEL.get_child_instances(mat)), PARAM_AMOUNT, overriding or ["<只有 MI_rocky_terrain 或读不到>"]))


unreal.log("========== ROCKSHELLMAT ==========")
try:
    main()
except Exception:
    # 准备阶段抛异常 ⇒ -ExecCmds 那条 Quit 再也执行不到，任务看着像挂死（踩过的坑）。
    import traceback
    unreal.log_error("ROCKSHELLMAT EXCEPTION:\n%s" % traceback.format_exc())
    FAILS.append("exception")
for f in FAILS:
    unreal.log_error("  failed: %s" % f)
unreal.log("ROCKSHELLMAT FAILED" if FAILS else "ROCKSHELLMAT OK")
