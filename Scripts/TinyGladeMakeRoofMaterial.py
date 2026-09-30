"""Lit roof tile material: port of TG `_nani_instanced_roof` (gbuffer variant b903e43ffb3da915).

Source: `D:/MyProject/Tiny Glade/tmp/shaders/_nani_instanced_roof.raster.hlsl_b903e43ffb3da915.{vs,ps}_main.glsl`
(excerpt + SHA-256 in Docs/TinyGlade/evidence/roof-shader-20260912.txt). TG's "chipped, worn"
tiles are entirely this shading; roof_tile has no damaged geometry variants.

Vertex shader (WPO): TG's resize, lift and wobble, applied after the instance transform (vs_main.glsl).
  resize       2026-09-30: recover the template from UV0 / Colour.R, use the actual instance scale to
               repeat TG's sign-based deformation, and subtract the scaled nominal bake. Bevels and
               flared ends retain their thickness-based size on half tiles and narrow ridge slabs.
  lift         template y > .1 (the wide, down-slope end) rises 6 cm along the tile normal, 2 cm on 10% of
               tiles (rand(seed*1664525 + roof id) > .9) (:176-209). The mesh bake already holds BAKED_LIFT_CM
               of it. The correction accounts for the instance scale; hip slabs and middle ridge slabs
               have no lift, matching is_main_roof_body. Ridge endpoints and slope tiles retain it.
  wobble       +-3 cm horizontally: clamp((value_noise3(world_m * .4) - .5) * 2.5, -.5, .5) * 6 cm (:296-557),
               sampled at the lifted world position. The same field moves every TG building shader (walls,
               bricks, plaster, windows, planks, ivy, spire).
  The material and tile cull sphere allow 32 cm WPO, including the bevel correction.

  NOT ported, on purpose (user decision 2026-09-15 after seeing both on screen). TG's tile VS also has:
  eave flare   :213      local z += (1 - smoothstep(0, 0.8 m, h)) * 0.115 m
  roof rise    :224-283  y += value_noise(xz_m * .5) * 0.3 m * smoothstep(0, 1 m, h)
               h = height above the roof pivot: `current_height`, added after both steps (:214-217), converges
               to `roof_pivot_y` in the _roof_tiles_gravity CS (:151, :155). TG's roof beams and spire VS add
               the same rise field without the weight.
  Neither is a missed port: 2026-09-14 shipped the rise with h read as a world height, 2026-09-15 shipped the
  rise and the flare with the correct weights; the user rejected the undulating roof ("it should not undulate")
  and chose to drop the flare with it. Don't add them back without asking. Without them the roof surface is
  CSHouseRoof.h's evaluator plus the tiles' own lift, so finials and eave / ridge decor sit on the evaluator.

Pixel shader, per pixel (VS varyings C3/C4/C6/C9/C10 are evaluated per pixel here):
  seed         per-tile int (ours: PerInstanceRandom + VertexColor.A, 24-bit exact on the instance path)
  cell         5x5 atlas cell = round(rand(seed*1664525)*5)*.2, round(rand(seed*1664525+34568)*5)*.2
  C9           template z*2 (mesh vertex colour R - .5, times 2); side face = -.95 < C9 < .9
  damage       roof_tile_damage at C10 (sides) or C6 (top); wear = (1 - damage) * C3
               The mask is 1 on the tile body and 0 on chips/rims, so wear lives on edges and corners.
               It must be the real linear 0..1 data (TG: BC4_UNORM). Squashed to 0..76 it makes every
               tile 70% worn and hides the chips; TinyGladeFixBc4Textures.py imports it as TC_Grayscale,
               sRGB off, so the shader reads it as-is.
  C3           pow(value_noise(world_xz_m + rand(seed^787234)), 1.5)   per-tile wear amount
  C4           simplex(.143*(xz + y)) / 4                               large-scale wear patches
  palette uv   world_m*.1 (+ .4*rand(seed^234) for half the tiles); worn pixels jump by rand(seed^2834)
               (TG: hard switch at wear > .01; here a smoothstep(.005, .02) cross-fade, see SHADE_HLSL)
  base         two planar palette samples blended by luminance + |N|, * .6
  colour       lerp(base*lerp(.9, 1.3, sat(1-3*C3)), wear_colour, sat(sat(5*wear)*weight + C4)) * lerp(1, .9, wear)
  wear_colour  roof_colors layer 1 ("dark wood" brown; layer 7 for palette 10) at world_xz_m*.2,
               * RoofWearColorScale (= TG's .6 * 1.5 * per-id factor, table below)
  roughness    lerp(.735, 1, wear)   (gbuffer target0.a, read as roughness by _deferred_solid_light_sun_sky)
  normal       N = faceted normal of the *displaced* surface (TG: normalize(cross(dFdx(P), -dFdy(P)))), so the
               lift and the wobble show up in the lighting. On the flat top (C9 > .999) roof_tile_normal at C6 is applied in
               TG's unnormalised frame dP/du, dP/dv in metres: normalize(N*z - dPdu*x - dPdv*y). Its xy is the
               gradient of the tile-top height (correlation with roof_tile_damage = -0.95 on both axes), hence
               the minus signs; the metre-scaled frame makes the relief about 1.5x the texture's raw xy.

Masked clipping: each slope tile carries its nearest face-bisector plane through the existing two-float
instance custom-data stream (normal angle + 4, world plane constant). Pixel discard trims crossed tile
ends at the hip; a single narrow slab covers the joint. Custom-data kind 0 = unlifted cap, -1 = lifted
ridge endpoint, >0 = clipped/lifted body. The mask also runs in Nanite / VSM rasterization.
Evaluate the signed distance at the displaced vertex and interpolate that scalar into the mask. In the
tested UE 5.7 Nanite/VSM path, recomputing it from pixel WorldPosition cut the visible tile but left an
uncut triangular shadow; sharing the vertex distance removes that mismatch.

TG palette ids (roof_color_ids -> texture array layer; UI icons in extracted/textures/color_icon):
  id  layer / look     RoofWearColorScale  RoofWearWeight   (wear texture)
  0   red (default)    1.35                1.0              layer01
  1   dark wood        0.315               2.0              layer01
  2   blue             0.9                 0.5              layer01
  3   turquoise        0.9                 0.5              layer01
  4   green            0.9                 0.5              layer01
  5   dark             0.9                 1.0              layer01
  6   green light      1.35                0.5              layer01
  7   grey             1.35                1.0              layer01
  8   ochre            1.35                1.0              layer01
  9   light blue       0.9                 0.5              layer01
  10  purple           0.9                 0.5              layer07
`MI_TinyGladeRoof_Turquoise` is palette 3; `MI_TinyGladeRoof_Grey` is palette 7, selected for the
user-confirmed 2026-09-29 reference. RoofBrightness is the UE adaptation (TG's own
lighting differs); theme/snow, highlight and dormer holes are not ported.
"""
import importlib.util
from pathlib import Path
import unreal
PKG='/PCGPlugins/HouseTest'
TEX=PKG+'/TinyGladeAsset/Textures'
MEL=unreal.MaterialEditingLibrary

PALETTES = {
    'Turquoise': dict(layer=3, wear_layer=1, scale=0.9, weight=0.5),
    'Grey': dict(layer=7, wear_layer=1, scale=1.35, weight=1.0),
}


def _baked_lift_cm():
    spec=importlib.util.spec_from_file_location('tg_roof_mesh',Path(__file__).with_name('TinyGladeMakeRoofMesh.py'))
    mesh=importlib.util.module_from_spec(spec);spec.loader.exec_module(mesh)
    return float(mesh.BAKED_LIFT_CM)


def wpo_hlsl():
    """WPO_HLSL with its @BakedLiftCm@ token filled from TinyGladeMakeRoofMesh.py."""
    code=WPO_HLSL.replace('@BakedLiftCm@','%.4f'%_baked_lift_cm())
    assert '@' not in code,code
    return code


HASH_HLSL = '''
    uint Mix(uint h) { h += h << 10; h ^= h >> 6; h += h << 3; h ^= h >> 11; h += h << 15; return h; }
    float Rand(uint s) { return float(Mix(s) & 0x7FFFFFu) / 8388608.0; }
    uint Seed(float R) { return uint(R * 16777216.0 + 0.5); }
    float2 Cell(uint S) { return float2(round(Rand(S * 1664525u) * 5.0), round(Rand(S * 1664525u + 34568u) * 5.0)) * 0.2; }
'''

# TG's 2D value noise (lattice hash + smoothstep bilinear): the C3 wear amount in SHADE_HLSL.
NOISE_HLSL = '''
    uint Temper(uint A) { A ^= A >> 11; A ^= (A << 7) & 0x9D2C5680u; A ^= (A << 15) & 0xEFC60000u; return A; }
    uint Lattice(int2 C)
    {
        uint2 U = uint2(C);
        uint H = U.y; H += H << 10; H ^= H >> 6; H += H << 3; H ^= H >> 11;
        uint A = Temper((U.x * 1664525u + (H + (H << 15)) + 1013904223u) * 1664525u);
        return A ^ (A >> 18);
    }
    float Unit(uint H) { return float(H & 0x7FFFFFu) / 8388608.0; }
    float ValueNoise(float2 P)
    {
        int2 I = int2(floor(P));
        float2 F = frac(P);
        float2 S = F * F * (3.0 - 2.0 * F);
        float V00 = Unit(Lattice(I));
        float V10 = Unit(Lattice(I + int2(1, 0)));
        float V01 = Unit(Lattice(I + int2(0, 1)));
        float V11 = Unit(Lattice(I + int2(1, 1)));
        return (1.0 - S.y) * (V00 * (1.0 - S.x) + V10 * S.x) + V01 * (1.0 - S.x) * S.y + V11 * S.x * S.y;
    }
'''

SHADE_HLSL = '''
struct FTGRoof
{
''' + HASH_HLSL + NOISE_HLSL + '''
    float2 Mod289(float2 X) { return X - 289.0 * trunc(X / 289.0); }
    float3 Mod289(float3 X) { return X - 289.0 * trunc(X / 289.0); }
    float Simplex(float2 V)
    {
        float2 I = floor(V + (V.x + V.y) * 0.366025403784439);
        float2 X0 = V - I + (I.x + I.y) * 0.211324865405187;
        float2 I1 = (X0.x > X0.y) ? float2(1.0, 0.0) : float2(0.0, 1.0);
        float4 X12 = X0.xyxy + float4(0.211324865405187, 0.211324865405187, -0.577350269189626, -0.577350269189626);
        X12.xy -= I1;
        I = Mod289(I);
        float3 P = I.y + float3(0.0, I1.y, 1.0);
        P = Mod289((P * 34.0 + 1.0) * P) + I.x + float3(0.0, I1.x, 1.0);
        P = (P * 34.0 + 1.0) * P;
        float3 M = max(0.5 - float3(dot(X0, X0), dot(X12.xy, X12.xy), dot(X12.zw, X12.zw)), 0.0);
        M = M * M; M = M * M;
        float3 X = 2.0 * frac(Mod289(P) * 0.024390243902439) - 1.0;
        float3 H = abs(X) - 0.5;
        float3 A0 = X - floor(X + 0.5);
        M *= 1.79284291400159 - 0.85373472095314 * (A0 * A0 + H * H);
        float3 G = float3(A0.x * X0.x + H.x * X0.y, A0.y * X12.x + H.y * X12.y, A0.z * X12.z + H.z * X12.w);
        return 130.0 * dot(M, G);
    }
};
FTGRoof Tg;
uint TileSeed = Tg.Seed(Rnd);
float3 Metres = WorldPos * 0.01;                              // TG is y-up: tg(x, y, z) = ue(x, z, y)
float3 FaceN = normalize(cross(ddx(WorldPos), ddy(WorldPos)));   // TG _224: faceted normal of the displaced surface
FaceN *= (dot(FaceN, NormalWS) < 0.0) ? -1.0 : 1.0;
float WearNoise = pow(frac(Tg.ValueNoise(Metres.xy + Tg.Rand(TileSeed ^ 787234u))), 1.5);                 // TG C3
float WearPatch = Tg.Simplex(0.143 * (Metres.xy + Metres.z)) * 0.25;                                    // TG C4
float2 AtlasCell = Tg.Cell(TileSeed);
float TplZ = VColor.r - 0.5;
float TplC9 = TplZ * 2.0;
float2 TopUV = (TplXY * float2(0.85, 0.92) + 0.5) * 0.2 + AtlasCell;                                    // TG C6
float2 SideUV = (float2(TplXY.y, TplZ) * float2(0.8, 0.9) + 0.5) * 0.2 + AtlasCell;                     // TG C10
bool bSide = TplC9 > -0.95 && TplC9 < 0.9;
// Linear greyscale import (R8): the value lives in .r, exactly as TG's BC4_UNORM sample.
float DamageTex = Texture2DSample(TileDamage, TileDamageSampler, bSide ? SideUV : TopUV).r;
float Wear = (1.0 - DamageTex) * WearNoise;
float3 PaletteUV = float3(Metres.x, Metres.z, Metres.y) * 0.1;
PaletteUV += (Tg.Rand(TileSeed ^ 34534u) < 0.5) ? Tg.Rand(TileSeed ^ 234u) * 0.4 : 0.0;
float3 WornUV = PaletteUV + Tg.Rand(TileSeed ^ 2834u);
// TG jumps to WornUV at Wear > 0.01, which draws stair-stepped chip outlines up close where that
// contour crosses texels, so cross-fade over a narrow band instead (identical outside it).
float WornMix = smoothstep(0.005, 0.02, Wear);
float3 ColorA = lerp(Texture2DSample(RoofColor, RoofColorSampler, PaletteUV.zy).rgb,
                     Texture2DSample(RoofColor, RoofColorSampler, WornUV.zy).rgb, WornMix);
float3 ColorB = lerp(Texture2DSample(RoofColor, RoofColorSampler, PaletteUV.xy).rgb,
                     Texture2DSample(RoofColor, RoofColorSampler, WornUV.xy).rgb, WornMix);
float WeightA = dot(ColorA, float3(0.2126, 0.7152, 0.0722)) + abs(FaceN.x);
float WeightB = dot(ColorB, float3(0.2126, 0.7152, 0.0722)) + abs(FaceN.y);
float Cut = max(WeightA, WeightB) - 0.2;
WeightA = max(WeightA - Cut, 0.0);
WeightB = max(WeightB - Cut, 0.0);
float3 Base = (ColorA * WeightA + ColorB * WeightB) / max(WeightA + WeightB, 1e-5) * 0.6;
float3 WearColor = Texture2DSample(RoofWearColor, RoofWearColorSampler, Metres.xy * 0.2).rgb * WearScale;   // WearScale = TG .6*1.5*per-id
float3 Albedo = lerp(Base * 0.9, Base * 1.3, saturate(1.0 - WearNoise * 3.0));
Albedo = lerp(Albedo, WearColor, saturate(saturate(Wear * 5.0) * WearWeight + WearPatch));
Albedo = lerp(Albedo, Albedo * 0.9, Wear);
return float4(Albedo, Wear);
'''

NORMAL_HLSL = '''
struct FTGRoofHash
{
''' + HASH_HLSL + '''
};
FTGRoofHash Tg;
float TplC9 = (VColor.r - 0.5) * 2.0;
float2 TopUV = (TplXY * float2(0.85, 0.92) + 0.5) * 0.2 + Tg.Cell(Tg.Seed(Rnd));
float3 P = WorldPos * 0.01;                                   // metres: TG's frame is not normalised
float3 Dp1 = ddx(P);
float3 Dp2 = ddy(P);
float3 FaceN = normalize(cross(Dp1, Dp2));
FaceN *= (dot(FaceN, NormalWS) < 0.0) ? -1.0 : 1.0;
float2 Duv1 = ddx(TopUV);
float2 Duv2 = ddy(TopUV);
float Det = Duv1.x * Duv2.y - Duv2.x * Duv1.y;
float InvDet = abs(Det) > 1e-12 ? 1.0 / Det : 0.0;
float3 DPdu = (Dp1 * Duv2.y - Dp2 * Duv1.y) * InvDet;
float3 DPdv = (Dp2 * Duv1.x - Dp1 * Duv2.x) * InvDet;
float3 TileN = UnpackNormalMap(Texture2DSample(TileNormal, TileNormalSampler, TopUV)).xyz;
float3 Bumped = normalize(FaceN * TileN.z - (DPdu * TileN.x + DPdv * TileN.y) * Strength);
return TplC9 > 0.999 ? Bumped : FaceN;
'''

WPO_HLSL = '''
struct FTGRoofWpo
{
''' + HASH_HLSL + NOISE_HLSL + '''
    float2 Lattice3(int3 C)
    {
        uint3 U = uint3(C);
        uint H = U.z; H += H << 10; H ^= H >> 6; H += H << 3; H ^= H >> 11;
        uint A = Temper((U.y * 1664525u + (H + (H << 15)) + 1013904223u) * 1664525u);
        uint V = Temper((U.x * 1664525u + (A ^ (A >> 18)) + 1013904223u) * 1664525u);
        V ^= V >> 18;
        uint J = V; J += J << 10; J ^= J >> 6; J += J << 3; J ^= J >> 11;
        return float2(Unit(V), Unit(J + (J << 15)));
    }
    float2 ValueNoise3(float3 P)
    {
        int3 I = int3(floor(P));
        float3 F = frac(P);
        float3 S = F * F * (3.0 - 2.0 * F);
        float2 Sum = 0.0;
        for (int Corner = 0; Corner < 8; ++Corner)
        {
            int3 D = int3(Corner & 1, (Corner >> 1) & 1, (Corner >> 2) & 1);
            float3 W = lerp(1.0 - S, S, float3(D));
            Sum += Lattice3(I + D) * (W.x * W.y * W.z);
        }
        return Sum;
    }
    float3 Deform(float3 Tpl, float3 Size)
    {
        float3 Half = Size * float3(0.50597 * 0.95, 0.5, 0.5);
        float3 Limit = Half * float3(3.5892038, 3.7037039, 3.7037039);
        float Common = max(min(max(Half.x, max(Half.y, Half.z)), min(Limit.x, min(Limit.y, Limit.z))), 1e-5);
        float3 P = Tpl * float3(1.9764018, 2.0, 2.0) * Common - (Common - Half) * sign(Tpl);
        P.xy = -P.xy;
        return P;
    }
};
FTGRoofWpo Tg;
// TG resizes the template using signs, preserving bevel thickness when a half tile or narrow hip slab is used.
// A nonuniform instance scale alone stretches the bevel and turns narrow hip slabs into wedges.
float3 X = LocalX;
float3 Y = LocalY;
float3 Z = LocalZ;
float3 Scale = max(float3(length(X), length(Y), length(Z)), 1e-5);
float3 Tpl = float3(TplXY, VColor.r - 0.5);
float3 Nominal = float3(45.0,64.0,10.0);
float3 Baked = Tg.Deform(Tpl, Nominal);
Baked.z += TplXY.y > 0.1 ? @BakedLiftCm@ : 0.0;
float3 Resized = Tg.Deform(Tpl, Nominal * Scale);
// ClipKind > 0: slope tile; -1: first/last horizontal ridge slab; 0: the other caps, no lifted end.
float Lift = ClipKind != 0.0 ? ((Tg.Rand(Tg.Seed(Rnd) * 1664525u + 1u) > 0.9) ? 2.0 : 6.0) : 0.0;
Resized.z += TplXY.y > 0.1 ? Lift : 0.0;
float3 Delta = Resized - Baked * Scale;
float3 Offset = X / Scale.x * Delta.x + Y / Scale.y * Delta.y + Z / Scale.z * Delta.z;
// wobble: TG's shared hand-made field, +-3 cm horizontally, sampled at the lifted position. TG's eave flare and
// roof rise sit between these two steps and are left out on purpose (module doc).
float3 Metres = (WorldPos + Offset) * 0.01;                   // TG is y-up: tg(x, y, z) = ue(x, z, y)
float2 Wobble = clamp((Tg.ValueNoise3(float3(Metres.x, Metres.z, Metres.y) * 0.4) - 0.5) * 2.5, -0.5, 0.5) * 6.0;
Offset.xy += Wobble;                                          // TG x -> ue x, TG z -> ue y
return Offset;
'''

# Slope tiles end on their nearest roof-face bisector. Evaluate before view-specific pixel/shadow setup.
# Data is generated by CSHouseTile::BuildPlan and carried by the existing two-float custom-data stream.
CLIP_DISTANCE_HLSL = '''
if (ClipData.x <= 0.0) return -1.0;
float Angle = ClipData.x - 4.0;
return dot(float2(cos(Angle), sin(Angle)), (WorldPos + Offset).xy) + ClipData.y;
'''


def build(name='M_TinyGladeRoof',save=True):
    """save=False leaves the material unsaved and skips the palette MIs: a dry run of the graph and its shader
    compile under another name (headless check before touching the real asset)."""
    mat=unreal.load_asset(PKG+'/'+name)
    if not mat:
        mat=unreal.AssetToolsHelpers.get_asset_tools().create_asset(name,PKG,unreal.Material,unreal.MaterialFactoryNew())
    for _ in range(16):
        if not MEL.get_num_material_expressions(mat): break
        MEL.delete_all_material_expressions(mat)
    assert not MEL.get_num_material_expressions(mat)
    # Reserve extra displacement for the sign-based bevel correction on nonuniformly scaled tiles.
    # Nanite clamps by this value; the tile producer's conservative cull sphere uses the same 32 cm.
    for k,v in dict(used_with_instanced_static_meshes=True,used_with_nanite=True,two_sided=False,tangent_space_normal=False,
        blend_mode=unreal.BlendMode.BLEND_MASKED,shading_model=unreal.MaterialShadingModel.MSM_DEFAULT_LIT,
        max_world_position_offset_displacement=32.0).items():mat.set_editor_property(k,v)
    def node(cls,**props):
        n=MEL.create_material_expression(mat,getattr(unreal,cls),0,0)
        for k,v in props.items(): n.set_editor_property(k,v)
        return n
    def link(a,b,pin,out=''):
        assert MEL.connect_material_expressions(a,out,b,pin),(a.get_name(),out,b.get_name(),pin)
    def custom(code,inputs,kind,description):
        """inputs: {pin: expression} or {pin: (expression, output pin name)}."""
        pins=[]
        for name in inputs:
            p=unreal.CustomInput();p.set_editor_property('input_name',name);pins.append(p)
        n=node('MaterialExpressionCustom',code=code,description=description,
            output_type=getattr(unreal.CustomMaterialOutputType,'CMOT_'+kind),inputs=pins)
        for name,src in inputs.items():
            src,out=src if isinstance(src,tuple) else (src,'')
            link(src,n,name,out)
        return n
    def scalar(name,value):return node('MaterialExpressionScalarParameter',parameter_name=name,default_value=value)
    def texture(name,tex,sampler):
        t=unreal.load_asset(TEX+'/'+tex);assert t,tex
        return node('MaterialExpressionTextureObjectParameter',parameter_name=name,texture=t,
            sampler_type=getattr(unreal.MaterialSamplerType,'SAMPLERTYPE_'+sampler))
    damage=unreal.load_asset(TEX+'/roof_tile_damage')
    assert damage and not damage.get_editor_property('srgb') and \
        damage.get_editor_property('compression_settings')==unreal.TextureCompressionSettings.TC_GRAYSCALE, \
        'roof_tile_damage must be the linear greyscale import (run TinyGladeFixBc4Textures.py first)'
    wp=node('MaterialExpressionWorldPosition');nws=node('MaterialExpressionVertexNormalWS')
    uv=node('MaterialExpressionTextureCoordinate',coordinate_index=0);vc=node('MaterialExpressionVertexColor')
    clip_kind=node('MaterialExpressionPerInstanceCustomData',data_index=0,const_default_value=0.0)
    clip_d=node('MaterialExpressionPerInstanceCustomData',data_index=1,const_default_value=0.0)
    clip_pair=node('MaterialExpressionAppendVector')
    link(clip_kind,clip_pair,'A');link(clip_d,clip_pair,'B')
    # Per-instance seed, two-path contract of CSGpuInstancedMeshComponent.h: PerInstanceRandom + VertexColor.A.
    rnd=custom('return PerInstance + saturate(VColorA);',
        {'PerInstance':node('MaterialExpressionPerInstanceRandom'),'VColorA':(vc,'A')},'FLOAT1','TG roof tile seed')
    shade=custom(SHADE_HLSL,{
        'WorldPos':wp,'Rnd':rnd,'TplXY':uv,'VColor':vc,'NormalWS':nws,
        'RoofColor':texture('RoofColor','roof_colors_layer00','COLOR'),
        'RoofWearColor':texture('RoofWearColor','roof_colors_layer01','COLOR'),
        'TileDamage':texture('TileDamage','roof_tile_damage','LINEAR_GRAYSCALE'),
        'WearScale':scalar('RoofWearColorScale',1.35),
        'WearWeight':scalar('RoofWearWeight',1.0)},'FLOAT4','TG roof shade (colour.rgb, wear)')
    base=custom('return Shade.rgb * Brightness;',{'Shade':shade,'Brightness':scalar('RoofBrightness',1.25)},
        'FLOAT3','TG roof base colour')
    rough=custom('return lerp(Intact, Worn, saturate(Shade.a));',{'Shade':shade,
        'Intact':scalar('RoofRoughnessIntact',.735),'Worn':scalar('RoofRoughnessWorn',1.0)},'FLOAT1','TG roof roughness')
    normal=custom(NORMAL_HLSL,{'WorldPos':wp,'NormalWS':nws,'Rnd':rnd,'TplXY':uv,'VColor':vc,
        'TileNormal':texture('TileNormal','roof_tile_normal','NORMAL'),
        'Strength':scalar('TileNormalStrength',1.0)},'FLOAT3','TG roof world normal')
    axes={}
    for name,value in [('LocalX',(1,0,0)),('LocalY',(0,1,0)),('LocalZ',(0,0,1))]:
        axis=node('MaterialExpressionConstant3Vector',constant=unreal.LinearColor(*value,0.0))
        transformed=node('MaterialExpressionTransform',transform_source_type=unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_LOCAL,
            transform_type=unreal.MaterialVectorCoordTransform.TRANSFORM_WORLD)
        link(axis,transformed,'')
        axes[name]=transformed
    wpo=custom(wpo_hlsl(),dict(WorldPos=wp,Rnd=rnd,TplXY=uv,VColor=vc,ClipKind=clip_kind,**axes),
        'FLOAT3','TG roof sign-resize / lift / wobble (WPO)')
    distance=custom(CLIP_DISTANCE_HLSL,{'WorldPos':wp,'Offset':wpo,'ClipData':clip_pair},'FLOAT1','TG roof signed clip distance at displaced vertex')
    clip_interp=node('MaterialExpressionVertexInterpolator')
    link(distance,clip_interp,'VS')
    mask=custom('return Distance < 0.0 ? 1.0 : 0.0;',{'Distance':clip_interp},'FLOAT1','TG roof hip clipping')
    for n,prop in [(base,'BASE_COLOR'),(normal,'NORMAL'),(rough,'ROUGHNESS'),(scalar('RoofSpecular',.5),'SPECULAR'),
                   (wpo,'WORLD_POSITION_OFFSET'),(mask,'OPACITY_MASK')]:
        assert MEL.connect_material_property(n,'',getattr(unreal.MaterialProperty,'MP_'+prop)),prop
    MEL.layout_material_expressions(mat);MEL.recompile_material(mat)
    if save:
        unreal.EditorAssetLibrary.save_loaded_asset(mat,False)
        build_palette_instances(mat)
    return mat


def build_palette_instances(parent):
    """One MI per non-default TG palette. Parameter values come from the id table in the module doc."""
    tools=unreal.AssetToolsHelpers.get_asset_tools()
    for suffix,p in PALETTES.items():
        name='MI_TinyGladeRoof_'+suffix
        mi=unreal.load_asset(PKG+'/'+name)
        if not mi:
            mi=tools.create_asset(name,PKG,unreal.MaterialInstanceConstant,unreal.MaterialInstanceConstantFactoryNew())
        MEL.set_material_instance_parent(mi,parent)
        MEL.clear_all_material_instance_parameters(mi)
        # 5.7's SetMaterialInstance*ParameterValue return False unconditionally (MaterialEditingLibrary.cpp),
        # so a wrong parameter name is only caught by reading the value back.
        for param,layer in (('RoofColor',p['layer']),('RoofWearColor',p['wear_layer'])):
            t=unreal.load_asset('%s/roof_colors_layer%02d'%(TEX,layer));assert t
            MEL.set_material_instance_texture_parameter_value(mi,param,t)
            assert MEL.get_material_instance_texture_parameter_value(mi,param)==t,(name,param)
        for param,value in (('RoofWearColorScale',p['scale']),('RoofWearWeight',p['weight'])):
            MEL.set_material_instance_scalar_parameter_value(mi,param,value)
            assert abs(MEL.get_material_instance_scalar_parameter_value(mi,param)-value)<1e-5,(name,param)
        MEL.update_material_instance(mi)
        unreal.EditorAssetLibrary.save_loaded_asset(mi,False)


if __name__=='__main__':build()
