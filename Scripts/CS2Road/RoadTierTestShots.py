# -*- coding: utf-8 -*-
"""Checks and shots of L_RoadTierTest (MakeRoadTierTest.py): the main roads moved over the small roads and back.

  UnrealEditor.exe <project> /PCGPlugins/CSRoadNet/L_RoadTierTest -ExecCmds="py <this file>" -RenderOffscreen
      "-ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False"

Waits until the network settles (no pending tile, every road tile baked into Nanite, the counts unchanged for 30
ticks), checks that every house is a GPU instance drawn by Nanite, and shoots each main road. Then, one edit at a
time: moves a main road as a drag would (UpdateSpline tells the network, which updates the tiles near it in its tick),
waits for it to settle, shoots it and from above, moves it back, waits again and checks that the roads and every
house are as they were. Needs the real editor (in UnrealEditor-Cmd the captures come out black); an editor in the
background ticks the network only with that -ini setting. Nothing is saved.

The last edit lays a main road on a small road at 14 degrees, the kind of junction that once hung the GPU: if the editor
goes down there, summary.json still holds the edits before it (it is written after each step).

Output: Saved/CS2RoadTierShots, <view>.png and summary.json. Prints CS2ROADTIERSHOT VERDICT OK or FAILED at the end.
"""
import json
import math
import os
import time
import traceback

import unreal

OUT_DIR = os.path.join(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir()), "CS2RoadTierShots")
W, H = 1600, 900
# Lumen keeps history across frames: one capture per frame until it settles.
WARMUP = 48
# Ticks the counts must stay as they are, and the longest wait for the network to settle (seconds): the first build
# bakes every tile, an edit only those near it.
STABLE_TICKS = 30
FIRST_TIMEOUT = 300.0
EDIT_TIMEOUT = 180.0
LOCAL = unreal.SplineCoordinateSpace.LOCAL

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
level_actors = ACTORS.get_all_level_actors()
network = next(a for a in level_actors if a.get_class().get_name() == "CSRoadNetworkActor")
ROADS = {a.get_actor_label(): a for a in level_actors if a.get_class().get_name() == "CSRoadSplineActor"}
KEYS = ("num_splines", "num_roads", "num_tiles", "num_nanite_tiles", "pending_tiles", "num_buildings", "num_bridge_piers", "skipped_bridge_piers", "last_error")


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
        spline.set_location_at_spline_point(i, point, LOCAL, False)
    # UCSRoadSplineComponent::UpdateSpline tells the network, as a drag in the editor does: the local update path.
    spline.update_spline()


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
STATE = {"start": time.time(), "logged": 0.0, "phase": "settle", "then": None, "settle_start": time.time(), "timeout": FIRST_TIMEOUT,
         "stable": 0, "last": None, "queue": [], "after": None, "view": None, "frames": 0, "handle": None, "edit": -1, "saved": None,
         "base_places": None, "base_roads": 0}


def write_summary():
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(os.path.join(OUT_DIR, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(SUMMARY, f, indent=2, ensure_ascii=False)


def fail(text):
    SUMMARY["failed"].append(text)
    unreal.log_error("CS2ROADTIERSHOT FAILED " + text)


def finish():
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    write_summary()
    ACTORS.destroy_actor(capture)
    unreal.log("CS2ROADTIERSHOT VERDICT {}".format("FAILED: " + "; ".join(SUMMARY["failed"]) if SUMMARY["failed"] else "OK"))
    unreal.SystemLibrary.quit_editor()


def settle(then, timeout):
    STATE.update(phase="settle", then=then, settle_start=time.time(), timeout=timeout, stable=0, last=None)


def settled(s):
    key = (s["num_roads"], s["num_buildings"], s["pending_tiles"], s["num_tiles"], s["num_nanite_tiles"])
    STATE["stable"] = STATE["stable"] + 1 if key == STATE["last"] else 0
    STATE["last"] = key
    return s["num_tiles"] > 0 and s["pending_tiles"] == 0 and s["num_nanite_tiles"] >= s["num_tiles"] and STATE["stable"] >= STABLE_TICKS


def shoot(views, after):
    STATE.update(phase="shoot", queue=list(views), after=after)
    begin_view(STATE["queue"].pop(0))


def begin_view(view):
    name, location, target = view
    capture.set_actor_location_and_rotation(location, unreal.MathLibrary.find_look_at_rotation(location, target), False, True)
    STATE["view"] = view
    STATE["frames"] = 0


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


def base():
    s = stats()
    s["settled_after_s"] = round(time.time() - STATE["start"], 1)
    SUMMARY["build"] = s
    unreal.log("CS2ROADTIERSHOT settled {}".format(json.dumps(s, ensure_ascii=False)))
    STATE["base_places"] = check_houses()
    STATE["base_roads"] = s["num_roads"]
    write_summary()
    shoot(VIEWS, next_edit)


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
    settle(moved, EDIT_TIMEOUT)


def moved():
    edit = EDITS[STATE["edit"]]
    s = stats()
    s["settled_after_s"] = round(time.time() - STATE["settle_start"], 1)
    s["houses_moved"] = len(set(house_places()) ^ set(STATE["base_places"]))
    SUMMARY["edits"].append({"name": edit["name"], "road": edit["road"], "moved": s})
    write_summary()
    unreal.log("CS2ROADTIERSHOT edit {} {}".format(edit["name"], json.dumps(s, ensure_ascii=False)))
    if s["houses_moved"] == 0:
        fail("{}: no house changed".format(edit["name"]))
    shoot([(edit["name"] + "_top", TOP[1], TOP[2]), orbit(edit["name"] + "_close", edit["look"], 200.0, 40.0, 130.0)], undo)


def undo():
    set_spline_points(EDITS[STATE["edit"]]["road"], STATE["saved"])
    settle(back, EDIT_TIMEOUT)


def back():
    edit = EDITS[STATE["edit"]]
    s = stats()
    s["settled_after_s"] = round(time.time() - STATE["settle_start"], 1)
    different = len(set(house_places()) ^ set(STATE["base_places"]))
    s["houses_different"] = different
    SUMMARY["edits"][-1]["back"] = s
    write_summary()
    unreal.log("CS2ROADTIERSHOT back {} {}".format(edit["name"], json.dumps(s, ensure_ascii=False)))
    if different or s["num_roads"] != STATE["base_roads"]:
        fail("{} back: {} houses differ, {} roads for {}".format(edit["name"], different, s["num_roads"], STATE["base_roads"]))
    next_edit()


def tick(delta):
    try:
        if STATE["phase"] == "settle":
            s = stats()
            now = time.time()
            if now - STATE["logged"] > 10.0:
                STATE["logged"] = now
                unreal.log("CS2ROADTIERSHOT waiting {:.0f} s {}".format(now - STATE["settle_start"], json.dumps(s, ensure_ascii=False)))
            if s["last_error"]:
                fail("the build failed: " + s["last_error"])
                finish()
                return
            if settled(s):
                STATE["then"]()
            elif now - STATE["settle_start"] > STATE["timeout"]:
                fail("did not settle in {:.0f} s: {}".format(STATE["timeout"], json.dumps(s, ensure_ascii=False)))
                finish()
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
        else:
            STATE["after"]()
    except Exception:
        fail(traceback.format_exc())
        finish()


unreal.log("CS2ROADTIERSHOT start " + json.dumps(stats(), ensure_ascii=False))
STATE["then"] = base
STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
