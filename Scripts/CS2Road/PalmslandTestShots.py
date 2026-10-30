# -*- coding: utf-8 -*-
"""Shots of the Palmsland test level (MakePalmslandTestLevel.py): viaducts and their piers, bridges,
the alley district with its avenue, boulevard, special and batch buildings, houses in the city.

  UnrealEditor.exe <project> /PCGPlugins/CSRoadNet/L_PalmslandSplines -ExecCmds="py <this file>" -RenderOffscreen
      "-ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False"

Waits until the network has written every road and building tile and baked the road tiles into Nanite
(an editor in the background ticks it only with that -ini setting), then captures each view until
Lumen settles. Needs the real editor (in UnrealEditor-Cmd the captures come out black). Nothing is
saved. Output: Saved/CS2PalmslandShots, <view>.png and summary.json.
"""
import json
import math
import os
import time

import unreal

OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), "CS2PalmslandShots")
W, H = 1600, 900
# Lumen keeps history across frames: one capture per frame until it settles.
WARMUP = 48
# Longest wait for the tiles (seconds).
TILES_TIMEOUT = 420.0

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
network = next(a for a in ACTORS.get_all_level_actors() if a.get_class().get_name() == "CSRoadNetworkActor")
KEYS = ("num_splines", "num_roads", "num_tiles", "num_nanite_tiles", "pending_tiles", "num_buildings", "num_bridge_piers", "skipped_bridge_piers", "num_marking_instances", "last_error")


def stats():
    return {k: network.get_editor_property(k) for k in KEYS}


def ue(x, z, y):
    """Unity metres (y up) to UE cm; the network sits at the origin."""
    return unreal.Vector(x * 100.0, -z * 100.0, y * 100.0)


def orbit(name, focus, azimuth, elevation, distance):
    """A view of focus (Unity x, y, z) from distance metres away, azimuth degrees from south (towards east), elevation degrees up."""
    a, e = math.radians(azimuth), math.radians(elevation)
    x, y, z = focus
    camera = (x + distance * math.cos(e) * math.sin(a), y + distance * math.sin(e), z - distance * math.cos(e) * math.cos(a))
    return (name, ue(camera[0], camera[2], camera[1]), ue(x, z, y))


views = [
    orbit("overview", (500.0, 545.0, 1200.0), 200.0, 42.0, 11000.0),
    orbit("interchange_1", (2481.9, 560.3, 584.8), 210.0, 32.0, 520.0),
    orbit("interchange_2", (1170.6, 570.5, -644.5), 150.0, 30.0, 520.0),
    orbit("interchange_3", (1113.8, 546.6, 2703.3), 30.0, 30.0, 520.0),
    orbit("tied_arch_bridge", (-1219.4, 565.0, 488.2), 60.0, 18.0, 260.0),
    orbit("suspension_bridge", (4398.8, 552.9, -941.4), 200.0, 20.0, 700.0),
    orbit("district", (2900.0, 523.3, -3450.0), 200.0, 45.0, 1150.0),
    ("district_street", ue(2884.0, -3120.0, 541.0), ue(2950.0, -3650.0, 523.3)),
    orbit("boulevard_end", (3350.0, 523.3, -3420.0), 180.0, 35.0, 190.0),
    orbit("city_east", (3150.0, 545.0, -750.0), 160.0, 35.0, 600.0),
    orbit("city_west", (-450.0, 554.8, 150.0), 40.0, 35.0, 600.0),
]


def pier_view():
    """Close to the bridge pier nearest the first interchange whose column is 8 m or taller."""
    piers = network.get_content().get_editor_property("bridge_piers")
    count = piers.get_instance_count() if piers else 0
    focus = ue(2481.9, 584.8, 560.3)
    best = None
    # Each pier is a column, a cap and a footing; a column's scale is its size in metres, its place its middle.
    for index in range(0, count, 3):
        result = piers.get_instance_transform(index, True)
        column = result[1] if isinstance(result, tuple) else result
        if not column or column.scale3d.z < 8.0:
            continue
        distance = (column.translation - focus).length()
        if best is None or distance < best[0]:
            best = (distance, column)
    if not best:
        return None
    middle = best[1].translation
    camera = middle + unreal.Vector(5000.0, -4000.0, 1000.0 - best[1].scale3d.z * 30.0)
    return ("pier_close", camera, middle)


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

STATE = {"ticks": 0, "start": time.time(), "logged": 0.0, "ready": False, "view": -1, "frames": 0, "handle": None, "done": [], "waited": 0.0}


def begin(index):
    name, location, target = views[index]
    capture.set_actor_location_and_rotation(location, unreal.MathLibrary.find_look_at_rotation(location, target), False, True)
    STATE["view"] = index
    STATE["frames"] = 0


def finish():
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(os.path.join(OUT_DIR, "summary.json"), "w", encoding="utf-8") as f:
        json.dump({"build": stats(), "waited_for_tiles": STATE["waited"], "shots": STATE["done"]}, f, indent=2, ensure_ascii=False)
    ACTORS.destroy_actor(capture)
    unreal.log("CS2PALMSHOT done")
    unreal.SystemLibrary.quit_editor()


def tick(delta):
    STATE["ticks"] += 1
    now = time.time() - STATE["start"]
    if not STATE["ready"]:
        s = stats()
        if now - STATE["logged"] > 10.0:
            STATE["logged"] = now
            unreal.log("CS2PALMSHOT waiting {:.0f} s {}".format(now, json.dumps(s)))
        settled = s["pending_tiles"] == 0 and s["num_tiles"] > 0 and s["num_nanite_tiles"] >= s["num_tiles"]
        if not settled and now < TILES_TIMEOUT:
            return
        STATE["ready"] = True
        STATE["waited"] = now
        unreal.log("CS2PALMSHOT tiles {} after {:.0f} s {}".format("settled" if settled else "timed out", now, json.dumps(s)))
        pier = pier_view()
        if pier:
            views.insert(4, pier)
        begin(0)
        return
    comp.capture_scene()
    STATE["frames"] += 1
    if STATE["frames"] < WARMUP:
        return
    name = views[STATE["view"]][0]
    unreal.RenderingLibrary.export_render_target(world, rt, OUT_DIR, name + ".png")
    STATE["done"].append(name)
    unreal.log("CS2PALMSHOT shot " + name)
    if STATE["view"] + 1 < len(views):
        begin(STATE["view"] + 1)
    else:
        finish()


unreal.log("CS2PALMSHOT start " + json.dumps(stats()))
STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
