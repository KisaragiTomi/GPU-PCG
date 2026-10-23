"""Read-only verification of saved CSSW copies; execute inside Unreal Editor."""
import json, traceback, re, unreal
from pathlib import Path

OUT = Path(unreal.Paths.project_saved_dir()) / 'CSSWCustomMaterial'
MEL = unreal.MaterialEditingLibrary
BUILD = json.loads((OUT / 'build-result.json').read_text(encoding='utf-8'))
FLAT = json.loads((OUT / 'flatten/result.json').read_text(encoding='utf-8')) if (OUT / 'flatten/result.json').exists() else None
REPORT = {'settings': {}, 'instances': [], 'graphs': {}, 'mismatches': []}

def serial(value):
    if isinstance(value, unreal.LinearColor):
        return [value.r, value.g, value.b, value.a]
    if isinstance(value, unreal.Object):
        return value.get_path_name()
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    return str(value)

def check(label, a, b):
    if a != b:
        REPORT['mismatches'].append({'field': label, 'source': a, 'copy': b})

def stats(obj):
    s = MEL.get_statistics(obj)
    return {k: s.get_editor_property(k) for k in (
        'num_vertex_shader_instructions', 'num_pixel_shader_instructions',
        'num_samplers', 'num_vertex_texture_samples', 'num_pixel_texture_samples')}

def main():
    graphs = json.loads((OUT / 'graphs.json').read_text(encoding='utf-8'))
    for name, info in BUILD['graphs'].items():
        a = unreal.load_asset(info['source'])
        b = unreal.load_asset(FLAT['material'] if name == 'M_FluxSurfaceOver' and FLAT else info['copy'])
        assert a and b, name
        count = MEL.get_num_material_expressions(b) if isinstance(b, unreal.Material) else MEL.get_num_material_expressions_in_function(b)
        flattened = name == 'M_FluxSurfaceOver' and count != info['after'] and (OUT / 'flatten/result.json').exists()
        if flattened:
            flat = json.loads((OUT / 'flatten/result.json').read_text(encoding='utf-8'))
            check(name + '.expression_count', flat['after'], count)
            task = unreal.AssetExportTask(); task.object = b
            file = OUT / 'flatten/validated-master.t3d'; task.filename = str(file)
            task.automated = True; task.prompt = False; task.replace_identical = True
            task.exporter = unreal.ObjectExporterT3D()
            assert unreal.Exporter.run_asset_export_task(task)
            raw = file.read_bytes(); raw = raw.decode('utf-16' if raw.startswith(b'\xff\xfe') else 'utf-8-sig')
            check('flatten.function_call_nodes', 0, raw.count('Class=/Script/Engine.MaterialExpressionMaterialFunctionCall'))
            check('flatten.function_io_nodes', 0, len(re.findall(r'Class=/Script/Engine.MaterialExpressionFunction(?:Input|Output)', raw)))
            check('flatten.function_owner_links', 0, len(re.findall(r'^\s+Function=', raw, re.M)))
            REPORT['flatten'] = flat
        else:
            check(name + '.expression_count', info['after'], count)
        calls = []
        for node in graphs[name]['nodes'].values():
            if flattened: continue
            if node['type'] != 'MaterialFunctionCall':
                continue
            dep = node['props']['MaterialFunction'].split("'")[1].split('.')[-1]
            if dep in BUILD['graphs']:
                fn = unreal.find_object(b, node['name']).get_editor_property('material_function')
                check(name + '.' + node['name'], BUILD['graphs'][dep]['copy'], fn.get_path_name())
                calls.append(fn.get_path_name())
        REPORT['graphs'][name] = {'expressions': count, 'function_copies': calls}
        if name == 'M_FluxSurfaceOver':
            for prop in ('blend_mode', 'shading_model', 'two_sided', 'use_material_attributes',
                         'tangent_space_normal', 'opacity_mask_clip_value', 'dithered_lod_transition',
                         'used_with_instanced_static_meshes'):
                x, y = serial(a.get_editor_property(prop)), serial(b.get_editor_property(prop))
                check('material.' + prop, x, y)
                REPORT['settings'][prop] = [x, y]
            REPORT['master_stats'] = {'original': stats(a), 'custom': stats(b)}
    for info in BUILD['instances']:
        a, b = [unreal.load_asset(info[k]) for k in ('source', 'copy')]
        expected_parent = (FLAT['material'] + '.' + FLAT['material'].split('/')[-1]) if FLAT and info['source'].endswith('/MI_RiverSurfaceOver') else info['parent']
        check(info['copy'] + '.parent', expected_parent, b.get_editor_property('parent').get_path_name())
        entry = {'source': info['source'], 'copy': info['copy'], 'parameters': {},
                 'stats': {'original': stats(a), 'custom': stats(b)}}
        for kind in ('scalar', 'vector', 'texture', 'static_switch'):
            names = getattr(MEL, 'get_' + kind + '_parameter_names')
            getter = getattr(MEL, 'get_material_instance_' + kind + '_parameter_value')
            na, nb = sorted(map(str, names(a))), sorted(map(str, names(b)))
            check(info['copy'] + '.' + kind + '_names', na, nb)
            entry['parameters'][kind] = {}
            for key in na:
                x, y = serial(getter(a, key)), serial(getter(b, key))
                check(info['copy'] + '.' + key, x, y)
                entry['parameters'][kind][key] = [x, y]
        REPORT['instances'].append(entry)
    (OUT / 'validation.json').write_text(json.dumps(REPORT, indent=2), encoding='utf-8')
    assert not REPORT['mismatches'], REPORT['mismatches']
    unreal.log('CSSW_SAVED_ASSET_VALIDATION_PASS')

try:
    main()
except Exception:
    (OUT / 'validation-error.txt').write_text(traceback.format_exc(), encoding='utf-8')
    unreal.log_error(traceback.format_exc())
    raise
