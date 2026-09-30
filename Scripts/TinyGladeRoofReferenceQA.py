"""Compare the installed roof with the user-confirmed 2026-09-29 TG screenshot.

Run with -ExecCmds="py <this file>" -RenderOffscreen in L_HouseGroundDemo.
Default: rebuild the roof material and save the reference palette on this demo and BP.
-RoofReferenceReload: only read saved assets, verify, and capture. The enlarged comparison
roof is temporary: its dimensions and camera helpers are never saved to the demo map.
"""
import importlib.util
import json
import math
import traceback
from pathlib import Path
import unreal

PKG = '/PCGPlugins/HouseTest'
SCRIPT_DIR = Path(__file__).resolve().parent
OUT = Path(unreal.Paths.project_saved_dir()) / 'RoofReference_20260930'
OUT.mkdir(exist_ok=True)
RELOAD = '-RoofReferenceReload' in unreal.SystemLibrary.get_command_line()
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
STATE = {'ticks': 0, 'rigs': [], 'hidden': []}

def module(name):
    spec = importlib.util.spec_from_file_location(name, SCRIPT_DIR/(name+'.py'))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m

def capture(name, loc, target, fov=53.0):
    delta = target-loc
    rot = unreal.Rotator(pitch=math.degrees(math.atan2(delta.z, math.hypot(delta.x,delta.y))),
                         yaw=math.degrees(math.atan2(delta.y,delta.x)),roll=0)
    cap = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D,loc,rot)
    comp = cap.get_component_by_class(unreal.SceneCaptureComponent2D)
    rt = unreal.RenderingLibrary.create_render_target2d(STATE['world'],1400,1200,unreal.TextureRenderTargetFormat.RTF_RGBA8)
    for k,v in dict(texture_target=rt,capture_source=unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR,
                    capture_every_frame=False,capture_on_movement=False,always_persist_rendering_state=True,fov_angle=fov).items():
        comp.set_editor_property(k,v)
    pp=unreal.PostProcessSettings()
    pp.set_editor_property('override_dynamic_global_illumination_method',True)
    pp.set_editor_property('dynamic_global_illumination_method',unreal.DynamicGlobalIlluminationMethod.LUMEN)
    comp.set_editor_property('post_process_settings',pp)
    STATE['rigs'].append((name,cap,comp,rt))

def setup():
    world=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    assert world.get_path_name().startswith(PKG+'/L_HouseGroundDemo.'),world.get_path_name()
    STATE['world']=world
    bp=unreal.load_asset(PKG+'/BP_TinyGladeHouse')
    if not RELOAD:
        module('TinyGladeMakeRoofMaterial').build()
    mat=unreal.load_asset(PKG+'/MI_TinyGladeRoof_Grey')
    assert mat
    mesh=unreal.load_asset(PKG+'/SM_TinyGladeRoofTile')
    assert mesh
    if not RELOAD:
        mesh.set_material(0,mat)
        assert unreal.EditorAssetLibrary.save_loaded_asset(mesh,False)
    assert mesh.get_material(0)==mat
    houses={a.get_actor_label():a for a in ACTORS.get_all_level_actors() if isinstance(a,unreal.CSHouseActor)}
    STATE['houses']=houses
    STATE['house']=houses['House_Pillar']
    report={}
    for a in [unreal.get_default_object(bp.generated_class())]+list(houses.values()):
        if not RELOAD:
            a.modify()
            a.set_editor_property('roof_material',mat)
        assert a.get_editor_property('roof_material')==mat
    if not RELOAD:
        assert unreal.EditorAssetLibrary.save_loaded_asset(bp,False)
    for label,a in houses.items():
        a.rebuild_house()
        assert not a.get_roof_tile_undrawable_reason(),(label,a.get_roof_tile_undrawable_reason())
        tile_mesh=a.get_editor_property('roof_tile_mesh')
        tile=next(c for c in a.get_components_by_class(unreal.CSGpuInstancedMeshComponent) if c.get_editor_property('base_mesh')==tile_mesh)
        assert tile.is_nanite_render_path(),label
        size=a.get_editor_property('footprint_size')
        report[label]={'tiles':a.get_roof_tile_count(),'mesh':tile_mesh.get_path_name(),'material':mat.get_path_name(),'nanite':True,
                       'footprint':[size.x,size.y],'row_pitch':a.get_editor_property('roof_tile_row_pitch'),
                       'column_pitch':a.get_editor_property('roof_tile_column_pitch')}
    if not RELOAD:
        assert unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()
    (OUT/('reloaded.json' if RELOAD else 'installed.json')).write_text(json.dumps(report,indent=2),encoding='utf-8')
    capture('demo',unreal.Vector(1950,1100,1540),unreal.Vector(1700,1520,540))
    unreal.log('ROOF REFERENCE SETUP '+str(report))

def comparison():
    for name,cap,comp,rt in STATE['rigs']: ACTORS.destroy_actor(cap)
    STATE['rigs']=[]
    house=STATE['house']
    STATE['size']=house.get_editor_property('footprint_size')
    house.set_editor_property('footprint_size',unreal.Vector2D(1000,800))
    house.rebuild_house()
    for a in STATE['houses'].values():
        if a!=house:
            a.set_is_temporarily_hidden_in_editor(True)
            STATE['hidden'].append(a)
    origin=house.get_actor_location()
    yaw=math.radians(house.get_actor_rotation().yaw)
    def to_world(x,y,z):
        return origin+unreal.Vector(x*math.cos(yaw)-y*math.sin(yaw),x*math.sin(yaw)+y*math.cos(yaw),z)
    top=house.get_editor_property('wall_height')+house.get_editor_property('roof_height_offset')+house.get_roof_rise()
    capture('reference-top',to_world(0,-220,top+1450),to_world(0,0,top-130),46)
    capture('reference-oblique',to_world(950,-1200,top+850),to_world(0,0,top-130),50)

def export():
    for name,cap,comp,rt in STATE['rigs']:
        unreal.RenderingLibrary.export_render_target(STATE['world'],rt,str(OUT),name+('-reloaded' if RELOAD else '')+'.png')

def tick(dt):
    STATE['ticks']+=1
    t=STATE['ticks']
    try:
        if t==10: setup()
        if t>10 and t%2==0:
            for name,cap,comp,rt in STATE['rigs']: comp.capture_scene()
        if t==180:
            export()
            comparison()
        if t==330:
            export()
            STATE['house'].set_editor_property('footprint_size',STATE['size'])
            for a in STATE['hidden']: a.set_is_temporarily_hidden_in_editor(False)
            unreal.log('ROOF REFERENCE QA COMPLETE '+('RELOAD' if RELOAD else 'INSTALL'))
            unreal.unregister_slate_post_tick_callback(STATE['handle'])
            unreal.SystemLibrary.quit_editor()
    except Exception:
        unreal.log_error(traceback.format_exc())
        unreal.unregister_slate_post_tick_callback(STATE['handle'])
        unreal.SystemLibrary.quit_editor()

STATE['handle']=unreal.register_slate_post_tick_callback(tick)
