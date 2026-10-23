# -*- coding: utf-8 -*-
"""
建 `M_TinyGladeCanopy` 母材质 + `MI_TinyGladeCanopy`：把 TG 树冠着色的四条移植缺口落地。

逐条机制与 TG 侧证据在 `Docs/TinyGlade/Tree_CanopyShading.md`，本文件头只记**为什么这么落**。

--- 为什么不能直接用 `M_TG_Canopy` / `M_TG_LeafCards` --------------------------------
`TinyGladeAsset/Materials/` 下那 8 个 `M_TG_*` 是**提取资产的看图材质**（gallery 用），
全仓零 C++/Python 引用，`M_TG_Texture` 底下还挂着 459 个 MI。动它们会波及整个提取资产库，
而它们本身也只有 BaseColor 一条线，四条缺口一条都没有。所以这里自建一张生产母材质，
与 `M_TinyGladeWall` / `M_TG_Grass` 并列。**`M_TG_*` 一律不动。**

--- 数据全在 `central_tree_leafcards` 的三个 UV 通道里（2026-09-09 实测逐位对上）--------
这是本脚本能成立的前提。探查 `central_tree_leafcards`（10396 三角 = 5198 张卡）得到：

    UV0        卡片角码，全网格只有 (0,0)(1,0)(0,1)(1,1) 四个值   -> billboard 角码 + 贴图 UV
    UV1.xy     prim_center 的 X,Y（局部 cm）                      -> 与四顶点几何中心逐位相同
    UV2.x      prim_center 的 Z（局部 cm，8.43~1434.4 = 树高）     -> 同上
    NORMAL     逐卡一致（一张卡 6 个顶点法线完全相同）             -> 等价于 TG 的 prim_normals
    VertexColor 烘好的叶色（r .066~.679 / g .096~.588 / b .005~.431，A 恒 1）

⚠️ **`_origUV` 那一份不能用**：它的 UV0 是把叶子摊进图集的连续 UV（每卡一个矩形子区域），
不是角码；UV 通道也只有 1 个，拿不到 prim_center。挂错网格的症状是叶片形状全乱且
cone-cull 整片乱收缩，材质本身不报任何错。

--- 深度浮雕：UE 的 PDO 只能往远推，所以符号与 TG **有意相反** -----------------------
TG 的净效果是「按 height 把像素拉近，最多 1.6 m」：VS 把整卡拉近 1.6 m，PS 再按 (1-h) 推回。
UE 的 `PixelDepthOffset` **只接受把像素往远推**，直接照抄 TG 的符号会得到一个凹陷。

两条可选映射，这里取第二条：
  ① WPO 沿视线抬 1.6 m + PDO 按 (1-h) 推回 —— 与 TG 逐位等价，但 WPO 会**同时改屏幕位置**
     （TG 改的是 clip.z，不动 x/y）。10 m 观察距离下卡片会放大约 19%，轮廓明显变胖。
  ② **只用 PDO**：`PDO = DepthBulge · distFade · (1 - height)`。
     h=1（叶簇最厚）不动、h=0（卡片边缘）往远推 1.6 m。

⚠️ **`DepthBulge` 的默认值不是 TG 的 160 cm，而是 60 cm** —— 浮雕量要按**卡片尺寸**等比，
不能照抄绝对值。TG 的叶卡宽约 1.3 TG 单位（≈130 cm）配 1.6 单位的浮雕，比值 1.23；
本项目 `central_tree_leafcards` 的卡片 span 实测只有 40~54 cm，同样比值给出 ~60 cm。
2026-09-09 用 160 出过一轮图：层次是出来了，但整棵树明显压暗一档（浮雕量是卡片宽的 3~4 倍，
叶子被推到彼此身后太远，自遮蔽过头）。换网格时这个值要跟着卡片尺寸重算。

②与 TG 的**相对**深度关系完全相同 —— 而深度浮雕要的就是相对关系（交线形状、SSAO、相互穿插），
差别只是整张卡片相对场景其它物体整体后退 `DepthBulge·distFade`。代价是树冠比真实位置略靠后，
可能被地面/树干多遮一点；换来的是不动几何、不改轮廓。`DepthBulge` 调 0 即可退回平卡片对照。

--- cone-cull 用的是「顶点收向卡心」，不是 discard -----------------------------------
TG 在 VS 里对整个顶点写 `gl_Position = NaN` 硬剔除，UE 材质里没有等价物（OpacityMask 是逐像素，
overdraw 一点不省）。这里改成 WPO 把顶点收向 prim_center：`fade=1` 时整张卡退化成一个点，
光栅化阶段自然不产像素，与 TG 的「先缩成一点再消失」观感一致且同样无 pop。

⚠️ `max_world_position_offset_displacement` 必须 >= 卡片半对角（实测卡片 span 约 40~54 cm），
留 0 的话某些剔除路径按未变形的包围盒算，屏幕边缘会闪。

⚠️ TG 的 `smoothstep(0.3, -0.2, k)` 是 **Min > Max 的反向 smoothstep**，UE 的 SmoothStep 节点
不接受。代数上 `1 - smoothstep_rev(0.3, -0.2, k)` 恒等于 `smoothstep(-0.2, 0.3, k)`，
这里直接写正向那条，省一个 OneMinus。

--- 逆光旁路（缺口②）只做到近似，且是**有意**的 -------------------------------------
TG 在延迟光照里让 `backlit = max(0, -0.75·dot(V,L))` 把 `N·L` 与厚度遮蔽**同时旁路**，
只剩 Fresnel 边缘项。UE 的光照在引擎 lighting pass 里，材质图碰不到 `N·L`。
这里用 `MSM_TWO_SIDED_FOLIAGE` 的 SubsurfaceColor 承担透射，强度按叶色给 —— 观感方向对，
但「逆光时忽略 N·L」这一条**没有复刻**。要真做只能改引擎或走自定义 shading model，
不在本脚本范围。

--- Python 侧的三条针脚纪律（2026-09-09 逐条实测，写错就静默换默认材质）--------------
① **单输入节点的输入针脚名一律传 `""`**，不是 `"Input"`。
   `ComponentMask` / `TransformPosition` / `OneMinus` / `Abs` 传 `"Input"` 全部返回 False。
   （`Normalize` 两种都收，这里统一用 `""`。）
② **`VertexColor` 没有 `"RGB"` 输出**，可用的是 `""`（即 RGB）与 `R/G/B/A`。
   `TextureSample` 反而 `"RGB"` 与 `""` 都收 —— 两者不一样，别互相照抄。
③ **`unreal.MaterialProperty` 枚举里没有 `MP_PIXEL_DEPTH_OFFSET`**（也没有 CustomData / ShadingModel）。
   接 PDO 只能走 `use_material_attributes = True` + `MakeMaterialAttributes`，它的针脚名是
   **无空格**的 `PixelDepthOffset` / `WorldPositionOffset` / `SubsurfaceColor` / `AmbientOcclusion`
   / `OpacityMask` / `BaseColor`（带空格的写法一律返回 False）。

连线一律走 `link()` 并**检查返回值** —— 这一族函数针脚名写错时返回 False 不抛异常，材质带着
编译错误存盘，引擎再静默换默认材质（一片灰），而所有断言照绿。saturate 用 Min/Max 组合而不是
Clamp（Clamp 的针脚名在这一族里最容易写错）。两条都是 `TinyGladeMakeIvyMaterials.py` 记过的坑。
"""
import json
import unreal

PKG = "/PCGPlugins/HouseTest"
ASSET = "/PCGPlugins/HouseTest/TinyGladeAsset"
MEL = unreal.MaterialEditingLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()

MAT_NAME = "M_TinyGladeCanopy"
MI_NAME = "MI_TinyGladeCanopy"

TEX_ALPHA = ASSET + "/Textures/canopy_alpha"        # R 剪影 / G height / B AO
TEX_PALETTE = ASSET + "/Textures/autumn_canopy"     # 512² 色板，随机 UV 当取色器用
OUT_JSON = r"C:\Users\KLW\AppData\Local\Temp\claude\D--MyProject-UnrealProject-UETest574-2\663fbe13-5a7c-4be3-a860-830f2ae635f7\scratchpad\make_canopy.json"

FAILURES = []


def log(msg):
    unreal.log("CANOPY %s" % msg)


def fail(msg):
    FAILURES.append(msg)
    unreal.log_error("CANOPY !! %s" % msg)


def link(src, out_name, dst, in_name):
    """连一条线并检查返回值（这一族函数针脚名写错时返回 False 不抛异常）。"""
    if not MEL.connect_material_expressions(src, out_name, dst, in_name):
        fail("连线失败：%s.%s -> %s.%s"
             % (src.get_class().get_name(), out_name or "<out>", dst.get_class().get_name(), in_name))
        return False
    return True


def node(mat, cls, x, y):
    return MEL.create_material_expression(mat, cls, x, y)


def scalar(mat, name, default, x, y, desc=""):
    n = node(mat, unreal.MaterialExpressionScalarParameter, x, y)
    n.set_editor_property("parameter_name", name)
    n.set_editor_property("default_value", float(default))
    if desc:
        n.set_editor_property("desc", desc)
    return n


def vector(mat, name, color, x, y):
    n = node(mat, unreal.MaterialExpressionVectorParameter, x, y)
    n.set_editor_property("parameter_name", name)
    n.set_editor_property("default_value", color)
    return n


def const(mat, v, x, y):
    n = node(mat, unreal.MaterialExpressionConstant, x, y)
    n.set_editor_property("r", float(v))
    return n


def tex_param(mat, name, path, x, y):
    n = node(mat, unreal.MaterialExpressionTextureSampleParameter2D, x, y)
    n.set_editor_property("parameter_name", name)
    tex = unreal.EditorAssetLibrary.load_asset(path)
    if tex is None:
        fail("贴图缺失 %s" % path)
    else:
        n.set_editor_property("texture", tex)
    return n


def uv(mat, index, x, y):
    n = node(mat, unreal.MaterialExpressionTextureCoordinate, x, y)
    n.set_editor_property("coordinate_index", index)
    return n


def mask(mat, src, out_name, r, g, b, a, x, y):
    n = node(mat, unreal.MaterialExpressionComponentMask, x, y)
    n.set_editor_property("r", r)
    n.set_editor_property("g", g)
    n.set_editor_property("b", b)
    n.set_editor_property("a", a)
    link(src, out_name, n, "")           # 纪律①：单输入用 ""
    return n


def binop(mat, cls, a_src, a_out, b_src, b_out, x, y):
    n = node(mat, cls, x, y)
    link(a_src, a_out, n, "A")
    link(b_src, b_out, n, "B")
    return n


def mul(mat, a, ao, b, bo, x, y):
    return binop(mat, unreal.MaterialExpressionMultiply, a, ao, b, bo, x, y)


def add(mat, a, ao, b, bo, x, y):
    return binop(mat, unreal.MaterialExpressionAdd, a, ao, b, bo, x, y)


def sub(mat, a, ao, b, bo, x, y):
    return binop(mat, unreal.MaterialExpressionSubtract, a, ao, b, bo, x, y)


def lerp(mat, a, ao, b, bo, t, to, x, y):
    n = node(mat, unreal.MaterialExpressionLinearInterpolate, x, y)
    link(a, ao, n, "A")
    link(b, bo, n, "B")
    link(t, to, n, "Alpha")
    return n


def one_minus(mat, src, out_name, x, y):
    n = node(mat, unreal.MaterialExpressionOneMinus, x, y)
    link(src, out_name, n, "")
    return n


def normalize(mat, src, out_name, x, y):
    n = node(mat, unreal.MaterialExpressionNormalize, x, y)
    link(src, out_name, n, "")
    return n


def saturate(mat, src, out_name, x, y):
    """用 Min/Max 组合代替 Clamp —— Clamp 的针脚名是这一族里最容易写错的一个。"""
    zero = const(mat, 0.0, x - 150, y + 80)
    one = const(mat, 1.0, x + 60, y + 80)
    hi = binop(mat, unreal.MaterialExpressionMax, src, out_name, zero, "", x, y)
    return binop(mat, unreal.MaterialExpressionMin, hi, "", one, "", x + 200, y)


def smoothstep(mat, mn, mn_o, mx, mx_o, val, val_o, x, y):
    n = node(mat, unreal.MaterialExpressionSmoothStep, x, y)
    link(mn, mn_o, n, "Min")
    link(mx, mx_o, n, "Max")
    link(val, val_o, n, "Value")
    return n


def custom(mat, name, code, out_type, input_names, x, y):
    n = node(mat, unreal.MaterialExpressionCustom, x, y)
    n.set_editor_property("code", code)
    n.set_editor_property("output_type", out_type)
    n.set_editor_property("description", name)
    ins = []
    for nm in input_names:
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", nm)
        ins.append(ci)
    n.set_editor_property("inputs", ins)
    return n


# ---------------------------------------------------------------- 建材质

def build():
    path = "%s/%s" % (PKG, MAT_NAME)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    mat = TOOLS.create_asset(MAT_NAME, PKG, unreal.Material, unreal.MaterialFactoryNew())
    if not mat:
        fail("建不出 %s" % MAT_NAME)
        return None

    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
    mat.set_editor_property("two_sided", True)
    # 没勾这一条的材质在实例化路径上会被引擎**静默**换成默认材质（一片灰，断言照绿）。
    mat.set_editor_property("used_with_instanced_static_meshes", True)
    # cone-cull 把顶点收向卡心，最大位移 = 卡片半对角（实测 span 40~54 cm），留余量。
    mat.set_editor_property("max_world_position_offset_displacement", 96.0)
    # TG 的 alpha 阈值就是 0.2。
    mat.set_editor_property("opacity_mask_clip_value", 0.2)
    # 纪律③：PDO 在 MaterialProperty 枚举里没有，只能走属性包。
    mat.set_editor_property("use_material_attributes", True)

    attrs = node(mat, unreal.MaterialExpressionMakeMaterialAttributes, 1200, 0)
    if not MEL.connect_material_property(attrs, "", unreal.MaterialProperty.MP_MATERIAL_ATTRIBUTES):
        fail("MakeMaterialAttributes 接不到根节点")

    # ---- 数据源 ----
    uv0 = uv(mat, 0, -2100, -200)          # 卡片角码 (0,0)(1,0)(0,1)(1,1)
    uv1 = uv(mat, 1, -2100, -60)           # prim_center.xy（局部 cm）
    uv2 = uv(mat, 2, -2100, 60)            # prim_center.z 在 .x
    vcol = node(mat, unreal.MaterialExpressionVertexColor, -2100, 180)

    # ---- A. prim_center 重建：float3(UV1.x, UV1.y, UV2.x) -> 世界空间 ----
    uv2x = mask(mat, uv2, "", True, False, False, False, -1900, 60)
    prim_local = node(mat, unreal.MaterialExpressionAppendVector, -1700, 0)
    link(uv1, "", prim_local, "A")
    link(uv2x, "", prim_local, "B")
    prim_ws = node(mat, unreal.MaterialExpressionTransformPosition, -1500, 0)
    prim_ws.set_editor_property("transform_source_type",
                               unreal.MaterialPositionTransformSource.TRANSFORMPOSSOURCE_LOCAL)
    prim_ws.set_editor_property("transform_type",
                               unreal.MaterialPositionTransformSource.TRANSFORMPOSSOURCE_WORLD)
    link(prim_local, "", prim_ws, "")

    # ---- B. 逐卡随机：种子是 prim_center（局部），同一张卡的 6 个顶点恒等 ----
    card_hash = custom(
        mat, "CardHash",
        # 两组独立的 hash（Dave Hoskins 的 hash33 变体），够拿 4 个不相关的随机数。
        "float3 p = frac(Seed * float3(0.1031, 0.1030, 0.0973));\n"
        "p += dot(p, p.yxz + 33.33);\n"
        "float3 r1 = frac((p.xxy + p.yzz) * p.zyx);\n"
        "float3 q = frac((Seed + 19.19) * float3(0.1031, 0.1030, 0.0973));\n"
        "q += dot(q, q.yxz + 33.33);\n"
        "float3 r2 = frac((q.xxy + q.yzz) * q.zyx);\n"
        "return float4(r1.x, r1.y, r2.x, r2.z);",
        unreal.CustomMaterialOutputType.CMOT_FLOAT4, ["Seed"], -1300, 0)
    link(prim_local, "", card_hash, "Seed")
    hash_xy = mask(mat, card_hash, "", True, True, False, False, -1080, -60)
    hash_zw = mask(mat, card_hash, "", False, False, True, True, -1080, 20)
    hash_w = mask(mat, card_hash, "", False, False, False, True, -1080, 100)

    # ---- C. 叶色：随机 UV 采两次色板（TG mode 1 的做法），零叶片贴图 ----
    det_a = scalar(mat, "PaletteDetailA", 0.01, -1300, 220, "第一次采样的卡片内渐变强度（TG 0.01）")
    det_b = scalar(mat, "PaletteDetailB", 0.20, -1300, 300, "第二次采样的卡片内渐变强度（TG 0.20）")
    uv_a = add(mat, hash_xy, "", mul(mat, uv0, "", det_a, "", -1080, 220), "", -880, 180)
    uv_b = add(mat, hash_zw, "", mul(mat, uv0, "", det_b, "", -1080, 300), "", -880, 300)

    pal_a = tex_param(mat, "CanopyPalette", TEX_PALETTE, -640, 160)
    link(uv_a, "", pal_a, "UVs")
    pal_b = tex_param(mat, "CanopyPalette", TEX_PALETTE, -640, 380)
    link(uv_b, "", pal_b, "UVs")

    b_dim = scalar(mat, "PaletteBDim", 0.5, -640, 620, "第二次采样的压暗系数（TG 0.5）")
    pal_b_dim = mul(mat, pal_b, "RGB", b_dim, "", -380, 420)
    # TG: mix(colA, colB, mix(0.1, 0.5, hash))
    mix_lo = scalar(mat, "PaletteMixLo", 0.1, -640, 700)
    mix_hi = scalar(mat, "PaletteMixHi", 0.5, -640, 780)
    mix_t = lerp(mat, mix_lo, "", mix_hi, "", hash_w, "", -380, 720)
    palette = lerp(mat, pal_a, "RGB", pal_b_dim, "", mix_t, "", -180, 260)

    use_baked = scalar(mat, "UseBakedVertexColor", 0.0, -380, 920,
                       "1 = 用网格里烘好的叶色，0 = 走色板（TG 路子）")
    # 纪律②：VertexColor 没有 "RGB" 输出，默认输出 "" 就是 RGB。
    leaf_color = lerp(mat, palette, "", vcol, "", use_baked, "", 60, 300)
    link(leaf_color, "", attrs, "BaseColor")

    # ---- D. 剪影 / height / AO：一张图三个通道 ----
    alpha_tex = tex_param(mat, "CanopyAlpha", TEX_ALPHA, -1300, -420)
    link(uv0, "", alpha_tex, "UVs")
    link(alpha_tex, "R", attrs, "OpacityMask")

    ao_str = scalar(mat, "AOStrength", 1.0, -1000, -260)
    ao = lerp(mat, const(mat, 1.0, -1000, -180), "", alpha_tex, "B", ao_str, "", -760, -240)
    link(ao, "", attrs, "AmbientOcclusion")

    # ---- E. 深度浮雕：只用 PDO，符号与 TG 相反（理由见文件头）----
    bulge = scalar(mat, "DepthBulge", 60.0, -1000, -600, "厚度凸起的深度量 cm；按卡片尺寸等比，不是 TG 的 160；0 = 退回平卡片")
    near = scalar(mat, "DepthBulgeNear", 160.0, -1000, -520, "近端：这个距离内不做浮雕（TG 1.6 m）")
    far = scalar(mat, "DepthBulgeFar", 320.0, -1000, -440, "远端：这个距离外满强度（TG 3.2 m）")
    pix_depth = node(mat, unreal.MaterialExpressionPixelDepth, -1000, -360)
    dist_fade = smoothstep(mat, near, "", far, "", pix_depth, "", -760, -480)
    inv_h = one_minus(mat, alpha_tex, "G", -760, -600)
    pdo = mul(mat, mul(mat, bulge, "", dist_fade, "", -560, -540), "", inv_h, "", -360, -560)
    link(pdo, "", attrs, "PixelDepthOffset")

    # ---- F. cone-cull 缩放淡出：顶点收向卡心 ----
    cam_pos = node(mat, unreal.MaterialExpressionCameraPositionWS, -1500, -1100)
    cam_to_prim = sub(mat, prim_ws, "", cam_pos, "", -1300, -1040)
    d = node(mat, unreal.MaterialExpressionDistance, -1300, -940)
    link(prim_ws, "", d, "A")
    link(cam_pos, "", d, "B")
    dir_cam_to_prim = normalize(mat, cam_to_prim, "", -1100, -1040)

    vnorm = node(mat, unreal.MaterialExpressionVertexNormalWS, -1300, -860)
    ndotv = node(mat, unreal.MaterialExpressionDotProduct, -900, -960)
    link(vnorm, "", ndotv, "A")
    link(dir_cam_to_prim, "", ndotv, "B")

    # TG: k = ndotv - |ndotv| * saturate((d - 5m) * -0.2)；cm 下 -0.2/m = -0.002/cm
    cull_near = scalar(mat, "ConeCullNear", 500.0, -1300, -760, "这个距离内永不剔除（TG 5 m）")
    near_term = mul(mat, sub(mat, d, "", cull_near, "", -1100, -780), "",
                    const(mat, -0.002, -1100, -700), "", -900, -780)
    near_sat = saturate(mat, near_term, "", -700, -780)
    abs_n = node(mat, unreal.MaterialExpressionAbs, -700, -900)
    link(ndotv, "", abs_n, "")
    k = sub(mat, ndotv, "", mul(mat, abs_n, "", near_sat, "", -400, -880), "", -200, -940)

    # TG 的 smoothstep(0.3, -0.2, k) 是反向的；1 - 它 恒等于 smoothstep(-0.2, 0.3, k)
    fade = smoothstep(mat, const(mat, -0.2, -200, -840), "", const(mat, 0.3, -200, -780), "",
                      k, "", 40, -900)
    cull_on = scalar(mat, "ConeCullEnable", 1.0, 40, -780, "0 = 关掉 cone-cull 收缩")
    fade2 = mul(mat, mul(mat, fade, "", fade, "", 240, -900), "", cull_on, "", 440, -900)

    world_pos = node(mat, unreal.MaterialExpressionWorldPosition, -200, -1100)
    to_center = sub(mat, prim_ws, "", world_pos, "", 40, -1080)
    wpo = mul(mat, to_center, "", fade2, "", 640, -1000)
    link(wpo, "", attrs, "WorldPositionOffset")

    # ---- G. 叶球法线：TG 在宽度方向张开 ±0.3，让平卡片看着是圆的 ----
    nb = scalar(mat, "NormalBulge", 0.3, -1000, 1120, "切线空间法线的横向张开量（TG ±0.3）；0 = 平的")
    centered = sub(mat, mul(mat, uv0, "", const(mat, 2.0, -1000, 1200), "", -800, 1120),
                   "", const(mat, 1.0, -800, 1200), "", -600, 1120)
    tn_xy = mul(mat, centered, "", nb, "", -400, 1120)
    tn = node(mat, unreal.MaterialExpressionAppendVector, -200, 1120)
    link(tn_xy, "", tn, "A")
    link(const(mat, 1.0, -400, 1220), "", tn, "B")
    tn_n = normalize(mat, tn, "", 40, 1120)
    link(tn_n, "", attrs, "Normal")

    # ---- H. 透射（缺口②的近似载体）与表面参数 ----
    trans_tint = vector(mat, "TransmissionTint", unreal.LinearColor(0.55, 0.75, 0.25, 1.0), -380, 1000)
    trans_str = scalar(mat, "TransmissionStrength", 0.9, -380, 1080)
    sss = mul(mat, mul(mat, leaf_color, "", trans_tint, "", 260, 980), "", trans_str, "", 460, 980)
    link(sss, "", attrs, "SubsurfaceColor")

    rough = scalar(mat, "Roughness", 0.85, 260, 1240, "TG 的高光很宽很软，粗糙度给高档")
    link(rough, "", attrs, "Roughness")
    spec = scalar(mat, "Specular", 0.25, 260, 1320)
    link(spec, "", attrs, "Specular")

    MEL.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(path)
    log("母材质完成 %s" % path)
    return mat


def build_mi(mat):
    path = "%s/%s" % (PKG, MI_NAME)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    mi = TOOLS.create_asset(MI_NAME, PKG, unreal.MaterialInstanceConstant,
                            unreal.MaterialInstanceConstantFactoryNew())
    if not mi:
        fail("建不出 %s" % MI_NAME)
        return None
    MEL.set_material_instance_parent(mi, mat)
    unreal.EditorAssetLibrary.save_asset(path)
    log("材质实例完成 %s" % path)
    return mi


mat = build()
mi = build_mi(mat) if mat else None

# 参数清单回传，供脚本外核对（材质图本身在 .uasset 里不可读）
params = {}
if mat:
    try:
        for p in MEL.get_scalar_parameter_names(mat):
            params[str(p)] = MEL.get_material_default_scalar_parameter_value(mat, p)
    except Exception as e:
        fail("读参数失败：%s" % e)

result = {
    "material": "%s/%s" % (PKG, MAT_NAME) if mat else None,
    "instance": "%s/%s" % (PKG, MI_NAME) if mi else None,
    "scalar_params": params,
    "failures": FAILURES,
}
with open(OUT_JSON, "w", encoding="utf-8") as f:
    json.dump(result, f, ensure_ascii=False, indent=2)
print("MAKE_CANOPY_DONE failures=%d -> %s" % (len(FAILURES), OUT_JSON))
