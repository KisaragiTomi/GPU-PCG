# -*- coding: utf-8 -*-
"""Shots of the GPU road network (ACSRoadNetworkActor) next to the static CS2 road import.

  UnrealEditor.exe <project> /PCGPlugins/CS2Roads/Maps/L_Palmsland -ExecCmds="py <this file>"

Builds the Palmsland snapshot into an ACSRoadNetworkActor, then captures every view twice from
the same SceneCapture2D: the static Roads meshes (Scripts/ImportCS2Roads.py) and the GPU mesh.
Needs the real editor (in UnrealEditor-Cmd the captures come out black). Nothing is saved.

Output: Saved/CS2RoadShots/<view>_static.png, <view>_gpu.png and summary.json.
"""
import json
import os

import unreal

NETWORK = os.environ.get("CS2ROAD_NETWORK", r"D:/MyProject/CitiesSkylines-Road2-C#/Artifacts/public-saves/palmsland-3/network.json")
OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), "CS2RoadShots")
W, H = 1600, 900
# Lumen keeps history across frames: one capture per frame until it settles.
WARMUP = 64
MAX_VIEWS = 4

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
level_actors = ACTORS.get_all_level_actors()
# The GPU mesh has no markings yet (lane lines, crosswalks and tire tracks are decals in RoadLab),
# so both variants leave the static Markings layer out and compare road surfaces only.
static_roads = [a for a in level_actors if a.actor_has_tag("CS2RoadLab") and a.actor_has_tag("Roads")]
static_markings = [a for a in level_actors if a.actor_has_tag("CS2RoadLab") and a.actor_has_tag("Markings")]
markers = sorted([a for a in level_actors if a.get_actor_label().startswith("View_")], key=lambda a: a.get_actor_label())

road_class = unreal.load_class(None, "/Script/CSRoadNet.CSRoadNetworkActor")
road = ACTORS.spawn_actor_from_class(road_class, unreal.Vector(0, 0, 0))
road.set_actor_label("CS2 GPU Roads")
road.set_editor_property("network_file", unreal.FilePath(NETWORK), notify_mode=unreal.PropertyAccessChangeNotifyMode.NEVER)
road.rebuild()
stats = {k: road.get_editor_property(k) for k in ("num_roads", "num_vertices", "num_triangles", "build_milliseconds", "last_error")}
stats["missing_atlases"] = [str(a) for a in road.get_editor_property("missing_atlases")]
unreal.log("CS2ROADSHOT build " + json.dumps(stats))

# ⚠️ unreal.Rotator positional arguments are (roll, pitch, yaw): always pass keywords.
focus = unreal.Vector(250544.8, -62526.2, 56085.7)
views = [("overview", focus + unreal.Vector(42000.0, -55000.0, 47000.0), focus)]
for marker in markers[:MAX_VIEWS]:
    target = marker.get_actor_location()
    views.append((marker.get_actor_label(), target + unreal.Vector(-6000.0, -6000.0, 5000.0), target))

rt = unreal.RenderingLibrary.create_render_target2d(world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8, unreal.LinearColor(0, 0, 0, 1), False)
capture = ACTORS.spawn_actor_from_class(unreal.SceneCapture2D, unreal.Vector(0, 0, 0))
comp = capture.get_component_by_class(unreal.SceneCaptureComponent2D)
comp.set_editor_property("texture_target", rt)
comp.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
comp.set_editor_property("fov_angle", 60.0)
comp.set_editor_property("capture_every_frame", False)
comp.set_editor_property("capture_on_movement", False)
# Without a view state the level's post-process exposure does not apply and Lumen never runs.
comp.set_editor_property("always_persist_rendering_state", True)

shots = []
for name, location, target in views:
    rotation = unreal.MathLibrary.find_look_at_rotation(location, target)
    shots.append((name + "_static", location, rotation, [road] + static_markings))
    shots.append((name + "_gpu", location, rotation, static_roads + static_markings))

STATE = {"ticks": 0, "shot": -1, "frames": 0, "handle": None, "done": []}


def begin(index):
    name, location, rotation, hidden = shots[index]
    capture.set_actor_location_and_rotation(location, rotation, False, True)
    # HiddenActors cannot be set through set_editor_property ("cannot be edited on templates").
    comp.clear_hidden_components()
    for actor in hidden:
        comp.hide_actor_components(actor, False)
    STATE["shot"] = index
    STATE["frames"] = 0


def finish():
    unreal.unregister_slate_post_tick_callback(STATE["handle"])
    summary = {"network": NETWORK, "build": stats, "static_actors": len(static_roads), "shots": STATE["done"]}
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(os.path.join(OUT_DIR, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)
    ACTORS.destroy_actor(capture)
    ACTORS.destroy_actor(road)
    unreal.log("CS2ROADSHOT done")
    unreal.SystemLibrary.quit_editor()


def tick(delta):
    STATE["ticks"] += 1
    # Let streaming, the proxies and the first Lumen frames settle before the first shot.
    if STATE["ticks"] < 30:
        return
    if STATE["shot"] < 0:
        begin(0)
    comp.capture_scene()
    STATE["frames"] += 1
    if STATE["frames"] < WARMUP:
        return
    name = shots[STATE["shot"]][0]
    unreal.RenderingLibrary.export_render_target(world, rt, OUT_DIR, name + ".png")
    STATE["done"].append(name)
    unreal.log("CS2ROADSHOT shot " + name)
    if STATE["shot"] + 1 < len(shots):
        begin(STATE["shot"] + 1)
    else:
        finish()


STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
