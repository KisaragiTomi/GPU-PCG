"""Bake the extracted TG tile's vertex deformation for UE, keeping TG's template coordinates.

Input topology is copied verbatim from roof_tile.json (20 vertices, 30 triangles; the
36-triangle file is roof_tile_backface, roof_tile_lod1 is a plain cube). TG has exactly one
visible tile shape: chips and wear are shading (roof_tile_damage / roof_tile_normal atlas),
not geometry variants. The native VS uses sign-based resizing, not uniform box scaling:
the bevel and the wide-end flare are sized by the tile's *thickness*, not by its width.

SCALE_T_CM is TG's own per-tile scale_t (2026-09-29, tiny-glade.exe + PDB,
system_roof::utils::assemble_rectangular_roof, evidence in Docs/TinyGlade/Roof_TerraceAndRidge.md):
  width  = row length / max(ceil(2 * row length m), 3)      -> <= 50 cm, typically ~45
  length = (0.8 + 0.4 * rand) * row spacing / 0.7           -> row spacing ~45 cm, so ~64 cm
  depth  = 0.1 m, a literal constant
Baking at that nominal size keeps the instance scale near 1, so the 2.2 cm bevel and the 3.2 cm
per-side flare that open TG's wedge gaps survive. (Until 2026-09-29 this baked 28 x 27 x 3 cm to fit
16.9 cm rows: a third of TG's depth, bevel 0.66 cm, flare 1 cm - a thin flat plate.)
No source asset, LOD proxy or backface mesh is overwritten.

What M_TinyGladeRoof reads from this mesh (TG `_nani_instanced_roof` VS varyings):
  UV0      = TG template position xy (roof_tile.json units, not remapped). The material
             forms C6 = (xy*(.85,.92)+.5)*.2 + cell and C10 = (yz*(.8,.9)+.5)*.2 + cell itself,
             so the top/side projection is chosen per pixel like TG, not per vertex.
  Colour.R = TG template z + 0.5 (C9 = 2z). Only R carries data: the GPU instance path
             clears A for the per-instance random (CSGpuInstancedMeshComponent.h channel table).

Orientation: TG's +y end is the wide, lifted end and must point down-slope, our local -Y.
A 180-degree turn about Z does that without mirroring, so the UV frame stays right-handed
(dP/du = -X, dP/dv = -Y, N = +Z, bitangent sign +1). That matters: CSHouseVine::BuildBaseMesh,
which uploads this mesh for the tile instances, forces every bitangent sign to +1. The
previous Y-mirror bake had a true sign of -1, so roof_tile_normal was applied with its X
relief flipped.
"""
import json
import math
from pathlib import Path
import unreal

MESH = '/PCGPlugins/HouseTest/SM_TinyGladeRoofTile'
# Down-slope tile-end lift baked into the vertices (TG: 6 cm, 2 cm on 10% of tiles, applied in its VS).
# M_TinyGladeRoof's WPO adds the per-tile remainder on top of this, so it reads the constant from here.
BAKED_LIFT_CM = 3.0
# TG scale_t (width across the row, length down the slope, depth) in cm at its nominal row: see the module doc.
# Resulting bounds: 49.7 x 64 x 13 cm (10 cm slab + the baked lift) = ACSHouseActor's RoofTileThickness 13.
SCALE_T_CM = (45.0, 64.0, 10.0)


def build(material):
    source = Path(__file__).resolve().parents[1] / 'Docs/TinyGlade/evidence/roof_tile-source.json'
    data = json.loads(source.read_text(encoding='utf-8'))
    original = data['Vertex_Position']['buffer']
    indices = data['indices']['buffer']
    assert len(original) == 20 and len(indices) == 90, (len(original), len(indices))
    # Native VS (_nani_instanced_roof vs_main.glsl:149-172): half = scale_t * (0.50597, 0.5, 0.5) with x * 0.95,
    # common = max(min(max(half), min(half * normalization)), epsilon).
    half = (SCALE_T_CM[0] * 0.50597 * 0.95, SCALE_T_CM[1] * 0.5, SCALE_T_CM[2] * 0.5)
    common = max(min(max(half), min(a*b for a,b in zip(half,(3.5892038,3.7037039,3.7037039)))), 1e-5)
    positions = []
    for v in original:
        p = [v[i] * (1.9764018 if i == 0 else 2.0) * common
             - (common-half[i]) * (1 if v[i] >= 0 else -1) for i in range(3)]
        p[2] += BAKED_LIFT_CM if v[1] > .1 else 0.0
        # Canonical UE tile: X across row, +Y up slope, +Z top (see module doc for the turn).
        positions.append((-p[0], -p[1], p[2]))
    centre = [sum(p[k] for p in positions) / len(positions) for k in range(3)]
    vertices, normals, uv, colors, triangles = [], [], [], [], []
    flipped = 0
    for start in range(0,len(indices),3):
        ids = indices[start:start+3]
        pts = [positions[i] for i in ids]
        a,b = [[pts[j][k]-pts[0][k] for k in range(3)] for j in (1,2)]
        c = (a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0])
        length = math.sqrt(sum(x*x for x in c))
        # Source order is clockwise seen from outside, so cross(b-a, c-a) points inward and a
        # rotation keeps it that way. UE treats that order as front-facing: emit it unchanged
        # and use the negated cross as the flat outward normal.
        n = tuple(-x/length for x in c)
        mid = [sum(p[k] for p in pts) / 3.0 - centre[k] for k in range(3)]
        flipped += sum(n[k]*mid[k] for k in range(3)) < 0.0
        base = len(vertices)
        for i,p in zip(ids,pts):
            v=original[i]
            vertices.append(unreal.Vector(*p)); normals.append(unreal.Vector(*n))
            uv.append(unreal.Vector2D(v[0], v[1]))
            # Nanite reads the asset colours directly: alpha must stay zero so
            # PerInstanceRandom + VertexColor.A has the same seed as the classic path.
            colors.append(unreal.LinearColor(v[2] + 0.5, 0.0, 0.0, 0.0))
        triangles.append(unreal.IntVector(base,base+1,base+2))
    assert flipped == 0, 'faces whose outward normal points at the tile centre: %d' % flipped
    buffers=unreal.GeometryScriptSimpleMeshBuffers(vertices=vertices,normals=normals,
        triangles=triangles,uv0=uv,vertex_colors=colors)
    dynamic=unreal.DynamicMesh(); dynamic.append_buffers_to_mesh(buffers)
    mesh=unreal.load_asset(MESH) if unreal.EditorAssetLibrary.does_asset_exist(MESH) else None
    if mesh:
        options=unreal.GeometryScriptCopyMeshToAssetOptions(enable_recompute_normals=False,enable_recompute_tangents=True)
        _,outcome=unreal.GeometryScript_AssetUtils.copy_mesh_to_static_mesh(dynamic,mesh,options,unreal.GeometryScriptMeshWriteLOD())
    else:
        options=unreal.GeometryScriptCreateNewStaticMeshAssetOptions(enable_recompute_normals=False,
            enable_recompute_tangents=True,enable_collision=False,enable_nanite=True)
        mesh,outcome=unreal.GeometryScript_NewAssetUtils.create_new_static_mesh_asset_from_mesh(dynamic,MESH,options)
    assert mesh and outcome==unreal.GeometryScriptOutcomePins.SUCCESS, str(outcome)
    mesh.set_material(0,material)
    subsystem=unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    settings=subsystem.get_nanite_settings(mesh)
    settings.set_editor_property('enabled',True)
    subsystem.set_nanite_settings(mesh,settings,True)
    unreal.EditorAssetLibrary.save_loaded_asset(mesh,False)
    unreal.log('TG roof tile baked: '+str(mesh.get_bounding_box()))
    return mesh
