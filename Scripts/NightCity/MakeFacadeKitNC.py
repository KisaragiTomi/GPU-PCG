# -*- coding: utf-8 -*-
"""Night City facade kits for the GPU facade generation (CSCityFacadeKit.h) and a level to look at them, in the Night
City content folder (NightCity_BuildingKit.md section 4.4):

  /Game/BuildingGenerator/Cyberpunk2077/Tests/FacadeKit/DA_FacadeKit_CharterHillA   wbr_cha_building_a parts
  /Game/BuildingGenerator/Cyberpunk2077/Tests/FacadeKit/DA_FacadeKit_CharterHillB   wbr_cha_building_b walls and cornice
  /Game/BuildingGenerator/Cyberpunk2077/Tests/FacadeKit/L_FacadeKit_CharterHill     buildings of three sizes per kit
  /Game/BuildingGenerator/Cyberpunk2077/Tests/FacadeKit/Meshes/SM_*_Nanite          copies with Nanite on, only of
                                                                                    parts that have none

The kits only point at the Night City meshes (Meshes/<xx>/SM_<hash>); nothing of theirs is changed. A part without
Nanite is copied with Nanite on, so that its instances go into GPU-Scene like the plugin's own kit.

QuarterTurns: which side of a part faces the street, from how Night City places it (Scripts/NightCity/nc_orient.py,
run on the instance dumps): the floor tiles next to each placed copy lie inside the building. building_a's windows
and cornices have their outside at -Y (two quarter turns); building_b's walls and doors at +Y.

  UnrealEditor-Cmd.exe <project> -ExecutePythonScript=<this file> -unattended -RenderOffscreen

Updates the kits in place; makes the level only when it does not exist (delete it to make it again).
"""
import unreal

NC = "/Game/BuildingGenerator/Cyberpunk2077"
FOLDER = NC + "/Tests/FacadeKit"
COPIES = FOLDER + "/Meshes"
LEVEL = FOLDER + "/L_FacadeKit_CharterHill"
TEMPLATE = "/Engine/Maps/Templates/Template_Default"
NEVER = unreal.PropertyAccessChangeNotifyMode.NEVER

ASSETS = unreal.EditorAssetLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)

# Part -> (Night City mesh, source name, quarter turns).
PARTS = {
    "a_wall": ("SM_75C359765B5FB481", "wbr_cha_building_a_wall_w300_h400_aa", 2),
    "a_window": ("SM_C462863AFD826788", "wbr_cha_building_a_wall_window_w300_h400_ac", 2),
    "a_pillar": ("SM_66F6CF0FCDB1B0BA", "wbr_cha_building_a_wall_pillar_aa", 0),
    "a_cornice": ("SM_A0B41F62BBADFA7C", "wbr_cha_building_a_roof_cornice_w300_aa", 2),
    "a_roof": ("SM_042B54B9283A6DB6", "wbr_cha_building_a_roof_w300_aa", 0),
    "b_wall": ("SM_6E499BBB42F56C0C", "wbr_cha_building_b_wall_w300_h400_ac", 0),
    "b_door": ("SM_EC5A1AF92817B42A", "wbr_cha_building_b_wall_w200_h400_door_ac", 0),
    "b_cornice": ("SM_225E25A0A90F8038", "wbr_cha_building_b_wall_cornice_w_300_aa", 2),
}

KITS = {
    "DA_FacadeKit_CharterHillA": {"wall": "a_wall", "window": "a_window", "door": "b_door", "corner": "a_pillar", "parapet": "a_cornice", "roof": "a_roof", "window_chance": 0.55},
    "DA_FacadeKit_CharterHillB": {"wall": "b_wall", "window": "a_window", "door": "b_door", "corner": "a_pillar", "parapet": "b_cornice", "roof": "a_roof", "window_chance": 0.4},
}


def log(text):
    unreal.log("NCFACADEKIT " + text)


def fail(text):
    unreal.log_error("NCFACADEKIT " + text)


def nanite_on(mesh):
    return mesh.get_editor_property("nanite_settings").get_editor_property("enabled")


def part_mesh(key):
    asset, source, _ = PARTS[key]
    path = "{}/Meshes/{}/{}".format(NC, asset[3:5], asset)
    mesh = ASSETS.load_asset(path)
    if not mesh:
        fail("{} ({}) not found at {}".format(key, source, path))
        return None
    box = mesh.get_bounding_box()
    log("{:10s} {} {}: box ({:.0f}, {:.0f}, {:.0f}) .. ({:.0f}, {:.0f}, {:.0f}), {} sections, Nanite {}".format(
        key, asset, source, box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z, mesh.get_num_sections(0), nanite_on(mesh)))
    if nanite_on(mesh):
        return mesh
    # A copy with Nanite on; the original stays as it is.
    copy_path = "{}/{}_Nanite".format(COPIES, asset)
    copy = ASSETS.load_asset(copy_path) if ASSETS.does_asset_exist(copy_path) else ASSETS.duplicate_asset(path, copy_path)
    if not copy:
        fail("could not copy {} to {}".format(path, copy_path))
        return mesh
    if not nanite_on(copy):
        settings = copy.get_editor_property("nanite_settings")
        settings.set_editor_property("enabled", True)
        copy.set_editor_property("nanite_settings", settings)
        unreal.CSStaticMeshBuildLibrary.clear_fast_build_and_rebuild(copy)
        ASSETS.save_loaded_asset(copy, False)
    log("{:10s} copy {} Nanite {}".format(key, copy_path, nanite_on(copy)))
    return copy


def make_kits(meshes):
    kits = {}
    for name, slots in KITS.items():
        path = FOLDER + "/" + name
        if ASSETS.does_asset_exist(path):
            kit = ASSETS.load_asset(path)
        else:
            factory = unreal.DataAssetFactory()
            factory.set_editor_property("data_asset_class", unreal.CSCityFacadeKit.static_class())
            kit = TOOLS.create_asset(name, FOLDER, unreal.CSCityFacadeKit, factory)
            log("made " + path)
        if not kit:
            fail("could not make " + path)
            continue
        for slot in ("wall", "window", "door", "corner", "parapet", "roof"):
            key = slots[slot]
            module = unreal.CSCityFacadeModule()
            module.set_editor_property("mesh", meshes[key])
            module.set_editor_property("quarter_turns", PARTS[key][2])
            kit.set_editor_property(slot, module)
        kit.set_editor_property("bay_width", 300.0)
        kit.set_editor_property("storey_height", 400.0)
        kit.set_editor_property("window_chance", slots["window_chance"])
        ASSETS.save_loaded_asset(kit, False)
        kits[name] = kit
    return kits


def make_level(kits):
    if ASSETS.does_asset_exist(LEVEL):
        log("{} exists; delete it to make it again".format(LEVEL))
        return
    if not LEVELS.new_level_from_template(LEVEL, TEMPLATE):
        fail("could not create {} from {}".format(LEVEL, TEMPLATE))
        return
    for actor in ACTORS.get_all_level_actors():
        component = actor.get_component_by_class(unreal.StaticMeshComponent) if isinstance(actor, unreal.StaticMeshActor) else None
        mesh = component.get_editor_property("static_mesh") if component else None
        if mesh and mesh.get_name() == "SM_Template_Map_Floor":
            ACTORS.destroy_actor(actor)
    ground = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(2500.0, 3000.0, 0.0))
    ground.set_actor_label("Ground")
    ground.set_actor_scale3d(unreal.Vector(160.0, 160.0, 1.0))
    component = ground.get_component_by_class(unreal.StaticMeshComponent)
    component.set_editor_property("static_mesh", unreal.load_asset("/Engine/BasicShapes/Plane"))
    component.set_material(0, unreal.load_asset("/Engine/BasicShapes/BasicShapeMaterial"))

    building_class = unreal.load_class(None, "/Script/CSRoadNet.CSCityBuildingActor")

    def building(label, kit, centre, size, yaw, height):
        actor = ACTORS.spawn_actor_from_class(building_class, unreal.Vector(centre[0], centre[1], 0.0), unreal.Rotator(0.0, 0.0, yaw))
        actor.set_actor_label(label)
        actor.set_editor_property("height", height, notify_mode=NEVER)
        actor.set_editor_property("kit", kit, notify_mode=NEVER)
        outline = actor.get_editor_property("outline")
        outline.clear_spline_points(False)
        half_x, half_y = size[0] * 0.5, size[1] * 0.5
        for x, y in ((-half_x, -half_y), (half_x, -half_y), (half_x, half_y), (-half_x, half_y)):
            outline.add_spline_point(unreal.Vector(x, y, 0.0), unreal.SplineCoordinateSpace.LOCAL, False)
            outline.set_spline_point_type(outline.get_number_of_spline_points() - 1, unreal.SplinePointType.LINEAR, False)
        outline.set_closed_loop(True, False)
        # Rebuilds it with the kit.
        outline.update_spline()
        return actor

    for row, name in enumerate(sorted(kits)):
        y = row * 4000.0
        tag = name[-1]
        building("{} Shop".format(tag), kits[name], (0.0, y), (900.0, 600.0), 0.0, 8.0)
        building("{} Block".format(tag), kits[name], (2200.0, y), (1500.0, 1200.0), 0.0, 20.0)
        building("{} Tower".format(tag), kits[name], (4800.0, y), (1200.0, 1200.0), 30.0, 40.0)
    if not LEVELS.save_current_level():
        fail("could not save " + LEVEL)
        return
    log("saved " + LEVEL)


def run():
    meshes = {}
    for key in PARTS:
        meshes[key] = part_mesh(key)
        if not meshes[key]:
            return
    kits = make_kits(meshes)
    for name, kit in kits.items():
        log("kit {} {}".format(name, kit.get_path_name()))
    make_level(kits)


run()
