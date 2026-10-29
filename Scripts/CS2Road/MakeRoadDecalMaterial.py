"""Makes the network actor's decal materials (CSRoadDecalInstances.h), DBuffer mesh decals:

- /PCGPlugins/CSRoadNet/M_RoadDecal, for a lane decal along an edge: its unit strip laid along the
  item's curve as RoadLab's RoadDecals.AddLane lays a strip's vertices.
- /PCGPlugins/CSRoadNet/M_RoadDecalFitted, for a junction connector or a crosswalk, which RoadLab
  projects onto the surface: its unit grid laid out across the curve as RoadLab's projected grid (a
  crosswalk's straight between its ends), each point at the height above the curve the instance
  carries for it (CSRoadDecal.usf DecalFitCS found them), lifted straight up. A triangle with a
  point off the surface is not drawn.

The vertex shader reads the instance's custom data (CSRoad::DecalData, the indices below); the
pixel shader samples the decal texture and fades it as the M_CS2 decal materials do
(Scripts/ImportCS2Roads.py): base colour albedo * Tint, opacity from albedo.a * Tint.a, the vertex
weight and the fades at unconnected ends. The actor makes one dynamic instance per decal surface
with that surface's albedo and Tint.

Run in the editor with -ExecutePythonScript=<this file> (RHI enabled: the materials compile).
"""
import unreal

PATH = '/PCGPlugins/CSRoadNet'
DEFAULT_SURFACE = '/PCGPlugins/CS2Roads/Materials/M_CS2_08_LaneLine_BaseColor'

TOOLS = unreal.AssetToolsHelpers.get_asset_tools()
LIB = unreal.EditorAssetLibrary
MEL = unreal.MaterialEditingLibrary


def log(text):
    unreal.log('CS2DECALMAT ' + text)


def expr(mat, cls, x, y, **props):
    node = MEL.create_material_expression(mat, cls, x, y)
    for key, value in props.items():
        node.set_editor_property(key, value)
    return node


def wire(a, out, b, pin):
    if not MEL.connect_material_expressions(a, out, b, pin):
        raise RuntimeError('Cannot connect {} -> {}'.format(out, pin))


def output(a, pin, prop):
    if not MEL.connect_material_property(a, pin, prop):
        raise RuntimeError('Cannot connect material output ' + str(prop))


def custom(mat, code, typ, inputs, x, y, description):
    node = expr(mat, unreal.MaterialExpressionCustom, x, y, code=code, output_type=typ, description=description)
    values = []
    for name in inputs:
        item = unreal.CustomInput()
        item.set_editor_property('input_name', name)
        values.append(item)
    node.set_editor_property('inputs', values)
    for name, (source, pin) in inputs.items():
        wire(source, pin, node, name)
    return node


# CSRoad::DecalData: A 0, B 3, C 6, D 9, Scale 12, T0 15, T1 16, Width 17, Lift 18, Straight 19,
# UMin 20, UMax 21, VMin 22, VCurve 23..26, Length 27, Weights 28..29, Connected 30..31, Smoothing 32;
# a fitted grid's columns at 33 and its heights from 34: per point (DecalStripRows + 1 rows of
# columns + 1, row-major) millimetres above the curve plus FitHeightZero (FitHeightOff: off the
# surface), two a float as first + second * 4096.
# The curve at Local.x of the instance's stretch (a fitted grid's last rows past the end fold onto
# it), and the frame RoadDecals.AddLane lays a strip in: right = (tangent.z, 0, -tangent.x) in Unity
# space (not normalised), the normal up; a crosswalk is the straight footprint between the ends, its
# right normalised.
FRAME = r'''
float D[20];
for (int i = 0; i < 20; i++) D[i] = GetPerInstanceCustomData(Parameters, i, 0.0f);
float3 A = float3(D[0], D[1], D[2]);
float3 B = float3(D[3], D[4], D[5]);
float3 C = float3(D[6], D[7], D[8]);
float3 E = float3(D[9], D[10], D[11]);
float3 S = float3(D[12], D[13], D[14]);
float t = min(lerp(D[15], D[16], Local.x), 1.0f);
float u = 1.0f - t;
float3 p = A * (u * u * u) + B * (3.0f * u * u * t) + C * (3.0f * u * t * t) + E * (t * t * t);
float3 d = (B - A) * (3.0f * u * u) + (C - B) * (6.0f * u * t) + (E - C) * (3.0f * t * t);
float3 tangent = dot(d, d) > 1e-12f ? normalize(d) : float3(1.0f, 0.0f, 0.0f);
float3 right = float3(0.0f - tangent.y, tangent.x, 0.0f);
float3 normal = cross(tangent, right);
normal = dot(normal, normal) > 1e-12f ? normalize(normal) : float3(0.0f, 0.0f, 1.0f);
if (normal.z < 0.0f) normal = 0.0f - normal;
if (D[19] > 0.5f)
{
	float3 chord = E - A;
	float2 side = float2(0.0f - chord.y, chord.x);
	p = lerp(A, E, t);
	right = float3(dot(side, side) > 1e-12f ? normalize(side) : float2(0.0f, 0.0f), 0.0f);
	normal = float3(0.0f, 0.0f, 1.0f);
}
'''

# Local: x along the strip, y across (CSRoad::MakeDecalStripMesh); lifted along the normal.
STRIP_WPO = FRAME + r'''
float3 position = p + right * ((Local.y - 0.5f) * D[17]) + normal * D[18];
return position - S * Local;
'''

# This point's height in millimetres plus 2048 (4095: off the surface), from Local (x along, y across:
# CSRoad::MakeDecalGridMesh, 8 rows of columns + 1 points).
HEIGHT = r'''
float columns = GetPerInstanceCustomData(Parameters, 33, 6.0f);
uint k = uint(round(Local.x * 8.0f)) * (uint(columns) + 1u) + uint(round(Local.y * columns));
uint pair = uint(GetPerInstanceCustomData(Parameters, 34 + int(k >> 1u), 0.0f));
uint q = (k & 1u) != 0u ? pair >> 12u : pair & 4095u;
'''

# The grid point across the curve where RoadLab's projected grid has it, at the surface's height,
# lifted straight up.
FIT_WPO = FRAME + HEIGHT + r'''
float3 position = p + right * ((Local.y - 0.5f) * D[17]);
position.z = p.z + (q != 4095u ? (float(q) - 2048.0f) * 0.1f : 0.0f) + D[18];
return position - S * Local;
'''

# 1 where the point is on the surface, carried to the pixels: a triangle with a point off it gets
# less than 1 inside, and is not drawn.
FIT_VALID = HEIGHT + r'''
return q != 4095u ? 1.0f : 0.0f;
'''

# UV0 at (t, across) of the decal: across the texture area, along the tiled distance's cubic.
UV = r'''
float t = TA.x;
float u = 1.0f - t;
float across = lerp(GetPerInstanceCustomData(Parameters, 20, 0.0f), GetPerInstanceCustomData(Parameters, 21, 0.0f), TA.y);
float along = GetPerInstanceCustomData(Parameters, 22, 0.0f)
	+ GetPerInstanceCustomData(Parameters, 23, 0.0f) * (u * u * u)
	+ GetPerInstanceCustomData(Parameters, 24, 0.0f) * (3.0f * u * u * t)
	+ GetPerInstanceCustomData(Parameters, 25, 0.0f) * (3.0f * u * t * t)
	+ GetPerInstanceCustomData(Parameters, 26, 0.0f) * (t * t * t);
return float2(across, along);
'''

# The M_CS2 decal opacity at t; none where a triangle has a point off the surface (Valid < 1).
OPACITY = r'''
if (Valid < 0.999f) return 0.0f;
float t = TA.x;
float len = GetPerInstanceCustomData(Parameters, 27, 0.0f);
float weight = lerp(GetPerInstanceCustomData(Parameters, 28, 1.0f), GetPerInstanceCustomData(Parameters, 29, 1.0f), t);
float s = max(GetPerInstanceCustomData(Parameters, 32, 0.0f), 0.0001f);
float a = lerp(smoothstep(0.0f, s, t * len), 1.0f, GetPerInstanceCustomData(Parameters, 30, 0.0f));
float b = lerp(smoothstep(0.0f, s, (1.0f - t) * len), 1.0f, GetPerInstanceCustomData(Parameters, 31, 0.0f));
return saturate((1.0f - pow(max(0.0001f, 1.0f - Base.a * Tint.a), weight)) * a * b);
'''

# (t, across) of a strip's or grid's point straight from its UV0 (across, along the stretch).
TA = r'''
return float2(min(lerp(GetPerInstanceCustomData(Parameters, 15, 0.0f), GetPerInstanceCustomData(Parameters, 16, 0.0f), Strip.y), 1.0f), Strip.x);
'''


def build(name, texture, fitted):
    path = PATH + '/' + name
    mat = LIB.load_asset(path) if LIB.does_asset_exist(path) else TOOLS.create_asset(name, PATH, unreal.Material, unreal.MaterialFactoryNew())
    MEL.delete_all_material_expressions(mat)
    # Blend mode first: a decal domain over the default opaque mode fails to compile.
    mat.set_editor_property('blend_mode', unreal.BlendMode.BLEND_TRANSLUCENT)
    mat.set_editor_property('material_domain', unreal.MaterialDomain.MD_DEFERRED_DECAL)
    mat.set_editor_property('two_sided', True)
    mat.set_editor_property('used_with_instanced_static_meshes', True)

    # Any per-instance custom data node makes the material fetch it; the custom nodes read the rest.
    flag = expr(mat, unreal.MaterialExpressionPerInstanceCustomData, -1500, 300, data_index=0)
    local = expr(mat, unreal.MaterialExpressionPreSkinnedPosition, -1500, 700)
    strip = expr(mat, unreal.MaterialExpressionTextureCoordinate, -1500, 0, coordinate_index=0)
    position = custom(mat, FIT_WPO if fitted else STRIP_WPO, unreal.CustomMaterialOutputType.CMOT_FLOAT3, {'Local': (local, ''), 'Flag': (flag, '')}, -1100, 700,
                      'Grid on the surface' if fitted else 'Strip along the curve')
    output(position, '', unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    if fitted:
        point = custom(mat, FIT_VALID, unreal.CustomMaterialOutputType.CMOT_FLOAT1, {'Local': (local, ''), 'Flag': (flag, '')}, -1100, 450, 'On the surface')
        valid = expr(mat, unreal.MaterialExpressionVertexInterpolator, -850, 450)
        if not MEL.connect_material_expressions(point, '', valid, '') and not MEL.connect_material_expressions(point, '', valid, 'VS'):
            raise RuntimeError('Cannot connect the vertex interpolator')
    else:
        valid = expr(mat, unreal.MaterialExpressionConstant, -850, 450, r=1.0)
    ta = custom(mat, TA, unreal.CustomMaterialOutputType.CMOT_FLOAT2, {'Strip': (strip, ''), 'Flag': (flag, '')}, -1150, 150, 'Point on the decal')
    uv = custom(mat, UV, unreal.CustomMaterialOutputType.CMOT_FLOAT2, {'TA': (ta, ''), 'Flag': (flag, '')}, -900, 0, 'Decal UV')
    albedo = expr(mat, unreal.MaterialExpressionTextureSampleParameter2D, -650, 0, parameter_name='albedo', texture=texture,
                  sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    wire(uv, '', albedo, 'UVs')
    tint = expr(mat, unreal.MaterialExpressionVectorParameter, -650, 300, parameter_name='Tint', default_value=unreal.LinearColor(1.0, 1.0, 1.0, 1.0))
    base = custom(mat, 'return Base * Tint;', unreal.CustomMaterialOutputType.CMOT_FLOAT3, {'Base': (albedo, 'RGB'), 'Tint': (tint, 'RGB')}, -350, 0, 'Base colour')
    output(base, '', unreal.MaterialProperty.MP_BASE_COLOR)
    opacity = custom(mat, OPACITY, unreal.CustomMaterialOutputType.CMOT_FLOAT1,
                     {'Base': (albedo, 'RGBA'), 'Tint': (tint, 'RGBA'), 'TA': (ta, ''), 'Valid': (valid, ''), 'Flag': (flag, '')}, -350, 300, 'Decal opacity')
    output(opacity, '', unreal.MaterialProperty.MP_OPACITY)
    rough = expr(mat, unreal.MaterialExpressionConstant, -350, 600, r=0.9)
    output(rough, '', unreal.MaterialProperty.MP_ROUGHNESS)

    MEL.recompile_material(mat)
    if not LIB.save_loaded_asset(mat, only_if_is_dirty=False):
        raise RuntimeError('Save failed ' + path)
    log('saved ' + path)


def main():
    surface = LIB.load_asset(DEFAULT_SURFACE)
    texture = MEL.get_material_default_texture_parameter_value(surface, 'albedo') if surface else None
    if not texture:
        raise RuntimeError('No albedo texture on ' + DEFAULT_SURFACE)
    build('M_RoadDecal', texture, False)
    build('M_RoadDecalFitted', texture, True)


try:
    main()
except Exception as error:
    unreal.log_error('CS2DECALMAT failed: {}'.format(error))
