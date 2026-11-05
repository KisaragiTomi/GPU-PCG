"""Create Batch01 profile/preview assets and bind defaults to the 24 explicitly listed source meshes.

Run in a Python commandlet after BuildPlacementBatch.py and compiling CSRoadNet.
Only saves this batch's assets and its named source meshes. Originals are backed up first.
"""
import hashlib
import json
from pathlib import Path
import shutil
import sys
import traceback
import unreal

ROOT=Path(unreal.Paths.project_dir()).resolve()
DATA=ROOT/'Plugins/PCGPlugins/Docs/NightCity/PlacementBatch01'
WORK=ROOT/'Saved/CodexNightCityPlacement'
FOLDER='/PCGPlugins/NightCity/PlacementBatch01'
ASSETS=unreal.EditorAssetLibrary
TOOLS=unreal.AssetToolsHelpers.get_asset_tools()
unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous([FOLDER],True)
sys.path.insert(0,str(Path(__file__).resolve().parent))
from ExportPlacementSources import read_source


def guid(text):
    value,success=unreal.GuidLibrary.parse_string_to_guid(text)
    if not success: raise RuntimeError('Invalid GUID '+text)
    return value


def save(asset):
    if not ASSETS.save_loaded_asset(asset,False): raise RuntimeError('Save failed '+asset.get_path_name())


def preview_material():
    path=FOLDER+'/M_PlacementPreview'
    if ASSETS.does_asset_exist(path):
        existing=unreal.load_asset(path)
        if not isinstance(existing,unreal.Material) or ASSETS.get_metadata_tag(existing,'PlacementBatch')!='01':
            raise RuntimeError('Refusing to replace unrelated asset '+path)
    m=unreal.load_asset(path) if ASSETS.does_asset_exist(path) else TOOLS.create_asset('M_PlacementPreview',FOLDER,unreal.Material,unreal.MaterialFactoryNew())
    if ASSETS.get_metadata_tag(m,'PlacementBatch')=='01': return m
    edit=unreal.MaterialEditingLibrary
    color=edit.create_material_expression(m,unreal.MaterialExpressionConstant3Vector)
    color.set_editor_property('constant',unreal.LinearColor(.02,.7,.25,1))
    edit.connect_material_property(color,'',unreal.MaterialProperty.MP_BASE_COLOR)
    edit.connect_material_property(color,'',unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    m.set_editor_property('two_sided',True)
    ASSETS.set_metadata_tag(m,'PlacementBatch','01')
    edit.recompile_material(m)
    save(m)
    return m


def preview(profile,material):
    if not profile['regions']: return None
    name='SM_PL_'+profile['mesh'][3:]
    path=FOLDER+'/Preview/'+name
    if ASSETS.does_asset_exist(path):
        existing=unreal.load_asset(path)
        if not isinstance(existing,unreal.StaticMesh) or ASSETS.get_metadata_tag(existing,'PlacementBatch')!='01':
            raise RuntimeError('Refusing to replace unrelated asset '+path)
    mesh=unreal.load_asset(path) if ASSETS.does_asset_exist(path) else TOOLS.create_asset(name,FOLDER+'/Preview',unreal.StaticMesh,None)
    desc=unreal.StaticMesh.create_static_mesh_description()
    group=desc.create_polygon_group()
    desc.set_polygon_group_material_slot_name(group,'Placement')
    for region in profile['regions']:
        vertices=[]
        for p in region['positions']:
            v=desc.create_vertex(); desc.set_vertex_position(v,unreal.Vector(*p)); vertices.append(v)
        for t in region['triangles']:
            # Profile uses mathematical CCW, StaticMesh uses the opposite render winding.
            ids=[]
            for index in [t[0],t[2],t[1]]:
                vi=desc.create_vertex_instance(vertices[index]); p=region['positions'][index]
                desc.set_vertex_instance_uv(vi,unreal.Vector2D(p[0]/100,p[2]/100),0)
                ids.append(vi)
            desc.create_triangle(group,ids)
    mesh.set_editor_property('static_materials',[unreal.StaticMaterial(material_interface=material,material_slot_name='Placement')])
    mesh.build_from_static_mesh_descriptions([desc],False,False)
    ASSETS.set_metadata_tag(mesh,'PlacementBatch','01')
    ASSETS.set_metadata_tag(mesh,'SourceMesh',profile['source_mesh'])
    ASSETS.set_metadata_tag(mesh,'Purpose','Derived visualization; authoritative geometry is in DA_PL profile; source-local cm, no offset.')
    save(mesh)
    return mesh


def import_profile(data):
    name='DA_PL_'+data['mesh'][3:]
    path=FOLDER+'/Profiles/'+name
    if ASSETS.does_asset_exist(path):
        profile=unreal.load_asset(path)
        if not isinstance(profile,unreal.CSCityPlacementProfile) or ASSETS.get_metadata_tag(profile,'PlacementBatch')!='01':
            raise RuntimeError('Refusing to replace unrelated asset '+path)
    else:
        factory=unreal.DataAssetFactory()
        factory.set_editor_property('data_asset_class',unreal.CSCityPlacementProfile.static_class())
        profile=TOOLS.create_asset(name,FOLDER+'/Profiles',unreal.CSCityPlacementProfile,factory)
    profile.set_editor_property('profile_id',guid(data['profile_id']))
    profile.set_editor_property('source_mesh',unreal.load_asset(data['source_mesh']))
    profile.set_editor_property('source_geometry_hash',data['source_geometry_hash'])
    profile.set_editor_property('state',unreal.CSCityPlacementState.READY if data['state']=='Ready' else unreal.CSCityPlacementState.PENDING_REVIEW)
    profile.set_editor_property('review_notes',data['review_notes'])
    regions=[]
    for r in data['regions']:
        region=unreal.CSCityPlacementRegion()
        for key,value in [('region_id',guid(r['region_id'])),('label',r['label']),('positions',[unreal.Vector(*p) for p in r['positions']]),
            ('triangles',[unreal.IntVector(*t) for t in r['triangles']]),('outward_normal',unreal.Vector(*r['outward_normal'])),
            ('preferred_up',unreal.Vector(*r['preferred_up'])),('allowed_categories',r['allowed_categories'])]:
            region.set_editor_property(key,value)
        regions.append(region)
    profile.set_editor_property('regions',regions)
    raw_hash=hashlib.sha256(json.dumps(data,sort_keys=True).encode()).hexdigest()
    previous=ASSETS.get_metadata_tag(profile,'BatchContentHash')
    if previous and previous!=raw_hash: profile.set_editor_property('data_revision',profile.get_editor_property('data_revision')+1)
    ASSETS.set_metadata_tag(profile,'PlacementBatch','01')
    ASSETS.set_metadata_tag(profile,'BatchContentHash',raw_hash)
    # UE Python consumes a bool return used with one output: success -> output, failure -> None.
    if profile.validate_data() is None: raise RuntimeError(path+': native validation rejected the profile')
    save(profile)
    return profile


def backup_sources(models):
    backup=WORK/'OriginalAssets'
    backup.mkdir(parents=True,exist_ok=True)
    manifest_path=backup/'manifest.json'
    manifest=json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    for data in models:
        key=data['mesh']; rel=Path('Content/BuildingGenerator/Cyberpunk2077/Meshes')/key[3:5]/(key+'.uasset')
        if key in manifest: continue
        source=ROOT/rel; target=backup/(key+'.uasset')
        if target.exists(): raise RuntimeError('Untracked backup already exists '+str(target))
        sha=hashlib.sha256(source.read_bytes()).hexdigest()
        shutil.copy2(source,target)
        assert hashlib.sha256(target.read_bytes()).hexdigest()==sha
        manifest[key]={'source':str(source),'backup':str(target),'sha256':sha}
        manifest_path.write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')


def make_review_level(rows):
    path=FOLDER+'/L_PlacementBatch01_Review'
    if ASSETS.does_asset_exist(path): return path
    levels=unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not levels.new_level_from_template(path,'/Engine/Maps/Templates/Template_Default'):
        raise RuntimeError('Cannot create review level')
    actors=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for i,row in enumerate(rows):
        # Side-by-side source and proxy; identical rotation/scale, separate presentation translations.
        origin=unreal.Vector((i%4)*3300,(i//4)*1700,0)
        src=unreal.load_asset(row['source_mesh'])
        source=actors.spawn_actor_from_class(unreal.StaticMeshActor,origin)
        source.set_actor_label(f'{i+1:02d}_Source_{row["mesh"]}')
        source.static_mesh_component.set_static_mesh(src)
        source.set_actor_enable_collision(False)
        if row['preview']:
            proxy=actors.spawn_actor_from_class(unreal.StaticMeshActor,origin+unreal.Vector(1500,0,0))
            proxy.set_actor_label(f'{i+1:02d}_Regions_{row["mesh"]}')
            proxy.static_mesh_component.set_static_mesh(unreal.load_asset(row['preview']))
            proxy.set_actor_enable_collision(False)
    if not levels.save_current_level(): raise RuntimeError('Review level save failed')
    return path


result={'batch':'PlacementBatch01','assets':[]}
try:
    manifest=json.loads((DATA/'manifest.json').read_text(encoding='utf-8'))
    models=[json.loads((DATA/(m['mesh']+'.json')).read_text(encoding='utf-8')) for m in manifest['models']]
    for data in models:
        source=unreal.load_asset(data['source_mesh'])
        if not source or read_source(source)['source_geometry_hash']!=data['source_geometry_hash']:
            raise RuntimeError('Source revision changed; export, rebuild and review first: '+data['source_mesh'])
        old=unreal.CSCityPlacementLibrary.get_default_profile(source)
        expected=FOLDER+'/Profiles/DA_PL_'+data['mesh'][3:]
        if old and old.get_path_name().split('.')[0]!=expected:
            raise RuntimeError('Source already has another default '+data['source_mesh'])
    backup_sources(models)
    material=preview_material()
    for data in models:
        profile=import_profile(data)
        source=unreal.load_asset(data['source_mesh'])
        old=unreal.CSCityPlacementLibrary.get_default_profile(source)
        if old and old!=profile: raise RuntimeError('Source already has another default '+data['source_mesh'])
        if unreal.CSCityPlacementLibrary.set_default_profile(source,profile) is None:
            raise RuntimeError('Native binding validation rejected '+data['source_mesh'])
        save(source)
        mesh=preview(data,material)
        result['assets'].append({'mesh':data['mesh'],'source_mesh':data['source_mesh'],'profile':profile.get_path_name(),'state':data['state'],
            'regions':len(data['regions']),'triangles':sum(len(r['triangles']) for r in data['regions']),'preview':mesh.get_path_name() if mesh else None})
        unreal.log('NCPLACEMENT SAVED '+data['mesh'])
    result['review_level']=make_review_level(result['assets'])
    result['success']=True
except Exception:
    result['success']=False; result['error']=traceback.format_exc()
    unreal.log_error(result['error'])
finally:
    (WORK/'import_result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
if not result['success']: raise RuntimeError('Placement import failed; see import_result.json')
