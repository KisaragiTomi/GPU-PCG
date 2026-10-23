"""Create transient static-permutation pairs for the active comparison scene."""
import unreal,builtins,json,traceback
from pathlib import Path
OUT=Path(unreal.Paths.project_saved_dir())/'CSSWCustomMaterial'
MEL=unreal.MaterialEditingLibrary
CASES={
    'no-foam':{'UseFoam':False,'SwithSim':True},
    'no-detail':{'UseDetailWave':False,'UseDetailWaveWPO':False,'SwithSim':True},
    'texture-simulation':{'SwithSim':False,'UseFoam':True,'UseDetailWave':True,'UseDetailWaveWPO':True},
    'texture-no-wpo':{'SwithSim':False,'UseFoam':True,'UseDetailWave':True,'UseDetailWaveWPO':False},
    'distortion-off':{'SwithSim':True,'UseFoam':True,'UseFoamDistortion':False,'UseDetailWave':True}}
try:
    result=[];pairs={}
    for case,switches in CASES.items():
        pair=[]
        for idx,parent in enumerate(builtins.cssw_state['saved_pair']):
            inst=unreal.new_object(unreal.MaterialInstanceConstant)
            MEL.set_material_instance_parent(inst,parent)
            # UE 5.7.4 setter always returns false despite applying the value.
            for key,value in switches.items():
                MEL.set_material_instance_static_switch_parameter_value(inst,key,value)
                assert MEL.get_material_instance_static_switch_parameter_value(inst,key)==value,(case,key)
            MEL.set_material_instance_vector_parameter_value(inst,'CSSW_SimCenter',unreal.LinearColor(1605,1785,53000,0))
            assert MEL.get_material_instance_vector_parameter_value(inst,'CSSW_SimCenter').b==53000
            MEL.update_material_instance(inst)
            pair.append(inst)
            stats=MEL.get_statistics(inst)
            result.append({'case':case,'variant':'original' if idx==0 else 'custom','switches':switches,'stats':{k:stats.get_editor_property(k) for k in ['num_vertex_shader_instructions','num_pixel_shader_instructions','num_samplers','num_vertex_texture_samples','num_pixel_texture_samples']}})
        pairs[case]=pair
    builtins.cssw_variants=pairs
    (OUT/'variant-stats.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    unreal.log('CSSW_VARIANTS_READY '+str(len(pairs)))
except Exception:
    (OUT/'variants-error.txt').write_text(traceback.format_exc());unreal.log_error(traceback.format_exc())
