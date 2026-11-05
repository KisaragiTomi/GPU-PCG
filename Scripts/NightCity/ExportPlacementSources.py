"""Read-only source-model export for the first placement-surface batch. Run in a Python commandlet."""
import hashlib
import json
from pathlib import Path
import traceback
import unreal

ROOT = Path(unreal.Paths.project_dir()).resolve()
OUT = ROOT / 'Saved/CodexNightCityPlacement'
OUT.mkdir(parents=True, exist_ok=True)
SELECTED = json.loads(Path(__file__).with_name('PlacementBatch01.sources.json').read_text(encoding='utf-8'))['models']


def vec(v):
    return [v.x, v.y, v.z]


def materials(sm, lod):
    result = unreal.GeometryScript_AssetUtils.get_section_material_list_from_static_mesh(sm, lod)
    # API returns materials, slot names and section material indices.
    vals = result if isinstance(result, tuple) else (result,)
    mats = next((v for v in vals if hasattr(v, '__iter__') and len(v) and (v[0] is None or isinstance(v[0], unreal.MaterialInterface))), [])
    out=[]
    for m in mats:
        chain=[]
        cur=m
        for _ in range(8):
            if cur is None: break
            chain.append(cur.get_path_name())
            try: cur=cur.get_editor_property('parent')
            except Exception: break
        out.append({'path':m.get_path_name() if m else '', 'parents':chain, 'metadata':{str(k):str(v) for k,v in unreal.EditorAssetLibrary.get_metadata_tag_values(m).items()} if m else {}})
    return out


def read_source(sm):
    """Read built source LOD0 without editing the mesh; also used for revision checks."""
    dm=unreal.DynamicMesh()
    opts=unreal.GeometryScriptCopyMeshFromAssetOptions()
    opts.set_editor_property('request_tangents',False)
    lod=unreal.GeometryScriptMeshReadLOD()
    lod.set_editor_property('lod_type',unreal.GeometryScriptLODType.SOURCE_MODEL)
    _, outcome=unreal.GeometryScript_AssetUtils.copy_mesh_from_static_mesh(sm,dm,opts,lod)
    if outcome != unreal.GeometryScriptOutcomePins.SUCCESS: raise RuntimeError('No source LOD0')
    _, positions, _=unreal.GeometryScript_MeshQueries.get_all_vertex_positions(dm,False)
    vertices=[vec(v) for v in unreal.GeometryScript_List.convert_vector_list_to_array(positions)]
    _, triangle_ids, _=unreal.GeometryScript_MeshQueries.get_all_triangle_i_ds(dm)
    ids=unreal.GeometryScript_List.convert_index_list_to_array(triangle_ids)
    triangles=[]
    triangle_normals=[]
    for tid in ids:
        t,valid=unreal.GeometryScript_MeshQueries.get_triangle_indices(dm,tid)
        mid,mvalid=unreal.GeometryScript_Materials.get_triangle_material_id(dm,tid)
        if not valid or not mvalid: raise RuntimeError('Incomplete source topology or material IDs')
        triangles.append([t.x,t.y,t.z,mid])
        _,n1,n2,n3,nvalid=unreal.GeometryScript_MeshQueries.get_triangle_normals(dm,tid)
        triangle_normals.append([vec(n1),vec(n2),vec(n3)] if nvalid else [])
    box=sm.get_bounding_box()
    entry=dict(path=sm.get_path_name(),vertices=vertices,triangles=triangles,triangle_normals=triangle_normals,materials=materials(sm,lod),bounds=[vec(box.min),vec(box.max)],geometry_source='SourceModel LOD0, apply build settings and scale')
    payload=json.dumps({k:entry[k] for k in ['vertices','triangles','triangle_normals','materials']},sort_keys=True,separators=(',',':'))
    entry['source_geometry_hash']=hashlib.sha256(payload.encode()).hexdigest()
    return entry


def main():
    report=[]
    for selected in SELECTED:
        key=selected['mesh']; entry=dict(selected)
        try:
            path=f'/Game/BuildingGenerator/Cyberpunk2077/Meshes/{key[3:5]}/{key}'
            sm=unreal.load_asset(path)
            if not sm: raise RuntimeError('Source asset not found: '+path)
            entry.update(read_source(sm))
            (OUT/(key+'.json')).write_text(json.dumps(entry,ensure_ascii=False,separators=(',',':')),encoding='utf-8')
            summary={k:v for k,v in entry.items() if k not in ['vertices','triangles','triangle_normals']}
            summary.update(vertex_count=len(entry['vertices']),triangle_count=len(entry['triangles']))
            report.append(summary)
            unreal.log('NCPLACEMENT EXPORTED '+key+' '+str(len(entry['triangles'])))
        except Exception:
            entry={k:v for k,v in entry.items() if k not in ['vertices','triangles','triangle_normals']}
            entry['error']=traceback.format_exc()
            report.append(entry)
            unreal.log_error('NCPLACEMENT '+entry['error'])
    (OUT/'sources.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    if any('error' in e for e in report): raise RuntimeError('Placement source export failed; see sources.json')


if __name__=='__main__': main()
