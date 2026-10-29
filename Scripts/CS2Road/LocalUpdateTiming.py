# -*- coding: utf-8 -*-
"""Times the local update (bLocalUpdate) on Palmsland converted to spline roads, a batch a frame.

  UnrealEditor.exe <project> -ExecCmds="py <this file>"
      "-ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False"

(the second line keeps actors ticking while the editor is in the background, for the last step).

Runs in the full editor so that frames go by between batches, as when editing: the upload memory
and pooled buffers a batch takes come back only after its frame. Spawns a network in the startup
level (nothing is saved), converts the Palmsland snapshot to spline roads (ConvertToSplines) and
times warm rebuilds: whole; in tiles with batch buildings from scratch, one rebuild (a batch of
MaxTilesPerBatch tiles) per frame until nothing is pending; with nothing changed; and after moving
one spline point near the middle of the network 5 m and back, three times. The network's own tick
is off, so a frame holds exactly one batch. Last, the tick is turned on and edits left to settle:
the network makes its traffic and bridge preview again for the whole network, logging the time it
took. CS2ROAD_NETWORK overrides the snapshot, CS2ROAD_TILE the tile size in metres (256),
CS2ROAD_BATCH the tiles per batch (16).

Output: CS2LOCALTIMING log lines and Saved/CS2LocalUpdateTiming_<tile>_<batch>.json.
"""
import json
import os

import unreal

NETWORK = os.environ.get("CS2ROAD_NETWORK", r"D:/MyProject/CitiesSkylines-Road2-C#/Artifacts/public-saves/palmsland-3/network.json")
TILE = float(os.environ.get("CS2ROAD_TILE", "256"))
BATCH = int(os.environ.get("CS2ROAD_BATCH", "16"))
OUT = os.path.join(unreal.Paths.project_saved_dir(), "CS2LocalUpdateTiming_{:.0f}_{}.json".format(TILE, BATCH))
NEVER = unreal.PropertyAccessChangeNotifyMode.NEVER
KEYS = ("build_milliseconds", "num_tiles", "tiles_rebuilt", "roads_rebuilt", "roads_graphed", "num_buildings", "building_tiles_rebuilt", "pending_tiles", "last_error")
PREVIEW_KEYS = ("num_traffic_lanes", "num_bridge_piers")

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
results = {"network": NETWORK, "tile_size": TILE, "max_tiles_per_batch": BATCH}
STATE = {"handle": None, "step": 0, "batches": [], "edit": None}


def stats():
    return {k: network.get_editor_property(k) for k in KEYS}


def timed(label):
    network.rebuild()
    result = stats()
    unreal.log("CS2LOCALTIMING {} {}".format(label, json.dumps(result)))
    results[label] = result
    return result


def setup():
    global network
    network = ACTORS.spawn_actor_from_class(unreal.load_class(None, "/Script/CSRoadNet.CSRoadNetworkActor"), unreal.Vector(0.0, 0.0, 0.0))
    network.set_actor_tick_enabled(False)
    network.set_editor_property("network_file", unreal.FilePath(NETWORK), notify_mode=NEVER)
    network.convert_to_splines()
    results["splines"] = network.get_editor_property("num_splines")
    results["roads"] = network.get_editor_property("num_roads")
    timed("whole (warming up)")
    timed("whole")
    network.set_editor_property("local_update", True, notify_mode=NEVER)
    network.set_editor_property("tile_size", TILE, notify_mode=NEVER)
    network.set_editor_property("max_tiles_per_batch", BATCH, notify_mode=NEVER)
    network.set_editor_property("batch_buildings", True, notify_mode=NEVER)


def edit_target():
    """The spline road whose first point is nearest the middle of all of them."""
    roads = [a for a in ACTORS.get_all_level_actors() if a.get_class().get_name() == "CSRoadSplineActor"]
    firsts = [r.get_editor_property("spline").get_location_at_spline_point(0, unreal.SplineCoordinateSpace.WORLD) for r in roads]
    middle = unreal.Vector(sum(p.x for p in firsts) / len(firsts), sum(p.y for p in firsts) / len(firsts), 0.0)
    return min(zip(roads, firsts), key=lambda pair: (pair[1].x - middle.x) ** 2 + (pair[1].y - middle.y) ** 2)[0]


def finish():
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(results, f, indent=2, ensure_ascii=False)
    unreal.log("CS2LOCALTIMING saved " + OUT)
    unreal.SystemLibrary.quit_editor()


def tick(delta):
    STATE["step"] += 1
    step = STATE["step"]
    # Let the editor settle first.
    if step < 30:
        return
    if step == 30:
        setup()
        return
    # From scratch: one batch a frame until nothing is pending.
    if "tiles from scratch" not in results:
        batch = timed("batch {}".format(len(STATE["batches"]) + 1))
        STATE["batches"].append(batch["build_milliseconds"])
        if batch["last_error"] or (batch["pending_tiles"] == 0) or len(STATE["batches"]) >= 1000:
            results["tiles from scratch"] = {"batches": len(STATE["batches"]), "total_milliseconds": sum(STATE["batches"]), "largest_batch_milliseconds": max(STATE["batches"]), "last_error": batch["last_error"]}
            unreal.log("CS2LOCALTIMING tiles from scratch " + json.dumps(results["tiles from scratch"]))
            if batch["last_error"]:
                finish()
        return
    if "tiles unchanged" not in results:
        timed("tiles unchanged")
        STATE["edit"] = edit_target()
        results["edited"] = STATE["edit"].get_actor_label()
        STATE["moves"] = [500.0, -500.0] * 3
        return
    # An edit that dirtied more than a batch finishes before the next one.
    if network.get_editor_property("pending_tiles") > 0:
        timed("draining")
        return
    if STATE["moves"]:
        dx = STATE["moves"].pop(0)
        spline = STATE["edit"].get_editor_property("spline")
        point = spline.get_location_at_spline_point(0, unreal.SplineCoordinateSpace.LOCAL)
        spline.set_location_at_spline_point(0, point + unreal.Vector(dx, 0.0, 0.0), unreal.SplineCoordinateSpace.LOCAL, True)
        timed("point moved {:+.0f} m, {} left".format(dx / 100.0, len(STATE["moves"])))
        return
    # Edits settle: 0.3 s after the last build the network's tick makes the preview again.
    if "settled" not in STATE:
        results["preview while editing"] = {k: network.get_editor_property(k) for k in PREVIEW_KEYS}
        network.set_actor_tick_enabled(True)
        STATE["settled"] = 0.0
        return
    STATE["settled"] += delta
    if STATE["settled"] < 1.5:
        return
    results["preview once settled"] = {k: network.get_editor_property(k) for k in PREVIEW_KEYS}
    unreal.log("CS2LOCALTIMING preview {}".format(json.dumps([results["preview while editing"], results["preview once settled"]])))
    finish()


STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
