"""Read-only fresh-process validation of saved Batch01 assets and native rejection paths."""
import hashlib
import json
import math
from pathlib import Path
import sys
import traceback
import unreal

ROOT=Path(unreal.Paths.project_dir()).resolve()
DATA=ROOT/'Plugins/PCGPlugins/Docs/NightCity/PlacementBatch01'
WORK=ROOT/'Saved/CodexNightCityPlacement'
sys.path.insert(0,str(Path(__file__).resolve().parent))
from ExportPlacementSources import read_source, vec


def require(condition,message):
    if not condition: raise AssertionError(message)


def distance(a,b):
    return math.sqrt(sum((x-y)**2 for x,y in zip(a,b)))


def guid(text):
    result,ok=unreal.GuidLibrary.parse_string_to_guid(text)
    require(ok,'Invalid GUID')
    return result


def region_from_json(data):
    r=unreal.CSCityPlacementRegion()
    for key,value in [('region_id',guid(data['region_id'])),('label',data['label']),
        ('positions',[unreal.Vector(*p) for p in data['positions']]),('triangles',[unreal.IntVector(*t) for t in data['triangles']]),
        ('outward_normal',unreal.Vector(*data['outward_normal'])),('preferred_up',unreal.Vector(*data['preferred_up'])),
        ('allowed_categories',data['allowed_categories'])]: r.set_editor_property(key,value)
    return r


def transient_profile(data):
    p=unreal.new_object(unreal.CSCityPlacementProfile)
    p.set_editor_property('profile_id',guid(data['profile_id']))
    p.set_editor_property('source_mesh',unreal.load_asset(data['source_mesh']))
    p.set_editor_property('source_geometry_hash',data['source_geometry_hash'])
    p.set_editor_property('state',unreal.CSCityPlacementState.READY)
    p.set_editor_property('regions',[region_from_json(r) for r in data['regions']])
    return p


result={'success':False,'assets':[],'negative_cases':[]}
try:
    manifest=json.loads((DATA/'manifest.json').read_text(encoding='utf-8'))
    imported=json.loads((WORK/'import_result.json').read_text(encoding='utf-8'))
    require(imported['success'] and len(imported['assets'])==24,'Incomplete import')
    backups=json.loads((WORK/'OriginalAssets/manifest.json').read_text(encoding='utf-8'))
    ids=set()
    for item in imported['assets']:
        data=json.loads((DATA/(item['mesh']+'.json')).read_text(encoding='utf-8'))
        source=unreal.load_asset(item['source_mesh'])
        p=unreal.load_asset(item['profile'])
        require(p is not None and p.validate_data() is not None,'Invalid saved profile')
        require(unreal.CSCityPlacementLibrary.get_default_profile(source)==p,'Binding not persisted')
        require(p.get_editor_property('source_mesh')==source,'Profile ownership changed')
        require(p.get_editor_property('profile_id').to_string()==guid(data['profile_id']).to_string(),'Profile ID changed')
        require(data['profile_id'] not in ids,'Duplicate profile ID'); ids.add(data['profile_id'])
        require(p.get_editor_property('data_revision')==1,'Unexpected revision: identical import must not increment')
        state=unreal.CSCityPlacementState.READY if data['regions'] else unreal.CSCityPlacementState.PENDING_REVIEW
        require(p.get_editor_property('state')==state,'Wrong state')
        digest=read_source(source)['source_geometry_hash']
        require(digest==data['source_geometry_hash']==p.get_editor_property('source_geometry_hash'),'Source geometry/material fingerprint changed')
        b=backups[item['mesh']]
        require(hashlib.sha256(Path(b['backup']).read_bytes()).hexdigest()==b['sha256'],'Backup damaged')
        regions=p.get_editor_property('regions')
        require(len(regions)==len(data['regions']),'Wrong region count')
        expected=[]
        for r,j in zip(regions,data['regions']):
            require(r.get_editor_property('region_id').to_string()==guid(j['region_id']).to_string(),'Region ID changed')
            positions=r.get_editor_property('positions'); triangles=r.get_editor_property('triangles')
            require(len(positions)==len(j['positions']) and len(triangles)==len(j['triangles']),'Array sizes changed')
            require(all(distance(vec(a),b)<1e-6 for a,b in zip(positions,j['positions'])),'Position serialization changed')
            require(all([t.x,t.y,t.z]==want for t,want in zip(triangles,j['triangles'])),'Topology changed')
            require(distance(vec(r.get_editor_property('outward_normal')),j['outward_normal'])<1e-6,'Normal changed')
            require(distance(vec(r.get_editor_property('preferred_up')),j['preferred_up'])<1e-6,'Tangent changed')
            require(list(map(str,r.get_editor_property('allowed_categories')))==j['allowed_categories'],'Categories changed')
            expected.extend([[j['positions'][t[0]],j['positions'][t[2]],j['positions'][t[1]]] for t in j['triangles']])
        if item['preview']:
            preview=read_source(unreal.load_asset(item['preview']))
            require(len(preview['triangles'])==len(expected),'Preview triangle count mismatch')
            remaining=list(expected)
            for tri in preview['triangles']:
                actual=[preview['vertices'][i] for i in tri[:3]]
                match=next((i for i,t in enumerate(remaining) if any(all(distance(actual[k],t[(k+shift)%3])<.001 for k in range(3)) for shift in range(3))),None)
                require(match is not None,'Preview topology, local coordinates or winding differs')
                remaining.pop(match)
        else: require(not expected,'Missing preview')
        result['assets'].append({'mesh':item['mesh'],'state':data['state'],'regions':len(regions),'triangles':len(expected),'source_fingerprint_unchanged':True,'binding_reloaded':True,'preview_matches_profile':True,'backup_verified':True})

    base=json.loads((DATA/(imported['assets'][0]['mesh']+'.json')).read_text(encoding='utf-8'))
    for case in ['ready_empty','duplicate_id','bad_index','reversed_winding','non_planar','non_finite','bad_normal','bad_hash']:
        p=transient_profile(base); rs=[region_from_json(r) for r in base['regions']]
        if case=='ready_empty': rs=[]
        elif case=='duplicate_id': rs.append(region_from_json(base['regions'][0]))
        elif case=='bad_index': rs[0].set_editor_property('triangles',[unreal.IntVector(999999,0,1)])
        elif case=='reversed_winding':
            t=base['regions'][0]['triangles'][0]
            rs[0].set_editor_property('triangles',[unreal.IntVector(t[0],t[2],t[1])])
        elif case in ['non_planar','non_finite']:
            points=list(rs[0].get_editor_property('positions'))
            points[1]=points[1]+rs[0].get_editor_property('outward_normal')*10 if case=='non_planar' else unreal.Vector(float('nan'),0,0)
            rs[0].set_editor_property('positions',points)
        elif case=='bad_normal': rs[0].set_editor_property('outward_normal',unreal.Vector(0,0,0))
        elif case=='bad_hash': p.set_editor_property('source_geometry_hash','x'*64)
        p.set_editor_property('regions',rs)
        require(p.validate_data() is None,'Accepted invalid '+case)
        result['negative_cases'].append(case)
    p=transient_profile(base)
    other=unreal.load_asset(imported['assets'][1]['source_mesh'])
    before=unreal.CSCityPlacementLibrary.get_default_profile(other)
    require(unreal.CSCityPlacementLibrary.set_default_profile(other,p) is None,'Accepted wrong source binding')
    require(unreal.CSCityPlacementLibrary.get_default_profile(other)==before,'Failed binding changed association')
    result['negative_cases'].append('wrong_source_binding_no_mutation')

    levels=unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    require(levels.load_level(imported['review_level']),'Cannot reopen review level')
    actors=unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    result['review_source_actors']=sum('_Source_SM_' in a.get_actor_label() for a in actors)
    result['review_region_actors']=sum('_Regions_SM_' in a.get_actor_label() for a in actors)
    require(result['review_source_actors']==24 and result['review_region_actors']==22,'Review map incomplete')
    result['region_count']=sum(a['regions'] for a in result['assets'])
    result['triangle_count']=sum(a['triangles'] for a in result['assets'])
    result['success']=True
except Exception:
    result['error']=traceback.format_exc()
    unreal.log_error(result['error'])
finally:
    (DATA/'validation.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
if not result['success']: raise RuntimeError('Batch validation failed; see validation.json')
unreal.log('NCPLACEMENT VALIDATED '+str(result['region_count'])+' regions / '+str(result['triangle_count'])+' triangles')
