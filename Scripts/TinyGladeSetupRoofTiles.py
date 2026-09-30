"""Install the user-confirmed 2026-09-29 reference roof on the BP and current level houses.

Does not switch levels. Save the current map after visual validation.
The source roof_tile / lod1 / backface / roof_spire assets remain available.

Layout values are TG's own (2026-09-29, system_roof::utils::assemble_rectangular_roof, see
Docs/TinyGlade/Roof_TerraceAndRidge.md): ~45 cm rows and columns, tile length = row spacing / 0.7,
a 10 cm slab (13 = slab + the 3 cm baked lift). The TG-scale tile is longer than wide, so the axis
swap must stay off. RoofTileLengthJitter (TG's 0.8..1.2 length) is left at its C++ default.
Writing the CDO propagates to placed houses and dirties the open map.

For an already-installed TG-scale mesh, repair_legacy_overrides(save_map=True) migrates
only the obsolete 2026-08-31 row/column overrides in the current level. It does not
rebake assets or reset deliberately customized roof settings.
"""
import importlib.util
from pathlib import Path
import unreal


def repair_legacy_overrides(save_map=False):
    """Remove the old density paired with the new 49.7 x 64 x 13 cm tile.

    Blueprint default changes do not replace serialized per-actor overrides. The
    House_Pillar fixture retained both old pitches after the 2026-09-29 asset bake,
    shrinking the new tile in-plane while leaving its full thickness unchanged.
    """
    mesh=unreal.load_asset('/PCGPlugins/HouseTest/SM_TinyGladeRoofTile')
    bp=unreal.load_asset('/PCGPlugins/HouseTest/BP_TinyGladeHouse')
    assert mesh and bp, 'Install the reference roof assets first'
    bounds=mesh.get_bounding_box()
    size=bounds.max-bounds.min
    assert abs(size.x-49.7064)<0.1 and abs(size.y-64.0)<0.1 and abs(size.z-13.0)<0.1, \
        'Legacy density migration requires the TG-scale roof mesh'
    defaults=unreal.get_default_object(bp.generated_class())
    legacy={'RoofTileRowPitch':16.897207260131836,'RoofTileColumnPitch':27.75071144104004}
    for name in legacy:
        assert abs(defaults.get_editor_property(name)-45.0)<0.01, (name,'Unexpected reference default')
    changes=[]
    actors=unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    for actor in actors:
        if not isinstance(actor,unreal.CSHouseActor) or actor.get_editor_property('RoofTileMesh')!=mesh:
            continue
        fields={name:old for name,old in legacy.items()
                if abs(actor.get_editor_property(name)-old)<0.001}
        if not fields: continue
        actor.modify()
        for name in fields:
            actor.set_editor_property(name,defaults.get_editor_property(name))
        actor.rebuild_house()
        if not actor.is_flat_roof() and actor.get_editor_property('bRoofTilesEnabled'):
            assert actor.get_roof_tile_count()>0 and not actor.get_roof_tile_undrawable_reason(),actor.get_actor_label()
        changes.append({'actor':actor.get_actor_label(),'old':fields,
                        'new':{name:actor.get_editor_property(name) for name in fields},
                        'tiles':actor.get_roof_tile_count()})
        unreal.log('TG ROOF migrated legacy density: '+str(changes[-1]))
    if changes and save_map:
        assert unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level(), 'Roof map save failed'
    return changes


def module(name):
    spec=importlib.util.spec_from_file_location(name,Path(__file__).with_name(name+'.py'))
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m


def main():
    # The extracted roof_tile_damage.png is scaled to 0..76 (see that script); the material needs TG's real mask.
    module('TinyGladeFixBc4Textures').main()
    module('TinyGladeMakeRoofMaterial').build()
    mat=unreal.load_asset('/PCGPlugins/HouseTest/MI_TinyGladeRoof_Grey');assert mat
    mesh=module('TinyGladeMakeRoofMesh').build(mat)
    bp=unreal.load_asset('/PCGPlugins/HouseTest/BP_TinyGladeHouse');assert bp
    actors=[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
            if isinstance(a,unreal.CSHouseActor)]
    # The GPU component snapshots the source once; changing an asset in place
    # needs a different source identity before reassigning the rebuilt tile.
    source=unreal.load_asset('/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/roof_tile')
    for a in actors:
        if a.get_editor_property('RoofTileMesh')==mesh:
            a.set_editor_property('RoofTileMesh',source)
            a.rebuild_house()
    for a in [unreal.get_default_object(bp.generated_class())]+actors:
        for name,value in [('RoofTileMesh',mesh),('RoofMaterial',mat),('bRoofTilesEnabled',True),
            ('bRoofTileSwapAxes',False),('RoofTileThickness',13.0),('RoofTileRowPitch',45.0),('RoofTileColumnPitch',45.0),
            ('RoofTileRowOverlap',1.43),('RoofTileColumnOverlap',1.10),('RoofTileScaleJitter',0.0),
            ('RoofFinialMesh',None),('RoofFinialMaterial',None)]:
            a.set_editor_property(name,value)
    unreal.EditorAssetLibrary.save_loaded_asset(bp,False)
    for a in actors:
        a.rebuild_house()
        assert a.get_roof_tile_count()>0 and not a.get_roof_tile_undrawable_reason(),a.get_actor_label()
        assert a.get_roof_finial_count()==0,a.get_actor_label()
        unreal.log('TG ROOF wired '+a.get_actor_label()+' tiles='+str(a.get_roof_tile_count()))
    unreal.log('TG ROOF SETUP OK')


if __name__=='__main__':main()
