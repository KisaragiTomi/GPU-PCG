"""Create four PendingReview anchor profiles from the measured original-scene pairs.

Run in UE Python commandlet after compiling CSRoadNet. Pass --validate-only to reload/check.
Never edits source meshes, maps, or existing profiles. A mismatch requires manual review.
"""
import json
from pathlib import Path
import sys
import uuid
import unreal

ROOT = Path(unreal.Paths.project_dir()).resolve()
DOCS = ROOT / 'Plugins/PCGPlugins/Docs/NightCity/AssembledExample01'
OUT = ROOT / 'Saved/CodexNightCityAttachment'
FOLDER = '/PCGPlugins/NightCity/AttachmentBatch01'
sys.path.insert(0, str(Path(__file__).resolve().parent))
from ExportPlacementSources import read_source

ASSETS = unreal.EditorAssetLibrary
LIB = unreal.CSCityAttachmentLibrary
NS = uuid.UUID('85665fba-c3e0-4257-86f1-e2a73c6b53ae')


def guid(key):
    value, success = unreal.GuidLibrary.parse_string_to_guid(str(uuid.uuid5(NS, key)))
    assert success
    return value


def close_vector(a, b, tolerance=1e-5):
    return max(abs(a.x-b.x), abs(a.y-b.y), abs(a.z-b.z)) <= tolerance


def ready_copy(profile):
    """A transient test fixture; inferred production assets always remain PendingReview."""
    result = unreal.new_object(unreal.CSCityAttachmentProfile)
    for key in ('profile_id', 'source_mesh', 'source_geometry_hash', 'anchors'):
        result.set_editor_property(key, profile.get_editor_property(key))
    result.set_editor_property('state', unreal.CSCityAttachmentState.READY)
    return result


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    context = json.loads((DOCS / 'context.json').read_text(encoding='utf-8'))
    review = json.loads((DOCS / 'review.json').read_text(encoding='utf-8'))
    unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous([FOLDER], True)
    validate_only = '--validate-only' in sys.argv
    specs = []
    pairs = []
    for family, body_key, mount_key in (
        ('D_F', 'SM_003AB0AD89462FF2', 'SM_821A5A78007995E0'),
        ('E_H', 'SM_5A0D3C272B46876B', 'SM_AF22004CD50D1C7A'),
    ):
        measured = [p['body_pivot_in_mount_local_cm'] for p in review['inferred_pairs']
                    if p['body'].split('/')[0] == body_key and p['mount'].split('/')[0] == mount_key]
        assert measured and all(p['rotation_match'] for p in review['inferred_pairs'])
        offset = [sum(p[i] for p in measured)/len(measured) for i in range(3)]
        names = []
        for kind, key, role, pos, yaw in (
            ('Body', body_key, unreal.CSCityAttachmentAnchorRole.PLUG, [-v for v in offset], 180),
            ('Mount', mount_key, unreal.CSCityAttachmentAnchorRole.SOCKET, [0, 0, 0], 0),
        ):
            name = f'DA_Attachment_AC_{family}_{kind}'
            names.append(name)
            mesh = unreal.load_asset(context['meshes'][key]['path'])
            assert mesh is not None, key
            current_hash = read_source(mesh)['source_geometry_hash']
            assert current_hash == context['source_geometry_hashes'][key], f'Source changed: {key}'
            specs.append(dict(name=name, key=key, mesh=mesh, fingerprint=current_hash, role=role,
                              pos=pos, yaw=yaw, interface='AirConditioner.'+family))
        pairs.append(dict(body=names[0], mount=names[1], offset=offset, observations=measured))

    # Read/check all existing destinations before any writes; protect authored edits on reruns.
    profiles = {}
    for spec in specs:
        path = FOLDER+'/'+spec['name']
        if ASSETS.does_asset_exist(path):
            profile = unreal.load_asset(path)
            check_profile(profile, spec)
            profiles[spec['name']] = profile
        elif validate_only:
            raise RuntimeError('Missing profile '+path)

    for spec in specs:
        if spec['name'] in profiles:
            continue
        factory = unreal.DataAssetFactory()
        factory.set_editor_property('data_asset_class', unreal.CSCityAttachmentProfile.static_class())
        profile = unreal.AssetToolsHelpers.get_asset_tools().create_asset(spec['name'], FOLDER, unreal.CSCityAttachmentProfile, factory)
        assert profile is not None
        profile.set_editor_property('profile_id', guid(spec['name']))
        profile.set_editor_property('source_mesh', spec['mesh'])
        profile.set_editor_property('source_geometry_hash', spec['fingerprint'])
        profile.set_editor_property('state', unreal.CSCityAttachmentState.PENDING_REVIEW)
        profile.set_editor_property('review_notes',
            'Inferred rigid assembly frame from AssembledExample01, not a measured physical contact point. '
            'Mount frame uses the mount mesh origin; body frame reproduces the mean observed relative pose. '
            'Review real contact geometry, support footprint and clearance before Ready. No wall anchor authored.')
        anchor = unreal.CSCityAttachmentAnchor()
        anchor.set_editor_property('anchor_id', guid(spec['name']+'/mate'))
        anchor.set_editor_property('label', 'AssemblyMate')
        anchor.set_editor_property('interface_type', spec['interface'])
        anchor.set_editor_property('role', spec['role'])
        anchor.set_editor_property('local_frame', unreal.Transform(
            location=unreal.Vector(*spec['pos']), rotation=unreal.Rotator(yaw=spec['yaw'])))
        profile.set_editor_property('anchors', [anchor])
        ASSETS.set_metadata_tag(profile, 'AttachmentExample', 'AssembledExample01')
        check_profile(profile, spec)
        assert ASSETS.save_loaded_asset(profile, False), spec['name']
        profiles[spec['name']] = profile

    checks = []
    for pair in pairs:
        body = profiles[pair['body']]
        mount = profiles[pair['mount']]
        body_id = body.get_editor_property('anchors')[0].anchor_id
        mount_id = mount.get_editor_property('anchors')[0].anchor_id
        assert LIB.are_anchors_compatible(mount, mount_id, body, body_id) is None, 'Draft should be rejected'
        # Test the measurements using transient reviewed copies, never change the saved assets' state.
        result = LIB.compute_connected_transform(ready_copy(mount), mount_id, unreal.Transform(), ready_copy(body), body_id)
        assert result is not None, 'Candidate pose failed'
        transform, error = result
        assert error == ''
        assert close_vector(transform.translation, unreal.Vector(*pair['offset'])), f"Computed {transform}, expected offset {pair['offset']}"
        assert close_vector(transform.rotation.rotate_vector(unreal.Vector(1, 0, 0)), unreal.Vector(1, 0, 0))
        assert close_vector(transform.rotation.rotate_vector(unreal.Vector(0, 0, 1)), unreal.Vector(0, 0, 1))
        max_residual = max(sum((p[i]-pair['offset'][i])**2 for i in range(3))**0.5 for p in pair['observations'])
        assert max_residual < 0.02, 'Observed assembly is not a consistent rigid pair'
        checks.append(dict(body=pair['body'], mount=pair['mount'], relative_translation_cm=pair['offset'],
                           max_observed_pivot_residual_cm=max_residual, pending_rejected=True, pose_verified=True))
    report = dict(mode='validate-only' if validate_only else 'create', assets=[p.get_path_name() for p in profiles.values()],
                  asset_count=len(profiles), state='PendingReview', pairs=checks)
    (OUT / ('ReloadValidation.json' if validate_only else 'Creation.json')).write_text(json.dumps(report, indent=2), encoding='utf-8')
    unreal.log('ATTACHMENT EXAMPLES PASSED: 4 profiles, 2 inferred pairs, no source/map writes')


def check_profile(profile, spec):
    assert isinstance(profile, unreal.CSCityAttachmentProfile), spec['name']
    assert ASSETS.get_metadata_tag(profile, 'AttachmentExample') == 'AssembledExample01'
    assert profile.get_editor_property('profile_id').to_string() == guid(spec['name']).to_string()
    assert profile.get_editor_property('source_mesh') == spec['mesh']
    assert profile.get_editor_property('source_geometry_hash') == spec['fingerprint']
    assert profile.get_editor_property('state') == unreal.CSCityAttachmentState.PENDING_REVIEW
    assert profile.get_editor_property('schema_version') == 1 and profile.get_editor_property('data_revision') == 1
    assert profile.validate_data() == '', f'Invalid profile: {spec["name"]}'
    anchors = profile.get_editor_property('anchors')
    assert len(anchors) == 1
    a = anchors[0]
    assert a.anchor_id.to_string() == guid(spec['name']+'/mate').to_string()
    assert str(a.label) == 'AssemblyMate' and a.max_connections == 1
    assert str(a.interface_type) == spec['interface'] and a.role == spec['role']
    assert close_vector(a.local_frame.translation, unreal.Vector(*spec['pos']))
    expected = unreal.Rotator(yaw=spec['yaw']).quaternion()
    for axis in (unreal.Vector(1, 0, 0), unreal.Vector(0, 0, 1)):
        assert close_vector(a.local_frame.rotation.rotate_vector(axis), expected.rotate_vector(axis))


if __name__ == '__main__':
    main()
