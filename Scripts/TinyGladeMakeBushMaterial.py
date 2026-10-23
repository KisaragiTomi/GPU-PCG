"""Port TG bush_body / bush_flowers to lit UE foliage, with zero emissive.

Run in the current editor. See Docs/TinyGlade/Bush_Material.md for the source
contracts and UE adaptations. Only dedicated bush materials and the flower
mesh's default material slot are saved; no level or shared material is saved.
Existing generated nodes are updated by their description, without deleting
editor-owned expressions. MIA_bush keeps its existing Tex / Alpha overrides.
"""
import hashlib
import json
import re
import shutil
from pathlib import Path
import unreal

ROOT = Path(unreal.Paths.project_dir()).resolve()
OUT = ROOT / 'Saved/TGBushMaterial_20260930'
BASE = '/PCGPlugins/HouseTest/TinyGladeAsset'
MAT, TEX = BASE + '/Materials', BASE + '/Textures'
MEL, EAL = unreal.MaterialEditingLibrary, unreal.EditorAssetLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()
OUT.mkdir(parents=True, exist_ok=True)

# _nani_bush VS: the same four lattice hashes and cubic value-noise interpolation.
# Flowers use wind_bend == source local Y * 0.6 (verified across all 360 vertices).
WIND = r'''
struct TGBushNoise {
    float hash(int2 p) {
        uint y = (uint)p.y;
        y += y << 10; y ^= y >> 6; y += y << 3; y ^= y >> 11;
        y += y << 15;
        uint h = (((uint)p.x * 1664525u + y) + 1013904223u) * 1664525u;
        h ^= h >> 11; h ^= (h << 7) & 2636928640u;
        h ^= (h << 15) & 4022730752u; h ^= h >> 18;
        return asfloat((h & 8388607u) | 1065353216u) - 1.0;
    }
    float noise(float2 p) {
        int2 i = (int2)floor(p);
        float2 f = frac(p); f = f*f*(3.0-2.0*f);
        return lerp(lerp(hash(i),hash(i+int2(1,0)),f.x),
                    lerp(hash(i+int2(0,1)),hash(i+int2(1,1)),f.x),f.y);
    }
};
TGBushNoise field;
float t = lerp(Clock, PreviewTime, saturate(FreezeWind)) * WindSpeed;
float n = field.noise(Origin.xy * 0.001 + float2(t * 1.2, 0));
float bend = LocalP.z * 0.01 * HeightWeight;
float x = -bend*bend * (sin(n * 3.14 - 1.57) + 0.3) * 7.35 * WindStrength;
return float3(x, 0, 0);
'''


def backup(path):
    rel = path.removeprefix('/PCGPlugins/') + '.uasset'
    src = ROOT/'Plugins/PCGPlugins/Content'/rel
    dst = OUT/'backup'/rel
    if src.is_file() and not dst.exists():
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        return {'asset': path, 'sha256': hashlib.sha256(src.read_bytes()).hexdigest()}
    return None


def export(obj, filename):
    task = unreal.AssetExportTask()
    for key, value in dict(object=obj, filename=str(filename), automated=True,
                           prompt=False, replace_identical=True).items():
        task.set_editor_property(key, value)
    assert unreal.Exporter.run_asset_export_task(task), filename


class Graph:
    def __init__(self, name, masked):
        path = MAT+'/'+name
        self.mat = unreal.load_asset(path) if EAL.does_asset_exist(path) else TOOLS.create_asset(name, MAT, unreal.Material, unreal.MaterialFactoryNew())
        self.nodes = {}
        copy = OUT/(name+'.copy')
        export(self.mat, copy)
        for key in re.findall(r'Begin Object Class=/Script/Engine\.MaterialExpression\w+ Name="([^"]+)"', copy.read_text(encoding='utf-8-sig')):
            n = unreal.find_object(self.mat, key)
            if n:
                self.nodes[str(n.get_editor_property('desc'))] = n
        for key, value in {
            'shading_model': unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE,
            'blend_mode': unreal.BlendMode.BLEND_MASKED if masked else unreal.BlendMode.BLEND_OPAQUE,
            'two_sided': True, 'tangent_space_normal': False,
            'use_material_attributes': True, 'opacity_mask_clip_value': 0.5,
            'used_with_instanced_static_meshes': True, 'used_with_nanite': True,
            'max_world_position_offset_displacement': 40.0,
        }.items(): self.mat.set_editor_property(key, value)
        self.attrs = self.node('Outputs', 'MakeMaterialAttributes')
        assert MEL.connect_material_property(self.attrs, '', unreal.MaterialProperty.MP_MATERIAL_ATTRIBUTES)

    def node(self, key, kind, **props):
        tag = 'TG Bush | '+key
        n = self.nodes.get(tag)
        if n is None:
            n = MEL.create_material_expression(self.mat, getattr(unreal, 'MaterialExpression'+kind), 0, 0)
            n.set_editor_property('desc', tag)
            self.nodes[tag] = n
        for prop, value in props.items(): n.set_editor_property(prop, value)
        return n

    def link(self, src, dst, pin='', output=''):
        assert MEL.connect_material_expressions(src, output, dst, pin), (src.get_name(), dst.get_name(), pin, output)

    def scalar(self, name, value, group='Surface'):
        return self.node(name, 'ScalarParameter', parameter_name=name, default_value=float(value), group=group)

    def vector(self, name, color, group='Surface'):
        return self.node(name, 'VectorParameter', parameter_name=name, default_value=unreal.LinearColor(*color, 1), group=group)

    def custom(self, name, code, inputs, count=3):
        pins = []
        for key in inputs:
            p = unreal.CustomInput(); p.set_editor_property('input_name', key); pins.append(p)
        n = self.node(name, 'Custom', description=name, code=code, inputs=pins,
                      output_type=getattr(unreal.CustomMaterialOutputType, 'CMOT_FLOAT'+(str(count) if count>1 else '1')))
        for key, value in inputs.items(): self.link(value, n, key)
        return n

    def to_pixel(self, name, n):
        result = self.node(name, 'VertexInterpolator'); self.link(n, result); return result

    def output(self, n, name, output=''):
        self.link(n, self.attrs, name, output)

    def common(self, flower=False):
        local = self.node('Source local position', 'PreSkinnedPosition')
        source_n = self.node('Source local normal', 'PreSkinnedNormal')
        raw_n = self.custom('TG scaled source normal', 'return TransformLocalVectorToWorld(Parameters, N);', {'N':source_n})
        normal = self.to_pixel('Interpolated scaled normal', raw_n)
        local_ps = self.to_pixel('Local position for shading', local)
        zero = self.node('Zero emissive', 'Constant3Vector', constant=unreal.LinearColor(0,0,0,1))
        self.output(zero, 'EmissiveColor')
        self.output(self.scalar('Roughness', 0.92), 'Roughness')
        self.output(self.scalar('Specular', 0.12), 'Specular')
        self.output(self.scalar('LeafThickness', 0.6), 'Opacity')
        side = self.node('Two sided normal correction', 'TwoSidedSign')
        self.output(self.custom('Preserve authored volume normal',
                               'return normalize(N + float3(0,0,0.000001)) * Side;', {'N':normal,'Side':side}), 'Normal')
        origin = self.node('Instance origin', 'TransformPosition',
                           transform_source_type=unreal.MaterialPositionTransformSource.TRANSFORMPOSSOURCE_LOCAL,
                           transform_type=unreal.MaterialPositionTransformSource.TRANSFORMPOSSOURCE_WORLD)
        self.link(zero, origin)
        clock = self.node('Wind time', 'Time')
        wind = self.custom('TG bush value noise wind', WIND, {
            'LocalP':local, 'Origin':origin, 'Clock':clock,
            'PreviewTime':self.scalar('PreviewTime',0,'Wind'), 'FreezeWind':self.scalar('FreezeWind',0,'Wind'),
            'WindSpeed':self.scalar('WindSpeed',1,'Wind'), 'WindStrength':self.scalar('WindStrength',1,'Wind'),
            'HeightWeight':self.scalar('WindHeightWeight',0.6 if flower else 0.5,'Wind')})
        self.output(wind, 'WorldPositionOffset')
        return local_ps, normal, self.to_pixel('Instance origin for shading', origin)

    def finish(self):
        MEL.layout_material_expressions(self.mat)
        MEL.recompile_material(self.mat)
        assert EAL.save_loaded_asset(self.mat, False)
        export(self.mat, OUT/(self.mat.get_name()+'.copy'))


def instance(name, parent, textures=None, scalars=None, vectors=None):
    path = MAT+'/'+name
    mi = unreal.load_asset(path) if EAL.does_asset_exist(path) else TOOLS.create_asset(name,MAT,unreal.MaterialInstanceConstant,unreal.MaterialInstanceConstantFactoryNew())
    MEL.set_material_instance_parent(mi, parent)
    for key, value in (textures or {}).items():
        tex = unreal.load_asset(TEX+'/'+value)
        assert tex, value
        MEL.set_material_instance_texture_parameter_value(mi,key,tex)
        assert MEL.get_material_instance_texture_parameter_value(mi,key) == tex
    for key, value in (scalars or {}).items():
        MEL.set_material_instance_scalar_parameter_value(mi,key,float(value))
        assert abs(MEL.get_material_instance_scalar_parameter_value(mi,key)-value) < 1.e-5
    for key, value in (vectors or {}).items():
        MEL.set_material_instance_vector_parameter_value(mi,key,unreal.LinearColor(*value,1))
    MEL.update_material_instance(mi)
    assert EAL.save_loaded_asset(mi,False)
    return mi


def build(install=True):
    backups = [backup(MAT+'/MIA_bush'), backup(BASE+'/Meshes/bush_flowers'),
               backup(MAT+'/M_TinyGladeBush'), backup(MAT+'/M_TinyGladeBushFlowers')] if install else []
    manifest = OUT/'backup-manifest.json'
    if not manifest.exists(): manifest.write_text(json.dumps([x for x in backups if x],indent=2),encoding='utf-8')
    g = Graph('M_TinyGladeBush', True)
    local, normal, origin = g.common()
    uv = g.node('Mesh UV0', 'TextureCoordinate', coordinate_index=0)
    flipped = g.custom('Original TG atlas V flip', 'return float2(UV.x, 1.0-UV.y);', {'UV':uv},2)
    alpha = g.node('Atlas alpha', 'TextureSampleParameter2D', parameter_name='Alpha',texture=unreal.load_asset(TEX+'/bush_alpha'),sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_COLOR,
                   sampler_source=unreal.SamplerSourceMode.SSM_CLAMP_WORLD_GROUP_SETTINGS)
    g.link(flipped,alpha,'UVs')
    camera = g.node('View direction', 'CameraVectorWS')
    cut = g.custom('TG angle and alpha cutout',
        'float keep = step(abs(dot(normalize(V), N)), max(Threshold,0.001));\nreturn Mask * lerp(1.0,keep,saturate(Enable));',
        {'Mask':alpha,'V':camera,'N':normal,'Threshold':g.scalar('ViewCullThreshold',0.5,'TG Source'),
         'Enable':g.scalar('ViewCullStrength',1,'TG Source')},1)
    g.link(alpha,cut,'Mask','A')
    g.output(cut,'OpacityMask')
    world = g.node('World position with wind', 'WorldPosition')
    palette_uv = g.custom('TG world palette coordinates',
        'return P.xy / max(PeriodCm,1.0) + UV * 0.02;',
        {'P':world,'UV':flipped,'PeriodCm':g.scalar('PalettePeriodCm',1000,'TG Source')},2)
    palette = g.node('Season palette', 'TextureSampleParameter2D', parameter_name='Tex',texture=unreal.load_asset(TEX+'/bush_color_summer'),
                     sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_COLOR,
                     sampler_source=unreal.SamplerSourceMode.SSM_WRAP_WORLD_GROUP_SETTINGS)
    g.link(palette_uv,palette,'UVs')
    color = g.custom('TG height shading and winter frost',
        'float3 c = Palette.rgb * lerp(RootShade,1.0,smoothstep(20.0,200.0,LocalP.z));\n'
        'float snow = smoothstep(0.2,1.0,saturate(normalize(N).z))*0.4*saturate(Winter);\n'
        'return saturate(lerp(c,0.56.xxx,snow)*Tint.rgb);',
        {'Palette':palette,'LocalP':local,'N':normal,'Winter':g.scalar('Winter',0,'Season'),
         'RootShade':g.scalar('RootShade',0.8,'TG Source'),'Tint':g.vector('Tint',(1,1,1))})
    g.output(color,'BaseColor')
    g.output(g.custom('UE foliage transmission','return Color*Strength;',{'Color':color,'Strength':g.scalar('Transmission',0.35)}),'SubsurfaceColor')
    g.finish()
    body = instance('MIA_bush' if install else 'MI_TinyGladeBush_Summer',g.mat,{'Tex':'bush_color_summer','Alpha':'bush_alpha'})
    for season, winter in [('Summer',0),('Autumn',0),('Winter',1)]:
        instance('MI_TinyGladeBush_'+season,g.mat,{'Tex':'bush_color_'+season.lower(),'Alpha':'bush_alpha'},{'Winter':winter})

    f = Graph('M_TinyGladeBushFlowers', False)
    local, normal, origin = f.common(flower=True)
    color = f.custom('TG flower theme and instance variation',r'''
uint h = (uint)((Origin.x-Origin.y)*0.2 + 2000.0);
h += h<<10; h ^= h>>6; h += h<<3; h ^= h>>11; h += h<<15;
float r = asfloat((h&8388607u)|1065353216u)-1.0;
float3 c = lerp(Tint.rgb,0.7.xxx,saturate(Winter)*0.5);
c = pow(saturate(c),1.0+1.5*r*Variation);
float snow = smoothstep(0.5,1.0,saturate(normalize(N).z)*0.8)*saturate(Winter);
return lerp(c,0.7.xxx,snow);
''', {'Origin':origin,'N':normal,'Tint':f.vector('FlowerColor',(0.551,0.690,0.588),'Season'),
      'Winter':f.scalar('Winter',0,'Season'),'Variation':f.scalar('FlowerVariation',1,'Season')})
    f.output(color,'BaseColor')
    f.output(f.custom('UE petal transmission','return Color*Strength;',{'Color':color,'Strength':f.scalar('Transmission',0.35)}),'SubsurfaceColor')
    f.finish()
    flowers = instance('MI_TinyGladeBushFlowers_Summer',f.mat)
    instance('MI_TinyGladeBushFlowers_Autumn',f.mat,scalars={'FlowerVariation':0.5},vectors={'FlowerColor':(0.529,0.263,0.084)})
    instance('MI_TinyGladeBushFlowers_Winter',f.mat,scalars={'FlowerVariation':0.5,'Winter':1},vectors={'FlowerColor':(0.529,0.263,0.084)})
    mesh = unreal.load_asset(BASE+'/Meshes/bush_flowers')
    if install and mesh.get_material(0) != flowers:
        mesh.set_material(0,flowers)
        assert EAL.save_loaded_asset(mesh,False)
    # Components whose explicit override is None resolve these mesh defaults.
    # Rebuild transient bush renderers so already-open scenes immediately update.
    refreshed=[]
    for actor in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
        if install and isinstance(actor,unreal.CSGroundActor):
            actor.rebuild_building_bushes();refreshed.append(actor.get_actor_label())
    result={'body':body.get_path_name(),'flowers':flowers.get_path_name(),'refreshed':refreshed,'emissive':[0,0,0],
            'body_expressions':MEL.get_num_material_expressions(g.mat),'flower_expressions':MEL.get_num_material_expressions(f.mat)}
    (OUT/'built.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print('TG BUSH BUILD '+json.dumps(result))


if __name__ == '__main__': build()
