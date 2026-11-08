"""Read source geometry for the frozen original-scene example; save no Unreal assets."""
import json,sys,traceback
from pathlib import Path
import unreal
ROOT=Path(unreal.Paths.project_dir()).resolve()
OUT=ROOT/'Saved/CodexNightCityAssembled'
OUT.mkdir(parents=True,exist_ok=True)
sys.path.insert(0,str(ROOT/'Plugins/PCGPlugins/Scripts/NightCity'))
from ExportPlacementSources import read_source
context=json.loads((ROOT/'Plugins/PCGPlugins/Docs/NightCity/AssembledExample01/context.json').read_text(encoding='utf-8'))
result={'geometry':{},'errors':{}}
for key,m in context['meshes'].items():
    try:
        mesh=unreal.load_asset(m['path'])
        if mesh is None: raise RuntimeError('Source mesh unavailable')
        result['geometry'][key]=read_source(mesh)
    except Exception: result['errors'][key]=traceback.format_exc()
(OUT/'geometry.json').write_text(json.dumps(result,ensure_ascii=False,separators=(',',':')),encoding='utf-8')
unreal.log('ASSEMBLED EXPORT '+str(len(result['geometry']))+' meshes; '+str(len(result['errors']))+' failures')
if result['errors']: raise RuntimeError('Some assembled-example source geometry could not be read')
