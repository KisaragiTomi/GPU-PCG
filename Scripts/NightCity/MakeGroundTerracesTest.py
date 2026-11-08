"""Build an instanced 28 m height-difference test, without editing source assets.

Run in a separate Unreal Python commandlet. Rebuilds only this script's tagged map.
Source stair: 10 steps at 20 cm, 300 cm run, 200 cm total rise; unit scale.
"""
import collections
import json
import math
from pathlib import Path
import random
import traceback
import unreal

ROOT = Path(unreal.Paths.project_dir()).resolve()
OUT = ROOT / 'Saved/CodexGroundTerraces'
FOLDER = '/Game/BuildingGenerator/Cyberpunk2077/Tests/GroundTerraces'
LEVEL = FOLDER + '/L_GroundTerraces_28m'
ASSETS = unreal.EditorAssetLibrary
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()
MESHES = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
STEP = 20.0
RISE = STEP * 10
RUN = 300.0
GROUPS = collections.defaultdict(list)
MESH_CACHE = {}
CONNECTIONS = []
BUILDING_OWNER = ''
BUILDING_ORIGINS = {}
PATH_SEGMENTS = []


def source(key):
    return '/Game/BuildingGenerator/Cyberpunk2077/Meshes/' + key[3:5] + '/' + key


def save(asset):
    assert ASSETS.save_loaded_asset(asset, False), asset.get_path_name()


def material(name, color, roughness=.8):
    path = FOLDER + '/Materials/M_' + name
    mat = unreal.load_asset(path) if ASSETS.does_asset_exist(path) else TOOLS.create_asset('M_' + name, FOLDER + '/Materials', unreal.Material, unreal.MaterialFactoryNew())
    if ASSETS.get_metadata_tag(mat, 'GroundTerraces') == '1':
        return path
    edit = unreal.MaterialEditingLibrary
    c = edit.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector)
    c.set_editor_property('constant', unreal.LinearColor(*color, 1))
    edit.connect_material_property(c, '', unreal.MaterialProperty.MP_BASE_COLOR)
    r = edit.create_material_expression(mat, unreal.MaterialExpressionConstant)
    r.set_editor_property('r', roughness)
    edit.connect_material_property(r, '', unreal.MaterialProperty.MP_ROUGHNESS)
    mat.set_editor_property('used_with_instanced_static_meshes', True)
    ASSETS.set_metadata_tag(mat, 'GroundTerraces', '1')
    edit.recompile_material(mat)
    save(mat)
    return path


def collision_copy(key, name, complex_collision=False):
    path = FOLDER + '/Meshes/' + name
    m = unreal.load_asset(path) if ASSETS.does_asset_exist(path) else ASSETS.duplicate_asset(source(key), path)
    assert m, path
    body = m.get_editor_property('body_setup')
    assert body, path
    if complex_collision or MESHES is None:
        body.set_editor_property('collision_trace_flag', unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE)
    if MESHES is not None and complex_collision:
        for section in range(m.get_num_sections(0)):
            MESHES.enable_section_collision(m, True, 0, section)
    elif MESHES is not None and not MESHES.get_simple_collision_count(m):
        MESHES.add_simple_collisions(m, unreal.ScriptingCollisionShapeType.BOX)
    ASSETS.set_metadata_tag(m, 'GroundTerraces', '1')
    ASSETS.set_metadata_tag(m, 'SourceMesh', source(key))
    save(m)
    return path


def add(category, mesh, location, scale=(1, 1, 1), yaw=0, mat='', rotation=None):
    tile = (math.floor(location[0] / 2400), math.floor(location[1] / 2400)) if category.startswith('Ground') else (0, 0)
    GROUPS[(category, mesh, mat, tile, BUILDING_OWNER)].append(unreal.Transform(location=unreal.Vector(*location), rotation=rotation or unreal.Rotator(yaw=yaw), scale=unreal.Vector(*scale)))


def box(category, lo, hi, mat):
    add(category, '/Engine/BasicShapes/Cube', tuple((a+b)/2 for a,b in zip(lo,hi)), tuple((b-a)/100 for a,b in zip(lo,hi)), mat=mat)


def floor_rect(name, x0, y0, x1, y1, z, exclude=()):
    for x in range(x0, x1, 600):
        for y in range(y0, y1, 600):
            if any(a <= x < c and b <= y < d for a,b,c,d in exclude):
                continue
            # Source is [-600,0] x [0,600], 20 cm thick, top at local Z=20.
            add('Ground_' + name, FLOOR, (x+600, y, z-20))


def rail(x0, y0, x1, y1, z):
    length = math.hypot(x1-x0, y1-y0)
    if length < 1:
        return
    yaw = math.degrees(math.atan2(y1-y0, x1-x0))
    for h in (55,105):
        add('Rail_Beams', '/Engine/BasicShapes/Cube', ((x0+x1)/2,(y0+y1)/2,z+h), (length/100,.08,.08), yaw, METAL)
    count = max(1, math.ceil(length/150))
    for i in range(count+1):
        t=i/count
        box('Rail_Posts',(x0+(x1-x0)*t-4,y0+(y1-y0)*t-4,z),(x0+(x1-x0)*t+4,y0+(y1-y0)*t+4,z+110),METAL)


def wall_x(name, x0, x1, y, low, high, gaps=(), top_rail=True):
    for x in range(x0, x1, 600):
        if any(a <= x < b for a,b in gaps):
            continue
        z=low
        while z < high-.1:
            height=min(800,high-z)
            add('Walls_'+name,WALL,(x+600,y,z),(1,1,height/800))
            z+=height
        box('Wall_Caps',(x,y-42,high-5),(x+600,y+42,high+15),TRIM)
        if top_rail:
            rail(x,y,x+600,y,high+15)


def wall_y(name, x, y0, y1, low, high):
    for y in range(y0,y1,600):
        z=low
        while z < high-.1:
            height=min(800,high-z)
            add('Walls_'+name,WALL,(x,y+600,z),(1,1,height/800),90)
            z+=height
        box('Wall_Caps',(x-42,y,high-5),(x+42,y+600,high+15),TRIM)
        rail(x,y,x,y+600,high+15)


def staircase(name, x0, width, y0, low, flights, landings=()):
    y=y0
    z=low
    for i in range(flights):
        for lane in range(width//300):
            # Authored top tread is -1 cm; +1 aligns it with the receiving platform.
            add('Stairs_'+name,STAIR,(x0+(lane+1)*300,y+RUN,z+RISE+1))
        for x in (x0-18,x0+width+18):
            # A sloped, reusable box beam, plus end posts. Pitch rotates local X toward +Z.
            length=math.hypot(RUN,RISE)
            rot=unreal.Rotator(pitch=math.degrees(math.atan2(RISE,RUN)),yaw=90)
            add('Stair_Handrails','/Engine/BasicShapes/Cube',(x,y+RUN/2,z+RISE/2+100),(length/100,.10,.10),mat=METAL,rotation=rot)
            for yy,zz in ((y,z),(y+RUN,z+RISE)):
                box('Stair_Posts',(x-5,yy-5,zz-5),(x+5,yy+5,zz+105),METAL)
        y+=RUN
        z+=RISE
        if i+1 in landings:
            box('Ground_StairLandings',(x0,y,z-30),(x0+width,y+600,z),TRIM)
            for x in (x0+20,x0+width-80):
                box('Stair_Supports',(x,y+240,low),(x+60,y+360,z-30),TRIM)
            rail(x0,y,x0,y+600,z)
            rail(x0+width,y,x0+width,y+600,z)
            y+=600
    box('Stair_EndClosures',(x0,y-20,low),(x0+width,y+20,z-20),TRIM)
    CONNECTIONS.append(dict(name=name,x=[x0,x0+width],start_y=y0,end_y=y,low=low,high=z,flights=flights,step_cm=STEP))
    return y,z


def planter(x,y,z,tree=True):
    box('Planter_Soil',(x-250,y-250,z),(x+250,y+250,z+35),SOIL)
    for lo,hi in [((x-275,y-275,z),(x+275,y-250,z+65)),((x-275,y+250,z),(x+275,y+275,z+65)),((x-275,y-250,z),(x-250,y+250,z+65)),((x+250,y-250,z),(x+275,y+250,z+65))]:
        box('Planter_Edges',lo,hi,TRIM)
    if tree:
        add('Trees_Park',source('SM_207FF5952CF86936'),(x,y,z+35),(.8,.8,.8),random.randrange(360))
    box('Bench_Seat',(x-220,y-390,z+40),(x+220,y-330,z+52),WOOD)
    for xx in (x-160,x+160):
        box('Bench_Legs',(xx-8,y-385,z),(xx+8,y-335,z+40),METAL)


def building(x,y,w,d,z,floors):
    global BUILDING_OWNER
    BUILDING_OWNER='GT_Building_'+str(len(BUILDING_ORIGINS)+1).zfill(2)
    BUILDING_ORIGINS[BUILDING_OWNER]=(x+w/2,y+d/2,z)
    base='/PCGPlugins/CSRoadNet/Houses/NCSimple/SM_NC_'

    def span(kind,start,length,yaw,zz):
        pieces=math.ceil(length/600)
        size=length/pieces
        dx,dy=math.cos(math.radians(yaw)),math.sin(math.radians(yaw))
        for i in range(pieces):
            add('Building_Facades',base+kind+'_W600_H400',(start[0]+dx*i*size,start[1]+dy*i*size,zz),(size/600,1,1),yaw)

    for f in range(floors):
        zz=z+400*f
        # Four real L-shaped corner modules reserve 350 cm on each adjoining wall.
        for xx,yy,yaw in ((x,y,90),(x+w,y,180),(x+w,y+d,-90),(x,y+d,0)):
            add('Building_Corners',base+'CornerWrap_H400',(xx,yy,zz),yaw=yaw)
        if f==0:
            side=(w-700-300)/2
            span('Window',(x+w-350,y),side,180,zz)
            add('Building_Doors',base+'Door_W300_H400',(x+w/2+150,y,zz),yaw=180)
            span('Window',(x+w/2-150,y),side,180,zz)
        else:
            span('Window',(x+w-350,y),w-700,180,zz)
        span('Wall',(x+350,y+d),w-700,0,zz)
        span('Window',(x,y+350),d-700,90,zz)
        span('Window',(x+w,y+d-350),d-700,-90,zz)
    box('Building_Roofs',(x-30,y-30,z+floors*400),(x+w+30,y+d+30,z+floors*400+30),TRIM)
    box('Building_Floors',(x,y,z-20),(x+w,y+d,z),TRIM)
    BUILDING_OWNER=''


def paths():
    """A simple pedestrian network; every flat connection is an instanced paving strip.

    Slabs are 1 cm above existing paving, so a shared endpoint has no curb to climb.
    The continuous routes join the existing stair mouths at their own elevations.
    """
    path_mat=material('PathPaving',(.18,.125,.065))
    edge_mat=material('PathBorder',(.55,.47,.31))
    cells=set()

    def segment(name,a,b,z,width=400):
        x0,y0=a; x1,y1=b
        assert x0==x1 or y0==y1
        xmin,xmax=min(x0,x1)-width/2,max(x0,x1)+width/2
        ymin,ymax=min(y0,y1)-width/2,max(y0,y1)+width/2
        # A shared 100 cm grid unions junctions without overlapping coplanar strips.
        for xx in range(int(xmin),int(xmax),100):
            for yy in range(int(ymin),int(ymax),100):
                cells.add((xx,yy,z))
        PATH_SEGMENTS.append(dict(name=name,a=[x0,y0,z],b=[x1,y1,z],width_cm=width))

    # Low-level paths stay north of the sunken courtyard and the foreground building.
    # The courtyard occupies X 1800..5400 and Y -4800..-1200: route around its north edge.
    segment('LowWestBranch',(-7800,-3400),(1200,-3400),0)
    segment('CourtWestBypass',(1200,-3400),(1200,-600),0)
    segment('LowNorthWalk',(1200,-600),(7800,-600),0)
    segment('LowEastBranch',(7800,-600),(7800,-3400),0)
    segment('MainStairApproach',(-5400,-3400),(-5400,-3200),0)
    segment('LowBuildingSide',(-6900,-3400),(-6900,-5900),0,200)
    segment('LowBuildingFront',(-6900,-5900),(-5400,-5900),0,200)
    segment('LowBuildingEntry',(-5400,-5900),(-5400,-5800),0,200)
    segment('CourtStairApproach',(3600,-600),(3600,-1000),0)
    segment('WestPerimeter',(-7800,-3400),(-7800,9000),0)
    segment('EastPerimeter',(7800,-3400),(7800,9000),0)
    segment('NorthPerimeter',(-7800,9000),(7800,9000),0)
    segment('ParkStairArrival',(-5400,200),(-5400,1600),1200)
    segment('ParkCrosswalk',(-5400,1600),(4200,1600),1200)
    segment('UpperStairBypass',(4200,1600),(4200,800),1200)
    segment('UpperStairFront',(4200,800),(5400,800),1200)
    # Stop at Y=1000 so the final 200 cm half-width meets the upper stair start at 1200.
    segment('UpperStairApproach',(5400,800),(5400,1000),1200)
    segment('GardenStairApproach',(0,1600),(0,1600),1200)
    segment('UpperStairArrival',(5400,4400),(5400,4800),2400)
    segment('UpperCrosswalk',(-4500,4800),(5400,4800),2400)
    for xx in (-4500,-300,3900):
        segment('UpperEntry_'+str(xx),(xx,4800),(xx,4900),2400)
        segment('PlinthEntry_'+str(xx),(xx,5600),(xx,5800),2600)
    segment('RaisedGardenWalk',(0,2600),(0,3800),1600)
    segment('CourtWalk',(3600,-4600),(3600,-2000),-400)
    for xx,yy,zz in sorted(cells):
        box('Paths_Paving',(xx,yy,zz-.5),(xx+100,yy+100,zz+1),path_mat)
        for dx,dy in ((-100,0),(100,0),(0,-100),(0,100)):
            if (xx+dx,yy+dy,zz) in cells:
                continue
            if dx:
                edge_x=xx if dx<0 else xx+94
                box('Paths_Edges',(edge_x,yy,zz+1),(edge_x+6,yy+100,zz+1.2),edge_mat)
            else:
                edge_y=yy if dy<0 else yy+94
                box('Paths_Edges',(xx,edge_y,zz+1),(xx+100,edge_y+6,zz+1.2),edge_mat)


def new_component(actor,cls):
    subsystem=unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    handles=subsystem.k2_gather_subobject_data_for_instance(actor)
    params=unreal.AddNewSubobjectParams(parent_handle=handles[0],new_class=cls,conform_transform_to_parent=True)
    handle,reason=subsystem.add_new_subobject(params)
    data=unreal.SubobjectDataBlueprintFunctionLibrary.get_data(handle)
    component=unreal.SubobjectDataBlueprintFunctionLibrary.get_associated_object(data)
    assert component, str(reason)
    return component


def label(text,location,size=130):
    a=ACTORS.spawn_actor_from_class(unreal.TextRenderActor,unreal.Vector(*location),unreal.Rotator(yaw=-90))
    a.set_actor_label('GT_Label_'+text)
    c=a.get_component_by_class(unreal.TextRenderComponent)
    c.set_text(text)
    c.set_world_size(size)
    c.set_text_render_color(unreal.Color(240,200,70,255))
    a.set_folder_path('GroundTerraces/Labels')


def emit():
    report=[]
    owners={}
    for (category,path,mat,tile,owner),transforms in sorted(GROUPS.items()):
        mesh=MESH_CACHE.setdefault(path,unreal.load_asset(path))
        assert mesh, path
        if owner:
            if owner not in owners:
                a=ACTORS.spawn_actor_from_class(unreal.Actor,unreal.Vector(0,0,0))
                a.set_actor_label(owner)
                a.set_folder_path('GroundTerraces/Buildings')
                if not a.root_component:
                    new_component(a,unreal.SceneComponent)
                a.root_component.set_mobility(unreal.ComponentMobility.STATIC)
                a.set_actor_location(unreal.Vector(*BUILDING_ORIGINS[owner]),False,True)
                owners[owner]=a
            a=owners[owner]
            c=new_component(a,unreal.InstancedStaticMeshComponent)
        else:
            a=ACTORS.spawn_actor_from_class(unreal.CSInstanceContainer,unreal.Vector(0,0,0))
            a.set_actor_label('GT_'+category+'_'+str(tile[0])+'_'+str(tile[1]))
            a.set_folder_path('GroundTerraces/'+category.split('_')[0])
            c=a.get_editor_property('instances')
        c.set_mobility(unreal.ComponentMobility.STATIC)
        c.set_editor_property('component_tags',['GTCategory='+category])
        c.set_static_mesh(mesh)
        c.set_collision_profile_name('BlockAll')
        if category.startswith('Trees'):
            c.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
        if mat:
            c.set_material(0,unreal.load_asset(mat))
        c.add_instances(transforms,False,world_space=True,update_navigation=False)
        assert c.get_instance_count()==len(transforms)
        report.append(dict(actor=a.get_actor_label(),component=c.get_name(),category=category,mesh=path,material=mat,instances=len(transforms)))
    return report


def main():
    global FLOOR,STAIR,WALL,TRIM,METAL,SOIL,WOOD
    OUT.mkdir(parents=True,exist_ok=True)
    random.seed(574)
    registry=unreal.AssetRegistryHelpers.get_asset_registry()
    registry.scan_paths_synchronous([FOLDER],True)
    if ASSETS.does_asset_exist(LEVEL):
        existing=unreal.load_asset(LEVEL)
        assert ASSETS.get_metadata_tag(existing,'GroundTerraces')=='1','Refusing to overwrite unrelated map'
        assert LEVELS.load_level(LEVEL)
        for actor in ACTORS.get_all_level_actors():
            if actor.get_actor_label().startswith('GT_'):
                ACTORS.destroy_actor(actor)
    else:
        assert LEVELS.new_level_from_template(LEVEL,'/Engine/Maps/Templates/Template_Default')
        for actor in ACTORS.get_all_level_actors():
            if isinstance(actor,unreal.StaticMeshActor):
                ACTORS.destroy_actor(actor)
    world=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    ASSETS.set_metadata_tag(world,'GroundTerraces','1')
    assert LEVELS.save_current_level()
    FLOOR=collision_copy('SM_3AF6CB60412A40E8','SM_GroundTile_600')
    STAIR=collision_copy('SM_029C66B51DC1EB37','SM_Stair_300x300_Rise200',True)
    WALL=collision_copy('SM_11F7A1D89C1A6BFE','SM_RetainingWall_600x800')
    TRIM=material('ConcreteTrim',(.32,.37,.40))
    METAL=material('DarkMetal',(.035,.055,.065),.45)
    SOIL=material('GardenSoil',(.07,.105,.038))
    WOOD=material('BenchWood',(.30,.13,.045))
    # The 36 x 36 m courtyard is a real gap in the upper ground tiles.
    courtyard=(1800,-4800,5400,-1200)
    floor_rect('LowPlaza',-7200,-6000,7200,0,0,[courtyard])
    floor_rect('SunkenCourt',1800,-4800,5400,-1200,-400)
    floor_rect('Park12m',-7200,0,7200,4200,1200)
    floor_rect('Upper24m',-7200,4200,7200,8400,2400)
    floor_rect('EastPromenade',7200,-6000,9000,8400,0)
    floor_rect('WestPromenade',-9000,-6000,-7200,8400,0)
    floor_rect('NorthPromenade',-9000,8400,9000,9600,0)
    # Long flights include level rest landings, all 2 m modules at unit scale.
    assert staircase('LowToPark',-6000,1200,-3000,0,6,(2,4))==(0,1200)
    assert staircase('ParkToUpper',4800,1200,1200,1200,6,(2,4))==(4200,2400)
    wall_x('ParkFront',-7200,7200,0,0,1200,[(-6000,-4800)])
    wall_x('UpperFront',-7200,7200,4200,1200,2400,[(4800,6000)])
    for x in (-7200,7200):
        wall_y('ParkSide',x,0,4200,0,1200)
        wall_y('UpperSide',x,4200,8400,0,2400)
    wall_x('UpperBack',-7200,7200,8400,0,2400)
    wall_x('CourtSouth',1800,5400,-4800,-400,0)
    wall_x('CourtNorth',1800,5400,-1200,-400,0,[(3000,4200)])
    wall_y('CourtWest',1800,-4800,-1200,-400,0)
    wall_y('CourtEast',5400,-4800,-1200,-400,0)
    assert staircase('CourtToPlaza',3000,1200,-1800,-400,2)==(-1200,0)
    # Raised park belvedere: +4 m relative to the park, approached by two flights.
    floor_rect('RaisedGarden',-1800,2400,1800,4200,1600)
    wall_x('GardenFront',-1800,1800,2400,1200,1600,[(-600,600)])
    wall_y('GardenWest',-1800,2400,4200,1200,1600)
    wall_y('GardenEast',1800,2400,4200,1200,1600)
    assert staircase('ParkToGarden',-600,1200,1800,1200,2)==(2400,1600)
    for x in (-6600,-3600,3000,6600):
        for y in (900,2700):
            planter(x,y,1200)
    for x in (-1200,1200):
        planter(x,3300,1600)
    for x in (2400,4800):
        planter(x,-3600,-400)
    for x in (-3600,-1200):
        planter(x,-4200,0)
    # Building thresholds are another one-flight (+2 m) ecology platform.
    for x,floors in ((-6000,5),(-1800,7),(2400,4)):
        floor_rect('BuildingPlinth',x,5400,x+3000,8400,2600)
        wall_x('BuildingPlinth',x,x+3000,5400,2400,2600,[(x+1200,x+1800)],False)
        wall_y('BuildingPlinth',x,5400,8400,2400,2600)
        wall_y('BuildingPlinth',x+3000,5400,8400,2400,2600)
        staircase('BuildingEntry_'+str(x),x+1200,600,5100,2400,1)
        building(x+300,6000,2400,1800,2600,floors)
    # A low foreground building makes the 24 m retaining wall easy to compare.
    building(-6600,-5700,2400,1800,0,2)
    paths()
    rows=emit()
    for actor in ACTORS.get_all_level_actors():
        if isinstance(actor,unreal.DirectionalLight):
            actor.set_actor_rotation(unreal.Rotator(pitch=-48,yaw=135),False)
            actor.light_component.set_editor_property('intensity',4.0)
        if isinstance(actor,unreal.SkyLight):
            actor.light_component.set_editor_property('intensity',1.2)
        if isinstance(actor,unreal.PlayerStart):
            actor.set_actor_location(unreal.Vector(-2400,-3000,120),False,True)
            actor.set_actor_rotation(unreal.Rotator(yaw=90),False)
    gm=unreal.load_class(None,'/Game/ThirdPerson/Blueprints/BP_ThirdPersonGameMode.BP_ThirdPersonGameMode_C')
    assert gm
    world.get_world_settings().set_editor_property('default_game_mode',gm)
    for text,location in [('LOW PLAZA  0 m',(-3000,-500,220)),('PARK  +12 m',(-3400,1000,1450)),('UPPER TERRACE  +24 m',(-3000,4700,2700)),('SUNKEN COURT  -4 m',(2100,-4700,-150)),('GARDEN  +16 m',(-1200,2800,1800))]:
        label(text,location)
    loc=unreal.Vector(18000,-21500,19000)
    target=unreal.Vector(0,1800,1300)
    unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).set_level_viewport_camera_info(loc,unreal.MathLibrary.find_look_at_rotation(loc,target))
    assert LEVELS.save_current_level()
    manifest=dict(level=LEVEL,ground_levels_cm=[-400,0,1200,1600,2400,2600],main_ground_difference_cm=2800,stair_rise_cm=RISE,step_height_cm=STEP,stair_scale=[1,1,1],connections=CONNECTIONS,groups=rows,instance_count=sum(r['instances'] for r in rows),ism_components=len(rows),source_assets_unchanged=True,building_actors=list(BUILDING_ORIGINS),path_segments=PATH_SEGMENTS)
    (OUT/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
    unreal.log('GROUND_TERRACES_BUILD_OK '+json.dumps({k:v for k,v in manifest.items() if k not in ('groups','connections')}))


if __name__=='__main__':
    try:
        main()
    except Exception:
        OUT.mkdir(parents=True,exist_ok=True)
        (OUT/'build_error.txt').write_text(traceback.format_exc(),encoding='utf-8')
        raise
