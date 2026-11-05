# -*- coding: utf-8 -*-
"""Shots of L_CityStreets (MakeCityStreetDemo.py): each Night City fabric's street from the street and from above, the
curved street wall, and the street lining a stretch of a network road beside the network's batch buildings.

  UnrealEditor.exe <project> /PCGPlugins/CSRoadNet/Streets/L_CityStreets -ExecCmds="py <this file>"

Nothing is saved. Output: Saved/CityStreetShots, <view>.png and summary.json (each street's status, plots and the
modules it draws per slot).
"""
import json
import os

import unreal

OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), "CityStreetShots")
W, H = 1600, 900
WARMUP = 48
SLOTS = ("Wall", "Window", "Door", "Corner", "Parapet", "Roof", "CornerInner", "GroundWall", "GroundWindow")
SPACING = 15000.0

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
streets = [a for a in ACTORS.get_all_level_actors() if a.get_class().get_name() == "CSCityStreetActor"]
# An editor in the background does not tick the network: it is built here (and lays its streets out with its roads).
networks = [a for a in ACTORS.get_all_level_actors() if a.get_class().get_name() == "CSRoadNetworkActor"]
for network in networks:
    network.rebuild()


def street_views(name, y):
    # From beyond the street's west end, raised; from above.
    return [
        (name + "_street", unreal.Vector(-4000.0, y - 3500.0, 2200.0), unreal.Vector(6000.0, y, 900.0), 70.0),
        (name + "_top", unreal.Vector(6000.0, y, 26000.0), unreal.Vector(6000.0, y + 1.0, 0.0), 50.0),
    ]


views = [("overview", unreal.Vector(-16000.0, -36000.0, 30000.0), unreal.Vector(6000.0, 25000.0, 0.0), 60.0)]
for i, name in enumerate(("A_scatter", "B_mixed", "C_wall", "D_core", "E_curve")):
    views += street_views(name, i * SPACING)
views += [
    ("D_core_far", unreal.Vector(-12000.0, 3 * SPACING - 16000.0, 9000.0), unreal.Vector(6000.0, 3 * SPACING, 5000.0), 60.0),
    ("F_road_top", unreal.Vector(6000.0, -20000.0, 34000.0), unreal.Vector(6000.0, -19999.0, 0.0), 50.0),
    ("F_road_street", unreal.Vector(-6000.0, -24000.0, 2500.0), unreal.Vector(6000.0, -20000.0, 800.0), 70.0),
]

rt = unreal.RenderingLibrary.create_render_target2d(world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8, unreal.LinearColor(0, 0, 0, 1), False)
capture = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, unreal.Vector(0, 0, 0))
comp = capture.get_component_by_class(unreal.SceneCaptureComponent2D)
comp.set_editor_property("texture_target", rt)
comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
comp.set_editor_property("capture_every_frame", False)
comp.set_editor_property("capture_on_movement", False)
# Without a view state the level's post-process exposure does not apply and Lumen never runs.
comp.set_editor_property("always_persist_rendering_state", True)

STATE = {"ticks": 0, "view": -1, "frames": 0, "handle": None, "done": []}


def summary():
    out = {}
    for street in streets:
        counts = {}
        for slot, component in zip(SLOTS, street.get_editor_property("facade")):
            if component:
                counts[slot] = component.debug_read_drawn_instance_count_sync()
        out[street.get_actor_label()] = {
            "status": street.get_editor_property("status"),
            "buildings": street.get_editor_property("num_buildings"),
            "plots": list(street.get_editor_property("plots")),
            "modules": counts,
        }
    for network in networks:
        keys =("num_roads", "num_buildings", "num_streets", "streets_rebuilt", "last_error")
        out[network.get_actor_label()] = {k: network.get_editor_property(k) for k in keys}
    return out


def begin(index):
    name, location, target, fov = views[index]
    comp.set_editor_property("fov_angle", fov)
    capture.set_actor_location_and_rotation(location, unreal.MathLibrary.find_look_at_rotation(location, target), False, True)
    STATE["view"] = index
    STATE["frames"] = 0


def finish():
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    os.makedirs(OUT_DIR, exist_ok=True)
    drawn = summary()
    with open(os.path.join(OUT_DIR, "summary.json"), "w", encoding="utf-8") as f:
        json.dump({"streets": drawn, "shots": STATE["done"]}, f, indent=2, ensure_ascii=False)
    empty = [label for label, info in drawn.items() if "modules" in info and sum(info["modules"].values()) == 0]
    unreal.log("CITYSTREETSHOT {} streets, {} drew nothing: {}".format(len(streets), len(empty), empty))
    ACTORS.destroy_actor(capture)
    unreal.log("CITYSTREETSHOT done")
    unreal.SystemLibrary.quit_editor()


def tick(delta):
    STATE["ticks"] += 1
    # Let the network build, the facade writes land and the first Lumen frames settle before the first shot.
    if STATE["ticks"] < 90:
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
    unreal.log("CITYSTREETSHOT shot " + name)
    if STATE["view"] + 1 < len(views):
        begin(STATE["view"] + 1)
    else:
        finish()


STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
