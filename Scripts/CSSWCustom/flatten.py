"""Inline all project and engine material functions into the Custom master.

No source functions are edited. The edited master is not saved here: validate
compilation, parameter equivalence and captures before explicitly saving it.
"""
import json, re, traceback, collections, unreal
from pathlib import Path

OUT = Path(unreal.Paths.project_saved_dir()) / 'CSSWCustomMaterial' / 'flatten'
OUT.mkdir(parents=True, exist_ok=True)
PATH = '/PCGPlugins/ShallowWater/Material30/Custom/M_CSSW_Surface_Custom'
MEL = unreal.MaterialEditingLibrary
MAT = unreal.load_asset(PATH)
GRAPHS = {}
CREATED = []
CONTEXTS = []
EDGES = []
OUTPUT_LABELS = {}
SKIPPED_PROPERTIES = {}

def output_label(obj, index):
    if isinstance(index, str): return index
    if not index: return ''
    key = (obj, index)
    if key not in OUTPUT_LABELS:
        # PostEditChange can regenerate attribute output labels using display names.
        # Read the duplicated node's current serialization, not its source's labels.
        task = unreal.AssetExportTask(); task.object = obj
        file = OUT / 'output-probe.t3d'; task.filename = str(file)
        task.automated = True; task.prompt = False; task.replace_identical = True
        task.exporter = unreal.ObjectExporterT3D()
        assert unreal.Exporter.run_asset_export_task(task)
        data = file.read_bytes(); data = data.decode('utf-16' if data.startswith(b'\xff\xfe') else 'utf-8-sig')
        props = dict(line.strip().split('=', 1) for line in data.splitlines() if '=' in line and not line.strip().startswith('Begin'))
        label = output_name({'name': obj.get_name(), 'type': obj.get_class().get_name().replace('MaterialExpression',''), 'props':props}, index)
        OUTPUT_LABELS[key] = label
    return OUTPUT_LABELS[key]

def connection(raw):
    match = re.search(r'Expression="[^\x27]*\x27[^\x27]*:(MaterialExpression\w+)\x27', raw)
    if not match:
        return None
    index = re.search(r'OutputIndex=(-?\d+)', raw)
    return match[1], int(index[1]) if index else 0

def graph(owner):
    path = owner.get_path_name()
    if path in GRAPHS:
        return GRAPHS[path]
    file = OUT / (owner.get_name() + '.t3d')
    task = unreal.AssetExportTask()
    task.object = owner; task.filename = str(file); task.automated = True
    task.prompt = False; task.replace_identical = True; task.exporter = unreal.ObjectExporterT3D()
    assert unreal.Exporter.run_asset_export_task(task), path
    raw = file.read_bytes()
    objects = {}; stack = []
    for line in raw.decode('utf-16' if raw.startswith(b'\xff\xfe') else 'utf-8-sig').splitlines():
        line = line.strip()
        if line.startswith('Begin Object'):
            name = re.search(r'Name="([^"]+)"', line)[1]
            obj = objects.setdefault(name, {'name': name, 'props': {}})
            cls = re.search(r'Class=/Script/\w+\.(\w+)', line)
            if cls: obj['type'] = cls[1].replace('MaterialExpression', '')
            stack.append(obj)
        elif line == 'End Object': stack.pop()
        elif stack and '=' in line:
            key, value = line.split('=', 1); stack[-1]['props'][key] = value
    nodes = {k: v for k, v in objects.items() if k.startswith('MaterialExpression') and v.get('type') != 'Comment'}
    result = {'owner': owner, 'nodes': nodes, 'objects': objects}
    GRAPHS[path] = result
    for name, node in nodes.items():
        obj = unreal.find_object(owner, name)
        node['obj'] = obj
        node['pins'] = list(MEL.get_material_expression_input_names(obj))
        node['sources'] = list(MEL.get_inputs_for_material_expression(MAT, obj))
    return result

def input_edge(node, i):
    src = node['sources'][i]
    if not src: return None
    props = node['props']; typ = node['type']; pin = node['pins'][i]
    if typ in ('Custom', 'SetMaterialAttributes', 'MaterialFunctionCall'):
        key = ('FunctionInputs' if typ == 'MaterialFunctionCall' else 'Inputs') + '(' + str(i) + ')'
        edge = connection(props.get(key, ''))
        assert edge and edge[0] == src.get_name(), (node['name'], pin, key)
        return edge
    candidates = [(k, connection(v)) for k, v in props.items()]
    candidates = [(k, e) for k, e in candidates if e and e[0] == src.get_name()]
    aliases = {'UVs': 'Coordinates', 'Tex': 'TextureObject', 'DDX': 'CoordinatesDX', 'DDY': 'CoordinatesDY',
               'Apply View MipBias': 'AutomaticViewMipBiasValue', 'VectorInput': 'VectorInput'}
    norm = lambda s: re.sub(r'[^a-z0-9]', '', s.lower())
    exact = [e for k, e in candidates if norm(k) == norm(aliases.get(pin, pin))]
    if len(exact) == 1: return exact[0]
    unique = set(e for k, e in candidates)
    assert len(unique) == 1, ('ambiguous input', node['name'], pin, candidates)
    return next(iter(unique))

def output_name(node, index):
    if index == 0: return ''
    raw = node['props'].get('Outputs(' + str(index) + ')', '')
    match = re.search(r'OutputName="([^"]+)"', raw)
    if match: return match[1]
    if node['type'] in ('VertexColor', 'VectorParameter', 'TextureSample', 'TextureSampleParameter2D'):
        return ['', 'R', 'G', 'B', 'A', 'RGBA'][index]
    if node['type'] == 'ScreenPosition':
        return ['ViewportUV', 'PixelPosition'][index]
    masks = [c.upper() for c in 'rgba' if 'Mask' + c.upper() + '=1' in raw]
    if len(masks) == 1: return masks[0]
    raise RuntimeError(('unnamed output', node['name'], index, raw))

def make(cls, label):
    obj = MEL.create_material_expression(MAT, cls)
    obj.set_editor_property('desc', label)
    CREATED.append(obj)
    return obj

def clone_expression(n, label):
    # Fresh nodes avoid DuplicateObject retaining its protected Function owner link.
    obj = make(n['obj'].get_class(), label)
    keys = list(dict.fromkeys(k.split('(')[0] for k in n['props']))
    # Preserve explicit editable defaults too: setting textures can auto-select a sampler.
    if n['type'].startswith('TextureSample'):
        keys = [k for k in keys if k != 'SamplerType'] + ['SamplerType']
    if n['type'] == 'ComponentMask':
        keys = [k for k in keys if k not in ('R','G','B','A')] + ['R','G','B','A']
    ignored = []
    for key in keys:
        if key in ('Material', 'Function', 'Desc', 'Outputs', 'MaterialExpressionGuid', 'ExpressionGUID',
                   'MaterialExpressionEditorX', 'MaterialExpressionEditorY'): continue
        try:
            value = n['obj'].get_editor_property(key)
        except Exception:
            ignored.append(key); continue
        obj.set_editor_property(key, value)
    SKIPPED_PROPERTIES.setdefault(n['type'], set()).update(ignored)
    return obj

def link(edge, obj, pin):
    assert edge, (obj.get_name(), pin, 'missing edge')
    src, index = edge
    output = output_label(src, index)
    assert MEL.connect_material_expressions(src, output, obj, pin), (src.get_name(), output, obj.get_name(), pin)
    EDGES.append((obj, pin, src))

class Context:
    def __init__(self, owner, parent=None, call=None):
        self.g = graph(owner); self.parent = parent; self.call = call
        self.cache = {}; self.children = {}; self.inputs = {}; self.busy = set()
        self.label = owner.get_name() if parent is None else parent.label + '/' + call['name'] + ':' + owner.get_name()
        CONTEXTS.append(self)
        if call:
            for key, value in call['props'].items():
                if key.startswith('FunctionInputs('):
                    guid = re.search(r'ExpressionInputId=([A-F0-9]+)', value)[1]
                    self.inputs[guid] = connection(value)

    def resolve(self, name, index=0):
        key = (name, index)
        if key in self.cache: return self.cache[key]
        assert key not in self.busy, ('cycle', self.label, key)
        self.busy.add(key)
        n = self.g['nodes'][name]; typ = n['type']; p = n['props']
        if typ == 'MaterialFunctionCall':
            if name not in self.children:
                self.children[name] = Context(n['obj'].get_editor_property('material_function'), self, n)
            child = self.children[name]
            raw = p['FunctionOutputs(' + str(index) + ')']
            guid = re.search(r'ExpressionOutputId=([A-F0-9]+)', raw)[1]
            outputs = [v for v in child.g['nodes'].values() if v['type'] == 'FunctionOutput' and v['props'].get('Id') == guid]
            assert len(outputs) == 1, (self.label, name, index, guid)
            edge = connection(outputs[0]['props'].get('A', ''))
            assert edge, ('unconnected function output', child.label, outputs[0]['name'])
            result = child.resolve(*edge)
        elif typ == 'FunctionInput':
            result = self.input(n)
        elif typ == 'GetMaterialAttributes':
            edge = input_edge(n, 0)
            assert edge, (self.label, name)
            if index == 0:
                result = self.resolve(*edge)
            else:
                objkey = (name, 'object')
                if objkey not in self.cache:
                    obj = make(unreal.MaterialExpressionBreakMaterialAttributes, self.label + ' | ' + name)
                    self.cache[objkey] = obj
                    link(self.resolve(*edge), obj, '')
                label = re.search(r'OutputName="([^"]+)"', p['Outputs(' + str(index) + ')'])[1]
                result = self.cache[objkey], label.replace(' ', '')
        elif typ == 'SetMaterialAttributes':
            objkey = (name, 'object')
            if objkey not in self.cache:
                obj = make(unreal.MaterialExpressionMakeMaterialAttributes, self.label + ' | ' + name)
                self.cache[objkey] = obj
                base = input_edge(n, 0)
                prior = None
                if base:
                    prior = make(unreal.MaterialExpressionBreakMaterialAttributes, self.label + ' | incoming attributes ' + name)
                    link(self.resolve(*base), prior, '')
                overrides = {pin.replace(' ', ''): input_edge(n, i) for i, pin in enumerate(n['pins']) if i > 0}
                for pin in MEL.get_material_expression_input_names(obj):
                    canonical = pin.replace(' ', '').replace('CustomizedUVs_', 'CustomizedUV')
                    if overrides.get(canonical): link(self.resolve(*overrides[canonical]), obj, pin)
                    elif prior: link((prior, canonical), obj, pin)
            result = self.cache[objkey], 0
        elif typ in ('Reroute', 'NamedRerouteDeclaration', 'FunctionOutput'):
            edge = connection(p.get('A' if typ == 'FunctionOutput' else 'Input', ''))
            assert edge, (self.label, name, 'empty reroute')
            result = self.resolve(*edge)
        elif typ == 'NamedRerouteUsage':
            declaration = re.search(r':(MaterialExpression\w+)\x27', p['Declaration'])[1]
            result = self.resolve(declaration)
        else:
            # Clone once per call context, then reconnect every serialized input.
            objkey = (name, 'object')
            if objkey not in self.cache:
                obj = clone_expression(n, self.label + ' | ' + name)
                assert obj, (self.label, name)
                self.cache[objkey] = obj
                current_pins = MEL.get_material_expression_input_names(obj)
                assert len(current_pins) == len(n['pins']), (self.label, name, current_pins, n['pins'])
                for i, pin in enumerate(n['pins']):
                    edge = input_edge(n, i)
                    if edge: link(self.resolve(*edge), obj, current_pins[i])
            result = (self.cache[objkey], index)
        self.cache[key] = result; self.busy.remove(key)
        return result

    def input(self, n):
        obj = n['obj']; p = n['props']; source = self.inputs.get(p['Id'])
        typ = obj.get_editor_property('input_type').name
        if source:
            result = self.parent.resolve(*source)
        else:
            assert obj.get_editor_property('use_preview_value_as_default'), ('required function input', self.label, n['name'])
            preview = connection(p.get('Preview', ''))
            if preview:
                result = self.resolve(*preview)
            else:
                raw = p.get('PreviewValue', '')
                values = [float(m[1]) if (m := re.search(c + r'=([-+\deE.]+)', raw)) else 0.0 for c in 'XYZW']
                width = {'FUNCTION_INPUT_SCALAR': 1, 'FUNCTION_INPUT_VECTOR2': 2, 'FUNCTION_INPUT_VECTOR3': 3, 'FUNCTION_INPUT_VECTOR4': 4}.get(typ)
                assert width, ('unsupported default', self.label, n['name'], typ)
                cls = getattr(unreal, 'MaterialExpressionConstant' + (str(width) + 'Vector' if width > 1 else ''))
                constant = make(cls, self.label + ' | default ' + str(obj.get_editor_property('input_name')))
                if width == 1: constant.set_editor_property('r', values[0])
                elif width == 2:
                    constant.set_editor_property('r', values[0]); constant.set_editor_property('g', values[1])
                else: constant.set_editor_property('constant', unreal.LinearColor(*values))
                return constant, 0
        width = {'FUNCTION_INPUT_SCALAR': 1, 'FUNCTION_INPUT_VECTOR2': 2, 'FUNCTION_INPUT_VECTOR3': 3, 'FUNCTION_INPUT_VECTOR4': 4}.get(typ)
        if width:
            # Native masks retain LWC precision and the function input's truncation.
            cast = make(unreal.MaterialExpressionComponentMask, self.label + ' | input cast ' + str(obj.get_editor_property('input_name')))
            for i, channel in enumerate('rgba'): cast.set_editor_property(channel, i < width)
            link(result, cast, '')
            result = cast, 0
        return result

def main():
    initial = graph(MAT)
    if any(n['type'] == 'MaterialFunctionCall' for n in initial['nodes'].values()):
        leftovers = [n['obj'] for n in initial['nodes'].values() if str(n['obj'].get_editor_property('desc')).startswith((MAT.get_name() + '/', MAT.get_name() + ' |'))]
        for obj in leftovers: MEL.delete_material_expression(MAT, obj)
        if leftovers: GRAPHS.clear()
    root = Context(MAT)
    original = list(root.g['nodes'].values())
    assert any(n['type'] == 'MaterialFunctionCall' for n in original), 'Already flattened'
    roots = []
    for prop in unreal.MaterialProperty:
        node = MEL.get_material_property_input_node(MAT, prop)
        if node:
            label = MEL.get_material_property_input_node_output_name(MAT, prop)
            n = root.g['nodes'][node.get_name()]
            matching = [i for i in range(40) if ('Outputs(' + str(i) + ')') in n['props'] and ('OutputName="' + label + '"') in n['props']['Outputs(' + str(i) + ')']]
            index = 0 if not label else (matching[0] if matching else None)
            assert index is not None, (prop, label)
            roots.append((prop, root.resolve(node.get_name(), index)))
    # Retain top-level parameters, detached expressions and custom material outputs.
    for n in original:
        if n['type'] not in ('MaterialFunctionCall', 'Reroute', 'NamedRerouteDeclaration', 'NamedRerouteUsage'):
            root.resolve(n['name'])
    # Preserve exposed parameters even when a function output does not use them.
    for context in CONTEXTS:
        for n in context.g['nodes'].values():
            if 'Parameter' in n['type'] and n['type'] != 'CollectionParameter': context.resolve(n['name'])
    for prop, edge in roots:
        assert MEL.connect_material_property(edge[0], output_label(*edge), prop), prop
    for n in original: MEL.delete_material_expression(MAT, n['obj'])
    # No function-local object or old root node may remain in an input.
    created = set(CREATED)
    foreign = []
    for obj in CREATED:
        for src in MEL.get_inputs_for_material_expression(MAT, obj):
            if src and src not in created: foreign.append([obj.get_name(), src.get_path_name()])
    assert not foreign, foreign
    MEL.layout_material_expressions(MAT)
    MEL.recompile_material(MAT)
    s = MEL.get_statistics(MAT)
    report = {'material': PATH, 'before': len(original), 'after': len(CREATED),
              'expanded_calls': len(CONTEXTS) - 1, 'function_assets': [p for p in GRAPHS if p != MAT.get_path_name()],
              'node_types': dict(collections.Counter(o.get_class().get_name().replace('MaterialExpression', '') for o in CREATED)),
              'foreign_connections': foreign, 'skipped_properties': {k:sorted(v) for k,v in SKIPPED_PROPERTIES.items()}, 'stats': {k: s.get_editor_property(k) for k in ('num_vertex_shader_instructions', 'num_pixel_shader_instructions', 'num_samplers', 'num_vertex_texture_samples', 'num_pixel_texture_samples')}}
    assert report['node_types'].get('MaterialFunctionCall', 0) == 0
    (OUT / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    assert report['stats']['num_pixel_shader_instructions'] > 0
    unreal.log('CSSW_FLATTEN_COMPILED_NOT_SAVED ' + json.dumps(report))

if __name__ == '__main__':
    try:
        main()
    except Exception:
        (OUT / 'error.txt').write_text(traceback.format_exc(), encoding='utf-8')
        unreal.log_error(traceback.format_exc())
        raise
