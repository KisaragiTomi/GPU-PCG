# UE Python: the local bounds (min/max, cm) of the meshes nc_rank.py lists (nc_bounds_list.json, the meshes placed 30
# times or more) -> nc_mesh_bounds.json, both in the data folder: the NC_DATA environment variable, else this script's
# folder. The asset registry only has each mesh's size; the placement analysis needs where its pivot is.
#
#   UnrealEditor-Cmd.exe <project> -ExecutePythonScript=<this file> -unattended -nosplash -RenderOffscreen
import json
import os

import unreal

DATA = os.environ.get("NC_DATA") or os.path.dirname(os.path.abspath(__file__))
NC = "/Game/BuildingGenerator/Cyberpunk2077/Meshes"
assets = json.load(open(os.path.join(DATA, "nc_bounds_list.json")))
out = {}
for i, asset in enumerate(assets):
    mesh = unreal.EditorAssetLibrary.load_asset("{}/{}/{}".format(NC, asset[3:5], asset))
    if not mesh:
        continue
    box = mesh.get_bounding_box()
    out[asset] = [round(box.min.x, 1), round(box.min.y, 1), round(box.min.z, 1), round(box.max.x, 1), round(box.max.y, 1), round(box.max.z, 1)]
    if i % 200 == 0:
        unreal.log("NCBOUNDS {} / {}".format(i, len(assets)))
        unreal.SystemLibrary.collect_garbage()
with open(os.path.join(DATA, "nc_mesh_bounds.json"), "w") as f:
    json.dump(out, f)
unreal.log("NCBOUNDS done {}".format(len(out)))
