"""Apply reviewed plan.json to independent material/function/instance copies in UE."""
import unreal,json,traceback,re
from pathlib import Path

OUT=Path(unreal.Paths.project_saved_dir())/'CSSWCustomMaterial'
PLAN=json.loads((OUT/'plan.json').read_text(encoding='utf-8'))
META=json.loads((OUT/'metadata.json').read_text(encoding='utf-8'))
G=json.loads((OUT/'graphs.json').read_text(encoding='utf-8'))
DEST='/PCGPlugins/ShallowWater/Material30/Custom'
MEL=unreal.MaterialEditingLibrary
EAL=unreal.EditorAssetLibrary
copies={};replacements={};report={'graphs':{},'instances':[]}

def clone(src,dest):
    # Never overwrite an earlier copy. Choose a different DEST for another build.
    if EAL.does_asset_exist(dest):
        raise RuntimeError('Destination already exists: '+dest)
    obj=EAL.duplicate_asset(src,dest)
    if not obj:raise RuntimeError('Duplicate failed '+src)
    return obj

def find(owner,name):
    obj=unreal.find_object(owner,name)
    if not obj:raise RuntimeError('Missing duplicated node '+name)
    return obj

def main():
    for name,p in PLAN.items():
        target=DEST+('/M_CSSW_Surface_Custom' if name=='M_FluxSurfaceOver' else '/Functions/'+name+'_Custom')
        copies[name]=clone(p['path'],target)
    for name,p in PLAN.items():
        owner=copies[name];is_mat=isinstance(owner,unreal.Material);repl={};replacements[name]=repl
        for n in G[name]['nodes'].values():
            if n['type']=='MaterialFunctionCall':
                fn=n['props']['MaterialFunction'].split("'")[1].split('.')[-1]
                if fn in copies:find(owner,n['name']).set_editor_property('material_function',copies[fn])
        for group in p['groups']:
            original=find(owner,group['root']);x,y=MEL.get_material_expression_node_position(original)
            node=MEL.create_material_expression(owner,unreal.MaterialExpressionCustom,x,y) if is_mat else MEL.create_material_expression_in_function(owner,unreal.MaterialExpressionCustom,x,y)
            node.set_editor_property('code',group['code'])
            node.set_editor_property('output_type',getattr(unreal.CustomMaterialOutputType,'CMOT_FLOAT'+str(group['width'])))
            node.set_editor_property('description','CSSW '+group['root'].replace('MaterialExpression','')+' ['+str(len(group['included']))+' nodes]')
            node.set_editor_property('desc','Original operations: '+', '.join(group['included']))
            inputs=[]
            for inp in group['inputs']:
                ci=unreal.CustomInput();ci.set_editor_property('input_name',inp['name']);inputs.append(ci)
            node.set_editor_property('inputs',inputs)
            repl[group['root']]=node
            for inp in group['inputs']:
                assert MEL.connect_material_expressions(find(owner,inp['node']),inp['output'],node,inp['name']), (name,group['root'],inp)
        for old,info in META[name]['nodes'].items():
            if old in p['convert']:continue
            target=find(owner,old)
            for pin,source in zip(info['input_names'],info['input_nodes']):
                if source in repl:
                    assert MEL.connect_material_expressions(repl[source],'',target,pin),(name,old,pin,source)
        # Setting MaterialFunction invokes UE PostEditChange and clears ALL call inputs.
        # Restore retained-source links too, preserving each original output index/name.
        for n in G[name]['nodes'].values():
            if n['type']!='MaterialFunctionCall':continue
            dep=n['props']['MaterialFunction'].split("'")[1].split('.')[-1]
            if dep not in copies:continue
            for i,pin in enumerate(META[name]['nodes'][n['name']]['input_names']):
                raw=n['props'].get('FunctionInputs('+str(i)+')','')
                match=re.search(r'Expression="[^\x27]*\x27[^\x27]*:(MaterialExpression\w+)\x27',raw)
                if not match:continue
                source=match[1];idx=re.search(r'OutputIndex=(-?\d+)',raw);idx=int(idx[1]) if idx else 0
                src=repl.get(source) or find(owner,source)
                out=''
                if source not in repl and idx:
                    sn=G[name]['nodes'][source];val=sn['props'].get('Outputs('+str(idx)+')','')
                    label=re.search(r'OutputName="([^"]*)"',val)
                    out=label[1] if label else ['','R','G','B','A','RGBA'][idx]
                assert MEL.connect_material_expressions(src,out,find(owner,n['name']),pin),(name,n['name'],pin,source,out)
        if is_mat:
            for prop,info in META[name]['roots'].items():
                if info['node'] in repl:assert MEL.connect_material_property(repl[info['node']],'',getattr(unreal.MaterialProperty,prop))
        for old in p['convert']:
            obj=find(owner,old)
            if is_mat:MEL.delete_material_expression(owner,obj)
            else:MEL.delete_material_expression_in_function(owner,obj)
        report['graphs'][name]={'source':p['path'],'copy':owner.get_path_name(),'before':p['before'],'replaced':len(p['convert']),'custom_added':len(p['groups']),'after':MEL.get_num_material_expressions(owner) if is_mat else MEL.get_num_material_expressions_in_function(owner)}
    # Refresh leaves first, then callers, then master. Dependency graph has no cycles.
    done=set()
    def update(name):
        if name in done:return
        done.add(name)
        for n in G[name]['nodes'].values():
            if n['type']=='MaterialFunctionCall':
                dep=n['props']['MaterialFunction'].split("'")[1].split('.')[-1]
                if dep in copies:update(dep)
        obj=copies[name]
        if isinstance(obj,unreal.Material):MEL.recompile_material(obj)
        else:MEL.update_material_function(obj)
        assert EAL.save_loaded_asset(obj,only_if_is_dirty=False)
    update('M_FluxSurfaceOver')
    instance_specs=[
        ('/PCGPlugins/ShallowWater/Material30/Surface/Templates/River/Materials/MI_RiverSurfaceOver','MI_CSSW_River_Custom','M_FluxSurfaceOver'),
        ('/PCGPlugins/ShallowWater/VelocityHeight/Material/MI_WaterSurface','MI_CSSW_WaterSurface_Custom','MI_CSSW_River_Custom'),
        ('/PCGPlugins/ShallowWater/VelocityHeight/CSSWData/MI_CSSW_Water_3333333','MI_CSSW_Water_3333333_Custom','MI_CSSW_WaterSurface_Custom'),
        ('/PCGPlugins/ShallowWater/VelocityHeight/CSSWData/MI_CSSW_Water_25155946','MI_CSSW_Water_25155946_Custom','MI_CSSW_River_Custom'),
        ('/PCGPlugins/SpaceColonization/CSSWData/MI_CSSW_Water_8141441','MI_CSSW_Water_8141441_Custom','MI_CSSW_River_Custom')]
    for src,name,parent in instance_specs:
        if not EAL.does_asset_exist(src):continue
        inst=clone(src,DEST+'/'+name)
        MEL.set_material_instance_parent(inst,copies[parent]);MEL.update_material_instance(inst)
        assert EAL.save_loaded_asset(inst,only_if_is_dirty=False)
        copies[name]=inst;report['instances'].append({'source':src,'copy':inst.get_path_name(),'parent':copies[parent].get_path_name()})
    (OUT/'build-result.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    unreal.log('CSSW_CUSTOM_BUILD_DONE')

try:main()
except Exception:
    (OUT/'build-error.txt').write_text(traceback.format_exc(),encoding='utf-8');unreal.log_error(traceback.format_exc())
