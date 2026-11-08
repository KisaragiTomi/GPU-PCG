"""Update only buildings and paths in the open GroundTerraces test, preserving other actors.

Run in the existing editor with this level open and no unsaved edits. Saves this map.
The saved map and manifest are backed up before the first write for this revision.
"""
import importlib
import json
from pathlib import Path
import shutil
import sys
import traceback
import unreal

sys.path.insert(0,str(Path(__file__).resolve().parent))
import MakeGroundTerracesTest as build
build=importlib.reload(build)
REVISION='2026-11-06-paths-corners'
OUT=build.OUT/REVISION


def snapshot(actors):
    out={}
    for a in actors:
        components=[]
        for c in a.get_components_by_class(unreal.InstancedStaticMeshComponent):
            components.append((c.get_name(),c.static_mesh.get_path_name() if c.static_mesh else '',[c.get_instance_transform(i,True).export_text() for i in range(c.get_instance_count())]))
        out[a.get_path_name()]=[a.get_actor_transform().export_text(),components]
    return out


def main():
    world=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    assert world.get_path_name().split('.')[0]==build.LEVEL
    dirty=[p.get_name() for p in unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages()]
    assert not dirty, 'Save pending map edits before this targeted update: '+str(dirty)
    OUT.mkdir(parents=True,exist_ok=True)
    map_file=build.ROOT/'Content/BuildingGenerator/Cyberpunk2077/Tests/GroundTerraces/L_GroundTerraces_28m.umap'
    backup=OUT/map_file.name
    if not backup.exists():
        shutil.copy2(map_file,backup)
        shutil.copy2(build.OUT/'manifest.json',OUT/'manifest_before.json')
    manifest=json.loads((build.OUT/'manifest.json').read_text(encoding='utf-8'))
    all_actors=build.ACTORS.get_all_level_actors()
    old=[a for a in all_actors if a.get_actor_label().startswith(('GT_Building_','GT_Paths_'))]
    preserve=[a for a in all_actors if a not in old]
    before=snapshot(preserve)
    build.TRIM=build.FOLDER+'/Materials/M_ConcreteTrim'
    # Prepare new independent building owners without changing the rest of the scene.
    for x,floors in ((-6000,5),(-1800,7),(2400,4)):
        build.building(x+300,6000,2400,1800,2600,floors)
    build.building(-6600,-5700,2400,1800,0,2)
    build.paths()
    with unreal.ScopedEditorTransaction('Ground Terraces: paths, corner walls and unified building actors'):
        # All new geometry is created and checked before deleting the prior building containers.
        rows=build.emit()
        assert len(set(r['actor'] for r in rows if r['category'].startswith('Building_')))==4
        after=snapshot(preserve)
        assert before==after,'Unrelated actors changed'
        for actor in old:
            assert build.ACTORS.destroy_actor(actor)
        assert build.LEVELS.save_current_level()
    retained=[r for r in manifest['groups'] if not r['category'].startswith(('Building_','Paths_'))]
    manifest['groups']=retained+rows
    manifest.update(revision=REVISION,building_actors=list(build.BUILDING_ORIGINS),path_segments=build.PATH_SEGMENTS,instance_count=sum(r['instances'] for r in retained+rows),ism_components=len(retained+rows))
    (build.OUT/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
    result=dict(revision=REVISION,level=build.LEVEL,preserved_actor_count=len(preserve),preserved_transforms_identical=True,removed_actors=len(old),building_actors=manifest['building_actors'],path_segments=len(build.PATH_SEGMENTS),instance_count=manifest['instance_count'],ism_components=manifest['ism_components'],backup=str(backup))
    (OUT/'update_result.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    unreal.log('GROUND_TERRACES_UPDATE_OK '+json.dumps(result))


if __name__=='__main__':
    try:
        main()
    except Exception:
        OUT.mkdir(parents=True,exist_ok=True)
        (OUT/'update_error.txt').write_text(traceback.format_exc(),encoding='utf-8')
        raise
