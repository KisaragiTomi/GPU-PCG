# -*- coding: utf-8 -*-
"""Creates /PCGPlugins/CSRoadNet/L_SplineRoadDemo: CS2 roads drawn as splines on flat ground.

  UnrealEditor-Cmd.exe <project> -ExecutePythonScript=<this file> -unattended -RenderOffscreen

The level starts from the engine's Basic template (sun, sky, fog); its floor gives way to a 4 km
ground 0.25 m under the roads, as a road's asphalt runs 0.2 m below its spline. One
ACSRoadNetworkActor builds from the ACSRoadSplineActors hanging under it: a Medium Road main
street crossed by a Small Road, a curved street ending 0.5 m short of the main street (it joins
it in a T), a two-lane highway passing 12 m over the main street and ramping down onto a street
at each end (a highway has no rounded dead end), and a closed ring road reached by a short link
that starts on the cross street and ends on a ring point. Moving a spline point in the editor
rebuilds the roads on the next tick. Refuses to overwrite the level; delete it first to make it
again.
"""
import unreal

LEVEL = "/PCGPlugins/CSRoadNet/L_SplineRoadDemo"
FOLDER = "/PCGPlugins/CSRoadNet"
GROUND_MATERIAL = FOLDER + "/M_SplineRoadDemoGround"
TEMPLATE = "/Engine/Maps/Templates/Template_Default"
NEVER = unreal.PropertyAccessChangeNotifyMode.NEVER
KEEP = unreal.AttachmentRule.KEEP_WORLD

ASSETS = unreal.EditorAssetLibrary
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)


def log(text):
    unreal.log("CS2SPLINEDEMO " + text)


def ground_material():
    """Flat RoadLab terrain green (#7d927d), fully rough."""
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
        unreal.log_error("CS2SPLINEDEMO {} exists; delete it to make it again".format(LEVEL))
        return
    if not LEVELS.new_level_from_template(LEVEL, TEMPLATE):
        unreal.log_error("CS2SPLINEDEMO could not create {} from {}".format(LEVEL, TEMPLATE))
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
    network.set_actor_label("CS2 Spline Roads")
    network.set_editor_property("source", unreal.CSRoadNetworkSource.SPLINES, notify_mode=NEVER)
    road_class = unreal.load_class(None, "/Script/CSRoadNet.CSRoadSplineActor")

    def road(label, prefab, points, closed=False):
        """A spline road through points (cm, network space)."""
        actor = ACTORS.spawn_actor_from_class(road_class, unreal.Vector(0.0, 0.0, 0.0))
        actor.set_actor_label(label)
        actor.attach_to_actor(network, "", KEEP, KEEP, KEEP, False)
        spline = actor.get_editor_property("spline")
        spline.set_editor_property("prefab", prefab, notify_mode=NEVER)
        spline.clear_spline_points(False)
        for x, y, z in points:
            spline.add_spline_point(unreal.Vector(x, y, z), unreal.SplineCoordinateSpace.LOCAL, False)
        spline.set_closed_loop(closed, False)
        spline.update_spline()

    road("Main Street", "Medium Road", [(-20000, 0, 0), (20000, 0, 0)])
    road("Cross Street", "Small Road", [(0, -15000, 0), (0, 15000, 0)])
    road("Side Street", "Small Road", [(9000, 50, 0), (9000, 6000, 0), (14000, 12000, 0)])
    road("North Street", "Small Road", [(-15000, 20000, 0), (-3000, 20000, 0)])
    road("South Street", "Small Road", [(-15000, -20000, 0), (-3000, -20000, 0)])
    road("Highway", "Highway Twoway - 2 lanes", [(-9000, -20000, 0), (-9000, -10000, 1200), (-9000, 10000, 1200), (-9000, 20000, 0)])
    road("Ring Road", "Small Road", [(12000, -8500, 0), (7000, -3500, 0), (2000, -8500, 0), (7000, -13500, 0)], closed=True)
    road("Ring Link", "Small Road", [(50, -8500, 0), (2000, -8500, 0)])

    network.rebuild()
    stats = {k: network.get_editor_property(k) for k in ("num_splines", "num_roads", "num_triangles", "num_marking_triangles", "last_error")}
    log("build " + str(stats))
    log("actors " + ", ".join(sorted("{} ({})".format(a.get_actor_label(), a.get_class().get_name()) for a in ACTORS.get_all_level_actors())))
    if not LEVELS.save_current_level():
        unreal.log_error("CS2SPLINEDEMO could not save " + LEVEL)
        return
    log("saved " + LEVEL)


build()
