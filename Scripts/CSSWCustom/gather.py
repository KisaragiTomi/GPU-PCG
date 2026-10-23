import unreal,json,re,traceback
from pathlib import Path
OUT=Path(unreal.Paths.project_saved_dir())/'CSSWCustomMaterial'
G=json.loads((OUT/'graphs.json').read_text(encoding='utf-8'))
MEL=unreal.MaterialEditingLibrary
mat=unreal.load_asset('/PCGPlugins/ShallowWater/Material30/Materials/M_FluxSurfaceOver')
result={}
try:
    pending=['M_FluxSurfaceOver'];seen=set()
    while pending:
        name=pending.pop()
        if name in seen:continue
        seen.add(name)
        g=G.get(name)
        if not g:continue
        owner=unreal.load_asset(g['root']['path'])
        result[name]={'path':owner.get_path_name(),'nodes':{},'roots':{}}
        for n in g['nodes'].values():
            node=unreal.find_object(owner,n['name'])
            if not node:raise RuntimeError(n['name'])
            result[name]['nodes'][n['name']]={'input_names':list(MEL.get_material_expression_input_names(node)), 'input_nodes':[x.get_name() if x else None for x in MEL.get_inputs_for_material_expression(mat,node)]}
            defaults={}
            for key in ('const_a','const_b','const_alpha','const_exponent','min_default','max_default','period','r','g','b','a','constant','equals_threshold'):
                try:
                    v=node.get_editor_property(key)
                    if isinstance(v,(float,int,bool)):defaults[key]=v
                    elif isinstance(v,unreal.LinearColor):defaults[key]=[v.r,v.g,v.b,v.a]
                except Exception:pass
            result[name]['nodes'][n['name']]['defaults']=defaults
            if n['type']=='MaterialFunctionCall':
                fn=node.get_editor_property('material_function')
                if fn:
                    fnname=fn.get_name();pending.append(fnname)
                    if fnname not in G:
                        task=unreal.AssetExportTask();task.object=fn;task.filename=str(OUT/(fnname+'.t3d'));task.automated=True;task.prompt=False;task.replace_identical=True;task.exporter=unreal.ObjectExporterT3D();unreal.Exporter.run_asset_export_task(task)
        if isinstance(owner,unreal.Material):
            for prop in unreal.MaterialProperty:
                try:
                    n=MEL.get_material_property_input_node(owner,prop)
                    if n:result[name]['roots'][prop.name]={'node':n.get_name(),'output':MEL.get_material_property_input_node_output_name(owner,prop)}
                except Exception:pass
    (OUT/'metadata.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    unreal.log('CSSW_METADATA_DONE '+str(len(result)))
except Exception:
    (OUT/'gather-error.txt').write_text(traceback.format_exc(),encoding='utf-8');unreal.log_error(traceback.format_exc())
    raise
