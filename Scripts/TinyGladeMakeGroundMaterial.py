# -*- coding: utf-8 -*-
"""原位重建 TG 三层地面：草地 → 深色土 → 浅色土路。

依据 _terrain_editing_floor PS 第 422–454、520 行：三张颜色图加一张
高度混合图；不同 smoothstep 区间产生深色路缘，高度低谷露出深色土斑。
_paths_path_mask_visual CS 最后一行的 dirt_mask 是各类路径遮罩的 max。
本项目目前只有 R 通道土路，因此 Road 与 DirtMask 共用同一个遮罩：

- 默认读**地面遮罩**贴图 GroundMask（ACSGroundActor::GroundMask：顶点色 R 在 GPU 上平滑 + 向外延伸，
  草 / 花 / 灌木读的也是它 —— 2026-09-29 用户"地形 mask 决定草，经过处理的地面 mask 作为地面 mesh 的
  遮罩控制贴图"）。遮罩 UV = UV0 × GroundMaskUVTransform.xy + .zw，两个参数都由地面 actor 的动态实例喂；
- 读地面遮罩时照 TG 的算法（floor PS 第 416–457、520 行）：深土与浅土都对**同一个**路径遮罩做高度竞争，
  W = HeightWeight(遮罩.G, dirtpath_heightmap)，深土 smoothstep(0.2, 0.65, W)、浅土 smoothstep(0.6, 1.0, W)。
  G 是只糊、不延伸的场 = TG 的 path_mask_blurred；深浅土的分界因此由高度噪声图决定 —— 路边是深浅交错的斑块，
  路面里高度图的低谷露深色土斑（用户 2026-09-29 给了 TG 道路截图："还是需要一些 noise 贴图辅助表现"，
  "直接看 tg 的反编译结果"）。此前一版按 G 取硬阈值（PathStart/End）再做竞争，噪声没地方发挥，成了一条均匀的深色带；
- 在 TG 的基础上只多一步：两层土都乘草的拒绝曲线 smoothstep(GroundMaskSoilStart, GroundMaskSoilEnd, 遮罩.R)，
  两端由地面 actor 按草的 MaskStart / MaskEnd 喂 —— 草长满处不露土（用户 2026-09-29："现在看来草长在了土上"；
  TG 原版在遮罩约 0.01 起就可能冒土斑，草密时看不出来）；
- 采样遮罩前先按 GroundMaskWarp 挪采样点（TG 的 path mask domain warp，Shaders/Private/CSGroundMaskWarp.ush，
  Custom 节点经 IncludeFilePaths 引它），与草 / 花 / 灌木散布同一个函数、同一对参数 —— 交界不再是路缘的等距外扩
  （用户 2026-09-29："交界过于生硬"）；
- GroundMaskEnabled = 0（没有地面 actor 喂参数时的默认）退回 VertexColor.R，材质照老办法画。
  参数名与 C++ 的 CSGroundMaskMaterial 必须逐字相同。

UV0 = 地面局部 XY / UVWorldPeriod（默认 500 cm），等价于 TG 米坐标 * 0.2。
再乘原版各层的 0.8 / 1.2 / 0.5 / 0.4，不能让四张图共用同一个平铺频率。
提取清单中 dirtpath_heightmap 是 BC7_SRGB；保留其 sRGB 采样，不按名字改色域。
草地图必须用 dirtpath_grass；grass_patch_summer 是带白色透明边框的草叶卡片。

保留材质与实例资产身份、已有 Grass/Dirt 参数覆盖，不切换或保存关卡。
详细证据和用户参考图见 Docs/TinyGlade/Ground_Material.md。
"""
import unreal

PKG = '/PCGPlugins/HouseTest'
TEX = PKG + '/TinyGladeAsset/Textures'
MEL = unreal.MaterialEditingLibrary

GROUND_HLSL = '''
struct TGGroundLayers
{
    float HeightWeight(float mask, float height)
    {
        float t = saturate(sqrt(saturate(mask)) * 0.6);
        float grassHeight = 0.15 * (1.0 - t);
        float dirtHeight = height * t;
        float cutoff = max(grassHeight, dirtHeight) - 0.1;
        float dirt = max(dirtHeight - cutoff, 0.0);
        return dirt / max(max(grassHeight - cutoff, 0.0) + dirt, 1e-6);
    }
};
TGGroundLayers layers;
float road = layers.HeightWeight(Road, Height);
float soil = layers.HeightWeight(DirtMask, Height);
// Gate = 这里不长草的概率（草的拒绝曲线）：两层土都乘它，草长满处一点土都不露。没有地面遮罩时恒 1 = 原版。
float soilAlpha = smoothstep(0.2, 0.65, soil) * Gate;
float pathAlpha = smoothstep(0.6, 1.0, road) * Gate;
// 原版先铺深色土，再覆盖浅土；低高度区域会从浅土中露出深土。
return lerp(lerp(Grass, DarkDirt * 1.1, soilAlpha), Dirt * 1.1, pathAlpha);
'''

# 地面遮罩 (R, G) → (TG 的路径遮罩, 草闸门)。x = G（只糊、已扰动 = TG 的 path_mask_blurred），深土与浅土都拿它做高度竞争；
# y = 草的拒绝曲线（与 CSGroundCover.usf 的 `smoothstep(CoverMaskStart, CoverMaskEnd, Mask)` 同一条，读 R）。
# 遮罩 UV：UV0 × 周期 = 地面局部坐标（cm），与散布那边的 P − 地面原点同一个原点；位移换回 UV0 再走原来的 xy/zw 换算。
WARP_INCLUDE = '/Plugin/PCGPlugins/Shaders/Private/CSGroundMaskWarp.ush'
MASK_UV_HLSL = '''
float Period = max(Warp.z, 1.0);
float2 Offset = CSGroundMaskWarp_Offset(UV * Period, Warp.y, Warp.x);
return (UV + Offset / Period) * Xform.xy + Xform.zw;
'''

# Sharpness：以画出来的路缘（G = 0.5）为中心把 G 拉陡再交给高度竞争。G 的模糊宽度跟着 GroundMaskExtendDistance（草要的
# "平滑延申"），比 TG 的 path_mask_blurred 宽，同一张噪声图在宽坡上切出来的边是糊的；拉陡 = 更接近 TG 那种碎而清楚的边。
# 1 = 原样。只影响地面着色，草不读它。
SOIL_REMAP_HLSL = '''
float path = saturate((Inner - 0.5) * Sharpness + 0.5);
return float2(path, smoothstep(SoilStart, max(SoilEnd, SoilStart + 1e-3), Mask));
'''
FLOOR_SHARPNESS_DEFAULT = 1.0


def build():
    layers = [('Grass', 'dirtpath_grass', 0.8),
              ('DarkDirt', 'dirtpath_2', 1.2),
              ('Dirt', 'dirtpath_1', 0.5),
              ('GroundBlendHeight', 'dirtpath_heightmap', 0.4)]
    textures = {name: unreal.load_asset(TEX + '/' + asset) for name, asset, _ in layers}
    for name, texture in textures.items():
        if not texture:
            raise RuntimeError('Missing TG ground texture: ' + name)
        if not texture.get_editor_property('srgb'):
            raise RuntimeError('Expected original BC7_SRGB sampling: ' + texture.get_path_name())

    mat = unreal.load_asset(PKG + '/M_TinyGladeGround')
    if not mat:
        mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            'M_TinyGladeGround', PKG, unreal.Material, unreal.MaterialFactoryNew())
    # 本版本批量删节点一次只移除部分节点；循环清空，但不删除材质资产本身。
    for _ in range(16):
        if MEL.get_num_material_expressions(mat) == 0:
            break
        MEL.delete_all_material_expressions(mat)
    if MEL.get_num_material_expressions(mat):
        raise RuntimeError('Could not clear the ground graph')
    mat.set_editor_property('blend_mode', unreal.BlendMode.BLEND_OPAQUE)
    mat.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_DEFAULT_LIT)

    def node(cls, x, y):
        return MEL.create_material_expression(mat, cls, x, y)

    uv = node(unreal.MaterialExpressionTextureCoordinate, -1100, 0)
    uv.set_editor_property('coordinate_index', 0)
    samples = {}
    for i, (name, _, scale) in enumerate(layers):
        tiling = node(unreal.MaterialExpressionMultiply, -870, i * 230)
        tiling.set_editor_property('const_b', scale)
        assert MEL.connect_material_expressions(uv, '', tiling, 'A')
        sample = node(unreal.MaterialExpressionTextureSampleParameter2D, -620, i * 230)
        sample.set_editor_property('parameter_name', name)
        sample.set_editor_property('texture', textures[name])
        sample.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
        assert MEL.connect_material_expressions(tiling, '', sample, 'UVs')
        samples[name] = sample
    vertex = node(unreal.MaterialExpressionVertexColor, -620, 970)

    # 地面遮罩：UV = (UV0 + 扰动位移 / 周期) × xy + zw（地面 actor 算好喂进来：一个 texel 对一个地面格点、中心对齐）。
    # 扰动 = TG 的 path mask domain warp，与草 / 花 / 灌木散布**同一个函数、同一对参数**（CSGroundMaskWarp.ush，
    # GroundMaskWarp = (强度 cm, 1/波长, UVWorldPeriod, 0)）—— 土边抖到哪草线跟到哪。默认强度 0 = 不扰动。
    mask_xform = node(unreal.MaterialExpressionVectorParameter, -1100, 1150)
    mask_xform.set_editor_property('parameter_name', 'GroundMaskUVTransform')
    mask_xform.set_editor_property('default_value', unreal.LinearColor(0.0, 0.0, 0.0, 0.0))
    mask_warp = node(unreal.MaterialExpressionVectorParameter, -1100, 1300)
    mask_warp.set_editor_property('parameter_name', 'GroundMaskWarp')
    mask_warp.set_editor_property('default_value', unreal.LinearColor(0.0, 0.0, 500.0, 0.0))
    mask_uv = node(unreal.MaterialExpressionCustom, -700, 1150)
    mask_uv.set_editor_property('description', 'Ground mask UV + TG path mask domain warp')
    mask_uv.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT2)
    mask_uv.set_editor_property('code', MASK_UV_HLSL)
    mask_uv.set_editor_property('include_file_paths', [WARP_INCLUDE])
    uv_inputs = []
    for name in ['UV', 'Xform', 'Warp']:
        item = unreal.CustomInput()
        item.set_editor_property('input_name', name)
        uv_inputs.append(item)
    mask_uv.set_editor_property('inputs', uv_inputs)
    assert MEL.connect_material_expressions(uv, '', mask_uv, 'UV')
    # ⚠️ 向量参数的第一个输出引脚是 RGB（三分量），要 .w 必须接 'RGBA' 引脚。
    assert MEL.connect_material_expressions(mask_xform, 'RGBA', mask_uv, 'Xform')
    assert MEL.connect_material_expressions(mask_warp, 'RGBA', mask_uv, 'Warp')
    # 默认贴图用引擎的纯黑（= 没有路）；运行时换成地面遮罩渲染目标（RG32F）。采样器类型只在编译期对默认贴图校验，
    # 运行时 MID 换贴图只查"是不是 2D"，所以 Color 型采样器接 RG32F 渲染目标是合法的：.R = 遮罩，.G = 只糊的场。
    mask_tex = node(unreal.MaterialExpressionTextureSampleParameter2D, -420, 1150)
    mask_tex.set_editor_property('parameter_name', 'GroundMask')
    mask_tex.set_editor_property('texture', unreal.load_asset('/Engine/EngineResources/Black'))
    mask_tex.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    assert MEL.connect_material_expressions(mask_uv, '', mask_tex, 'UVs')
    mask_enabled = node(unreal.MaterialExpressionScalarParameter, -420, 1380)
    mask_enabled.set_editor_property('parameter_name', 'GroundMaskEnabled')
    mask_enabled.set_editor_property('default_value', 0.0)

    # 草闸门的两端（2026-09-29 用户："保留草的 mask，让土向外延申；现在看来草长在了土上"）：
    # 闸门 = 草的拒绝曲线 smoothstep(草.MaskStart, 草.MaskEnd, 遮罩.R) = "这里不长草的概率"，两层土都乘它 ⇒ 草长满的地方
    # 一点土都不露。两个端点由地面 actor 按草的阈值喂，默认值与草的默认阈值相同。只作用在地面遮罩这条路上。
    soil_start = node(unreal.MaterialExpressionScalarParameter, -420, 1480)
    soil_start.set_editor_property('parameter_name', 'GroundMaskSoilStart')
    soil_start.set_editor_property('default_value', 0.15)
    soil_end = node(unreal.MaterialExpressionScalarParameter, -420, 1580)
    soil_end.set_editor_property('parameter_name', 'GroundMaskSoilEnd')
    soil_end.set_editor_property('default_value', 0.45)
    sharpness = node(unreal.MaterialExpressionScalarParameter, -420, 1680)
    sharpness.set_editor_property('parameter_name', 'GroundMaskFloorSharpness')
    sharpness.set_editor_property('default_value', FLOOR_SHARPNESS_DEFAULT)
    soil_remap = node(unreal.MaterialExpressionCustom, -300, 1480)
    soil_remap.set_editor_property('description', 'Ground mask: TG path mask (G) + grass gate (R)')
    soil_remap.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT2)
    soil_remap.set_editor_property('code', SOIL_REMAP_HLSL)
    remap_inputs = []
    for name in ['Mask', 'Inner', 'SoilStart', 'SoilEnd', 'Sharpness']:
        item = unreal.CustomInput()
        item.set_editor_property('input_name', name)
        remap_inputs.append(item)
    soil_remap.set_editor_property('inputs', remap_inputs)
    assert MEL.connect_material_expressions(mask_tex, 'R', soil_remap, 'Mask')
    assert MEL.connect_material_expressions(mask_tex, 'G', soil_remap, 'Inner')
    assert MEL.connect_material_expressions(soil_start, '', soil_remap, 'SoilStart')
    assert MEL.connect_material_expressions(soil_end, '', soil_remap, 'SoilEnd')
    assert MEL.connect_material_expressions(sharpness, '', soil_remap, 'Sharpness')

    # x = TG 的路径遮罩（深土与浅土共用，同 TG：dirt_mask 在只有土路时就是 path mask），与顶点色 R 按 GroundMaskEnabled
    # 插值（0 = 老办法）；y = 草闸门，与 1 插值（没有地面遮罩时不设闸，同原版）。
    path_pick = node(unreal.MaterialExpressionComponentMask, -180, 1480)
    gate_pick = node(unreal.MaterialExpressionComponentMask, -180, 1580)
    for pick, channel in ((path_pick, 'r'), (gate_pick, 'g')):
        for c in 'rgba':
            pick.set_editor_property(c, c == channel)
        assert MEL.connect_material_expressions(soil_remap, '', pick, '')
    path_mask = node(unreal.MaterialExpressionLinearInterpolate, -60, 1100)
    assert MEL.connect_material_expressions(vertex, 'R', path_mask, 'A')
    assert MEL.connect_material_expressions(path_pick, '', path_mask, 'B')
    assert MEL.connect_material_expressions(mask_enabled, '', path_mask, 'Alpha')
    gate = node(unreal.MaterialExpressionLinearInterpolate, -60, 1220)
    gate.set_editor_property('const_a', 1.0)
    assert MEL.connect_material_expressions(gate_pick, '', gate, 'B')
    assert MEL.connect_material_expressions(mask_enabled, '', gate, 'Alpha')

    blend = node(unreal.MaterialExpressionCustom, -220, 50)
    blend.set_editor_property('description', 'TG Ground: grass, dark soil, light path')
    blend.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    blend.set_editor_property('code', GROUND_HLSL)
    inputs = []
    for name in ['Grass', 'DarkDirt', 'Dirt', 'Height', 'Road', 'DirtMask', 'Gate']:
        item = unreal.CustomInput()
        item.set_editor_property('input_name', name)
        inputs.append(item)
    blend.set_editor_property('inputs', inputs)
    for name in ['Grass', 'DarkDirt', 'Dirt']:
        assert MEL.connect_material_expressions(samples[name], 'RGB', blend, name)
    assert MEL.connect_material_expressions(samples['GroundBlendHeight'], 'R', blend, 'Height')
    for name in ['Road', 'DirtMask']:
        assert MEL.connect_material_expressions(path_mask, '', blend, name)
    assert MEL.connect_material_expressions(gate, '', blend, 'Gate')
    assert MEL.connect_material_property(blend, '', unreal.MaterialProperty.MP_BASE_COLOR)

    rough = node(unreal.MaterialExpressionConstant, -220, 320)
    rough.set_editor_property('r', 0.95)
    assert MEL.connect_material_property(rough, '', unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.recompile_material(mat)
    assert unreal.EditorAssetLibrary.save_loaded_asset(mat)

    # 补齐已存在的地面实例参数；旧 Grass/Dirt 覆盖值保留。
    mi = unreal.load_asset(PKG + '/MI_TinyGladeGround')
    if mi and mi.get_editor_property('parent') == mat:
        for name in ['DarkDirt', 'GroundBlendHeight']:
            MEL.set_material_instance_texture_parameter_value(mi, name, textures[name])
        MEL.update_material_instance(mi)
        assert unreal.EditorAssetLibrary.save_loaded_asset(mi)
    unreal.log('TG GROUND THREE LAYERS SAVED: ' + mat.get_path_name())
    return mat


if __name__ == '__main__':
    build()
