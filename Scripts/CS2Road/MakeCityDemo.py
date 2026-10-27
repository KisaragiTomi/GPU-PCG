# -*- coding: utf-8 -*-
"""Creates /PCGPlugins/CSRoadNet/L_CityDemo: the city's local update on a CS2 spline road network.

  UnrealEditor-Cmd.exe <project> -ExecutePythonScript=<this file> -unattended -RenderOffscreen

The order LocalUpdateSystem_Plan.md lays down: two special buildings (ACSCityBuildingActor, convex
outlines pushed up) are placed first and reserve their plots; an ACSCityAlleyRegion lays its alleys
out around them (Generate), once; a Medium Road avenue then runs across the alleys, which are cut
back under it and join it; the network fills the plots along every road with batch buildings. The
network builds in 128 m tiles (bLocalUpdate) and so do the batch buildings: dragging the avenue or
a special building rewrites only the tiles around it, and the alleys are never laid out again.
Same flat ground as L_SplineRoadDemo. Refuses to overwrite the level; delete it first to make it
again.
"""
import unreal

LEVEL = "/PCGPlugins/CSRoadNet/L_CityDemo"
FOLDER = "/PCGPlugins/CSRoadNet"
GROUND_MATERIAL = FOLDER + "/M_SplineRoadDemoGround"
TEMPLATE = "/Engine/Maps/Templates/Template_Default"
NEVER = unreal.PropertyAccessChangeNotifyMode.NEVER
KEEP = unreal.AttachmentRule.KEEP_WORLD

ASSETS = unreal.EditorAssetLibrary
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)


def log(text):
    unreal.log("CS2CITYDEMO " + text)


def ground_material():
    """L_SplineRoadDemo's ground: flat RoadLab terrain green (#7d927d), fully rough."""
    if ASSETS.does_asset_exist(GROUND_MATERIAL):
        return ASSETS.load_asset(GROUND_MATERIAL)
    edit = unreal.MaterialEditingLibrary
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset("M_SplineRoadDemoGround", FOLDER, unreal.Material, unreal.MaterialFactoryNew())
    colour = edit.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -300, 0)
    colour.set_editor_property("constant", unreal.LinearColor(0.205, 0.288, 0.205, 1.0))
    edit.connect_material_property(colour, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = edit.create_material_expression(material, unreal.MaterialExpressionConstant, -300, 200)
    rough.set_editor_property("r", 1.0)
    edit.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    edit.recompile_material(material)
    ASSETS.save_loaded_asset(material, False)
    return material


def build():
    if ASSETS.does_asset_exist(LEVEL):
        unreal.log_error("CS2CITYDEMO {} exists; delete it to make it again".format(LEVEL))
        return
    if not LEVELS.new_level_from_template(LEVEL, TEMPLATE):
        unreal.log_error("CS2CITYDEMO could not create {} from {}".format(LEVEL, TEMPLATE))
        return

    # The template's floor is 10 m wide.
    for actor in ACTORS.get_all_level_actors():
        component = actor.get_component_by_class(unreal.StaticMeshComponent) if isinstance(actor, unreal.StaticMeshActor) else None
        mesh = component.get_editor_property("static_mesh") if component else None
        if mesh and mesh.get_name() == "SM_Template_Map_Floor":
            ACTORS.destroy_actor(actor)
    ground = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(0.0, 0.0, -25.0))
    ground.set_actor_label("Ground")
    ground.set_actor_scale3d(unreal.Vector(4000.0, 4000.0, 1.0))
    component = ground.get_component_by_class(unreal.StaticMeshComponent)
    component.set_editor_property("static_mesh", unreal.load_asset("/Engine/BasicShapes/Plane"))
    component.set_material(0, ground_material())

    network = ACTORS.spawn_actor_from_class(unreal.load_class(None, "/Script/CSRoadNet.CSRoadNetworkActor"), unreal.Vector(0.0, 0.0, 0.0))
    network.set_actor_label("CS2 City")
    network.set_editor_property("source", unreal.CSRoadNetworkSource.SPLINES, notify_mode=NEVER)
    network.set_editor_property("local_update", True, notify_mode=NEVER)
    network.set_editor_property("tile_size", 128.0, notify_mode=NEVER)
    network.set_editor_property("batch_buildings", True, notify_mode=NEVER)

    # 1. Special buildings first: their plots are reserved before any alley is laid out.
    building_class = unreal.load_class(None, "/Script/CSRoadNet.CSCityBuildingActor")

    def building(label, centre, size, yaw, height, setback, colour):
        """A rectangular outline of size (cm) turned by yaw, pushed up height (m)."""
        actor = ACTORS.spawn_actor_from_class(building_class, unreal.Vector(centre[0], centre[1], 0.0), unreal.Rotator(0.0, 0.0, yaw))
        actor.set_actor_label(label)
        actor.set_editor_property("height", height, notify_mode=NEVER)
        actor.set_editor_property("setback", setback, notify_mode=NEVER)
        actor.set_editor_property("color", unreal.LinearColor(*colour), notify_mode=NEVER)
        outline = actor.get_editor_property("outline")
        outline.clear_spline_points(False)
        half_x, half_y = size[0] * 0.5, size[1] * 0.5
        for x, y in ((-half_x, -half_y), (half_x, -half_y), (half_x, half_y), (-half_x, half_y)):
            outline.add_spline_point(unreal.Vector(x, y, 0.0), unreal.SplineCoordinateSpace.LOCAL, False)
            outline.set_spline_point_type(outline.get_number_of_spline_points() - 1, unreal.SplinePointType.LINEAR, False)
        outline.set_closed_loop(True, False)
        outline.update_spline()
        return actor

    building("Town Hall", (-6000.0, 3500.0), (4000.0, 2800.0), 0.0, 22.0, 6.0, (0.80, 0.52, 0.38, 1.0))
    building("Tower", (8000.0, -4000.0), (1800.0, 1800.0), 30.0, 48.0, 5.0, (0.45, 0.55, 0.70, 1.0))

    # 2. The alleys, laid out once around the reserved plots.
    region = ACTORS.spawn_actor_from_class(unreal.load_class(None, "/Script/CSRoadNet.CSCityAlleyRegion"), unreal.Vector(0.0, 0.0, 0.0))
    region.set_actor_label("Old Town")
    region.get_editor_property("bounds").set_box_extent(unreal.Vector(15000.0, 10000.0, 500.0), True)
    region.generate()

    # 3. A main road over them: the alleys it covers are cut back and join it.
    road = ACTORS.spawn_actor_from_class(unreal.load_class(None, "/Script/CSRoadNet.CSRoadSplineActor"), unreal.Vector(0.0, 0.0, 0.0))
    road.set_actor_label("Avenue")
    road.attach_to_actor(network, "", KEEP, KEEP, KEEP, False)
    spline = road.get_editor_property("spline")
    spline.set_editor_property("prefab", "Medium Road", notify_mode=NEVER)
    spline.clear_spline_points(False)
    for x, y in ((-24000.0, -13000.0), (0.0, -2000.0), (24000.0, 12000.0)):
        spline.add_spline_point(unreal.Vector(x, y, 0.0), unreal.SplineCoordinateSpace.LOCAL, False)
    spline.update_spline()

    # 4. Roads and batch buildings.
    network.rebuild()
    keys = ("num_splines", "num_roads", "num_tiles", "num_buildings", "num_triangles", "last_error")
    log("build " + str({k: network.get_editor_property(k) for k in keys}))
    log("alleys {}".format(len(region.get_editor_property("generated"))))
    if not LEVELS.save_current_level():
        unreal.log_error("CS2CITYDEMO could not save " + LEVEL)
        return
    log("saved " + LEVEL)


build()
