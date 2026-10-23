import unreal,json,re
from pathlib import Path
OUT=Path(unreal.Paths.project_saved_dir())/'CSSWCustomMaterial'
plan=json.loads((OUT/'plan.json').read_text());meta=json.loads((OUT/'metadata.json').read_text());g=json.loads((OUT/'graphs.json').read_text());build=json.loads((OUT/'build-result.json').read_text())
MEL=unreal.MaterialEditingLibrary
mat=unreal.load_asset(build['graphs']['M_FluxSurfaceOver']['copy']);report=[]
for name,p in plan.items():
    owner=unreal.load_asset(build['graphs'][name]['copy']);repl={}
    for i in range(160):
        n=unreal.find_object(owner,'MaterialExpressionCustom_'+str(i))
        if n:
            desc=n.get_editor_property('description')
            if desc.startswith('CSSW '):repl['MaterialExpression'+desc.split()[1]]=n
    for key,info in meta[name]['nodes'].items():
        if key in p['convert']:continue
        n=unreal.find_object(owner,key);inputs=MEL.get_inputs_for_material_expression(mat,n)
        for pin,src,actual in zip(info['input_names'],info['input_nodes'],inputs):
            expected=(repl.get(src) or unreal.find_object(owner,src)) if src else None
            if actual!=expected:report.append([name,key,pin,src,str(expected),str(actual)])
    for group in p['groups']:
        node=repl[group['root']];actual=MEL.get_inputs_for_material_expression(mat,node)
        for ip,a in zip(group['inputs'],actual):
            e=unreal.find_object(owner,ip['node'])
            if e!=a:report.append([name,group['root'],ip['name'],str(e),str(a)])
    for k in ('scalar','vector','texture','static_switch'):
        if name=='M_FluxSurfaceOver':
            fun=getattr(MEL,'get_'+k+'_parameter_names',None)
            if fun:unreal.log(k+' '+str(fun(mat)))
(OUT/'link-audit.json').write_text(json.dumps(report,indent=2))
unreal.log('CSSW_LINK_MISMATCHES '+str(len(report)))
unreal.log(str(report[:10]))
