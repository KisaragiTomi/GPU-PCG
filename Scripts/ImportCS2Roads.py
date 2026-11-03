"""Import a CS2 Road Lab surface export and save a native Unreal level.

Run in the editor with -ExecutePythonScript=<this file> (RHI enabled).
CS2ROAD_SOURCE selects the directory containing unreal-scene.json.
Only /PCGPlugins/CS2Roads is authored. Existing unrelated content is untouched.
"""
import json, math, os, pathlib, traceback
import unreal

SOURCE=pathlib.Path(os.environ.get('CS2ROAD_SOURCE',r'D:/MyProject/CitiesSkylines-Road2-C#/Artifacts/ue-palmsland'))
DEST=os.environ.get('CS2ROAD_DEST','/PCGPlugins/CS2Roads')
PROBE=os.environ.get('CS2ROAD_PROBE','0')=='1'
OUT=pathlib.Path(unreal.Paths.project_saved_dir())/'CS2Roads';OUT.mkdir(exist_ok=True)
TOOLS=unreal.AssetToolsHelpers.get_asset_tools();LIB=unreal.EditorAssetLibrary
MEL=unreal.MaterialEditingLibrary;MESH=unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
ACTORS=unreal.get_editor_subsystem(unreal.EditorActorSubsystem);LEVEL=unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
MANAGER=unreal.InterchangeManager.get_interchange_manager_scripted()
def log(s):unreal.log('CS2_IMPORT '+s)
def save(asset):
    if not LIB.save_loaded_asset(asset,only_if_is_dirty=False):raise RuntimeError('Save failed '+asset.get_path_name())
def expr(mat,cls,x,y,**props):
    obj=MEL.create_material_expression(mat,cls,x,y)
    for k,v in props.items():obj.set_editor_property(k,v)
    return obj
def wire(a,out,b,pin):
    if not MEL.connect_material_expressions(a,out,b,pin):raise RuntimeError('Cannot connect '+pin)
def output(a,pin,prop):
    if not MEL.connect_material_property(a,pin,prop):raise RuntimeError('Cannot connect material output')
def import_file(filename,destination,name,p=None):
    if p is None:p=unreal.InterchangeGenericAssetsPipeline()
    p.asset_name=name;p.use_source_name_for_asset=False;p.scene_name_sub_folder=False;p.asset_type_sub_folders=False
    params=unreal.ImportAssetParameters();params.is_automated=True;params.destination_name=name
    params.override_pipelines=[unreal.SoftObjectPath(p.get_path_name())]
    source=MANAGER.create_source_data(str(filename))
    if not MANAGER.import_asset(destination,source,params):raise RuntimeError('Import failed '+str(filename))
def custom(mat,code,typ,inputs,x,y):
    node=expr(mat,unreal.MaterialExpressionCustom,x,y,code=code,output_type=typ)
    values=[]
    for name,(source,pin) in inputs.items():
        item=unreal.CustomInput();item.set_editor_property('input_name',name);values.append(item)
    node.set_editor_property('inputs',values)
    for name,(source,pin) in inputs.items():wire(source,pin,node,name)
    return node

def materials(scene):
    textures={};result={}
    for info in scene['materials']:
        for channel in ['albedo','normal','mask','detail']:
            relative=info.get(channel)
            if not relative or relative in textures:continue
            name=pathlib.Path(relative).stem
            import_file(SOURCE/relative,DEST+'/Textures',name)
            texture=LIB.load_asset(DEST+'/Textures/'+name)
            if not isinstance(texture,unreal.Texture2D):raise RuntimeError('Missing texture '+name)
            texture.set_editor_property('srgb',channel in ['albedo','detail'])
            texture.set_editor_property('compression_settings',unreal.TextureCompressionSettings.TC_BC7)
            save(texture);textures[relative]=texture
    for info in scene['materials']:
        name=info['name'];path=DEST+'/Materials/'+name
        mat=LIB.load_asset(path) if LIB.does_asset_exist(path) else TOOLS.create_asset(name,DEST+'/Materials',unreal.Material,unreal.MaterialFactoryNew())
        MEL.delete_all_material_expressions(mat)
        kind=info['kind'];mat.set_editor_property('two_sided',kind!='surface')
        mat.set_editor_property('blend_mode',unreal.BlendMode.BLEND_TRANSLUCENT if kind=='decal' else unreal.BlendMode.BLEND_OPAQUE)
        uv=expr(mat,unreal.MaterialExpressionTextureCoordinate,-1000,0,coordinate_index=0)
        def tex(channel,coord=uv):
            sample=expr(mat,unreal.MaterialExpressionTextureSampleParameter2D,-700,len(mat.get_name())*2,
                        parameter_name=channel,texture=textures[info[channel]],
                        sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_COLOR if channel in ['albedo','detail'] else unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
            wire(coord,'',sample,'UVs');return sample
        tint=expr(mat,unreal.MaterialExpressionVectorParameter,-700,600,parameter_name='Tint',default_value=unreal.LinearColor(*info['tint']))
        if kind=='terrain':
            output(tint,'RGB',unreal.MaterialProperty.MP_BASE_COLOR)
            rough=expr(mat,unreal.MaterialExpressionConstant,-300,600,r=1.0);output(rough,'',unreal.MaterialProperty.MP_ROUGHNESS)
        else:
            base=tex('albedo')
            if kind=='surface':
                if info.get('detail'):
                    world=expr(mat,unreal.MaterialExpressionWorldPosition,-1200,300)
                    coord=custom(mat,'return World.xy / 500.0;',unreal.CustomMaterialOutputType.CMOT_FLOAT2,{'World':(world,'')},-900,300)
                    detail=tex('detail',coord);mask=tex('mask')
                    blend=custom(mat,'return lerp(Base, Detail, Mask.b);',unreal.CustomMaterialOutputType.CMOT_FLOAT3,{'Base':(base,'RGB'),'Detail':(detail,'RGB'),'Mask':(mask,'RGBA')},-250,0)
                    output(blend,'',unreal.MaterialProperty.MP_BASE_COLOR)
                else:output(base,'RGB',unreal.MaterialProperty.MP_BASE_COLOR);mask=tex('mask') if info.get('mask') else None
                if mask:
                    rough=custom(mat,'return clamp(1.0-Mask.a,0.1,1.0);',unreal.CustomMaterialOutputType.CMOT_FLOAT1,{'Mask':(mask,'RGBA')},-250,260)
                else:rough=expr(mat,unreal.MaterialExpressionConstant,-250,260,r=.85)
                output(rough,'',unreal.MaterialProperty.MP_ROUGHNESS)
                if info.get('normal'):
                    normal=tex('normal')
                    decoded=custom(mat,'float2 xy=float2(P.r*P.a,P.g)*2-1; return normalize(float3(xy.x*0.7,-xy.y*0.7,sqrt(max(0.0,1-dot(xy,xy)))));',
                        unreal.CustomMaterialOutputType.CMOT_FLOAT3,{'P':(normal,'RGBA')},-250,450)
                    output(decoded,'',unreal.MaterialProperty.MP_NORMAL)
            else:
                mat.set_editor_property('translucency_lighting_mode',unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING)
                tint_base=custom(mat,'return Base*Tint;',unreal.CustomMaterialOutputType.CMOT_FLOAT3,{'Base':(base,'RGB'),'Tint':(tint,'RGB')},-250,0)
                output(tint_base,'',unreal.MaterialProperty.MP_BASE_COLOR)
                coords=[expr(mat,unreal.MaterialExpressionTextureCoordinate,-900,800+i*130,coordinate_index=i) for i in [1,2,3]]
                opacity=custom(mat,'float s=max(BA.y,0.0001); float a=lerp(smoothstep(0,s,UV.x),1,RG.y); float b=lerp(smoothstep(0,s,UV.y),1,BA.x); return saturate((1-pow(max(0.0001,1-Base.a*Tint.a),RG.x))*a*b);',
                    unreal.CustomMaterialOutputType.CMOT_FLOAT1,{'Base':(base,'RGBA'),'Tint':(tint,'RGBA'),'UV':(coords[0],''),'RG':(coords[1],''),'BA':(coords[2],'')},-250,900)
                output(opacity,'',unreal.MaterialProperty.MP_OPACITY)
                rough=expr(mat,unreal.MaterialExpressionConstant,-250,600,r=.9);output(rough,'',unreal.MaterialProperty.MP_ROUGHNESS)
        MEL.recompile_material(mat);save(mat);result[name]=mat;log('material '+name)
    return result,textures

def pipeline():
    p=unreal.InterchangeGenericAssetsPipeline()
    # UE 5.7's glTF translator already applies metres -> centimetres.
    p.import_offset_uniform_scale=1.0;p.scene_name_sub_folder=False;p.asset_type_sub_folders=False
    common=p.common_meshes_properties
    common.recompute_normals=False;common.recompute_tangents=True;common.use_full_precision_u_vs=True
    common.remove_degenerates=False;common.import_lods=False
    mesh=p.mesh_pipeline;mesh.import_skeletal_meshes=False;mesh.import_geometry_caches=False;mesh.combine_static_meshes=True
    mesh.generate_lightmap_u_vs=False;mesh.build_nanite=False;mesh.collision=False
    p.material_pipeline.import_materials=False
    return p

def main():
    unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous([DEST],force_rescan=True)
    scene=json.loads((SOURCE/'unreal-scene.json').read_text(encoding='utf-8'))
    mats,textures=materials(scene);p=pipeline();records=[]
    for i,item in enumerate(scene['meshes']):
        dest=DEST+'/Meshes/'+item['layer'];path=dest+'/'+item['name']
        imported=not LIB.does_asset_exist(path)
        if imported:
            import_file(SOURCE/item['file'],dest,item['name'],p)
        mesh=LIB.load_asset(path)
        if not isinstance(mesh,unreal.StaticMesh):raise RuntimeError('StaticMesh missing '+path)
        tris=mesh.get_num_triangles(0);bounds=mesh.get_bounding_box()
        actual_min=[bounds.min.x,bounds.min.y,bounds.min.z];actual_max=[bounds.max.x,bounds.max.y,bounds.max.z]
        err=max(abs(a-b) for a,b in zip(actual_min+actual_max,item['local_min_cm']+item['local_max_cm']))
        if tris!=item['triangles'] or err>1:raise RuntimeError(f'Mesh mismatch {path}: tris={tris}/{item["triangles"]}, bounds_error_cm={err}, actual={actual_min}/{actual_max}')
        slots=mesh.get_editor_property('static_materials')
        dirty=imported
        for slot,entry in enumerate(slots):
            material_name=str(entry.get_editor_property('imported_material_slot_name'))
            if material_name not in mats:material_name=str(entry.get_editor_property('material_slot_name'))
            if material_name not in mats:raise RuntimeError('Unmapped material '+material_name)
            if mesh.get_material(slot)!=mats[material_name]:mesh.set_material(slot,mats[material_name]);dirty=True
        if item['layer']=='Markings' and MESH.get_num_uv_channels(mesh,0)<4:raise RuntimeError('Lost decal fade UV channels')
        if dirty:save(mesh)
        records.append(dict(item,asset=path,bounds_error_cm=err));log(f'mesh {i+1}/{len(scene["meshes"])} {path} triangles={tris}')
        if i%16==15:unreal.SystemLibrary.collect_garbage()
    map_path=DEST+'/Maps/'+('L_CS2ImportProbe' if PROBE else 'L_Palmsland')
    if PROBE and LIB.does_asset_exist(map_path):LIB.delete_asset(map_path)
    if LIB.does_asset_exist(map_path):
        raise RuntimeError('Level already exists; refusing to overwrite '+map_path)
    if not LEVEL.new_level(map_path):raise RuntimeError('Cannot create level '+map_path)
    for rec in records:
        mesh=LIB.load_asset(rec['asset']);actor=ACTORS.spawn_actor_from_class(unreal.StaticMeshActor,unreal.Vector(*rec['origin_cm']))
        actor.set_actor_label(rec['name']);actor.set_folder_path('Palmsland/'+rec['layer']);actor.static_mesh_component.set_static_mesh(mesh)
        if rec['layer']!='Roads':actor.static_mesh_component.set_cast_shadow(False)
        actor.tags=['CS2RoadLab',rec['layer']]
    sun=ACTORS.spawn_actor_from_class(unreal.DirectionalLight,unreal.Vector(0,0,120000));sun.set_actor_label('Palmsland Sun')
    sun.set_actor_rotation(unreal.Rotator(pitch=-50,yaw=-35,roll=0),False)
    sun.light_component.set_mobility(unreal.ComponentMobility.MOVABLE);sun.light_component.set_intensity(75000.0)
    sky=ACTORS.spawn_actor_from_class(unreal.SkyLight,unreal.Vector(0,0,120000));sky.set_actor_label('Palmsland Skylight')
    sky.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
    sky.light_component.set_editor_property('intensity',1.0);sky.light_component.set_editor_property('real_time_capture',True)
    ACTORS.spawn_actor_from_class(unreal.SkyAtmosphere,unreal.Vector(0,0,0))
    pp=ACTORS.spawn_actor_from_class(unreal.PostProcessVolume,unreal.Vector());pp.set_editor_property('unbound',True)
    settings=pp.get_editor_property('settings')
    settings.set_editor_property('override_auto_exposure_min_brightness',True);settings.set_editor_property('auto_exposure_min_brightness',13.0)
    settings.set_editor_property('override_auto_exposure_max_brightness',True);settings.set_editor_property('auto_exposure_max_brightness',13.0)
    pp.set_editor_property('settings',settings)
    focus=[250544.8,-62526.2,56085.7] if not PROBE else [0,0,0]
    start=ACTORS.spawn_actor_from_class(unreal.PlayerStart,unreal.Vector(focus[0],focus[1],focus[2]+3000));start.set_actor_label('Palmsland Overview Start')
    camera=unreal.Vector(focus[0]+42000,focus[1]-55000,focus[2]+47000)
    rotation=unreal.MathLibrary.find_look_at_rotation(camera,unreal.Vector(*focus))
    unreal.EditorLevelLibrary.set_level_viewport_camera_info(camera,rotation)
    for i,view in enumerate(scene.get('views') or []):
        marker=ACTORS.spawn_actor_from_class(unreal.TargetPoint,unreal.Vector(*(v*100 for v in view['position'])))
        marker.set_actor_label(f'View_{i+1:02d}_'+view['Name']);marker.set_folder_path('Palmsland/Viewpoints')
    if not LEVEL.save_current_level():raise RuntimeError('Level save failed')
    summary={'level':map_path,'roads':scene['roads'],'source_triangles':scene['triangles'],'meshes':records,'materials':len(mats),'textures':len(textures)}
    (OUT/('probe-import.json' if PROBE else 'import-result.json')).write_text(json.dumps(summary,indent=2),encoding='utf-8')
    log('COMPLETE '+map_path)

try:main()
except Exception:
    (OUT/'import-error.txt').write_text(traceback.format_exc(),encoding='utf-8')
    unreal.log_error('CS2_IMPORT_FAILED '+traceback.format_exc());raise
