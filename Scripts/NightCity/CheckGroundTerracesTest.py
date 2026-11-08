"""Reload, collision-check and capture the generated test in a separate real editor.

Launch UnrealEditor.exe with L_GroundTerraces_28m, -RenderOffscreen and
-ExecCmds="py <this file>". Quits only its own editor after saving the report.
"""
import json
import collections
import math
import re
import traceback
from pathlib import Path
import unreal

ROOT=Path(unreal.Paths.project_dir()).resolve()
OUT=ROOT/'Saved/CodexGroundTerraces'
MANIFEST=json.loads((OUT/'manifest.json').read_text(encoding='utf-8'))
ACTORS=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
WORLD=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
assert WORLD.get_path_name().split('.')[0]==MANIFEST['level']
VIEWS=[
    ('overview',(17500,-18500,16000),(0,1800,1800),65),
    ('park_stairs',(-7800,-4400,2300),(-4200,1800,1700),75),
    ('high_wall',(10800,1000,1400),(6500,6600,2200),75),
    ('sunken_court',(7600,-6900,2100),(2900,-2700,-200),70),
    ('building_corners',(-9000,3000,5200),(-4500,6800,4000),65),
]
REPORT={'level':MANIFEST['level'],'checks':[],'screenshots':[]}


def check(name,passed,detail):
    REPORT['checks'].append(dict(name=name,passed=bool(passed),detail=detail))


def validate():
    expected=sorted((r['actor'],r['category'],r['mesh'].split('.')[0],r['instances']) for r in MANIFEST['groups'])
    actual=[]
    stairs=[]
    for a in ACTORS.get_all_level_actors():
        if not a.get_actor_label().startswith('GT_'):
            continue
        for c in a.get_components_by_class(unreal.InstancedStaticMeshComponent):
            label=a.get_actor_label()[3:]
            category=label.rsplit('_',2)[0]
            category=next((str(t).split('=',1)[1] for t in c.component_tags if str(t).startswith('GTCategory=')),category)
            path=c.static_mesh.get_path_name().split('.')[0]
            actual.append((a.get_actor_label(),category,path,c.get_instance_count()))
            if category.startswith('Stairs_'):
                for i in range(c.get_instance_count()):
                    s=c.get_instance_transform(i,True).scale3d
                    stairs.append(abs(s.x-1)+abs(s.y-1)+abs(s.z-1)<.0001)
    check('persisted_instance_groups',sorted(actual)==expected,{'groups':len(actual),'instances':sum(r[3] for r in actual)})
    check('stairs_unit_scale',all(stairs) and bool(stairs),{'instances':len(stairs)})
    check('height_quantization',all(abs(z/20-round(z/20))<.0001 for z in MANIFEST['ground_levels_cm']),MANIFEST['ground_levels_cm'])
    probes=[('LowPlaza',-1800,-1800,0),('Park',-2400,1800,1200),('Upper',0,4800,2400),('RaisedGarden',0,3000,1600),('SunkenCourt',3600,-3000,-400),('EastWalk',8100,6000,0),('FirstTread',-5700,-2985,20),('TopTread',-5700,-15,1200),('StairLanding',-5700,-2100,400)]
    for name,x,y,z in probes:
        hit=unreal.SystemLibrary.line_trace_single(WORLD,unreal.Vector(x,y,z+100),unreal.Vector(x,y,z-150),unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,False,[],unreal.DrawDebugTrace.NONE)
        text=hit.export_text() if hit else ''
        match=re.search(r'ImpactPoint=\(X=([^,]+),Y=([^,]+),Z=([^\)]+)\)',text)
        actual_z=float(match.group(3)) if match else None
        check('collision_'+name,actual_z is not None and abs(actual_z-z)<2,{'expected_z':z,'actual_z':actual_z,'hit':text if actual_z is None else ''})
    # Check the complete 24 m wall at three elevations from the lower promenade.
    for z in (200,1200,2200):
        hit=unreal.SystemLibrary.line_trace_single(WORLD,unreal.Vector(8000,6300,z),unreal.Vector(7000,6300,z),unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,False,[],unreal.DrawDebugTrace.NONE)
        check('retaining_wall_collision_'+str(z),hit is not None,str(hit))
    if MANIFEST.get('building_actors'):
        validate_buildings_and_paths()
    REPORT['passed']=all(r['passed'] for r in REPORT['checks'])


def validate_buildings_and_paths():
    actors=ACTORS.get_all_level_actors()
    buildings=[a for a in actors if a.get_actor_label() in MANIFEST['building_actors']]
    check('four_independent_building_actors',len(buildings)==4,[a.get_actor_label() for a in buildings])
    for a in buildings:
        components=a.get_components_by_class(unreal.InstancedStaticMeshComponent)
        categories={str(t).split('=',1)[1] for c in components for t in c.component_tags if str(t).startswith('GTCategory=')}
        required={'Building_Facades','Building_Corners','Building_Doors','Building_Roofs','Building_Floors'}
        check('single_owner_'+a.get_actor_label(),required<=categories and all(c.get_owner()==a and c.get_attach_parent()==a.root_component for c in components),sorted(categories))
        previous=a.get_actor_location()
        before=[c.get_instance_transform(i,True).translation for c in components for i in range(c.get_instance_count())]
        delta=unreal.Vector(137,211,89)
        try:
            a.set_actor_location(previous+delta,False,True)
            after=[c.get_instance_transform(i,True).translation for c in components for i in range(c.get_instance_count())]
            error=max((q-p-delta).length() for p,q in zip(before,after))
            check('move_together_'+a.get_actor_label(),error<.01,{'max_translation_error_cm':error})
        finally:
            a.set_actor_location(previous,False,True)
    cells=set()
    for a in actors:
        for c in a.get_components_by_class(unreal.InstancedStaticMeshComponent):
            if 'GTCategory=Paths_Paving' not in [str(t) for t in c.component_tags]:
                continue
            for i in range(c.get_instance_count()):
                p=c.get_instance_transform(i,True).translation
                cells.add((round((p.x-50)/100),round((p.y-50)/100),round(p.z-.25)))
    def near(x,y,z):
        candidates=[c for c in cells if c[2]==z and abs(c[0]*100+50-x)<151 and abs(c[1]*100+50-y)<151]
        assert candidates, 'No path near '+str((x,y,z))
        return min(candidates,key=lambda c:(c[0]*100+50-x)**2+(c[1]*100+50-y)**2)
    edges=collections.defaultdict(list)
    for connection in MANIFEST['connections']:
        x=sum(connection['x'])/2
        a=near(x,connection['start_y']-50,connection['low'])
        b=near(x,connection['end_y']+50,connection['high'])
        edges[a].append(b); edges[b].append(a)
    start=near(-2400,-3400,0)
    visited={start}; todo=collections.deque([start])
    while todo:
        x,y,z=todo.popleft()
        for other in [(x-1,y,z),(x+1,y,z),(x,y-1,z),(x,y+1,z)]+edges[(x,y,z)]:
            if other in cells and other not in visited:
                visited.add(other); todo.append(other)
    goals=[(-5400,-5750,0),(-4500,5950,2600),(-300,5950,2600),(3900,5950,2600),(0,3500,1600),(3600,-4000,-400)]
    check('paths_connect_all_regions_and_buildings',len(visited)==len(cells) and all(near(*g) in visited for g in goals),{'reachable_cells':len(visited),'total_cells':len(cells),'stair_links':len(MANIFEST['connections']),'destinations':goals})
    obstruction=[]
    ground_failures=[]
    for s in MANIFEST['path_segments']:
        a,b=s['a'],s['b']; z=a[2]
        length=math.hypot(b[0]-a[0],b[1]-a[1])
        count=max(1,math.ceil(length/200))
        points=[(a[0]+(b[0]-a[0])*i/count,a[1]+(b[1]-a[1])*i/count) for i in range(count+1)]
        for x,y in points:
            hit=unreal.SystemLibrary.line_trace_single(WORLD,unreal.Vector(x,y,z+80),unreal.Vector(x,y,z-20),unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,False,[],unreal.DrawDebugTrace.NONE)
            match=re.search(r'ImpactPoint=\(X=([^,]+),Y=([^,]+),Z=([^\)]+)\)',hit.export_text() if hit else '')
            if not match or abs(float(match.group(3))-(z+1))>1.1:
                ground_failures.append([s['name'],x,y,hit.export_text() if hit else 'no ground'])
        if length:
            hit=unreal.SystemLibrary.line_trace_single(WORLD,unreal.Vector(a[0],a[1],z+100),unreal.Vector(b[0],b[1],z+100),unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,False,[],unreal.DrawDebugTrace.NONE)
            if hit:
                obstruction.append([s['name'],hit.export_text()])
    check('path_surface_samples',not ground_failures,ground_failures)
    check('path_centrelines_clear',not obstruction,obstruction)


rt=unreal.RenderingLibrary.create_render_target2d(WORLD,1600,1000,unreal.TextureRenderTargetFormat.RTF_RGBA8,unreal.LinearColor(0,0,0,1),False)
capture=ACTORS.spawn_actor_from_class(unreal.SceneCapture2D,unreal.Vector(0,0,0))
comp=capture.get_component_by_class(unreal.SceneCaptureComponent2D)
comp.set_editor_property('texture_target',rt)
comp.set_editor_property('capture_source',unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
comp.set_editor_property('capture_every_frame',False)
comp.set_editor_property('capture_on_movement',False)
comp.set_editor_property('always_persist_rendering_state',True)
STATE={'ticks':0,'view':-1,'frames':0,'handle':None,'busy':False}


def finish():
    unreal.unregister_slate_post_tick_callback(STATE['handle'])
    (OUT/'validation.json').write_text(json.dumps(REPORT,ensure_ascii=False,indent=2),encoding='utf-8')
    unreal.log('GROUND_TERRACES_CHECK_DONE '+str(REPORT.get('passed')))
    ACTORS.destroy_actor(capture)
    unreal.SystemLibrary.quit_editor()


def tick(delta):
    if STATE['busy']:
        return
    STATE['busy']=True
    try:
        STATE['ticks']+=1
        if STATE['ticks']<90:
            return
        if STATE['view']<0:
            try:
                validate()
            except Exception:
                REPORT['validation_error']=traceback.format_exc()
                REPORT['passed']=False
            STATE['view']=0
        if STATE['frames']==0:
            name,loc,target,fov=VIEWS[STATE['view']]
            capture.set_actor_location_and_rotation(unreal.Vector(*loc),unreal.MathLibrary.find_look_at_rotation(unreal.Vector(*loc),unreal.Vector(*target)),False,True)
            comp.set_editor_property('fov_angle',fov)
        comp.capture_scene()
        STATE['frames']+=1
        if STATE['frames']<48:
            return
        name=VIEWS[STATE['view']][0]
        unreal.RenderingLibrary.export_render_target(WORLD,rt,str(OUT),name+'.png')
        REPORT['screenshots'].append(str(OUT/(name+'.png')))
        unreal.log('GROUND_TERRACES_SHOT '+name)
        STATE['view']+=1
        STATE['frames']=0
        if STATE['view']==len(VIEWS):
            finish()
    except Exception:
        REPORT['capture_error']=traceback.format_exc()
        REPORT['passed']=False
        finish()
    finally:
        STATE['busy']=False


STATE['handle']=unreal.register_slate_post_tick_callback(tick)
