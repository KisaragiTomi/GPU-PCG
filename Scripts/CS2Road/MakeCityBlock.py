# -*- coding: utf-8 -*-
"""Creates the building module the CS2 road network and the city buildings put their buildings together from when
none is set (ACSCityBuildingActor::DefaultModule):

  /PCGPlugins/CSRoadNet/Houses/SM_CityBlock   the engine's 1 m cube with Nanite on, so that its copies go into
                                             GPU-Scene and no tile uploads a mesh of its own
  /PCGPlugins/CSRoadNet/Houses/M_CityBlock    plain plaster, for instanced meshes and Nanite

  UnrealEditor-Cmd.exe <project> -ExecutePythonScript=<this file> -unattended -RenderOffscreen

Makes only what is missing; can run again.
"""
import unreal

FOLDER = "/PCGPlugins/CSRoadNet/Houses"
MESH = FOLDER + "/SM_CityBlock"
MATERIAL = FOLDER + "/M_CityBlock"
CUBE = "/Engine/BasicShapes/Cube"

ASSETS = unreal.EditorAssetLibrary


def log(text):
    unreal.log("CS2CITYBLOCK " + text)


def material():
    if ASSETS.does_asset_exist(MATERIAL):
        return ASSETS.load_asset(MATERIAL)
    edit = unreal.MaterialEditingLibrary
    made = unreal.AssetToolsHelpers.get_asset_tools().create_asset("M_CityBlock", FOLDER, unreal.Material, unreal.MaterialFactoryNew())
    colour = edit.create_material_expression(made, unreal.MaterialExpressionConstant3Vector, -300, 0)
    colour.set_editor_property("constant", unreal.LinearColor(0.72, 0.68, 0.60, 1.0))
    edit.connect_material_property(colour, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = edit.create_material_expression(made, unreal.MaterialExpressionConstant, -300, 200)
    rough.set_editor_property("r", 0.85)
    edit.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    # Without these the instances draw with the default material in a cooked build.
    made.set_editor_property("used_with_instanced_static_meshes", True)
    made.set_editor_property("used_with_nanite", True)
    edit.recompile_material(made)
    ASSETS.save_loaded_asset(made, False)
    log("made " + MATERIAL)
    return made


def mesh(surface):
    if ASSETS.does_asset_exist(MESH):
        made = ASSETS.load_asset(MESH)
    else:
        made = ASSETS.duplicate_asset(CUBE, MESH)
        if not made:
            unreal.log_error("CS2CITYBLOCK could not copy {} to {}".format(CUBE, MESH))
            return None
        log("made " + MESH)
    made.set_material(0, surface)
    settings = made.get_editor_property("nanite_settings")
    settings.set_editor_property("enabled", True)
    # The change rebuilds it; one built fast writes no Nanite data, so that is cleared and built again
    # (CSStaticMeshBuildLibrary.h).
    made.set_editor_property("nanite_settings", settings)
    unreal.CSStaticMeshBuildLibrary.clear_fast_build_and_rebuild(made)
    ASSETS.save_loaded_asset(made, False)
    return made


def run():
    made = mesh(material())
    if not made:
        return
    box = made.get_bounding_box()
    nanite = made.get_editor_property("nanite_settings").get_editor_property("enabled")
    log("{}: box {} .. {}, Nanite {}, material {}".format(MESH, box.min, box.max, nanite, made.get_material(0).get_path_name()))


run()
