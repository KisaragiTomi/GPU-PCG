# -*- coding: utf-8 -*-
"""Shots of L_CityDemo (MakeCityDemo.py): alleys, the avenue over them, special and batch buildings.

  UnrealEditor.exe <project> /PCGPlugins/CSRoadNet/L_CityDemo -ExecCmds="py <this file>"

Rebuilds the network (an editor in the background does not tick it) and times warm rebuilds: whole,
in tiles from scratch, in tiles with nothing changed, after moving the avenue's middle point 5 m,
and after moving the tower 10 m (both moved back after). Then captures each view until Lumen
settles. Needs the real editor (in UnrealEditor-Cmd the captures come out black). Nothing is saved.

Output: Saved/CS2CityShots, <view>.png and summary.json (with the timings).
"""
import json
import os

import unreal

OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), "CS2CityShots")
W, H = 1600, 900
# Lumen keeps history across frames: one capture per frame until it settles.
WARMUP = 48
NEVER = unreal.PropertyAccessChangeNotifyMode.NEVER
ALWAYS = unreal.PropertyAccessChangeNotifyMode.ALWAYS

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
level_actors = ACTORS.get_all_level_actors()
network = next(a for a in level_actors if a.get_class().get_name() == "CSRoadNetworkActor")
avenue = next(a for a in level_actors if a.get_actor_label() == "Avenue")
tower = next(a for a in level_actors if a.get_actor_label() == "Tower")
network.rebuild()
keys = ("num_splines", "num_roads", "num_tiles", "num_buildings", "num_triangles", "num_marking_triangles", "build_milliseconds", "last_error")
stats = {k: network.get_editor_property(k) for k in keys}
unreal.log("CS2CITYSHOT build " + json.dumps(stats))


def timed(label):
    network.rebuild()
    keys = ("build_milliseconds", "tiles_rebuilt", "roads_rebuilt", "building_tiles_rebuilt", "num_buildings", "last_error")
    result = {k: network.get_editor_property(k) for k in keys}
    unreal.log("CS2CITYSHOT time {} {}".format(label, json.dumps(result)))
    return result


def move_avenue(dy):
    spline = avenue.get_editor_property("spline")
    point = spline.get_location_at_spline_point(1, unreal.SplineCoordinateSpace.LOCAL)
    spline.set_location_at_spline_point(1, point + unreal.Vector(0.0, dy, 0.0), unreal.SplineCoordinateSpace.LOCAL, True)


def move_tower(dx):
    tower.set_actor_location(tower.get_actor_location() + unreal.Vector(dx, 0.0, 0.0), False, False)
    # Rebuilds the prism and tells the network, as the editor's move does.
    tower.set_editor_property("height", tower.get_editor_property("height"), notify_mode=ALWAYS)


timings = {}
network.set_editor_property("local_update", False, notify_mode=NEVER)
timed("whole (warming up)")
timings["whole"] = timed("whole")
network.set_editor_property("local_update", True, notify_mode=NEVER)
timings["tiles from scratch"] = timed("tiles from scratch")
timings["tiles unchanged"] = timed("tiles unchanged")
move_avenue(500.0)
timings["avenue moved 5 m"] = timed("avenue moved 5 m")
move_avenue(-500.0)
timings["avenue moved back"] = timed("avenue moved back")
move_tower(1000.0)
timings["tower moved 10 m"] = timed("tower moved 10 m")
move_tower(-1000.0)
timings["tower moved back"] = timed("tower moved back")

views = [
    ("overview", unreal.Vector(-2000.0, -36000.0, 40000.0), unreal.Vector(0.0, -1000.0, 0.0)),
    ("top", unreal.Vector(0.0, -1.0, 52000.0), unreal.Vector(0.0, 0.0, 0.0)),
    ("avenue", unreal.Vector(-7000.0, -10000.0, 5000.0), unreal.Vector(0.0, -2000.0, 0.0)),
    ("townhall", unreal.Vector(-15000.0, -6000.0, 6500.0), unreal.Vector(-6000.0, 3500.0, 800.0)),
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
        json.dump({"build": stats, "timings": timings, "shots": STATE["done"]}, f, indent=2, ensure_ascii=False)
    ACTORS.destroy_actor(capture)
    unreal.log("CS2CITYSHOT done")
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
    unreal.log("CS2CITYSHOT shot " + name)
    if STATE["view"] + 1 < len(views):
        begin(STATE["view"] + 1)
    else:
        finish()


STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
