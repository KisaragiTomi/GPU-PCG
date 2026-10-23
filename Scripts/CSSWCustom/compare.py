"""Same-frame A/B/A captures using baked CSSW data. No project level is saved.

Both variants receive identical parameters. Each capture set includes an A/A
control to reveal capture noise. Test cases explicitly distinguish saved defaults
from a valid UV mapping override for the existing CSSW_SimCenter.B contract.
"""
import unreal,json,math,traceback,builtins
from pathlib import Path
OUT=Path(unreal.Paths.project_saved_dir())/'CSSWCustomMaterial'
DEST='/PCGPlugins/ShallowWater/Material30/Custom'
STATE={'ticks':0,'rigs':[],'pairs':[],'results':[]}
MEL=unreal.MaterialEditingLibrary

def setup():
    unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
    world=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    actors=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    STATE.update(world=world,actors=actors)
    for cmd in ('r.AntiAliasingMethod 0','r.MotionBlurQuality 0','r.SSR.Quality 0','r.Shadow.Virtual.Enable 0','r.ScreenPercentage 100'):
        unreal.SystemLibrary.execute_console_command(world,cmd)
    light=actors.spawn_actor_from_class(unreal.DirectionalLight,unreal.Vector(0,0,10000),unreal.Rotator(pitch=-42,yaw=-35,roll=0))
    light.light_component.set_editor_property('intensity',40.0)
    light.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
    light.light_component.set_editor_property('light_color',unreal.Color(255,245,225,255))
    sky=actors.spawn_actor_from_class(unreal.SkyLight,unreal.Vector(0,0,9000))
    sky.light_component.set_editor_property('intensity',1.0)
    sky.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
    actors.spawn_actor_from_class(unreal.SkyAtmosphere,unreal.Vector(0,0,0))
    # A neutral floor provides depth, contact edges and a refraction background.
    floor=actors.spawn_actor_from_class(unreal.StaticMeshActor,unreal.Vector(1605,1785,-520))
    floor.static_mesh_component.set_static_mesh(unreal.load_asset('/Engine/BasicShapes/Cube'))
    floor.set_actor_scale3d(unreal.Vector(700,700,2))
    water=actors.spawn_actor_from_class(unreal.StaticMeshActor,unreal.Vector(0,0,0))
    water.set_actor_label('CSSW A-B comparison (unsaved)')
    water.static_mesh_component.set_static_mesh(unreal.load_asset('/PCGPlugins/ShallowWater/VelocityHeight/CSSWData/SM_CSSW_Water_3333333'))
    STATE['component']=water.static_mesh_component
    original=unreal.load_asset('/PCGPlugins/ShallowWater/VelocityHeight/CSSWData/MI_CSSW_Water_3333333')
    custom=unreal.load_asset(DEST+'/MI_CSSW_Water_3333333_Custom')
    STATE['saved_pair']=(original,custom)
    pairs=[]
    for material in (original,custom):
        mid=unreal.MaterialLibrary.create_dynamic_material_instance(world,material)
        mid.set_vector_parameter_value('CSSW_SimCenter',unreal.LinearColor(1605,1785,53000,0))
        pairs.append(mid)
    STATE['valid_pair']=pairs
    # Baked mesh is in CSSW local space. Use bounds for full and near views.
    center=unreal.Vector(-275,4058,4800)
    views=[('overview',unreal.Vector(18500,-23500,23500),center,48),
           ('surface',unreal.Vector(5200,-8500,7800),unreal.Vector(0,3500,2200),52),
           ('grazing',unreal.Vector(15000,2500,5200),unreal.Vector(-1000,4200,4000),52)]
    for name,loc,target,fov in views:
        delta=target-loc
        rot=unreal.Rotator(pitch=math.degrees(math.atan2(delta.z,math.hypot(delta.x,delta.y))),yaw=math.degrees(math.atan2(delta.y,delta.x)),roll=0)
        a=actors.spawn_actor_from_class(unreal.SceneCapture2D,loc,rot)
        c=a.get_component_by_class(unreal.SceneCaptureComponent2D)
        rt=unreal.RenderingLibrary.create_render_target2d(world,1280,800,unreal.TextureRenderTargetFormat.RTF_RGBA8)
        for key,value in {'texture_target':rt,'capture_source':unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR,'capture_every_frame':False,'capture_on_movement':False,'fov_angle':fov}.items():c.set_editor_property(key,value)
        pp=unreal.PostProcessSettings()
        pp.set_editor_property('override_auto_exposure_bias',True)
        pp.set_editor_property('auto_exposure_bias',4.0)
        for key in ('min','max'):
            pp.set_editor_property('override_auto_exposure_'+key+'_brightness',True)
            pp.set_editor_property('auto_exposure_'+key+'_brightness',1.0)
        pp.set_editor_property('override_dynamic_global_illumination_method',True)
        pp.set_editor_property('dynamic_global_illumination_method',unreal.DynamicGlobalIlluminationMethod.NONE)
        pp.set_editor_property('override_reflection_method',True)
        pp.set_editor_property('reflection_method',unreal.ReflectionMethod.NONE)
        c.set_editor_property('post_process_settings',pp)
        STATE['rigs'].append((name,c,rt))
    STATE['component'].set_material(0,pairs[0])

def capture(case,pair):
    # Time cannot advance while this callback runs: A/B/A use the same frame time.
    world=STATE['world'];comp=STATE['component']
    timestamp=unreal.SystemLibrary.get_game_time_in_seconds(world)
    for name,c,rt in STATE['rigs']:
        for tag,material in [('original',pair[0]),('custom',pair[1]),('control',pair[0])]:
            comp.set_material(0,material)
            c.capture_scene()
            unreal.RenderingLibrary.export_render_target(world,rt,str(OUT),case+'-'+name+'-'+tag+'.png')
    STATE['results'].append({'case':case,'world_time':timestamp,'uv_override':case!='saved','size':[1280,800]})
    (OUT/'capture-cases.json').write_text(json.dumps(STATE['results'],indent=2),encoding='utf-8')

def tick(dt):
    STATE['ticks']+=1
    if STATE['ticks'] not in (200,500,800):return
    try:
        t=STATE['ticks']
        capture('saved' if t==200 else 'valid-'+str(t),STATE['saved_pair'] if t==200 else STATE['valid_pair'])
        if t==800:
            (OUT/'capture-cases.json').write_text(json.dumps(STATE['results'],indent=2),encoding='utf-8')
            unreal.unregister_slate_post_tick_callback(STATE['handle']);unreal.log('CSSW_COMPARISON_DONE')
    except Exception:
        (OUT/'compare-error.txt').write_text(traceback.format_exc(),encoding='utf-8');unreal.log_error(traceback.format_exc());unreal.unregister_slate_post_tick_callback(STATE['handle'])

try:
    setup();STATE['handle']=unreal.register_slate_post_tick_callback(tick)
    builtins.cssw_capture=capture;builtins.cssw_state=STATE
except Exception:
    (OUT/'compare-error.txt').write_text(traceback.format_exc(),encoding='utf-8');unreal.log_error(traceback.format_exc())
