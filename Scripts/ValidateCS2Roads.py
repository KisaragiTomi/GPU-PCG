"""Reopen the saved Palmsland map, audit references, and capture four real UE views.

Run with -ExecCmds="py <this file>" in an editor with RHI enabled. Outputs are
written to Saved/CS2Roads. Capture actors are temporary and never saved.
"""
import json, os, pathlib, traceback
import unreal

OUT=pathlib.Path(unreal.Paths.project_saved_dir())/'CS2Roads'
DEST=os.environ.get('CS2ROAD_DEST','/PCGPlugins/CS2Roads')
PROBE=os.environ.get('CS2ROAD_PROBE','0')=='1'
ACTORS=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
LEVEL=unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
STATE={'frame':0,'job':0,'warmup':0,'handle':None,'capture':None}

def finish(error=None):
    if STATE['handle']:unreal.unregister_slate_post_tick_callback(STATE['handle'])
    if STATE['capture']:ACTORS.destroy_actor(STATE['capture'])
    if error:
        (OUT/'validation-error.txt').write_text(error,encoding='utf-8')
        unreal.log_error('CS2_VALIDATE_FAILED '+error)
    else:
        (OUT/('probe-validation.json' if PROBE else 'validation.json')).write_text(json.dumps(STATE['report'],indent=2),encoding='utf-8')
        unreal.log('CS2_VALIDATE_COMPLETE')
    unreal.SystemLibrary.quit_editor()

def build():
    result=json.loads((OUT/('probe-import.json' if PROBE else 'import-result.json')).read_text(encoding='utf-8'))
    if not LEVEL.load_level(result['level']):raise RuntimeError('Cannot reopen saved map')
    world=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    # Explicit migration for maps created by the initial low-lux import draft.
    if os.environ.get('CS2ROAD_FIX_LIGHTING')=='1':
        for actor in ACTORS.get_all_level_actors():
            if isinstance(actor,unreal.DirectionalLight):
                actor.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
                actor.light_component.set_intensity(75000.)
            elif isinstance(actor,unreal.SkyLight):actor.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
            elif isinstance(actor,unreal.PostProcessVolume):
                pp=actor.get_editor_property('settings')
                for bound in ['min','max']:
                    pp.set_editor_property('override_auto_exposure_'+bound+'_brightness',True)
                    pp.set_editor_property('auto_exposure_'+bound+'_brightness',13.)
                actor.set_editor_property('settings',pp)
        if not LEVEL.save_current_level():raise RuntimeError('Cannot save corrected lighting')
    found={a.get_actor_label():a for a in ACTORS.get_all_level_actors() if 'CS2RoadLab' in [str(t) for t in a.tags]}
    if len(found)!=len(result['meshes']):raise RuntimeError('Saved level mesh actor count mismatch')
    triangles=0
    for item in result['meshes']:
        actor=found[item['name']];mesh=actor.static_mesh_component.static_mesh
        if not mesh or mesh.get_path_name().split('.')[0]!=item['asset']:raise RuntimeError('Wrong mesh reference '+item['name'])
        if mesh.get_num_triangles(0)!=item['triangles']:raise RuntimeError('Triangle count changed on reload')
        pos=actor.get_actor_location();actual=[pos.x,pos.y,pos.z]
        if max(abs(a-b) for a,b in zip(actual,item['origin_cm']))>.01:raise RuntimeError('Actor position changed')
        for i in range(mesh.get_num_sections(0)):
            if mesh.get_material(i) is None:raise RuntimeError('Missing material '+item['name'])
        triangles+=item['triangles']
    STATE['report']={'map':result['level'],'mesh_actors':len(found),'triangles':triangles,'materials':result['materials'],'textures':result['textures'],'screenshots':[]}
    STATE['world']=world
    # Centimetres in UE: the same downtown corner and interchange inspected in Godot.
    focus=(250544.8,-62526.2,56085.7)
    STATE['jobs']=[
        ('palmsland-overview',(0,-1050000,2050000),(0,0,55000)),
        ('palmsland-interchange',(278195,-103480,101030),(248195,-58480,56030)),
        ('palmsland-highway-close',(focus[0]+6500,focus[1]-8500,focus[2]+12500),focus),
        ('palmsland-junction',(48250.394,-225073.02,66894.58),(41750.394,-216573.02,54394.58)),
    ]
    if PROBE:STATE['jobs']=[('probe-junction',(3500,-4500,6000),(0,0,0))]
    rt=unreal.RenderingLibrary.create_render_target2d(world,1600,1000,unreal.TextureRenderTargetFormat.RTF_RGBA8,unreal.LinearColor(0,0,0,1),False)
    cap=ACTORS.spawn_actor_from_class(unreal.SceneCapture2D,unreal.Vector())
    STATE['capture']=cap;STATE['rt']=rt
    comp=cap.get_component_by_class(unreal.SceneCaptureComponent2D);STATE['comp']=comp
    for key,value in dict(texture_target=rt,capture_source=unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR,fov_angle=65.,capture_every_frame=False,capture_on_movement=False,always_persist_rendering_state=True).items():
        comp.set_editor_property(key,value)
    pp=unreal.PostProcessSettings()
    pp.set_editor_property('override_dynamic_global_illumination_method',True)
    pp.set_editor_property('dynamic_global_illumination_method',unreal.DynamicGlobalIlluminationMethod.LUMEN)
    pp.set_editor_property('override_reflection_method',True)
    pp.set_editor_property('reflection_method',unreal.ReflectionMethod.LUMEN)
    comp.set_editor_property('post_process_settings',pp);comp.set_editor_property('post_process_blend_weight',1.)
    unreal.SystemLibrary.execute_console_command(world,'r.Streaming.PoolSize 3000')
    unreal.SystemLibrary.execute_console_command(world,'t.MaxFPS 30')
    unreal.log('CS2_VALIDATE references OK, capturing views')

def tick(delta):
    try:
        STATE['frame']+=1
        if STATE['frame']<90:return
        if STATE['job']>=len(STATE['jobs']):finish();return
        name,eye,target=STATE['jobs'][STATE['job']]
        if STATE['warmup']==0:
            loc=unreal.Vector(*eye);rot=unreal.MathLibrary.find_look_at_rotation(loc,unreal.Vector(*target))
            STATE['capture'].set_actor_location_and_rotation(loc,rot,False,False)
            unreal.EditorLevelLibrary.set_level_viewport_camera_info(loc,rot)
        STATE['comp'].capture_scene();STATE['warmup']+=1
        if STATE['warmup']<128:return
        unreal.RenderingLibrary.export_render_target(STATE['world'],STATE['rt'],str(OUT),name+'.png')
        STATE['report']['screenshots'].append(name+'.png')
        unreal.log('CS2_VALIDATE captured '+name)
        STATE['job']+=1;STATE['warmup']=0
    except Exception:finish(traceback.format_exc())

try:
    build();STATE['handle']=unreal.register_slate_post_tick_callback(tick)
except Exception:finish(traceback.format_exc())
