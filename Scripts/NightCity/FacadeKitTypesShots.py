# -*- coding: utf-8 -*-
"""Shots of L_FacadeKit_Types (MakeFacadeKitNC.py): Night City's own round corners and overhang parts put together by the
GPU facade.

  UnrealEditor.exe <project> /Game/BuildingGenerator/Cyberpunk2077/Tests/FacadeKit/L_FacadeKit_Types -ExecCmds="py <this file>"

Needs the real editor (in UnrealEditor-Cmd the captures come out black). Nothing is saved.

Output: Saved/NCFacadeKitTypesShots, <view>.png and summary.json (each building's drawn modules per slot).
"""
import json
import os

import unreal

OUT_DIR = os.path.join(unreal.Paths.project_saved_dir(), "NCFacadeKitTypesShots")
W, H = 1600, 900
WARMUP = 48
SLOTS = ("Wall", "Window", "Door", "Corner", "Parapet", "Roof", "CornerInner", "GroundWall", "GroundWindow", "RoofCorner", "ParapetCorner", "Soffit", "Column")
ROW, COLUMN = 7000.0, 6000.0

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
actors = [a for a in ACTORS.get_all_level_actors() if a.get_class().get_name() == "CSCityBuildingActor"]
# Rebuilt as the editor's property change does (Rebuild is not exposed to Python).
for building in actors:
    building.set_editor_property("height", building.get_editor_property("height"), notify_mode=unreal.PropertyAccessChangeNotifyMode.ALWAYS)
# The sun from the cameras' side, more sky for the undersides. Nothing is saved.
for actor in ACTORS.get_all_level_actors():
    if isinstance(actor, unreal.DirectionalLight):
        actor.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=-35.0, yaw=62.0), False)
    if isinstance(actor, unreal.SkyLight):
        actor.get_component_by_class(unreal.SkyLightComponent).set_intensity(3.0)


def close(name, row, column, dx=-3000.0, dy=-3800.0, dz=300.0, tz=1200.0, fov=70.0):
    x, y = column * COLUMN, row * ROW
    return (name, unreal.Vector(x + dx, y + dy, dz), unreal.Vector(x, y, tz), fov)


views = [("overview", unreal.Vector(-9000.0, -9000.0, 14000.0), unreal.Vector(9000.0, 10000.0, 0.0), 65.0)]
views += [
    close("lch_box", 0, 0), close("lch_box_top", 0, 0, dx=-1500.0, dy=-1500.0, dz=4000.0, tz=1800.0),
    close("lch_l", 0, 1), close("lch_setback", 0, 2, dz=1500.0, tz=2000.0), close("lch_tower", 0, 3, dx=-4000.0, dy=-4500.0, dz=1500.0, tz=2200.0),
    close("lch900_box", 1, 0, dx=-4000.0, dy=-4500.0, dz=1000.0), close("lch900_drum", 1, 1, dz=800.0, tz=1800.0), close("lch900_capsule", 1, 2, dx=-4000.0, dy=-4000.0, dz=1500.0),
    close("plaza_front", 2, 0, dx=-3800.0, dy=-4200.0, dz=200.0, tz=1300.0), close("plaza_sides", 2, 1, dx=-4200.0, dy=-3800.0, dz=200.0, tz=1300.0),
    close("plaza_front_sides", 2, 2, dx=-4200.0, dy=-4200.0, dz=200.0, tz=1300.0), close("plaza_all", 2, 3, dx=-4200.0, dy=-4200.0, dz=200.0, tz=1300.0),
    close("plaza_under", 2, 0, dx=-1800.0, dy=-2000.0, dz=180.0, tz=900.0, fov=90.0),
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
    for building in actors:
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
    unreal.log("NCKITTYPES {} buildings, {} drew nothing: {}".format(len(drawn), len(empty), empty))
    ACTORS.destroy_actor(capture)
    unreal.log("NCKITTYPES done")
    unreal.SystemLibrary.quit_editor()


def tick(delta):
    STATE["ticks"] += 1
    # Let streaming, the proxies, the facade writes and the first Lumen frames settle before the first shot.
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
    unreal.log("NCKITTYPES shot " + name)
    if STATE["view"] + 1 < len(views):
        begin(STATE["view"] + 1)
    else:
        finish()


STATE["handle"] = unreal.register_slate_post_tick_callback(tick)
