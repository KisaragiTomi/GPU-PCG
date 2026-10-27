# -*- coding: utf-8 -*-
"""Times the local update (bLocalUpdate) on Palmsland converted to spline roads.

  UnrealEditor-Cmd.exe <project> -ExecutePythonScript=<this file> -unattended -RenderOffscreen

Spawns a network in the startup level (nothing is saved), converts the Palmsland snapshot to spline
roads (ConvertToSplines) and times warm rebuilds: whole; in tiles with batch buildings from scratch;
with nothing changed; and after moving one spline point near the middle of the network 5 m and back,
three times. CS2ROAD_NETWORK overrides the snapshot, CS2ROAD_TILE the tile size in metres (1024).

Building every tile in one call holds each tile's uploads and RDG buffers until the frame ends, and
the composition meshes are uploaded once per tile using them: with small tiles on a crowded GPU the
first tiled build can run out of video memory.

Output: CS2LOCALTIMING log lines and Saved/CS2LocalUpdateTiming_<tile>.json.
"""
import json
import os

import unreal

NETWORK = os.environ.get("CS2ROAD_NETWORK", r"D:/MyProject/CitiesSkylines-Road2-C#/Artifacts/public-saves/palmsland-3/network.json")
TILE = float(os.environ.get("CS2ROAD_TILE", "1024"))
OUT = os.path.join(unreal.Paths.project_saved_dir(), "CS2LocalUpdateTiming_{:.0f}.json".format(TILE))
NEVER = unreal.PropertyAccessChangeNotifyMode.NEVER

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
network = ACTORS.spawn_actor_from_class(unreal.load_class(None, "/Script/CSRoadNet.CSRoadNetworkActor"), unreal.Vector(0.0, 0.0, 0.0))
network.set_editor_property("network_file", unreal.FilePath(NETWORK), notify_mode=NEVER)
network.set_editor_property("source", unreal.CSRoadNetworkSource.NETWORK_FILE, notify_mode=NEVER)
network.convert_to_splines()
results = {"network": NETWORK, "tile_size": TILE, "splines": network.get_editor_property("num_splines"), "roads": network.get_editor_property("num_roads")}


def timed(label):
    network.rebuild()
    keys = ("build_milliseconds", "num_tiles", "tiles_rebuilt", "roads_rebuilt", "num_buildings", "building_tiles_rebuilt", "last_error")
    result = {k: network.get_editor_property(k) for k in keys}
    unreal.log("CS2LOCALTIMING {} {}".format(label, json.dumps(result)))
    results[label] = result


timed("whole (warming up)")
timed("whole")
network.set_editor_property("local_update", True, notify_mode=NEVER)
network.set_editor_property("tile_size", TILE, notify_mode=NEVER)
network.set_editor_property("batch_buildings", True, notify_mode=NEVER)
timed("tiles from scratch")
timed("tiles unchanged")

# The spline road whose first point is nearest the middle of all of them.
roads = [a for a in ACTORS.get_all_level_actors() if a.get_class().get_name() == "CSRoadSplineActor"]
firsts = [r.get_editor_property("spline").get_location_at_spline_point(0, unreal.SplineCoordinateSpace.WORLD) for r in roads]
middle = unreal.Vector(sum(p.x for p in firsts) / len(firsts), sum(p.y for p in firsts) / len(firsts), 0.0)
road = min(zip(roads, firsts), key=lambda pair: (pair[1].x - middle.x) ** 2 + (pair[1].y - middle.y) ** 2)[0]
spline = road.get_editor_property("spline")
results["edited"] = road.get_actor_label()
for round_index in range(3):
    for dx in (500.0, -500.0):
        point = spline.get_location_at_spline_point(0, unreal.SplineCoordinateSpace.LOCAL)
        spline.set_location_at_spline_point(0, point + unreal.Vector(dx, 0.0, 0.0), unreal.SplineCoordinateSpace.LOCAL, True)
        timed("point moved {:+.0f} m, round {}".format(dx / 100.0, round_index + 1))

with open(OUT, "w", encoding="utf-8") as f:
    json.dump(results, f, indent=2, ensure_ascii=False)
unreal.log("CS2LOCALTIMING saved " + OUT)
