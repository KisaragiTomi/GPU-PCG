"""Generate the CS2 road data layout shared by C++ and HLSL.

Reads the verbatim decompiled C# under <CS2RoadLab>/Port/Original (see
D:\\MyProject\\CitiesSkylines-Road2-C#) and writes Shaders/Private/CSRoad/CSRoadTypes.ush:
flag constants named <Enum>_<Value> and plain structs in source field order.
The same file is included by C++ (namespace CSRoad, after CSRoadMath.h) and by HLSL.

    python Scripts/CS2Road/generate_types.py [path-to-CS2RoadLab]
"""
from pathlib import Path
import re
import sys

PLUGIN = Path(__file__).resolve().parents[2]
LAB = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(r'D:/MyProject/CitiesSkylines-Road2-C#')
ORIGINAL = LAB / 'Port' / 'Original'
OUT = PLUGIN / 'Shaders' / 'Private' / 'CSRoad' / 'CSRoadTypes.ush'

# (output prefix, file, enum name inside the file)
ENUMS = [
    ('CompositionFlags_General', 'Game/Game.Prefabs/CompositionFlags.cs', 'General'),
    ('CompositionFlags_Side', 'Game/Game.Prefabs/CompositionFlags.cs', 'Side'),
    ('CompositionState', 'Game/Game.Prefabs/CompositionState.cs', 'CompositionState'),
    ('NetSectionFlags', 'Game/Game.Prefabs/NetSectionFlags.cs', 'NetSectionFlags'),
    ('NetPieceFlags', 'Game/Game.Prefabs/NetPieceFlags.cs', 'NetPieceFlags'),
    ('NetPieceRequirements', 'Game/Game.Prefabs/NetPieceRequirements.cs', 'NetPieceRequirements'),
    ('NetEdgeMatchType', 'Game/Game.Prefabs/NetEdgeMatchType.cs', 'NetEdgeMatchType'),
    ('LaneFlags', 'Game/Game.Prefabs/LaneFlags.cs', 'LaneFlags'),
    ('RoadFlags', 'Game/Game.Prefabs/RoadFlags.cs', 'RoadFlags'),
    ('MeshFlags', 'Game/Game.Prefabs/MeshFlags.cs', 'MeshFlags'),
    ('MeshLayer', 'Game/Game.Prefabs/MeshLayer.cs', 'MeshLayer'),
    ('GeometryFlags', 'Game/Game.Net/GeometryFlags.cs', 'GeometryFlags'),
    ('Layer', 'Game/Game.Net/Layer.cs', 'Layer'),
    ('PlacementFlags', 'Game/Game.Net/PlacementFlags.cs', 'PlacementFlags'),
    ('RoadTypes', 'Game/Game.Net/RoadTypes.cs', 'RoadTypes'),
    ('ObjectPlacementFlags', 'Game/Game.Objects/PlacementFlags.cs', 'PlacementFlags'),
]

# (struct name, file, struct name inside the file); emitted in this order.
STRUCTS = [
    ('CompositionFlags', 'Game/Game.Prefabs/CompositionFlags.cs', 'CompositionFlags'),
    ('Edge', 'Game/Game.Net/Edge.cs', 'Edge'),
    ('Node', 'Game/Game.Net/Node.cs', 'Node'),
    ('Curve', 'Game/Game.Net/Curve.cs', 'Curve'),
    ('Elevation', 'Game/Game.Net/Elevation.cs', 'Elevation'),
    ('Composition', 'Game/Game.Net/Composition.cs', 'Composition'),
    ('Upgraded', 'Game/Game.Net/Upgraded.cs', 'Upgraded'),
    ('Owner', 'Game/Game.Common/Owner.cs', 'Owner'),
    ('Segment', 'Game/Game.Net/Segment.cs', 'Segment'),
    ('EdgeGeometry', 'Game/Game.Net/EdgeGeometry.cs', 'EdgeGeometry'),
    ('EdgeNodeGeometry', 'Game/Game.Net/EdgeNodeGeometry.cs', 'EdgeNodeGeometry'),
    ('StartNodeGeometry', 'Game/Game.Net/StartNodeGeometry.cs', 'StartNodeGeometry'),
    ('EndNodeGeometry', 'Game/Game.Net/EndNodeGeometry.cs', 'EndNodeGeometry'),
    ('NodeGeometry', 'Game/Game.Net/NodeGeometry.cs', 'NodeGeometry'),
    ('NetCompositionData', 'Game/Game.Prefabs/NetCompositionData.cs', 'NetCompositionData'),
    ('NetCompositionPiece', 'Game/Game.Prefabs/NetCompositionPiece.cs', 'NetCompositionPiece'),
    ('NetCompositionLane', 'Game/Game.Prefabs/NetCompositionLane.cs', 'NetCompositionLane'),
    ('NetCompositionCrosswalk', 'Game/Game.Prefabs/NetCompositionCrosswalk.cs', 'NetCompositionCrosswalk'),
    ('NetCompositionCarriageway', 'Game/Game.Prefabs/NetCompositionCarriageway.cs', 'NetCompositionCarriageway'),
    ('NetCompositionMeshData', 'Game/Game.Prefabs/NetCompositionMeshData.cs', 'NetCompositionMeshData'),
    ('NetGeometryData', 'Game/Game.Prefabs/NetGeometryData.cs', 'NetGeometryData'),
    ('NetData', 'Game/Game.Prefabs/NetData.cs', 'NetData'),
    ('RoadData', 'Game/Game.Prefabs/RoadData.cs', 'RoadData'),
    ('PlaceableNetData', 'Game/Game.Prefabs/PlaceableNetData.cs', 'PlaceableNetData'),
    ('NetLaneData', 'Game/Game.Prefabs/NetLaneData.cs', 'NetLaneData'),
    ('NetPieceData', 'Game/Game.Prefabs/NetPieceData.cs', 'NetPieceData'),
    ('NetPieceLane', 'Game/Game.Prefabs/NetPieceLane.cs', 'NetPieceLane'),
    ('NetVertexMatchData', 'Game/Game.Prefabs/NetVertexMatchData.cs', 'NetVertexMatchData'),
    ('NetCrosswalkData', 'Game/Game.Prefabs/NetCrosswalkData.cs', 'NetCrosswalkData'),
    ('NetSubSection', 'Game/Game.Prefabs/NetSubSection.cs', 'NetSubSection'),
    ('NetSectionPiece', 'Game/Game.Prefabs/NetSectionPiece.cs', 'NetSectionPiece'),
    ('NetGeometrySection', 'Game/Game.Prefabs/NetGeometrySection.cs', 'NetGeometrySection'),
    ('NetGeometryEdgeState', 'Game/Game.Prefabs/NetGeometryEdgeState.cs', 'NetGeometryEdgeState'),
    ('NetGeometryNodeState', 'Game/Game.Prefabs/NetGeometryNodeState.cs', 'NetGeometryNodeState'),
    ('MeshMaterial', 'Game/Game.Prefabs/MeshMaterial.cs', 'MeshMaterial'),
    ('GeometrySystem_EdgeData', 'Game/Game.Net/GeometrySystem.cs', 'EdgeData'),
    ('GeometrySystem_IntersectionData', 'Game/Game.Net/GeometrySystem.cs', 'IntersectionData'),
    ('CompositionParameters', 'Game/Game.Rendering/BatchDataHelpers.cs', 'CompositionParameters'),
]

ENUM_TYPES = {name for _, _, name in ENUMS}
ENUM_TYPES |= {'CompositionFlags.General', 'CompositionFlags.Side', 'RotationSymmetry', 'SubReplacementType',
               'ObjectRequirementFlags'}
SCALAR = {'float': 'float', 'int': 'int', 'uint': 'uint', 'byte': 'uint', 'ushort': 'uint', 'bool': 'uint',
          'Entity': 'int', 'quaternion': 'float4'}
VECTOR = {'float2', 'float3', 'float4', 'int2', 'int3', 'int4', 'uint2', 'uint3', 'uint4'}
# Unity matrices are column structs (c0..c3); HLSL matrix types would change the packing.
MATRIX = {'float3x4': 'Float3x4', 'float4x4': 'Float4x4'}
NAMED = {'Bounds1', 'Bounds2', 'Bounds3', 'Bezier4x2', 'Bezier4x3'}
SKIP = {'EntityArchetype'}


def body(text, header):
    start = text.index(header)
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[opening + 1:end - 1]


def parse_value(token, previous):
    token = token.strip()
    if token == '':
        return previous + 1
    if token == 'uint.MaxValue':
        return 0xFFFFFFFF
    token = token.rstrip('uU')
    if token.lower().startswith('0x'):
        return int(token, 16)
    return int(token)


def enum_values(path, name):
    text = (ORIGINAL / path).read_text(encoding='utf-8-sig')
    m = re.search(r'public enum ' + re.escape(name) + r'\b[^{]*', text)
    content = body(text, m.group())
    values, previous = [], -1
    for line in content.split('\n'):
        line = line.split('//')[0].strip().rstrip(',')
        if not line or line.startswith('['):
            continue
        key, _, value = line.partition('=')
        # Flag combinations (A | B) are folded from already-known members.
        if '|' in value:
            known = dict(values)
            number = 0
            for part in value.split('|'):
                number |= known[part.strip()]
        else:
            number = parse_value(value, previous)
        values.append((key.strip(), number))
        previous = number
    return values


def struct_fields(path, name):
    text = (ORIGINAL / path).read_text(encoding='utf-8-sig')
    content = body(text, 'public struct ' + name)
    # Remove nested blocks (methods, nested types) so only direct fields remain.
    flat, depth = [], 0
    for ch in content:
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
        elif depth == 0:
            flat.append(ch)
    fields = []
    for m in re.finditer(r'public ([A-Za-z0-9_.<>]+) (m_[A-Za-z0-9_]+)\s*;', ''.join(flat)):
        fields.append((m.group(1), m.group(2)))
    return fields


def field_type(cs_type):
    cs_type = cs_type.replace('Game.Objects.', '')
    if cs_type in SKIP:
        return None
    if cs_type in SCALAR:
        return SCALAR[cs_type]
    if cs_type in VECTOR or cs_type in NAMED:
        return cs_type
    if cs_type in MATRIX:
        return MATRIX[cs_type]
    if cs_type in {s for s, _, _ in STRUCTS}:
        return cs_type
    if cs_type in ENUM_TYPES:
        return 'uint'
    raise KeyError(f'unmapped C# field type {cs_type}')


def main():
    lines = [
        '// Generated by Scripts/CS2Road/generate_types.py from the decompiled Cities: Skylines II',
        '// sources (Port/Original of CitiesSkylines-Road2-C#). Do not edit by hand.',
        '// Shared by C++ (namespace CSRoad, after CSRoadMath.h) and HLSL (after CSRoadMath.ush).',
        '#pragma once',
        '',
        '#ifdef __cplusplus',
        'namespace CSRoad',
        '{',
        '#endif',
        '',
        '// Entities are 1-based on both sides so default-initialised fields mean Entity.Null, as in C#.',
        'static const int Entity_Null = 0;',
        '',
    ]
    for prefix, path, name in ENUMS:
        lines.append(f'// {path.split("/")[-1]}: {name}')
        for key, value in enum_values(path, name):
            lines.append(f'static const uint {prefix}_{key} = 0x{value:X}u;')
        lines.append('')
    flatten = []
    for out_name, path, name in STRUCTS:
        fields = [(field_type(t), n) for t, n in struct_fields(path, name)]
        fields = [(t, n) for t, n in fields if t]
        lines.append(f'struct {out_name}')
        lines.append('{')
        for t, n in fields:
            lines.append(f'\t{t} {n};')
        lines.append('};')
        lines.append('')
        # Field-order flattening used by the golden-data tests (same order as the C# exporter).
        flatten.append(f'inline void CSRoadFlatten(const {out_name}& V, TArray<double>& Out)')
        flatten.append('{')
        for _, n in fields:
            flatten.append(f'\tCSRoadFlatten(V.{n}, Out);')
        flatten.append('}')
    lines += ['#ifdef __cplusplus'] + flatten + ['} // namespace CSRoad', '#endif', '']
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text('\n'.join(lines), encoding='utf-8')
    print(f'wrote {OUT} ({len(ENUMS)} enums, {len(STRUCTS)} structs)')


if __name__ == '__main__':
    main()
