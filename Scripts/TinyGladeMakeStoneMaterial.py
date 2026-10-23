# -*- coding: utf-8 -*-
"""
石阶材质 + 天光 —— 修"石阶背光面是一片纯黑"。

2026-08-30 实测的两半根因，缺一不可：

  ① **两张演示关卡里没有 SkyLight**（`TinyGladeSetupLighting.py` 只加了 PPV 与高度雾）。
     没有任何天光贡献 ⇒ 背光面直接归零。石阶不是特例 —— 同一张图里背光那半边草地也是纯黑。
     这是场景级根因，光换材质修不好。
  ② 石阶原本借用 `M_TinyGladeBrick`，那是个**纯常数灰 + 粗糙度**、没贴图没法线的材质。
     有天光之后它也只是"均匀的灰"，读不出石头。

对位 TG：它的 AO 被硬夹在 `mix(0.6, 1.0, ao)`，**阴影区永远压不黑** —— 那种"柔和"有一大半
来自这条，不是来自资产（见 `Docs/TinyGlade/Progress_ModuleComparison.md（卷四）`）。SkyLight 是 UE 侧最接近的一手。

⚠️ **TG 的 stone_floor 系列没有 albedo 贴图**（只有 normal / roughness / height / seed）：
它的石头颜色来自调色板，细节来自法线与粗糙度。所以这里也是常数底色 + 法线/粗糙度贴图，
**不是漏配了 albedo**。

⚠️ **必须用三平面**：石阶网格 `stairs_step` 是 100³ 居中立方体，被逐实例**非均匀**缩放到
约 60×100×45，任何跟 UV 走的贴图都会被拉成条。TG 同样对这类构件用三平面。

⚠️ 脚本把值烘进 **BP CDO 与关卡实例两处**：只改 CDO，关卡里已存在的实例不会变。
"""
import unreal

PKG = "/PCGPlugins/HouseTest"
MEL = unreal.MaterialEditingLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()

NORMAL_TEX = "/PCGPlugins/HouseTest/TinyGladeAsset/Textures/stone_floor_normal"
ROUGH_TEX = "/PCGPlugins/HouseTest/TinyGladeAsset/Textures/stone_floor_roughness"
WORLD_ALIGNED = ("/Engine/Functions/Engine_MaterialFunctions01/Texturing/"
                 "WorldAlignedTexture.WorldAlignedTexture")

# 石头底色取 TG 调色板里石构件那一档的中间值。偏暖的浅灰，不是中性灰 ——
# 中性灰在偏暖的日照下会读成"塑料"。
STONE_RGB = unreal.LinearColor(0.52, 0.49, 0.44, 1.0)
TILE_CM = 200.0        # 三平面的世界平铺尺寸；石阶长边约 100 cm，取 2 倍避免图案重复得太密


def world_aligned(mat, tex_path, x, y, tile, sampler):
    """
    用引擎自带的 WorldAlignedTexture 做三平面。拿不到函数或贴图就返回 None，
    调用方退回常数 —— 这份脚本的**首要价值是天光**，材质细节不该成为它跑不完的理由。

    `sampler` 必须与贴图的导入设置一致（TextureObject 的采样器类型会被校验）：
    法线图 NORMAL；stone_floor_roughness 是 TG 的 BC4 线性数据，按 TinyGladeFixBc4Textures.py
    导成 TC_Grayscale + sRGB 关 ⇒ LINEAR_GRAYSCALE（2026-09-14 之前按 sRGB 颜色读、值域又被压成 0..76，
    粗糙度实际只有 0..0.07）。
    """
    tex = unreal.EditorAssetLibrary.load_asset(tex_path)
    fn = unreal.EditorAssetLibrary.load_asset(WORLD_ALIGNED)
    if not tex or not fn:
        unreal.log_warning("STONE 三平面退回常数：tex=%s fn=%s" % (bool(tex), bool(fn)))
        return None
    try:
        obj = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureObject, x - 300, y)
        obj.set_editor_property("texture", tex)
        obj.set_editor_property("sampler_type", sampler)
        size = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, x - 300, y + 120)
        size.set_editor_property("r", tile)
        call = MEL.create_material_expression(mat, unreal.MaterialExpressionMaterialFunctionCall, x, y)
        call.set_editor_property("material_function", fn)
        MEL.connect_material_expressions(obj, "", call, "TextureObject")
        MEL.connect_material_expressions(size, "", call, "TextureSize")
        return call
    except Exception as exc:
        unreal.log_warning("STONE 三平面建图失败，退回常数：%s" % exc)
        return None


path = "%s/M_TinyGladeStone" % PKG
if unreal.EditorAssetLibrary.does_asset_exist(path):
    unreal.EditorAssetLibrary.delete_asset(path)
mat = TOOLS.create_asset("M_TinyGladeStone", PKG, unreal.Material, unreal.MaterialFactoryNew())
# 石阶走 UCSGpuInstancedMeshComponent，必须勾这个，否则实例路径下材质编译不出来。
mat.set_editor_property("used_with_instanced_static_meshes", True)

base = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -700, 0)
base.set_editor_property("constant", STONE_RGB)

# 逐实例的明暗微差。**这一项依赖 S2 那轮修好的 PerInstanceRandom**：
# 它此前取的是 InterlockedAdd 的槽位（每 dab 重掷 ⇒ 画一笔路全场变色），
# 现在是格身份哈希，同一格同一层恒等，拿来做颜色变化才成立。
#
# ⚠️ 读的是 `PerInstanceRandom + VertexColor.A`，不是裸的 `PerInstanceRandom`（裁决六 ③）：
# 烘成 StaticMesh 之后**没有实例了**，`PerInstanceRandom` 恒等于 0，这一支会塌成
# `lerp(0.88, 1.12, 0) = 0.88`，整片石阶烘出来是同一个色，而且一条断言都不会红。
# 两条路互斥地为零（实例路上传基础网格时 A 被清零，烘焙路把随机数写进 A），
# 相加之后取值逐位相同。通道字典在 `CSGpuInstancedMeshComponent` 类注释。
try:
    rnd_live = MEL.create_material_expression(mat, unreal.MaterialExpressionPerInstanceRandom, -900, 200)
    vcol = MEL.create_material_expression(mat, unreal.MaterialExpressionVertexColor, -900, 320)
    rnd = MEL.create_material_expression(mat, unreal.MaterialExpressionAdd, -700, 200)
    MEL.connect_material_expressions(rnd_live, "", rnd, "A")
    MEL.connect_material_expressions(vcol, "A", rnd, "B")
    # 0.88 ~ 1.12 的乘性抖动：够打散"一整片同色"，又不会让某块石头突兀。
    scale = MEL.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, -450, 150)
    lo = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -700, 300)
    lo.set_editor_property("r", 0.88)
    hi = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -700, 360)
    hi.set_editor_property("r", 1.12)
    MEL.connect_material_expressions(lo, "", scale, "A")
    MEL.connect_material_expressions(hi, "", scale, "B")
    MEL.connect_material_expressions(rnd, "", scale, "Alpha")
    tint = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -250, 0)
    MEL.connect_material_expressions(base, "", tint, "A")
    MEL.connect_material_expressions(scale, "", tint, "B")
    color_out = tint
except Exception as exc:
    unreal.log_warning("STONE 逐实例色差跳过：%s" % exc)
    color_out = base
MEL.connect_material_property(color_out, "", unreal.MaterialProperty.MP_BASE_COLOR)

norm = world_aligned(mat, NORMAL_TEX, -250, 420, TILE_CM, unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
if norm:
    MEL.connect_material_property(norm, "", unreal.MaterialProperty.MP_NORMAL)

rough = world_aligned(mat, ROUGH_TEX, -250, 700, TILE_CM, unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GRAYSCALE)
if rough:
    MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
else:
    c = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -250, 700)
    c.set_editor_property("r", 0.88)
    MEL.connect_material_property(c, "", unreal.MaterialProperty.MP_ROUGHNESS)

MEL.recompile_material(mat)
unreal.EditorAssetLibrary.save_loaded_asset(mat)
unreal.log("STONE built %s" % mat.get_name())

# ---- 挂到 CDO 与两张关卡的实例；同时补 SkyLight（① 那一半） ----
A = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

bp = unreal.EditorAssetLibrary.load_asset("%s/BP_TinyGladeGround" % PKG)
if bp:
    unreal.get_default_object(bp.generated_class()).set_editor_property("StairMaterial", mat)
    unreal.EditorAssetLibrary.save_loaded_asset(bp)
    unreal.log("STONE CDO done")

for level in ("L_TerrainOpsDemo", "L_HouseGroundDemo"):
    unreal.EditorLoadingAndSavingUtils.load_map("%s/%s" % (PKG, level))
    n = 0
    for a in A.get_all_level_actors():
        name = a.get_class().get_name()
        if "Ground" in name and "Shaper" not in name:
            a.set_editor_property("StairMaterial", mat)
            n += 1

    skies = [a for a in A.get_all_level_actors() if isinstance(a, unreal.SkyLight)]
    if not skies:
        sky = A.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(0.0, 0.0, 1000.0),
                                       unreal.Rotator(roll=0.0, pitch=0.0, yaw=0.0))
        sky.set_actor_label("TG_SkyLight")
        skies = [sky]
        unreal.log("STONE %s + SkyLight" % level)

    for sky in skies:
        comp = sky.light_component
        # ⚠️ 这才是"背光面纯黑"的真因，不是"没有天光"：
        # `L_TerrainOpsDemo` 的 DemoSky 是 SLS_CAPTURED_SCENE 且 real_time_capture=False ——
        # 静态立方图必须显式重捕获，从没捕过就是一张黑图 ⇒ 环境光贡献恒为零，
        # 而 Details 面板里它看着一切正常（可见、强度 1.0、affects world）。
        # 对照组：L_HouseGroundDemo 那盏 real_time_capture=True，同样的场景就不黑。
        # 地形与房子一直在变，实时捕获也是这个工程唯一说得通的模式。
        comp.set_editor_property("real_time_capture", True)
        # ⚠️ **强度与下半球色不在这里设**（原先这里写死 `intensity = 1.0`）：它们是布光参数，
        #    归 `TinyGladeSetupLighting.py::setup_skylight`。两个脚本都写同一个属性的话，
        #    画面就取决于**谁后跑**，而两次都会打出"成功"——这类顺序依赖已经在石阶材质上
        #    踩过一次（见 `TinyGladeSetupStairs.py` 顶部）。这里只修"从没捕获过"那条根因。
        unreal.log("STONE %s skylight '%s' -> real_time_capture" % (level, sky.get_actor_label()))

    # 历史注记：这里原来还要给塑形物自持的旧路石阶（`StepMaterial`）也刷一份材质 —— 它一直
    # 留空 ⇒ 引擎拿 `WorldGridMaterial`（灰白棋盘格）顶上，出图里占掉整条石阶一半的面积。
    # 旧路已随 2026-08-30「裁决一」第二步整条删除（属性也一并没了），演示关卡从此只有
    # 地面自己这一条石阶，`StairMaterial` 就是它的全部。

    unreal.EditorLoadingAndSavingUtils.save_current_level()
    unreal.log("STONE %s -> %d ground actors, skylights=%d" % (level, n, len(skies)))

unreal.log("STONE DONE")
