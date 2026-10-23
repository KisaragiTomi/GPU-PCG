# -*- coding: utf-8 -*-
"""
造**远处那一档**的两样资产（2026-09-22 用户："让远处使用组合草。近处使用单根草。
并且远处只有法线运动没有 WPO。"）：

  · `SM_TG_GrassClump`  一簇草：18 片草叶、每片 1 个三角（18 三角 / 54 顶点），
    半径 45 cm、叶高 45–65 cm。UV.y = t（叶根 0 → 叶尖 1），与单根草
    `SM_TG_GrassBlade` 同一套约定 —— 风场公式吃的就是这个 t。
  · `M_TG_GrassFar` + `MI_TG_GrassFar`  远处材质：**从 `M_TG_Grass` / `MI_TG_Grass`
    复制**再改两处 —— 拆掉世界位置偏移（WPO），把同一个风场接到**法线**上。

为什么是"复制再改"而不是新写一份：近 / 远两档在过渡带里是**同框**的（带内逐株二选一），
albedo / 粗糙度 / sheen / 顶点色那一整套只要差一点点，交界处就会读成一条色带。
复制保证除了这两处之外逐位相同，也保证参数名一致 —— `MI_TG_GrassFar` 直接是
`MI_TG_Grass` 的副本、只换了父材质，所有手调过的值原样跟过去。

⚠️ 跑之前先确认 `MI_TG_Grass` 的风参数是你要的：这份脚本**不改**近处材质。
⚠️ 重跑会**覆盖**这三个资产：两份材质先删后建；网格原地重建（不删，地面在内存里直接引用它）。
   只想重建网格（比如补 Nanite 数据）时先设环境变量 `GRASSCLUMP_MESH_ONLY=1`，材质一个都不动。

跑法（无头）：
  UnrealEditor-Cmd.exe <uproject> -ExecutePythonScript="...\\TinyGladeMakeGrassClump.py"
    -unattended -nosplash -stdout -AbsLog=...
"""
import math
import os

import unreal

ASSET = "/PCGPlugins/HouseTest/TinyGladeAsset"
MESH_DIR = "%s/Meshes" % ASSET
MAT_DIR = "%s/Materials" % ASSET

CLUMP = "SM_TG_GrassClump"
NEAR_MAT = "%s/M_TG_Grass" % MAT_DIR
NEAR_MI = "%s/MI_TG_Grass" % MAT_DIR
FAR_MAT = "%s/M_TG_GrassFar" % MAT_DIR
FAR_MI = "%s/MI_TG_GrassFar" % MAT_DIR

# 一簇的形状（cm）。半径取 45 cm ≈ 远处一格的一半（1.5 簇/m² ⇒ 格距 82 cm），
# 簇与簇之间因此刚好搭上、不留空地，又不会重叠成一堵墙。
BLADES = 18
RADIUS = 45.0
H_MIN, H_MAX = 45.0, 65.0
W_MIN, W_MAX = 10.0, 14.0       # 叶根宽（远处一片叶只有 1–2 px，比单根草宽一倍才看得见）
TILT_MIN, TILT_MAX = 5.0, 22.0  # 往外倒多少度（簇心直立、边缘外倾，读成一丛而不是一把刷子）

# 风只扰动法线时的强度：法线 = normalize(着色法线 + 风向 × 弯曲量 × 它)。
# 0.35 ≈ 近处 t=1 处 WPO 25 cm 在 50 cm 叶上造成的倾角（约 27°）的正弦。
NORMAL_WIND_STRENGTH = 0.35

NORMAL_HLSL = """// 远处不动顶点：把近处那一份弯曲量（同一个风场）折进法线里，
// 风扫过时整片草地的明暗随之起伏 —— 这是 WPO 那条路在远处唯一还看得见的效果。
float3 Nw = N + D * (B * S);
return normalize(Nw);
"""

eal = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()

FAILED = []


def log(msg):
    unreal.log("GRASSCLUMP %s" % msg)


def fail(msg):
    FAILED.append(msg)
    unreal.log_error("GRASSCLUMP FAILED: %s" % msg)


# ---------------------------------------------------------------------------
# 1) 一簇草的网格
# ---------------------------------------------------------------------------

def hash01(i, salt):
    """确定性的伪随机：重跑两次必须得到同一簇草（否则每跑一次远处的草都换一茬）。"""
    x = math.sin((i + 1) * 12.9898 + salt * 78.233) * 43758.5453
    return x - math.floor(x)


def build_clump():
    path = "%s/%s" % (MESH_DIR, CLUMP)
    # 已存在就**原地重建**、不删：地面的 CDO 与关卡实例在内存里直接引用这个对象（`Grass.LOD.FarMesh`），
    # 删掉会把那些引用置空，远处组合草当场消失（存盘的话就真丢了）。

    desc = unreal.StaticMesh.create_static_mesh_description()
    group = desc.create_polygon_group()

    golden = math.radians(137.508)   # 黄金角：叶子在圆盘上分得最开，不结块
    for i in range(BLADES):
        # 落点：r = R√(i/N) 让叶子在圆盘上均匀（而不是全挤在中心）
        r = RADIUS * math.sqrt((i + 0.5) / BLADES)
        a = i * golden + hash01(i, 3) * 0.6
        bx, by = r * math.cos(a), r * math.sin(a)

        h = H_MIN + (H_MAX - H_MIN) * hash01(i, 11)
        w = W_MIN + (W_MAX - W_MIN) * hash01(i, 17)
        # 外倾：越靠边倒得越狠，方向背对簇心
        tilt = math.radians(TILT_MIN + (TILT_MAX - TILT_MIN) * (r / RADIUS) * hash01(i, 23))
        yaw = a + (hash01(i, 29) - 0.5) * 1.2
        lean = math.sin(tilt) * h
        tx, ty = bx + lean * math.cos(yaw), by + lean * math.sin(yaw)
        tz = math.cos(tilt) * h

        # 叶宽方向：与外倾方向垂直，这样一簇里的叶面朝向各不相同（远处才有体积感）
        wx, wy = -math.sin(yaw) * w * 0.5, math.cos(yaw) * w * 0.5

        verts = []
        for px, py, pz, u, v in ((bx - wx, by - wy, 0.0, 0.0, 0.0),
                                 (bx + wx, by + wy, 0.0, 1.0, 0.0),
                                 (tx, ty, tz, 0.5, 1.0)):
            vtx = desc.create_vertex()
            desc.set_vertex_position(vtx, unreal.Vector(px, py, pz))
            vi = desc.create_vertex_instance(vtx)
            desc.set_vertex_instance_uv(vi, unreal.Vector2D(u, v), 0)
            verts.append(vi)
        desc.create_triangle(group, verts)

    sm = eal.load_asset(path) if eal.does_asset_exist(path) else tools.create_asset(CLUMP, MESH_DIR, unreal.StaticMesh, None)
    # 2026-09-29 起地被一律走 Nanite（组件按资产的 Nanite 开关自动换路）：开关要在构建**之前**打开。
    nanite = sm.get_editor_property("nanite_settings")
    nanite.set_editor_property("enabled", True)
    # 18 个三角，回退网格就用全精度（与两种花同一设置）。
    nanite.set_editor_property("fallback_target", unreal.NaniteFallbackTarget.RELATIVE_ERROR)
    nanite.set_editor_property("fallback_relative_error", 0.0)
    sm.set_editor_property("nanite_settings", nanite)
    # ⚠️ 第三个参数（fast build）必须是 False：快速构建直接写渲染数据、**不把 MeshDescription 存进源模型**，
    #    之后任何重建（包括开 Nanite）都没有源几何可建 —— 2026-09-29 实测开关开着、`NaniteTriangles = 0`，
    #    日志里连 NaniteBuild 那一行都没有，组件退回引擎 ISM 代理，还报 "nearly zero normals"。
    sm.build_from_static_mesh_descriptions([desc], False, False)
    sm.set_editor_property("light_map_resolution", 8)
    log("网格 %s：%d 顶点 / %d 三角" % (path, desc.get_vertex_count(), desc.get_triangle_count()))
    return sm


# ---------------------------------------------------------------------------
# 2) 远处材质：复制近处那份，拆 WPO、把风接到法线
# ---------------------------------------------------------------------------

def walk(mat, node, seen):
    """收集一棵输入子树里的全部节点（找风场节点与风向参数用）。"""
    if node is None or node in seen:
        return
    seen.append(node)
    for one in mel.get_inputs_for_material_expression(mat, node):
        walk(mat, one, seen)


def node_of_class(nodes, cls_name):
    return [n for n in nodes if n.get_class().get_name() == cls_name]


def build_far_material():
    if not eal.does_asset_exist(NEAR_MAT):
        fail("%s 不存在 —— 远处材质是它的副本，先跑造草那一遍" % NEAR_MAT)
        return None
    for path in (FAR_MAT, FAR_MI):
        if eal.does_asset_exist(path):
            eal.delete_asset(path)

    mat = eal.duplicate_asset(NEAR_MAT, FAR_MAT)
    if not mat:
        fail("复制 %s -> %s 失败" % (NEAR_MAT, FAR_MAT))
        return None

    # --- 拆 WPO：删掉喂给它的那个节点，属性就空出来了（引擎据此判定"这个材质没有 WPO"）---
    wpo = mel.get_material_property_input_node(mat, unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    bend = wind_dir = None
    if wpo:
        subtree = []
        walk(mat, wpo, subtree)
        # 风场：子树里那个 Custom 节点（`TG Wind Bend`，输出 float1 的弯曲量）
        customs = [n for n in node_of_class(subtree, "MaterialExpressionCustom")
                   if "wind" in str(n.get_editor_property("description")).lower()]
        bend = customs[0] if customs else None
        dirs = [n for n in node_of_class(subtree, "MaterialExpressionVectorParameter")
                if str(n.get_editor_property("parameter_name")).lower() == "winddirection"]
        wind_dir = dirs[0] if dirs else None
        log("WPO 子树 %d 个节点：顶节点 %s，风场 %s，风向 %s"
            % (len(subtree), wpo.get_class().get_name(),
               bend.get_editor_property("description") if bend else None,
               wind_dir.get_editor_property("parameter_name") if wind_dir else None))
        mel.delete_material_expression(mat, wpo)
    if mel.get_material_property_input_node(mat, unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET):
        fail("WPO 没拆干净：远处材质仍然带着世界位置偏移")
    else:
        log("WPO 已拆（远处不动顶点）")

    if bend is None or wind_dir is None:
        fail("没在 WPO 子树里找到风场 / 风向节点 —— 近处材质的结构变了，脚本要跟着改")
        return None

    # --- 把同一份弯曲量折进法线 ---
    shade_n = mel.get_material_property_input_node(mat, unreal.MaterialProperty.MP_NORMAL)
    if shade_n is None:
        fail("近处材质的 Normal 是空的 —— 远处没法在它上面叠风")
        return None
    # 2026-09-29 起近处材质的 Normal 顶上是 `TG Near Normal Wind`（WPO 淡出多少、法线风接过多少，
    # TinyGladeSetupCoverNanite.py 加的）。远处要的是它**底下**那份着色法线，否则风在远处叠两遍。
    if (shade_n.get_class().get_name() == "MaterialExpressionCustom"
            and str(shade_n.get_editor_property("description")) == "TG Near Normal Wind"):
        below = mel.get_inputs_for_material_expression(mat, shade_n)
        shade_n = below[0] if below else None
        if shade_n is None:
            fail("TG Near Normal Wind 的 N 输入是空的")
            return None
        log("近处 Normal 是 TG Near Normal Wind：远处接它底下的着色法线 %s" % shade_n.get_class().get_name())

    strength = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -300, 380)
    strength.set_editor_property("parameter_name", "NormalWindStrength")
    strength.set_editor_property("default_value", NORMAL_WIND_STRENGTH)
    strength.set_editor_property("group", "Wind")

    custom = mel.create_material_expression(mat, unreal.MaterialExpressionCustom, -120, 220)
    custom.set_editor_property("description", "TG Far Normal Wind")
    custom.set_editor_property("code", NORMAL_HLSL)
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    ins = []
    for name in ("N", "D", "B", "S"):
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", name)
        ins.append(ci)
    custom.set_editor_property("inputs", ins)
    # 风场经 VertexInterpolator 进像素着色器：风场在 `HeightAsBladeT = True` 那一支读 `PreSkinnedPosition`（只在顶点着色器里有），
    # 直接接进 Normal 的话那种 MI 编译失败、静默换成默认材质（2026-09-29 近处材质踩过，见 TinyGladeSetupCoverNanite.py 的 vs_to_ps）。
    bend_ps = mel.create_material_expression(mat, unreal.MaterialExpressionVertexInterpolator, -300, 300)
    if not mel.connect_material_expressions(bend, "", bend_ps, ""):
        fail("接不上 VertexInterpolator")
    for src, name in ((shade_n, "N"), (wind_dir, "D"), (bend_ps, "B"), (strength, "S")):
        if not mel.connect_material_expressions(src, "", custom, name):
            fail("接不上 Custom 节点的输入 %s" % name)
    if not mel.connect_material_property(custom, "", unreal.MaterialProperty.MP_NORMAL):
        fail("接不上 Normal")

    # 实例路径的死规矩：没勾这一条会被**静默**换成默认材质，画面一片灰而断言照绿。
    # ⚠️ C++ 签名带一个 `bool& bNeedsRecompile` 出参，但 Python 侧收成**单个 bool**（解包会抛
    #    TypeError）。回读母材质上的开关才是真话。
    mel.set_material_usage(mat, unreal.MaterialUsage.MATUSAGE_INSTANCED_STATIC_MESHES)
    if not mat.get_editor_property("bUsedWithInstancedStaticMeshes"):
        fail("bUsedWithInstancedStaticMeshes 没勾上 —— 实例路径会被静默换成默认材质")
    log("bUsedWithInstancedStaticMeshes = %s" % mat.get_editor_property("bUsedWithInstancedStaticMeshes"))

    mel.recompile_material(mat)
    eal.save_asset(FAR_MAT)
    st = mel.get_statistics(mat)
    log("材质 %s：vs=%d ps=%d 指令" % (FAR_MAT, st.num_vertex_shader_instructions,
                                       st.num_pixel_shader_instructions))

    # --- MI：近处 MI 的副本，只换父材质 ⇒ 手调过的风参数原样跟过去 ---
    if eal.does_asset_exist(NEAR_MI):
        mi = eal.duplicate_asset(NEAR_MI, FAR_MI)
        if not mi:
            fail("复制 %s -> %s 失败" % (NEAR_MI, FAR_MI))
            return mat
        mel.set_material_instance_parent(mi, mat)
        mel.update_material_instance(mi)
        eal.save_asset(FAR_MI)
        log("材质实例 %s：父 = %s" % (FAR_MI, mi.get_editor_property("Parent").get_name()))
    else:
        fail("%s 不存在 —— 远处只好直接用母材质" % NEAR_MI)
    return mat


# ---------------------------------------------------------------------------

def main():
    # 只重建网格、不动远处材质（材质是近处那份的复制品，重跑会把 M_TG_GrassFar / MI_TG_GrassFar 整个重生成）：
    # 在跑本脚本的进程里先设环境变量 GRASSCLUMP_MESH_ONLY=1。
    mesh_only = os.environ.get("GRASSCLUMP_MESH_ONLY") == "1"
    mesh = build_clump()
    mat = eal.load_asset(FAR_MAT) if mesh_only else build_far_material()
    if mesh and mat:
        far = eal.load_asset(FAR_MI) if eal.does_asset_exist(FAR_MI) else mat
        mesh.set_editor_property("static_materials",
                                 [unreal.StaticMaterial(material_interface=far,
                                                        material_slot_name="GrassFar")])
        eal.save_asset("%s/%s" % (MESH_DIR, CLUMP))
        box = mesh.get_bounding_box()
        log("一簇 %.0f×%.0f×%.0f cm，材质槽 = %s"
            % (box.max.x - box.min.x, box.max.y - box.min.y, box.max.z - box.min.z,
               mesh.get_material(0).get_name() if mesh.get_material(0) else None))
    log("FAILED=%d %s" % (len(FAILED), "; ".join(FAILED)))
    log("DONE")


main()
