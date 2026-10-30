# -*- coding: utf-8 -*-
"""Turns /PCGPlugins/CSRoadNet/L_PalmslandSplines into the general test level of the spline road
network: viaducts and bridges, main roads, alleys and houses on Palmsland's terrain.

  UnrealEditor-Cmd.exe <project> -ExecutePythonScript=<this file> -unattended -RenderOffscreen

Adds to the level MakePalmslandSplines.py made what it lacks, leaving its splines (and any edit to
them) as they are; run again, it adds nothing:
- the ground: the snapshot's terrain as a Landscape (UCSRoadNetTools::ConvertTerrainToLandscape),
  half a metre below the heightmap the roads measure their elevation from, shaped to the ground roads by the
  network's "CS2 Roads" edit layer (bShapeTerrain; made the first time the level ticks in an editor);
- bridge piers under the elevated roads and batch buildings along the roads CS2 zones;
- the snapshot's small roads (alleys too) in the small-road tier: a main road moved or drawn over one
  cuts it and its ends join that road; the junctions the snapshot drew stay as they are;
- on the flat land north-east of the city (523.3 m, inside straight Palmsland roads): two special
  buildings reserving their plots, then a district of generated alleys turned to the roads around it
  and reaching over the western one (a snapshot alley, so a small road too), cut back to it for good
  when generated and joining it; an avenue from the northern road to the city's edge and a boulevard
  from the western road ending inside the district, main roads the alleys give way to while they lie
  over them.
Then rebuilds the network and saves the level. CS2ROAD_NETWORK overrides the snapshot (only for the
nodes the new roads start from).
"""
import json
import math
import os

import unreal

LEVEL = "/PCGPlugins/CSRoadNet/L_PalmslandSplines"
NETWORK = os.environ.get("CS2ROAD_NETWORK", r"D:/MyProject/CitiesSkylines-Road2-C#/Artifacts/public-saves/palmsland-3/network.json")
NEVER = unreal.PropertyAccessChangeNotifyMode.NEVER
KEEP = unreal.AttachmentRule.KEEP_WORLD

# The flat land's level (Unity metres): ground roads are drawn on it, the landscape lies 0.5 m below.
FLAT = 523.26
# The roads around the flat land run 9.08 degrees off the Unity x and z axes.
TURN = math.atan2(216.0, 1352.0)
DISTRICT = (2900.0, -3500.0)
# Snapshot nodes the new main roads start from: on the northern, the city-edge and the western road.
NORTH, SOUTH, WEST = 436176, 2404219, 2404915
# The snapshot's small roads (by prefab name): the small-road tier.
SMALL = ("Small Road", "Alley", "Gravel Road", "Pedestrian Street")

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)


def log(text):
    unreal.log("CS2TESTLEVEL " + text)


def ue(x, z, y):
    """Unity metres (y up) to the network's (and the level's) cm."""
    return unreal.Vector(x * 100.0, -z * 100.0, y * 100.0)


def along(du, dv):
    """A point of the district, du and dv metres along its two directions from its centre."""
    c, s = math.cos(TURN), math.sin(TURN)
    return DISTRICT[0] + du * c - dv * s, DISTRICT[1] + du * s + dv * c


def labelled(label):
    return next((a for a in ACTORS.get_all_level_actors() if a.get_actor_label() == label), None)


def road(network, label, prefab, points):
    """A main road through Unity points (x, z, y), under the network."""
    actor = ACTORS.spawn_actor_from_class(unreal.load_class(None, "/Script/CSRoadNet.CSRoadSplineActor"), ue(*points[0]))
    actor.set_actor_label(label)
    actor.attach_to_actor(network, "", KEEP, KEEP, KEEP, False)
    spline = actor.get_editor_property("spline")
    spline.set_editor_property("prefab", prefab, notify_mode=NEVER)
    spline.clear_spline_points(False)
    for point in points:
        spline.add_spline_point(ue(*point), unreal.SplineCoordinateSpace.WORLD, False)
    spline.update_spline()
    return actor


def building(label, x, z, size, yaw, height, setback, colour):
    """A special building on the landscape: a rectangular outline (metres) turned by yaw (degrees, UE), pushed up height (m)."""
    actor = ACTORS.spawn_actor_from_class(unreal.load_class(None, "/Script/CSRoadNet.CSCityBuildingActor"), ue(x, z, FLAT - 0.5), unreal.Rotator(0.0, 0.0, yaw))
    actor.set_actor_label(label)
    actor.set_editor_property("height", height, notify_mode=NEVER)
    actor.set_editor_property("setback", setback, notify_mode=NEVER)
    actor.set_editor_property("color", unreal.LinearColor(*colour), notify_mode=NEVER)
    outline = actor.get_editor_property("outline")
    outline.clear_spline_points(False)
    half_x, half_y = size[0] * 50.0, size[1] * 50.0
    for px, py in ((-half_x, -half_y), (half_x, -half_y), (half_x, half_y), (-half_x, half_y)):
        outline.add_spline_point(unreal.Vector(px, py, 0.0), unreal.SplineCoordinateSpace.LOCAL, False)
        outline.set_spline_point_type(outline.get_number_of_spline_points() - 1, unreal.SplinePointType.LINEAR, False)
    outline.set_closed_loop(True, False)
    outline.update_spline()
    return actor


def small_roads_tier(network):
    """The network's small roads in the small-road tier; how many were not yet."""
    tiered = 0
    for actor in ACTORS.get_all_level_actors():
        if actor.get_class().get_name() != "CSRoadSplineActor" or actor.get_attach_parent_actor() != network:
            continue
        spline = actor.get_editor_property("spline")
        if spline.get_editor_property("prefab").startswith(SMALL) and spline.get_editor_property("tier") != unreal.CSRoadTier.ALLEY:
            spline.set_editor_property("tier", unreal.CSRoadTier.ALLEY, notify_mode=NEVER)
            tiered += 1
    return tiered


def build():
    snapshot = json.load(open(NETWORK, encoding="utf-8"))
    nodes = {n["id"]: n["position"] for n in snapshot["nodes"]}
    if not LEVELS.load_level(LEVEL):
        unreal.log_error("CS2TESTLEVEL could not open " + LEVEL)
        return
    network = next((a for a in ACTORS.get_all_level_actors() if a.get_class().get_name() == "CSRoadNetworkActor"), None)
    if not network:
        unreal.log_error("CS2TESTLEVEL no road network in " + LEVEL)
        return

    if not any(isinstance(a, unreal.LandscapeProxy) for a in ACTORS.get_all_level_actors()):
        unreal.CSRoadNetTools.convert_terrain_to_landscape(network, None)
        if network.get_editor_property("last_error"):
            unreal.log_error("CS2TESTLEVEL no landscape: " + network.get_editor_property("last_error"))
            return
        log("ground: the snapshot's terrain as a landscape")

    network.set_editor_property("batch_buildings", True, notify_mode=NEVER)
    network.set_editor_property("build_bridge_piers", True, notify_mode=NEVER)
    # The landscape follows the ground roads through the network's "CS2 Roads" edit layer. The layer is made and
    # merged when the network first builds its whole preview, which takes ticks: in an editor, not in this script.
    network.set_editor_property("shape_terrain", True, notify_mode=NEVER)
    # Before the district: the alleys it lays out are cut back to the small roads already there.
    log("small roads put in the small-road tier: {}".format(small_roads_tier(network)))

    if not labelled("North-east District"):
        # 1. Special buildings first: their plots are reserved before the alleys are laid out.
        yaw = -math.degrees(TURN)
        building("Town Hall", *along(250.0, -200.0), (60.0, 40.0), yaw, 22.0, 8.0, (0.80, 0.52, 0.38, 1.0))
        building("Tower", *along(-200.0, 200.0), (24.0, 24.0), yaw + 30.0, 60.0, 6.0, (0.45, 0.55, 0.70, 1.0))
        # 2. The alleys, laid out once, turned to the roads around them; the western road crosses the
        # district's west end and cuts them.
        region = ACTORS.spawn_actor_from_class(unreal.load_class(None, "/Script/CSRoadNet.CSCityAlleyRegion"), ue(DISTRICT[0], DISTRICT[1], FLAT), unreal.Rotator(0.0, 0.0, yaw))
        region.set_actor_label("North-east District")
        region.get_editor_property("bounds").set_box_extent(unreal.Vector(45000.0, 35000.0, 1000.0), True)
        region.generate()
        log("alleys: {}".format(len(region.get_editor_property("generated"))))
        # 3. Main roads over them: an avenue from the northern road to the city's edge, and a boulevard from
        # the western road ending inside the district.
        north, south, west = nodes[NORTH], nodes[SOUTH], nodes[WEST]
        road(network, "Avenue", "Medium Road Divided", [(north[0], north[2], north[1]), (2950.0, -3600.0, FLAT), (2880.0, -3150.0, FLAT), (south[0], south[2], south[1])])
        road(network, "Boulevard", "Medium Road", [(west[0], west[2], west[1]), (2950.0, -3480.0, FLAT), (3350.0, -3420.0, FLAT)])

    # 4. Roads, piers and batch buildings.
    network.rebuild()
    keys = ("num_splines", "num_roads", "num_tiles", "pending_tiles", "num_buildings", "num_bridge_piers", "skipped_bridge_piers", "last_error")
    log("build " + str({k: network.get_editor_property(k) for k in keys}))
    if network.get_editor_property("last_error"):
        unreal.log_error("CS2TESTLEVEL the build failed; nothing saved")
        return
    if not LEVELS.save_current_level():
        unreal.log_error("CS2TESTLEVEL could not save " + LEVEL)
        return
    log("saved " + LEVEL)


build()
