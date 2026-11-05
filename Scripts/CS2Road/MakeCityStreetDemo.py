# -*- coding: utf-8 -*-
"""Creates /PCGPlugins/CSRoadNet/Streets/L_CityStreets: city streets (ACSCityStreetActor, CSCityStreetActor.h), each a
spline lined with buildings put together on the GPU from a facade kit.

  UnrealEditor-Cmd.exe <project> -ExecutePythonScript=<this file> -unattended -nosplash -RenderOffscreen

- Four streets on their own (no road network), 120 m along +X, 12 m wide, buildings on both sides, one per Night City
  fabric (NightCity_BuildingKit.md section 2), 150 m apart along +Y: A low-rise scatter, B mid-rise mixed, C mid-rise
  street wall, D high-rise core; all from the plugin's simplified Night City concrete kit (MakeNCSimpleModules.py).
- E: a curved street wall, buildings on its right only, its middle point scaled up (Z 2.5): taller there.
- F: a road network with one main road (Medium Road, 200 m) at y = -200 m and a street hung under it, lining it from
  50 m to 150 m (fabric C); the network's batch buildings (its default kit) line the rest of the road.

Refuses to overwrite the level; delete it first to make it again. Look at it with CityStreetShots.py.
"""
import json

import unreal

LEVEL = "/PCGPlugins/CSRoadNet/Streets/L_CityStreets"
TEMPLATE = "/Engine/Maps/Templates/Template_Default"
GROUND_MATERIAL = "/PCGPlugins/CSRoadNet/M_SplineRoadDemoGround"
KIT = "/PCGPlugins/CSRoadNet/Houses/NCSimple/DA_NCSimple_Concrete"

NEVER = unreal.PropertyAccessChangeNotifyMode.NEVER
ALWAYS = unreal.PropertyAccessChangeNotifyMode.ALWAYS
ASSETS = unreal.EditorAssetLibrary
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
WORLD_SPACE = unreal.SplineCoordinateSpace.WORLD
KEEP = unreal.AttachmentRule.KEEP_WORLD

FABRICS = (
    ("A Low-rise scatter", unreal.CSCityFabric.LOW_SCATTER),
    ("B Mid-rise mixed", unreal.CSCityFabric.MID_MIXED),
    ("C Street wall", unreal.CSCityFabric.STREET_WALL),
    ("D High-rise core", unreal.CSCityFabric.HIGH_CORE),
)
SPACING = 150.0
LENGTH = 120.0


def log(text):
    unreal.log("CITYSTREETDEMO " + text)


def fail(text):
    unreal.log_error("CITYSTREETDEMO " + text)


def street(label, kit, fabric, points, sides=unreal.CSCityStreetSides.BOTH, width=12.0, curved=False):
    """A street on its own spline; the fabric's values first (set as the editor's property change does), then the rest."""
    actor = ACTORS.spawn_actor_from_class(unreal.CSCityStreetActor, unreal.Vector(points[0][0] * 100.0, points[0][1] * 100.0, 0.0))
    actor.set_actor_label(label)
    actor.set_editor_property("fabric", fabric, notify_mode=ALWAYS)
    actor.set_editor_property("kit", kit, notify_mode=NEVER)
    actor.set_editor_property("sides", sides, notify_mode=NEVER)
    actor.set_editor_property("street_width", width, notify_mode=NEVER)
    spline = actor.get_editor_property("street")
    spline.clear_spline_points(False)
    for x, y in points:
        spline.add_spline_point(unreal.Vector(x * 100.0, y * 100.0, 0.0), WORLD_SPACE, False)
    for i in range(len(points)):
        spline.set_spline_point_type(i, unreal.SplinePointType.CURVE if curved else unreal.SplinePointType.LINEAR, False)
    spline.update_spline()
    return actor


def summary(actor):
    return {
        "buildings": actor.get_editor_property("num_buildings"),
        "plots": len(actor.get_editor_property("plots")),
        "status": actor.get_editor_property("status"),
    }


def build():
    if ASSETS.does_asset_exist(LEVEL):
        fail("{} exists; delete it to make it again".format(LEVEL))
        return
    kit = unreal.load_asset(KIT)
    if not kit:
        fail("no kit at {} (run Scripts/NightCity/MakeNCSimpleModules.py)".format(KIT))
        return
    if not LEVELS.new_level_from_template(LEVEL, TEMPLATE):
        fail("could not create {} from {}".format(LEVEL, TEMPLATE))
        return
    for actor in ACTORS.get_all_level_actors():
        component = actor.get_component_by_class(unreal.StaticMeshComponent) if isinstance(actor, unreal.StaticMeshActor) else None
        mesh = component.get_editor_property("static_mesh") if component else None
        if mesh and mesh.get_name() == "SM_Template_Map_Floor":
            ACTORS.destroy_actor(actor)
    # Flat ground under everything, which the streets' buildings stand on (bOnGround traces it).
    lo_x, hi_x, lo_y, hi_y = -150.0, 330.0, -320.0, 5 * SPACING + 100.0
    ground = ACTORS.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector((lo_x + hi_x) * 50.0, (lo_y + hi_y) * 50.0, -25.0))
    ground.set_actor_label("Ground")
    ground.set_actor_scale3d(unreal.Vector(hi_x - lo_x, hi_y - lo_y, 1.0))
    component = ground.get_component_by_class(unreal.StaticMeshComponent)
    component.set_editor_property("static_mesh", unreal.load_asset("/Engine/BasicShapes/Plane"))
    component.set_material(0, ASSETS.load_asset(GROUND_MATERIAL) or unreal.load_asset("/Engine/BasicShapes/BasicShapeMaterial"))

    made = {}
    for i, (label, fabric) in enumerate(FABRICS):
        y = i * SPACING
        made[label] = street(label, kit, fabric, [(0.0, y), (LENGTH, y)])
    # E: curved, the right side only, taller at its middle point.
    y = len(FABRICS) * SPACING
    curve = street("E Curved street wall", kit, unreal.CSCityFabric.STREET_WALL, [(0.0, y), (60.0, y + 30.0), (120.0, y)], sides=unreal.CSCityStreetSides.RIGHT, curved=True)
    spline = curve.get_editor_property("street")
    spline.set_scale_at_spline_point(1, unreal.Vector(1.0, 1.0, 2.5), True)
    made["E Curved street wall"] = curve

    # F: a network road, a street lining its middle.
    network = ACTORS.spawn_actor_from_class(unreal.load_class(None, "/Script/CSRoadNet.CSRoadNetworkActor"), unreal.Vector(0.0, 0.0, 0.0))
    network.set_actor_label("F Road network")
    network.set_editor_property("local_update", True, notify_mode=NEVER)
    network.set_editor_property("tile_size", 64.0, notify_mode=NEVER)
    network.set_editor_property("batch_buildings", True, notify_mode=NEVER)
    road = ACTORS.spawn_actor_from_class(unreal.load_class(None, "/Script/CSRoadNet.CSRoadSplineActor"), unreal.Vector(0.0, 0.0, 0.0))
    road.set_actor_label("F Main road")
    road.attach_to_actor(network, "", KEEP, KEEP, KEEP, False)
    road_spline = road.get_editor_property("spline")
    road_spline.set_editor_property("prefab", "Medium Road", notify_mode=NEVER)
    road_spline.clear_spline_points(False)
    for x in (-40.0, 60.0, 160.0):
        road_spline.add_spline_point(unreal.Vector(x * 100.0, -200.0 * 100.0, 0.0), WORLD_SPACE, False)
    for i in range(3):
        road_spline.set_spline_point_type(i, unreal.SplinePointType.LINEAR, False)
    road_spline.update_spline()
    lined = ACTORS.spawn_actor_from_class(unreal.CSCityStreetActor, unreal.Vector(60.0 * 100.0, -200.0 * 100.0, 0.0))
    lined.set_actor_label("F Street on the main road")
    lined.attach_to_actor(road, "", KEEP, KEEP, KEEP, False)
    lined.set_editor_property("fabric", unreal.CSCityFabric.STREET_WALL, notify_mode=ALWAYS)
    lined.set_editor_property("kit", kit, notify_mode=NEVER)
    lined.set_editor_property("follow_road", True, notify_mode=NEVER)
    lined.set_editor_property("follow_from", 50.0, notify_mode=NEVER)
    lined.set_editor_property("follow_to", 150.0, notify_mode=ALWAYS)
    network.rebuild()
    made["F Street on the main road"] = lined

    keys = ("num_splines", "num_roads", "num_buildings", "num_streets", "streets_rebuilt", "last_error")
    stats = {k: network.get_editor_property(k) for k in keys}
    log("network " + json.dumps(stats))
    for label, actor in made.items():
        log("{}: {}".format(label, json.dumps(summary(actor), ensure_ascii=False)))
    if not LEVELS.save_current_level():
        fail("could not save " + LEVEL)
        return
    log("saved " + LEVEL)


build()
