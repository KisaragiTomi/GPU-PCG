# -*- coding: utf-8 -*-
"""Shots of L_NCSimpleShapes (MakeNCSimpleModules.py): the plugin's simplified Night City kits on every footprint and
stacking, from above (footprints, terraces) and from the street (ground floors, corners, setbacks).

  UnrealEditor.exe <project> /PCGPlugins/CSRoadNet/Houses/NCSimple/L_NCSimpleShapes -ExecCmds="py <this file>"

Needs the real editor (in UnrealEditor-Cmd the captures come out black). Nothing is saved.

Output: Saved/NCSimpleShapesShots, <view>.png and summary.json (each building's massing and drawn modules per slot).
"""
import json
import os

import unreal

OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), "NCSimpleShapesShots")
W, H = 1600, 900
WARMUP = 48
SLOTS = ("Wall", "Window", "Door", "Corner", "Parapet", "Roof", "CornerInner", "GroundWall", "GroundWindow")

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
buildings = [a for a in ACTORS.get_all_level_actors() if a.get_class().get_name() == "CSCityBuildingActor"]
# Rebuilt as the editor's property change does (Rebuild is not exposed to Python).
for building in buildings:
    building.set_editor_property("height", building.get_editor_property("height"), notify_mode=unreal.PropertyAccessChangeNotifyMode.ALWAYS)

# Concrete kit: columns at x = 0, 40, 80, 120, 160 m (box, L, U, T, courtyard), rows at y = 0, 45, 90 m (none, setback,
# podium and tower); the glass kit's block 150 m further along y.
views = [
    ("concrete_top", unreal.Vector(8000.0, 4500.0, 30000.0), unreal.Vector(8000.0, 4501.0, 0.0), 50.0),
    ("glass_top", unreal.Vector(8000.0, 19500.0, 30000.0), unreal.Vector(8000.0, 19501.0, 0.0), 50.0),
    ("concrete_south", unreal.Vector(8000.0, -9000.0, 5000.0), unreal.Vector(8000.0, 4500.0, 1200.0), 70.0),
    ("concrete_north", unreal.Vector(8000.0, 19000.0, 6000.0), unreal.Vector(8000.0, 4500.0, 1200.0), 70.0),
    ("glass_south", unreal.Vector(8000.0, 6000.0, 5000.0), unreal.Vector(8000.0, 19500.0, 1200.0), 70.0),
    ("glass_north", unreal.Vector(8000.0, 34000.0, 6000.0), unreal.Vector(8000.0, 19500.0, 1200.0), 70.0),
    ("concrete_west", unreal.Vector(-9000.0, 4500.0, 4000.0), unreal.Vector(8000.0, 4500.0, 1200.0), 70.0),
    ("concrete_L_street", unreal.Vector(2200.0, -2600.0, 700.0), unreal.Vector(4000.0, 0.0, 600.0), 70.0),
    ("concrete_U_back", unreal.Vector(10500.0, 3200.0, 1600.0), unreal.Vector(8000.0, 0.0, 600.0), 70.0),
    ("glass_podium", unreal.Vector(-1800.0, 24000.0 - 5200.0, 2200.0), unreal.Vector(0.0, 24000.0, 1800.0), 70.0),
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


def modules():
    """Each building's drawn modules per slot, read back from the GPU (blocking)."""
    out = {}
    for building in buildings:
        counts = {}
        for slot, component in zip(SLOTS, building.get_editor_property("facade")):
            if component:
                counts[slot] = component.debug_read_drawn_instance_count_sync()
        out[building.get_actor_label()] = counts
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
    drawn = modules()
    with open(os.path.join(OUT_DIR, "summary.json"), "w", encoding="utf-8") as f:
        json.dump({"modules": drawn, "shots": STATE["done"]}, f, indent=2, ensure_ascii=False)
    empty = [label for label, counts in drawn.items() if sum(counts.values()) == 0]
    unreal.log("NCSIMPLESHOT {} buildings, {} drew nothing: {}".format(len(drawn), len(empty), empty))
    ACTORS.destroy_actor(capture)
    unreal.log("NCSIMPLESHOT done")
    unreal.SystemLibrary.quit_editor()


def tick(delta):
    STATE["ticks"] += 1
    # Let streaming, the proxies, the facade writes and the first Lumen frames settle before the first shot.
    if STATE["ticks"] < 60:
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
    unreal.log("NCSIMPLESHOT shot " + name)
    if STATE["view"] + 1 < len(views):
        begin(STATE["view"] + 1)
    else:
        finish()


STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
