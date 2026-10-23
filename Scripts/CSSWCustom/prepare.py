"""Export original dependencies and regenerate the plan (does not modify assets)."""
import json, runpy, unreal
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = Path(unreal.Paths.project_saved_dir()) / 'CSSWCustomMaterial'
OUT.mkdir(exist_ok=True)
def export(path):
    source = unreal.load_asset(path)
    assert source, path
    task = unreal.AssetExportTask()
    task.object = source
    task.filename = str(OUT / (source.get_name() + '.t3d'))
    task.automated = True
    task.prompt = False
    task.replace_identical = True
    task.exporter = unreal.ObjectExporterT3D()
    assert unreal.Exporter.run_asset_export_task(task)
    return source.get_name()

pending = [export('/PCGPlugins/ShallowWater/Material30/Materials/M_FluxSurfaceOver')]
seen = set(pending)
while pending:
    runpy.run_path(str(HERE / 'parse_exports.py'), run_name='__main__')
    graphs = json.loads((OUT / 'graphs.json').read_text(encoding='utf-8'))
    next_batch = []
    for name in pending:
        for node in graphs[name]['nodes'].values():
            if node['type'] != 'MaterialFunctionCall':
                continue
            path = node['props']['MaterialFunction'].split("'")[1]
            dep = path.split('.')[-1]
            if dep not in seen:
                seen.add(dep)
                next_batch.append(export(path))
    pending = next_batch
runpy.run_path(str(HERE / 'gather.py'), run_name='__main__')
runpy.run_path(str(HERE / 'plan.py'), run_name='__main__')
