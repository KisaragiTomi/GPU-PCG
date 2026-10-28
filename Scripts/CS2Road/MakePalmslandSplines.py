# -*- coding: utf-8 -*-
"""Creates /PCGPlugins/CSRoadNet/L_PalmslandSplines: the Palmsland snapshot as spline roads, kept in the level.

  UnrealEditor-Cmd.exe <project> -ExecutePythonScript=<this file> -unattended -RenderOffscreen

ConvertToSplines turns the snapshot's edges into ACSRoadSplineActors hanging under one
ACSRoadNetworkActor, with every node's and edge's data on the spline points (node offsets and
prefabs, elevations, upgrades, outside connections, whether something owns them). The level keeps
those splines and the network builds from them (Source = Splines); the snapshot stays as NetworkFile
for its terrain alone, which the road vertices follow and new roads measure their elevation from.
The network updates in 256 m tiles (bLocalUpdate), so an edit rewrites only the tiles around it, and
the first build after loading writes 16 tiles a frame. The level starts from the engine's Basic
template (sun, sky, fog) without its floor: Palmsland's ground is the snapshot's terrain.
CS2ROAD_NETWORK overrides the snapshot. Refuses to overwrite the level; delete it first to make it
again.
"""
import os

import unreal

LEVEL = "/PCGPlugins/CSRoadNet/L_PalmslandSplines"
TEMPLATE = "/Engine/Maps/Templates/Template_Default"
NETWORK = os.environ.get("CS2ROAD_NETWORK", r"D:/MyProject/CitiesSkylines-Road2-C#/Artifacts/public-saves/palmsland-3/network.json")
NEVER = unreal.PropertyAccessChangeNotifyMode.NEVER
STATS = ("num_splines", "num_roads", "num_vertices", "num_triangles", "num_marking_triangles", "build_milliseconds", "last_error")

ASSETS = unreal.EditorAssetLibrary
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)


def log(text):
    unreal.log("CS2PALMSLAND " + text)


def build():
    if ASSETS.does_asset_exist(LEVEL):
        unreal.log_error("CS2PALMSLAND {} exists; delete it to make it again".format(LEVEL))
        return
    if not os.path.isfile(NETWORK):
        unreal.log_error("CS2PALMSLAND no snapshot at " + NETWORK)
        return
    if not LEVELS.new_level_from_template(LEVEL, TEMPLATE):
        unreal.log_error("CS2PALMSLAND could not create {} from {}".format(LEVEL, TEMPLATE))
        return

    # The template's 10 m floor would cut through the roads: Palmsland's ground is the snapshot's terrain.
    for actor in ACTORS.get_all_level_actors():
        component = actor.get_component_by_class(unreal.StaticMeshComponent) if isinstance(actor, unreal.StaticMeshActor) else None
        mesh = component.get_editor_property("static_mesh") if component else None
        if mesh and mesh.get_name() == "SM_Template_Map_Floor":
            ACTORS.destroy_actor(actor)

    network = ACTORS.spawn_actor_from_class(unreal.load_class(None, "/Script/CSRoadNet.CSRoadNetworkActor"), unreal.Vector(0.0, 0.0, 0.0))
    network.set_actor_label("Palmsland")
    network.set_editor_property("network_file", unreal.FilePath(NETWORK), notify_mode=NEVER)
    network.set_editor_property("source", unreal.CSRoadNetworkSource.NETWORK_FILE, notify_mode=NEVER)
    # In tiles before converting: the rebuild that ends the conversion then writes one batch, where the
    # whole network's meshes at once (some 15 million road and 10 million marking vertices) can find too
    # little video memory.
    network.set_editor_property("local_update", True, notify_mode=NEVER)
    network.set_editor_property("tile_size", 256.0, notify_mode=NEVER)
    network.set_editor_property("max_tiles_per_batch", 16, notify_mode=NEVER)
    network.convert_to_splines()
    stats = {k: network.get_editor_property(k) for k in STATS}
    stats["pending_tiles"] = network.get_editor_property("pending_tiles")
    log("first batch " + str(stats))
    roads = [a for a in ACTORS.get_all_level_actors() if a.get_class().get_name() == "CSRoadSplineActor" and a.get_attach_parent_actor() == network]
    log("spline roads under the network: {}".format(len(roads)))
    if stats["last_error"] or not roads or network.get_editor_property("source") != unreal.CSRoadNetworkSource.SPLINES:
        unreal.log_error("CS2PALMSLAND the conversion failed; nothing saved")
        return
    if not LEVELS.save_current_level():
        unreal.log_error("CS2PALMSLAND could not save " + LEVEL)
        return
    log("saved " + LEVEL)


build()
