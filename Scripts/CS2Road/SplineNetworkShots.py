# -*- coding: utf-8 -*-
"""Shots of roads drawn as splines (ACSRoadSplineActor) on the Palmsland terrain.

  UnrealEditor.exe <project> /PCGPlugins/CS2Roads/Maps/L_Palmsland -ExecCmds="py <this file>"

Draws a small network on open ground 900 m from the city, under an ACSRoadNetworkActor that
builds from splines and takes only its terrain from the Palmsland snapshot: a Medium Road
crossed by a Small Road, a curved Small Road ending 0.5 m short of the Medium Road (it joins
it in a T), a two-lane highway 12 m up passing over both, and a Small Road whose first
segment is 3 m long (merged into the next). Needs the real editor (in UnrealEditor-Cmd the
captures come out black). Nothing is saved.

Output: Saved/CS2RoadSplineShots, <view>.png and summary.json.
"""
import json
import os

import unreal

NETWORK = os.environ.get("CS2ROAD_NETWORK", r"D:/MyProject/CitiesSkylines-Road2-C#/Artifacts/public-saves/palmsland-3/network.json")
OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), "CS2RoadSplineShots")
W, H = 1600, 900
# Lumen keeps history across frames: one capture per frame until it settles.
WARMUP = 48
# Unity (5750, -3000): the snapshot terrain is 531.8-532.1 m within 160 m of it. Unity metres to
# UE: (x, -z, y) * 100; the network sits at the origin, where its terrain matches the level's.
# The level's terrain mesh is RoadLab's display terrain: the heightmap lowered 0.18 m, and lowered
# under the snapshot's own roads only. A road's asphalt runs below its curve, so a road drawn on
# the bare heightmap sinks under that mesh: the drawing sits 0.4 m higher, still a ground road.
CENTRE = unreal.Vector(575000.0, 300000.0, 53228.0)
NEVER = unreal.PropertyAccessChangeNotifyMode.NEVER
KEEP = unreal.AttachmentRule.KEEP_WORLD

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
network = ACTORS.spawn_actor_from_class(unreal.load_class(None, "/Script/CSRoadNet.CSRoadNetworkActor"), unreal.Vector(0, 0, 0))
network.set_actor_label("CS2 Spline Roads")
network.set_editor_property("source", unreal.CSRoadNetworkSource.SPLINES, notify_mode=NEVER)
network.set_editor_property("network_file", unreal.FilePath(NETWORK), notify_mode=NEVER)
road_class = unreal.load_class(None, "/Script/CSRoadNet.CSRoadSplineActor")
roads = []


def road(label, prefab, points, height=0.0):
    """A spline road through points given in cm from CENTRE, height over the ground."""
    actor = ACTORS.spawn_actor_from_class(road_class, CENTRE)
    actor.set_actor_label(label)
    actor.attach_to_actor(network, "", KEEP, KEEP, KEEP, False)
    spline = actor.get_editor_property("spline")
    spline.set_editor_property("prefab", prefab, notify_mode=NEVER)
    spline.clear_spline_points(False)
    for x, y in points:
        spline.add_spline_point(unreal.Vector(x, y, height), unreal.SplineCoordinateSpace.LOCAL, False)
    spline.update_spline()
    roads.append(actor)


road("Medium Road", "Medium Road", [(-15000, 0), (15000, 0)])
road("Cross Street", "Small Road", [(0, -15000), (0, 15000)])
road("Side Street", "Small Road", [(8000, 50), (8000, 6000), (12000, 12000)])
road("Highway", "Highway Twoway - 2 lanes", [(-8000, -12000), (-8000, 12000)], height=1200.0)
road("Short Segment Street", "Small Road", [(-15000, -8000), (-14700, -8000), (-2000, -8000)])
network.rebuild()
stats = {k: network.get_editor_property(k) for k in ("num_splines", "num_roads", "num_vertices", "num_triangles", "num_marking_instances", "build_milliseconds", "last_error")}
unreal.log("CS2SPLINESHOT build " + json.dumps(stats))


def at(x, y, z):
    return CENTRE + unreal.Vector(x, y, z)


views = [
    ("overview", at(-26000, -26000, 30000), at(0, 0, 0)),
    ("cross", at(-4000, -4000, 3500), at(0, 0, 0)),
    ("tee", at(5000, -3500, 3000), at(8000, 0, 0)),
    ("overpass", at(-14000, -6000, 4000), at(-8000, 0, 600)),
    ("merge", at(-17000, -12000, 3000), at(-14000, -8000, 0)),
]

rt = unreal.RenderingLibrary.create_render_target2d(world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8, unreal.LinearColor(0, 0, 0, 1), False)
capture = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, unreal.Vector(0, 0, 0))
comp = capture.get_component_by_class(unreal.SceneCaptureComponent2D)
comp.set_editor_property("texture_target", rt)
comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
comp.set_editor_property("fov_angle", 50.0)
comp.set_editor_property("capture_every_frame", False)
comp.set_editor_property("capture_on_movement", False)
# Without a view state the level's post-process exposure does not apply and Lumen never runs.
comp.set_editor_property("always_persist_rendering_state", True)

STATE = {"ticks": 0, "view": -1, "frames": 0, "handle": None, "done": []}


def begin(index):
    name, location, target = views[index]
    capture.set_actor_location_and_rotation(location, unreal.MathLibrary.find_look_at_rotation(location, target), False, True)
    STATE["view"] = index
    STATE["frames"] = 0


def finish():
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(os.path.join(OUT_DIR, "summary.json"), "w", encoding="utf-8") as f:
        json.dump({"network": NETWORK, "build": stats, "shots": STATE["done"]}, f, indent=2, ensure_ascii=False)
    for actor in [capture] + roads + [network]:
        ACTORS.destroy_actor(actor)
    unreal.log("CS2SPLINESHOT done")
    unreal.SystemLibrary.quit_editor()


def tick(delta):
    STATE["ticks"] += 1
    # Let streaming, the proxies and the first Lumen frames settle before the first shot.
    if STATE["ticks"] < 30:
        return
    if STATE["view"] < 0:
        begin(0)
    comp.capture_scene()
    STATE["frames"] += 1
    if STATE["frames"] < WARMUP:
        return
    name = views[STATE["view"]][0]
    unreal.RenderingLibrary.export_render_target(world, rt, OUT_DIR, name + ".png")
    STATE["done"].append(name)
    unreal.log("CS2SPLINESHOT shot " + name)
    if STATE["view"] + 1 < len(views):
        begin(STATE["view"] + 1)
    else:
        finish()


STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
