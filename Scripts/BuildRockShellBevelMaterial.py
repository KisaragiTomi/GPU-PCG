# -*- coding: utf-8 -*-
"""岩壳假倒角的两份消费材质（**UE 编辑器里跑**，可独立执行，也被 `SetupRockShellBevel.py` 复用）。

1. `graft_rockshell_bevel_into_master()`：把倒角子图嫁接进 TG 母材质 `M_TG_Texture`，挂在
   静态开关 `RockShellBevel`（默认关）后面，只有 `MI_rocky_terrain` 打开它。开关关着的
   458 个 MI 编出来的 Normal 就是 `VertexNormalWS` ⇒ 与 Normal 引脚不接时同一个向量，
   而且静态开关在编译期裁掉整棵倒角子图，它们一条指令都不多付。
   这是运行时岩壳（`ACSGroundActor::RockShellMaterial = MI_rocky_terrain`）真正走的那一份。
2. `build_rockshell_bevel_material()`：独立材质 `M_TinyGladeRockShell`（`M_TinyGladeStone` 复制 +
   倒角子图），给直接摆放的图案 StaticMesh（路线 A）看图用。复制时把 Stone 的
   `lerp(0.88, 1.12, PerInstanceRandom + VertexColor.A)` 明度抖动改成只读 PerInstanceRandom ——
   字典 v2 的 A 是外向角 θ，再让它进明度会绕每块石头扫出一圈明暗和一道接缝。

**两条路线的子图已经分叉**（2026-09-04）：

- 运行时壳（母材质 `M_TG_Texture`）走 **v3**（`add_bevel_subgraph_v3`）：邻接载荷在 UV1..UV5，
  由 `RockShellBevelPayloadCS` 每趟披挂重写，法线照 TG :194 原式混向**真邻面**。
- 直摆的图案 StaticMesh（`M_TinyGladeRockShell`）留在 **v2**（`add_bevel_subgraph`）：资产上没有
  那几条 UV，只能继续用顶点色的慢变量 + 相对旋转（绕折痕切向倒一个假定的半二面角）。

两版共用的口径不变：所有长度常数都是 TG 原生的**图案空间**（5.53 m 胞腔），材质用标量
`RockShellPatternScale` 把世界距离与世界坐标换算回图案空间 —— 运行时壳由
`ACSGroundActor::EnsureRockShellMesh` 的动态子实例写入本 actor 的图案缩放，直摆资产吃默认值 1。
机制、通道字典与参数见 `Docs/TinyGlade/RockShell_EdgeBevel.md`。

用法::

    UnrealEditor-Cmd.exe <uproject> -ExecutePythonScript="<本文件>" -unattended -nosplash -stdout

日志自诊断：最后一行是 `ROCKBEVELMAT DONE standalone=<bool> master=<bool>`。可重跑（幂等）。
"""

import unreal

SRC_MAT = "/PCGPlugins/HouseTest/M_TinyGladeStone"
DST_DIR = "/PCGPlugins/HouseTest"
DST_NAME = "M_TinyGladeRockShell"
DST_MAT = DST_DIR + "/" + DST_NAME

MASTER_MAT = "/PCGPlugins/HouseTest/TinyGladeAsset/Materials/M_TG_Texture"
SHELL_MI = "/PCGPlugins/HouseTest/TinyGladeAsset/Materials/MI_rocky_terrain"
SWITCH_NAME = "RockShellBevel"
GROUP = "Rock Shell"      # 与 TinyGladeMakeRockShellShading.py 的裙压暗参数同一组

lib = unreal.MaterialEditingLibrary
ea = unreal.EditorAssetLibrary


def _log(msg):
    unreal.log("ROCKBEVELMAT %s" % msg)


def _save(path):
    """存盘并**核对返回值**。`ea.save_asset` 失败只返回 False，不抛 —— 2026-09-04 撞过一次：
    `FUncontrolledChangelistsDiscoverAssetsTask` 正好在扫同一批 .uasset，SavePackage 的
    「先把原文件移走」拿到 ERROR_SHARING_VIOLATION（Error Code 32），改动全留在内存里没落盘，
    而脚本照样报 `DONE master=True`。假绿比不绿贵得多，所以这里必须喊出来。"""
    if ea.save_asset(path):
        return True
    unreal.log_error("ROCKBEVELMAT 存盘失败 %s —— 改动只在内存里。多半是别的进程或引擎自己的"
                     "资产扫描占着文件（Error 32），重跑一次通常就好。" % path)
    return False



def _custom_input(name):
    ci = unreal.CustomInput()
    ci.set_editor_property("input_name", name)
    return ci


def _scalar_param(mat, name, default, x, y):
    e = lib.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, x, y)
    e.set_editor_property("parameter_name", name)
    e.set_editor_property("default_value", default)
    e.set_editor_property("group", GROUP)
    return e


def add_bevel_subgraph(mat, x, y):
    """在 (x, y) 附近建整套倒角子图，返回输出**世界空间**法线的 Custom 节点。
    子图里每个节点都是新建的、不接任何已有节点 ⇒ 上游整棵树都归本子图所有，可整棵删（幂等重跑）。"""
    vc = lib.create_material_expression(mat, unreal.MaterialExpressionVertexColor, x - 750, y)
    wp = lib.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, x - 750, y + 200)
    vn = lib.create_material_expression(mat, unreal.MaterialExpressionVertexNormalWS, x - 750, y + 350)
    # 运行时由 ACSGroundActor 的动态子实例写入图案缩放；直摆资产用默认 1
    p_scale = _scalar_param(mat, "RockShellPatternScale", 1.0, x - 750, y + 500)

    # 噪声域：图案空间 XY + 逐石头相位偏移 + 沿 Z 变化（对应 TG 的 cell_id×0.1 与 y×0.5）。
    # 世界坐标先除以图案缩放 ⇒ 噪声特征尺寸随胞腔一起缩。
    dom = lib.create_material_expression(mat, unreal.MaterialExpressionCustom, x - 500, y + 150)
    dom.set_editor_property("description", "RockBevelNoiseDomain")
    dom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    dom.set_editor_property("inputs", [_custom_input("WP"), _custom_input("Phase"), _custom_input("PatternScale")])
    dom.set_editor_property("code",
        "float3 p = WP / max(PatternScale, 1e-3);\n"
        "return float3(p.x + Phase * 1370.0 + p.z * 0.5,\n"
        "              p.y + Phase * 2710.0 + p.z * 0.5, 0.0);")
    lib.connect_material_expressions(wp, "", dom, "WP")
    lib.connect_material_expressions(vc, "B", dom, "Phase")
    lib.connect_material_expressions(p_scale, "", dom, "PatternScale")

    # 单倍频 value noise（TG displace:618 的对位物；域单位图案 cm，scale 0.01 → 特征 ~1 m）
    noi = lib.create_material_expression(mat, unreal.MaterialExpressionNoise, x - 250, y + 150)
    noi.set_editor_property("noise_function", unreal.NoiseFunction.NOISEFUNCTION_VALUE_ALU)
    noi.set_editor_property("scale", 0.01)
    noi.set_editor_property("levels", 1)
    noi.set_editor_property("turbulence", False)
    noi.set_editor_property("output_min", -1.0)
    noi.set_editor_property("output_max", 1.0)
    lib.connect_material_expressions(dom, "", noi, "Position")

    # 参数（带宽/咬深照抄 TG，图案 cm；HalfDihedral 是相对旋转的假定半二面角）
    p_band_cap = _scalar_param(mat, "BevelBandCap", 20.0, x - 500, y + 450)
    p_band_skirt = _scalar_param(mat, "BevelBandSkirt", 8.0, x - 500, y + 550)
    p_amp_cap = _scalar_param(mat, "BevelNoiseAmpCap", 50.0, x - 500, y + 650)
    p_amp_skirt = _scalar_param(mat, "BevelNoiseAmpSkirt", 25.0, x - 500, y + 750)
    p_dist_max = _scalar_param(mat, "BevelDistMax", 50.0, x - 500, y + 850)      # = VertexColor::RimDistMaxCm
    p_theta_flip = _scalar_param(mat, "BevelThetaFlip", 1.0, x - 500, y + 950)   # 导入轴翻转时改 -1（倒角弯错边）
    p_strength = _scalar_param(mat, "BevelStrength", 0.5, x - 500, y + 1050)     # TG 的"弯一半"
    p_half_dihedral = _scalar_param(mat, "BevelHalfDihedral", 45.0, x - 500, y + 1150)  # 度：满强度时倒过去的角

    # 逐像素假倒角 · 方案乙（相对旋转）：绕折痕切向把自己的法线倒 HalfDihedral×Strength×t，
    # 盖往外倒、裙往上倒。相对自己的面旋转 ⇒ 披挂后盖裙怎么倾都自动跟随。
    bev = lib.create_material_expression(mat, unreal.MaterialExpressionCustom, x, y + 300)
    bev.set_editor_property("description", "RockShellEdgeBevel")
    bev.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    bev.set_editor_property("inputs", [
        _custom_input("CapF"), _custom_input("DistN"), _custom_input("ThetaN"),
        _custom_input("NoiseVal"), _custom_input("NormalWS"),
        _custom_input("BandCap"), _custom_input("BandSkirt"),
        _custom_input("AmpCap"), _custom_input("AmpSkirt"),
        _custom_input("DistMax"), _custom_input("ThetaFlip"),
        _custom_input("Strength"), _custom_input("HalfDihedral"),
        _custom_input("PatternScale")])
    bev.set_editor_property("code",
        "float d = DistN * DistMax / max(PatternScale, 1e-3);   // 世界 cm -> 图案 cm\n"
        "float band = CapF > 0.5 ? BandCap : BandSkirt;\n"
        "float amp  = CapF > 0.5 ? AmpCap  : AmpSkirt;\n"
        "float t = 1.0 - min(smoothstep(0.0, band, d + NoiseVal * amp),\n"
        "                    smoothstep(0.0, band * 0.5, d));\n"
        "float th = ThetaN * 6.28318530718;\n"
        "float3 o = float3(cos(th), ThetaFlip * sin(th), 0.0);\n"
        "float3 T = normalize(cross(float3(0.0, 0.0, 1.0), o));\n"
        "float3 axis = CapF > 0.5 ? T : -T;\n"
        "float ang = HalfDihedral * 0.01745329252 * Strength * t;\n"
        "float3 N = normalize(NormalWS);\n"
        "float s; float c; sincos(ang, s, c);\n"
        "return normalize(N * c + cross(axis, N) * s + axis * (dot(axis, N) * (1.0 - c)));")
    for src, out, pin in ((vc, "R", "CapF"), (vc, "G", "DistN"), (vc, "A", "ThetaN"),
                          (noi, "", "NoiseVal"), (vn, "", "NormalWS"),
                          (p_band_cap, "", "BandCap"), (p_band_skirt, "", "BandSkirt"),
                          (p_amp_cap, "", "AmpCap"), (p_amp_skirt, "", "AmpSkirt"),
                          (p_dist_max, "", "DistMax"), (p_theta_flip, "", "ThetaFlip"),
                          (p_strength, "", "Strength"), (p_half_dihedral, "", "HalfDihedral"),
                          (p_scale, "", "PatternScale")):
        lib.connect_material_expressions(src, out, bev, pin)
    return bev




def _texcoord(mat, index, x, y):
    tc = lib.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate, x, y)
    tc.set_editor_property("coordinate_index", index)
    return tc


def add_bevel_subgraph_v3(mat, x, y):
    """岩壳假倒角 v3 子图：邻接走 UV1..UV5，法线**混向真邻面**（TG :194 原式）。
    返回输出**世界空间**法线的 Custom 节点；子图内每个节点都是新建的 ⇒ 可整棵删（幂等重跑）。

    与 v2（`add_bevel_subgraph`）的三处实质不同：
      1. 折痕距离不再是顶点色 G 的 8 bit 归一值，而是 UV1/UV2 里 one-hot 三角形高插值出来的
         **精确垂距**（世界 cm）—— 参数 `BevelDistMax` 随之消失。
      2. 不再绕假想轴倒固定角，而是照 TG 原式 `lerp(自身法线, 邻面法线, t × Strength)` ——
         `BevelThetaFlip` / `BevelHalfDihedral` 随之消失（外向角 θ 与假定半二面角都不需要了）。
      3. 三条边各带一个邻面法线、逐像素按最近边挑 ⇒ 盖-盖相邻边也吃得到倒角，v2 只做盖-裙折痕。

    载荷由 `RockShellBevelPayloadCS` 每趟披挂重写，通道字典见 `CSGroundRockShell.h` 的
    `namespace TexCoord`。⚠️ 因此这份子图**只能给运行时壳用** —— 直摆的图案 StaticMesh 没有
    这些 UV，那条路继续走 v2（`build_rockshell_bevel_material`）。
    """
    vc = lib.create_material_expression(mat, unreal.MaterialExpressionVertexColor, x - 750, y)
    wp = lib.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, x - 750, y + 200)
    vn = lib.create_material_expression(mat, unreal.MaterialExpressionVertexNormalWS, x - 750, y + 350)
    p_scale = _scalar_param(mat, "RockShellPatternScale", 1.0, x - 750, y + 500)

    # UV 通道字典（CSGroundRockShell.h::TexCoord）：1=(d0,d1) 2=(d2,_) 3/4/5=oct(邻面法线 0/1/2)
    uv_d01 = _texcoord(mat, 1, x - 750, y - 400)
    uv_d2 = _texcoord(mat, 2, x - 750, y - 300)
    uv_n0 = _texcoord(mat, 3, x - 750, y - 200)
    uv_n1 = _texcoord(mat, 4, x - 750, y - 100)
    uv_n2 = _texcoord(mat, 5, x - 750, y - 20)
    uv_nf = _texcoord(mat, 6, x - 750, y + 60)   # 自身面法线：材质能画出硬边的前提

    # 噪声域与 v2 同式：图案空间 XY + 逐石头相位 + 沿 Z 变化（TG 的 cell_id×0.1 与 y×0.5）
    dom = lib.create_material_expression(mat, unreal.MaterialExpressionCustom, x - 500, y + 150)
    dom.set_editor_property("description", "RockBevelNoiseDomainV3")
    dom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    dom.set_editor_property("inputs", [_custom_input("WP"), _custom_input("Phase"), _custom_input("PatternScale")])
    dom.set_editor_property("code", """float3 p = WP / max(PatternScale, 1e-3);
return float3(p.x + Phase * 1370.0 + p.z * 0.5,
              p.y + Phase * 2710.0 + p.z * 0.5, 0.0);""")
    lib.connect_material_expressions(wp, "", dom, "WP")
    lib.connect_material_expressions(vc, "B", dom, "Phase")
    lib.connect_material_expressions(p_scale, "", dom, "PatternScale")

    noi = lib.create_material_expression(mat, unreal.MaterialExpressionNoise, x - 250, y + 150)
    noi.set_editor_property("noise_function", unreal.NoiseFunction.NOISEFUNCTION_VALUE_ALU)
    noi.set_editor_property("scale", 0.01)
    noi.set_editor_property("levels", 1)
    noi.set_editor_property("turbulence", False)
    noi.set_editor_property("output_min", -1.0)
    noi.set_editor_property("output_max", 1.0)
    lib.connect_material_expressions(dom, "", noi, "Position")

    p_band_cap = _scalar_param(mat, "BevelBandCap", 20.0, x - 500, y + 450)
    p_band_skirt = _scalar_param(mat, "BevelBandSkirt", 8.0, x - 500, y + 550)
    p_amp_cap = _scalar_param(mat, "BevelNoiseAmpCap", 50.0, x - 500, y + 650)
    p_amp_skirt = _scalar_param(mat, "BevelNoiseAmpSkirt", 25.0, x - 500, y + 750)
    p_strength = _scalar_param(mat, "BevelStrength", 0.5, x - 500, y + 850)   # TG :194 的 0.5
    # 0 = TG 原式（`mix(自身面法线, 邻面面法线, t × 0.5)`）；1 = band 内取 chamfer 小面的常数
    # 法线，两条边界各一道真硬边。**默认 0 = TG 原样**（2026-09-04 用户：先按 TG 的方案做一版）。
    # 硬边那一档保留为可选：它偏离 TG，见 Docs/TinyGlade/RockShell_EdgeBevel.md 的「硬边」节。
    p_hardness = _scalar_param(mat, "BevelHardness", 0.0, x - 500, y + 950)
    p_hard_th = _scalar_param(mat, "BevelHardThreshold", 0.5, x - 500, y + 1050)

    bev = lib.create_material_expression(mat, unreal.MaterialExpressionCustom, x, y + 300)
    bev.set_editor_property("description", "RockShellEdgeBevelV3")
    bev.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    bev.set_editor_property("inputs", [
        _custom_input("CapF"), _custom_input("D01"), _custom_input("D2"),
        _custom_input("N0"), _custom_input("N1"), _custom_input("N2"), _custom_input("FaceN"),
        _custom_input("NoiseVal"), _custom_input("NormalWS"),
        _custom_input("BandCap"), _custom_input("BandSkirt"),
        _custom_input("AmpCap"), _custom_input("AmpSkirt"),
        _custom_input("Strength"), _custom_input("PatternScale"),
        _custom_input("Hardness"), _custom_input("HardThreshold")])
    bev.set_editor_property("code", """// 三条边的垂距（世界 cm）。one-hot 三角形高插值出来的精确值，不是顶点插值近似。
float3 d3 = float3(D01.x, D01.y, D2.x);
float d = min(min(d3.x, d3.y), d3.z);
// d 是三者之一的原值（没经过任何运算）⇒ 相等比较精确。与 TG ps_main:104 同一种写法，但那边比的
// 是原始重心坐标：各向异性三角形上 min(lambda) != min(lambda*h)，它会选错边，这里不会。
// 轮廓边的「高」被 kernel 顶成 1e6 ⇒ 永远不中选，倒角带在石头外沿自然收口。
float2 e = (d == d3.x) ? N0 : ((d == d3.y) ? N1 : N2);

// 八面体解码 ×2（编码在 CSGroundRockShell.usf::CSRockShell_OctEncode）。
// HLSL 不能在 Custom 节点里定义辅助函数，所以照抄两遍而不是抽出来。
float3 nn = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
if (nn.z < 0.0) nn.xy = (1.0 - abs(float2(nn.y, nn.x)))
                      * float2(nn.x >= 0.0 ? 1.0 : -1.0, nn.y >= 0.0 ? 1.0 : -1.0);
float3 nf = float3(FaceN.x, FaceN.y, 1.0 - abs(FaceN.x) - abs(FaceN.y));
if (nf.z < 0.0) nf.xy = (1.0 - abs(float2(nf.y, nf.x)))
                      * float2(nf.x >= 0.0 ? 1.0 : -1.0, nf.y >= 0.0 ? 1.0 : -1.0);
float3 Nn = normalize(nn);          // 邻面的面法线
float3 Nf = normalize(nf);          // 自身的面法线
float3 Ns = normalize(NormalWS);    // 全平均的平滑法线（几何这一层不表达硬边）

// 带宽 / 咬深是 TG 原生的图案空间口径 ⇒ 把世界距离除回图案空间再套那批常数
float dp = d / max(PatternScale, 1e-3);
float band = CapF > 0.5 ? BandCap : BandSkirt;
float amp  = CapF > 0.5 ? AmpCap  : AmpSkirt;
float t = 1.0 - min(smoothstep(0.0, band, dp + NoiseVal * amp),
                    smoothstep(0.0, band * 0.5, dp));

// 软 = TG :194 原式，混向邻面。⚠️ 折痕两侧都收敛到同一个值 ⇒ 数学上必然是圆的，画不出硬边。
float3 Soft = lerp(Ns, Nn, t * Strength);

// 硬 = band 内整块换成 chamfer 小面的**常数**法线（自身与邻面的平分向量）。
// step 不做渐变 ⇒ band 的两条边界各是一道真硬边；而边界位置本身带噪声，
// 所以啃出来的缺口边缘也是硬的 —— 这正是「硬边由材质画」要的东西。
float3 Bi = Nf + Nn;
float BiLen = length(Bi);
float3 Nb = (BiLen > 1e-4) ? (Bi / BiLen) : Nf;   // 对折退化（Nf ≈ -Nn）时退回自身面法线
float3 Hard = lerp(Ns, Nb, step(HardThreshold, t));

return normalize(lerp(Soft, Hard, saturate(Hardness)));""")
    for src, out, pin in ((vc, "R", "CapF"), (uv_d01, "", "D01"), (uv_d2, "", "D2"),
                          (uv_n0, "", "N0"), (uv_n1, "", "N1"), (uv_n2, "", "N2"),
                          (uv_nf, "", "FaceN"),
                          (noi, "", "NoiseVal"), (vn, "", "NormalWS"),
                          (p_band_cap, "", "BandCap"), (p_band_skirt, "", "BandSkirt"),
                          (p_amp_cap, "", "AmpCap"), (p_amp_skirt, "", "AmpSkirt"),
                          (p_strength, "", "Strength"), (p_scale, "", "PatternScale"),
                          (p_hardness, "", "Hardness"), (p_hard_th, "", "HardThreshold")):
        lib.connect_material_expressions(src, out, bev, pin)
    return bev


def _delete_subtree(mat, root):
    """删掉 root 及其全部上游表达式。只用于本脚本自建的子图（它不接任何已有节点）。"""
    seen, order, stack = set(), [], [root]
    while stack:
        node = stack.pop()
        if node is None:
            continue
        key = node.get_path_name()
        if key in seen:
            continue
        seen.add(key)
        order.append(node)
        stack.extend(lib.get_inputs_for_material_expression(mat, node))
    for node in order:
        lib.delete_material_expression(mat, node)
    return len(order)


def _owns_bevel_switch(mat, node, depth=0):
    """node 是不是本脚本挂在 Normal 上的那棵树（根是 TwoSidedSign 乘法，下面是 RockShellBevel 开关）。"""
    if node is None or depth > 2:
        return False
    if (isinstance(node, unreal.MaterialExpressionStaticSwitchParameter)
            and str(node.get_editor_property("parameter_name")) == SWITCH_NAME):
        return True
    return any(_owns_bevel_switch(mat, n, depth + 1) for n in lib.get_inputs_for_material_expression(mat, node))


def _first_of(nodes, cls):
    for n in nodes:
        if isinstance(n, cls):
            return n
    return None


def _fix_stone_jitter(mat):
    """把复制自 Stone 的明度抖动 `lerp(0.88, 1.12, PerInstanceRandom + VertexColor.A)` 改成只读
    PerInstanceRandom：字典 v2 的 A 是外向角，不能再进明度。找不到那条支路就原样保留并警告。"""
    tint = lib.get_material_property_input_node(mat, unreal.MaterialProperty.MP_BASE_COLOR)
    lerp = _first_of(lib.get_inputs_for_material_expression(mat, tint), unreal.MaterialExpressionLinearInterpolate) \
        if isinstance(tint, unreal.MaterialExpressionMultiply) else None
    add = _first_of(lib.get_inputs_for_material_expression(mat, lerp), unreal.MaterialExpressionAdd) if lerp else None
    add_inputs = lib.get_inputs_for_material_expression(mat, add) if add else []
    rnd = _first_of(add_inputs, unreal.MaterialExpressionPerInstanceRandom)
    vcol = _first_of(add_inputs, unreal.MaterialExpressionVertexColor)
    if rnd is None or vcol is None:
        unreal.log_warning("ROCKBEVELMAT 没在 %s 里找到 Stone 的 PerInstanceRandom + VertexColor.A 抖动支路，"
                           "明度抖动原样保留（若 Stone 的图变了，这里的匹配要跟着改）" % DST_MAT)
        return False
    lib.connect_material_expressions(rnd, "", lerp, "Alpha")
    lib.delete_material_expression(mat, add)
    lib.delete_material_expression(mat, vcol)
    _log("stone brightness jitter now reads PerInstanceRandom only (VertexColor.A is theta)")
    return True


def build_rockshell_bevel_material():
    """独立材质 `M_TinyGladeRockShell`：直摆图案 StaticMesh（路线 A）的消费端。"""
    if ea.does_asset_exist(DST_MAT):
        _log("existing %s deleted for clean rebuild" % DST_MAT)
        ea.delete_asset(DST_MAT)

    if ea.does_asset_exist(SRC_MAT):
        mat = ea.duplicate_asset(SRC_MAT, DST_MAT)
        _log("duplicated %s" % SRC_MAT)
        blank = False
    else:
        mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            DST_NAME, DST_DIR, unreal.Material, unreal.MaterialFactoryNew())
        unreal.log_warning("ROCKBEVELMAT %s 不存在，退回从零建（BaseColor 用参数色）" % SRC_MAT)
        blank = True
    if not mat:
        unreal.log_error("ROCKBEVELMAT 材质创建失败")
        return None

    if blank:
        col = lib.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -950, 700)
        col.set_editor_property("parameter_name", "StoneColor")
        col.set_editor_property("default_value", unreal.LinearColor(0.45, 0.43, 0.40, 1.0))
        lib.connect_material_property(col, "", unreal.MaterialProperty.MP_BASE_COLOR)
    else:
        _fix_stone_jitter(mat)

    bev = add_bevel_subgraph(mat, -700, 1000)
    # 世界空间法线输出：图案网格没有切线基，也绕开切线空间换算（Stone 原来接在 Normal 上的
    # 三平面法线贴图随之被顶掉 —— 直摆看图不需要它）
    mat.set_editor_property("tangent_space_normal", False)
    lib.connect_material_property(bev, "", unreal.MaterialProperty.MP_NORMAL)

    lib.recompile_material(mat)
    if not _save(DST_MAT): return None
    _log("material %s ready (相对旋转版，HalfDihedral=45，PatternScale=1)" % DST_MAT)
    return mat


def graft_rockshell_bevel_into_master():
    """把倒角子图嫁接进 `M_TG_Texture` 的 Normal 引脚（静态开关 `RockShellBevel` 守卫），并给
    `MI_rocky_terrain` 打开开关。返回是否成功。"""
    mat = ea.load_asset(MASTER_MAT)
    if not isinstance(mat, unreal.Material):
        unreal.log_error("ROCKBEVELMAT 母材质 %s 不存在或不是 Material" % MASTER_MAT)
        return False

    normal_node = lib.get_material_property_input_node(mat, unreal.MaterialProperty.MP_NORMAL)
    if normal_node is not None:
        is_ours = _owns_bevel_switch(mat, normal_node)
        if not is_ours:
            # 不猜：Normal 上接着别人的东西时覆盖掉会静默改掉 459 个 MI 的法线。
            unreal.log_error("ROCKBEVELMAT %s 的 Normal 引脚已接着 %s，不是本脚本的 %s 开关 —— 人工确认后再跑"
                             % (MASTER_MAT, normal_node.get_class().get_name(), SWITCH_NAME))
            return False
        _log("old bevel subtree removed from %s (%d nodes)" % (MASTER_MAT, _delete_subtree(mat, normal_node)))

    # 运行时壳走 v3：载荷在 UV1..UV5，由 RockShellBevelPayloadCS 每趟披挂重写。
    # 直摆资产没有这些 UV，那条路留在 v2（build_rockshell_bevel_material）。
    bev = add_bevel_subgraph_v3(mat, -400, 1300)
    vn_off = lib.create_material_expression(mat, unreal.MaterialExpressionVertexNormalWS, -400, 1100)
    sw = lib.create_material_expression(mat, unreal.MaterialExpressionStaticSwitchParameter, -150, 1200)
    sw.set_editor_property("parameter_name", SWITCH_NAME)
    sw.set_editor_property("default_value", False)
    sw.set_editor_property("group", GROUP)
    # 静态开关的两个引脚按序取名（True / False），不写死拼法
    pins = [str(p) for p in lib.get_material_expression_input_names(sw)]
    if len(pins) < 2:
        unreal.log_error("ROCKBEVELMAT 静态开关的输入引脚只有 %s" % pins)
        return False
    lib.connect_material_expressions(bev, "", sw, pins[0])       # True：倒角
    lib.connect_material_expressions(vn_off, "", sw, pins[1])    # False：原样顶点法线 = 不接 Normal 时的那根
    # 两支都是世界空间法线，而引擎只给**切线空间**法线乘 TwoSidedSign（MaterialTemplate.ush：
    # `#if MATERIAL_TANGENTSPACENORMAL ... WorldNormal *= TwoSidedSign`）。母材质是 two-sided，
    # 背面的翻号得自己补上 —— 补上之后开关关着的那支 = VertexNormalWS × TwoSidedSign，
    # 与 Normal 不接时引擎算出的向量逐位同义，其余 458 个 MI 的正反面着色都不变；
    # 静态开关又让它们的 shader 里根本不含倒角那棵树。
    tss = lib.create_material_expression(mat, unreal.MaterialExpressionTwoSidedSign, -150, 1400)
    signed = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, 60, 1250)
    lib.connect_material_expressions(sw, "", signed, "A")
    lib.connect_material_expressions(tss, "", signed, "B")
    mat.set_editor_property("tangent_space_normal", False)
    lib.connect_material_property(signed, "", unreal.MaterialProperty.MP_NORMAL)
    lib.recompile_material(mat)
    if not _save(MASTER_MAT): return False
    _log("bevel grafted into %s behind static switch %s (default off, pins %s)" % (MASTER_MAT, SWITCH_NAME, pins[:2]))

    mi = ea.load_asset(SHELL_MI)
    if not isinstance(mi, unreal.MaterialInstanceConstant):
        unreal.log_error("ROCKBEVELMAT %s 不存在或不是 MaterialInstanceConstant" % SHELL_MI)
        return False
    lib.set_material_instance_static_switch_parameter_value(mi, SWITCH_NAME, True)
    lib.update_material_instance(mi)
    if not _save(SHELL_MI): return False
    _log("%s: %s = True" % (SHELL_MI, SWITCH_NAME))
    return True


# 被 SetupRockShellBevel.py exec 复用时设 ROCKBEVEL_AS_LIBRARY=True 跳过自动执行
# （exec 继承调用方 __name__，不能用 __main__ 守卫）。
if not globals().get("ROCKBEVEL_AS_LIBRARY"):
    _ok_standalone = build_rockshell_bevel_material() is not None
    _ok_master = graft_rockshell_bevel_into_master()
    _log("DONE standalone=%s master=%s" % (_ok_standalone, _ok_master))
