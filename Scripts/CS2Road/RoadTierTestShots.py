# -*- coding: utf-8 -*-
"""Checks and shots of L_RoadTierTest (MakeRoadTierTest.py): the main roads moved over the small roads and back.

  UnrealEditor.exe <project> /PCGPlugins/CSRoadNet/L_RoadTierTest -ExecCmds="py <this file>" -RenderOffscreen
      "-ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False"

Waits until the network has written every tile and baked the road tiles into Nanite, checks that every house is a GPU
instance drawn by Nanite, and shoots each main road. Then, one edit at a time: moves a main road, rebuilds, shoots it
and from above, moves it back, rebuilds and checks that the roads and every house are as they were. Needs the real
editor (in UnrealEditor-Cmd the captures come out black). Nothing is saved.

The last edit lays a main road on a small road at 14 degrees, the kind of junction that once hung the GPU: if the editor
goes down there, summary.json still holds the edits before it (it is written after each).

Output: Saved/CS2RoadTierShots, <view>.png and summary.json. Prints CS2ROADTIERSHOT VERDICT OK or FAILED at the end.
"""
import json
import math
import os
import time

import unreal

OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), "CS2RoadTierShots")
W, H = 1600, 900
# Lumen keeps history across frames: one capture per frame until it settles.
WARMUP = 48
# Longest wait for the tiles (seconds).
TILES_TIMEOUT = 240.0
LOCAL = unreal.SplineCoordinateSpace.LOCAL

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
level_actors = ACTORS.get_all_level_actors()
network = next(a for a in level_actors if a.get_class().get_name() == "CSRoadNetworkActor")
ROADS = {a.get_actor_label(): a for a in level_actors if a.get_class().get_name() == "CSRoadSplineActor"}
KEYS = ("num_splines", "num_roads", "num_tiles", "num_nanite_tiles", "pending_tiles", "num_buildings", "num_bridge_piers", "tiles_rebuilt", "building_tiles_rebuilt", "build_milliseconds", "last_error")


def stats():
    return {k: network.get_editor_property(k) for k in KEYS}


def metres(x, y, z=0.0):
    return unreal.Vector(x * 100.0, y * 100.0, z * 100.0)


def orbit(name, focus, azimuth, elevation, distance):
    """A view of focus (m) from distance metres away, azimuth degrees from -Y towards +X, elevation degrees up."""
    a, e = math.radians(azimuth), math.radians(elevation)
    x, y = focus
    return (name, metres(x + distance * math.cos(e) * math.sin(a), y - distance * math.cos(e) * math.cos(a), distance * math.sin(e)), metres(x, y))


TOP = ("top", metres(10.0, -0.1, 760.0), metres(10.0, 0.0))
VIEWS = [
    TOP,
    orbit("a_crossing", (-200.0, -40.0), 150.0, 35.0, 170.0),
    orbit("b_slant_end", (60.0, -30.0), 150.0, 40.0, 130.0),
    orbit("c_drawn", (-300.0, 40.0), 220.0, 35.0, 150.0),
    orbit("d_along", (203.0, -40.0), 210.0, 40.0, 150.0),
    orbit("e_over", (260.0, 40.0), 240.0, 25.0, 130.0),
    ("street", metres(-40.0, 40.0, 3.0), metres(-200.0, 40.0, 6.0)),
]

# name, road, new points (m, in the network's space) or an offset to all of them, where to look.
EDITS = [
    {"name": "f_across", "road": "F Drag me", "points": [(50.0, 195.0), (50.0, 140.0), (50.0, 70.0)], "look": (50.0, 110.0)},
    {"name": "f_along", "road": "F Drag me", "points": [(-70.0, 123.0), (0.0, 123.0), (70.0, 123.0)], "look": (0.0, 123.0)},
    {"name": "a_gone", "road": "A Crossing", "offset": (0.0, 2000.0), "look": (-200.0, 0.0)},
    {"name": "c_west", "road": "C Drawn", "offset": (-40.0, 0.0), "look": (-310.0, 40.0)},
    {"name": "c_east", "road": "C Drawn", "offset": (20.0, 0.0), "look": (-280.0, 40.0)},
    {"name": "d_gone", "road": "D Along", "offset": (500.0, 0.0), "look": (200.0, -40.0)},
    {"name": "d_wedge", "road": "D Along", "points": [(210.0, -80.0), (190.0, 0.0)], "look": (200.0, -40.0)},
]


def components():
    return network.get_content().get_components_by_class(unreal.CSGpuInstancedMeshComponent)


def house_places():
    """Every house's place, rounded to a tenth of a millimetre, sorted: what the layout came out as, whichever component draws it."""
    out = []
    for component in components():
        for index in range(component.get_instance_count()):
            result = component.get_instance_transform(index, True)
            transform = result[1] if isinstance(result, tuple) else result
            t = transform.translation
            out.append((round(t.x, 2), round(t.y, 2), round(t.z, 2)))
    return sorted(out)


def spline_points(road):
    spline = ROADS[road].get_editor_property("spline")
    return [spline.get_location_at_spline_point(i, LOCAL) for i in range(spline.get_number_of_spline_points())]


def set_spline_points(road, points):
    spline = ROADS[road].get_editor_property("spline")
    for i, point in enumerate(points):
        spline.set_location_at_spline_point(i, point, LOCAL, i + 1 == len(points))


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

SUMMARY = {"build": None, "houses": None, "edits": [], "failed": []}
STATE = {"start": time.time(), "logged": 0.0, "queue": [], "view": None, "frames": 0, "handle": None, "phase": "tiles", "edit": -1, "saved": None, "base_places": None, "base_roads": 0}


def write_summary():
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(os.path.join(OUT_DIR, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(SUMMARY, f, indent=2, ensure_ascii=False)


def fail(text):
    SUMMARY["failed"].append(text)
    unreal.log_error("CS2ROADTIERSHOT FAILED " + text)


def check_houses():
    drawn = components()
    undrawable = sorted({c.get_material_undrawable_reason() for c in drawn} - {""})
    places = house_places()
    SUMMARY["houses"] = {"components": len(drawn), "instances": len(places), "not_nanite": sum(0 if c.is_nanite_render_path() else 1 for c in drawn), "undrawable": undrawable}
    unreal.log("CS2ROADTIERSHOT houses " + json.dumps(SUMMARY["houses"], ensure_ascii=False))
    if not drawn or SUMMARY["houses"]["not_nanite"]:
        fail("houses: {} components, {} not drawn by Nanite".format(len(drawn), SUMMARY["houses"]["not_nanite"]))
    if undrawable:
        fail("houses: materials that cannot draw: " + "; ".join(undrawable))
    if len(places) != network.get_editor_property("num_buildings"):
        fail("houses: {} instances for {} buildings".format(len(places), network.get_editor_property("num_buildings")))
    return places


def begin_view(view):
    name, location, target = view
    capture.set_actor_location_and_rotation(location, unreal.MathLibrary.find_look_at_rotation(location, target), False, True)
    STATE["view"] = view
    STATE["frames"] = 0


def next_edit():
    STATE["edit"] += 1
    if STATE["edit"] >= len(EDITS):
        finish()
        return
    edit = EDITS[STATE["edit"]]
    STATE["saved"] = spline_points(edit["road"])
    if "points" in edit:
        points = [metres(x, y) for x, y in edit["points"]]
    else:
        points = [p + metres(*edit["offset"]) for p in STATE["saved"]]
    set_spline_points(edit["road"], points)
    network.rebuild()
    moved = stats()
    moved["houses_moved"] = len(set(house_places()) ^ set(STATE["base_places"]))
    SUMMARY["edits"].append({"name": edit["name"], "road": edit["road"], "moved": moved})
    write_summary()
    unreal.log("CS2ROADTIERSHOT edit {} {}".format(edit["name"], json.dumps(moved, ensure_ascii=False)))
    if moved["last_error"]:
        fail("{}: {}".format(edit["name"], moved["last_error"]))
    if moved["houses_moved"] == 0:
        fail("{}: no house changed".format(edit["name"]))
    STATE["queue"] = [(edit["name"] + "_top", TOP[1], TOP[2]), orbit(edit["name"] + "_close", edit["look"], 200.0, 40.0, 130.0)]
    STATE["phase"] = "edit"
    begin_view(STATE["queue"].pop(0))


def undo_edit():
    edit = EDITS[STATE["edit"]]
    set_spline_points(edit["road"], STATE["saved"])
    network.rebuild()
    back = stats()
    places = house_places()
    different = len(set(places) ^ set(STATE["base_places"]))
    back["houses_different"] = different
    SUMMARY["edits"][-1]["back"] = back
    write_summary()
    unreal.log("CS2ROADTIERSHOT back {} {}".format(edit["name"], json.dumps(back, ensure_ascii=False)))
    if back["last_error"]:
        fail("{} back: {}".format(edit["name"], back["last_error"]))
    if different or back["num_roads"] != STATE["base_roads"]:
        fail("{} back: {} houses differ, {} roads for {}".format(edit["name"], different, back["num_roads"], STATE["base_roads"]))
    next_edit()


def finish():
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    write_summary()
    ACTORS.destroy_actor(capture)
    unreal.log("CS2ROADTIERSHOT VERDICT {}".format("FAILED: " + "; ".join(SUMMARY["failed"]) if SUMMARY["failed"] else "OK"))
    unreal.SystemLibrary.quit_editor()


def tick(delta):
    now = time.time() - STATE["start"]
    if STATE["phase"] == "tiles":
        s = stats()
        if now - STATE["logged"] > 10.0:
            STATE["logged"] = now
            unreal.log("CS2ROADTIERSHOT waiting {:.0f} s {}".format(now, json.dumps(s, ensure_ascii=False)))
        settled = s["pending_tiles"] == 0 and s["num_tiles"] > 0 and s["num_nanite_tiles"] >= s["num_tiles"]
        if not settled and now < TILES_TIMEOUT and not s["last_error"]:
            return
        SUMMARY["build"] = s
        unreal.log("CS2ROADTIERSHOT tiles {} after {:.0f} s {}".format("settled" if settled else "NOT settled", now, json.dumps(s, ensure_ascii=False)))
        if not settled:
            fail("the tiles did not settle: {}".format(json.dumps(s, ensure_ascii=False)))
            finish()
            return
        STATE["base_places"] = check_houses()
        STATE["base_roads"] = s["num_roads"]
        STATE["queue"] = list(VIEWS)
        STATE["phase"] = "views"
        begin_view(STATE["queue"].pop(0))
        return
    comp.capture_scene()
    STATE["frames"] += 1
    if STATE["frames"] < WARMUP:
        return
    name = STATE["view"][0]
    unreal.RenderingLibrary.export_render_target(world, rt, OUT_DIR, name + ".png")
    unreal.log("CS2ROADTIERSHOT shot " + name)
    if STATE["queue"]:
        begin_view(STATE["queue"].pop(0))
    elif STATE["phase"] == "views":
        next_edit()
    else:
        undo_edit()


unreal.log("CS2ROADTIERSHOT start " + json.dumps(stats(), ensure_ascii=False))
STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
