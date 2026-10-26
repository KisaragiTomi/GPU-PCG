// Golden-data exporter for the UE GPU road port.
// Runs the C# RoadLab port (verbatim CS2 algorithms + managed adapters) and writes
// composition, geometry, parameter, junction seam, deformed-mesh and refined-mesh reference data.
using System.Reflection;
using System.Text;
using System.Text.Json;
using Game.Net;
using Game.Prefabs;
using Game.Rendering;
using RoadPort;
using Unity.Entities;
using Unity.Mathematics;

static class Program
{
    static int Main(string[] args)
    {
        string lab = Environment.GetEnvironmentVariable("CS2ROADLAB") ?? @"D:\MyProject\CitiesSkylines-Road2-C#";
        if (args.Length == 0) { Console.Error.WriteLine("usage: GoldenDump <output-dir> [scenario ids...] [--no-mesh]"); return 2; }
        string outDir = args[0];
        var selected = args.Skip(1).ToHashSet();
        bool withMesh = !selected.Contains("--no-mesh");
        selected.Remove("--no-mesh");
        // --perturb=<seed>: nudge every curve x/z by -1, 0 or +1 ulp, to measure how sensitive the
        // reference itself is to rounding (the noise floor the GPU comparison has to live with).
        string perturbArg = selected.FirstOrDefault(a => a.StartsWith("--perturb="));
        int perturbSeed = perturbArg == null ? -1 : int.Parse(perturbArg["--perturb=".Length..]);
        // Perturbed exports keep the refined triangle counts (their noise floor) but no vertices.
        if (perturbArg != null) selected.Remove(perturbArg);
        Directory.CreateDirectory(outDir);
        var assets = new RoadAssets(Path.Combine(lab, "Assets", "CS2"));
        var jobs = new List<(string Id, RoadSpec[] Specs, Func<float3, float> Terrain, bool LeftHand)>();
        foreach (var prefab in new[] { "Small Road", "Medium Road Divided", "Highway Twoway - 2 lanes" })
            foreach (var s in RoadScenarios.Create(prefab))
            {
                if (s.Roads.Length == 0) continue;
                string id = prefab == "Small Road" ? s.Id : $"{s.Id}@{prefab.Replace(' ', '_')}";
                jobs.Add((id, s.Roads, null, false));
            }
        jobs.Add(("cross-lefthand", RoadScenarios.Create("Small Road").First(s => s.Id == "cross").Roads, null, true));
        string palmsland = Path.Combine(lab, "Artifacts", "public-saves", "palmsland-3", "network.json");
        if (File.Exists(palmsland) && (selected.Count == 0 || selected.Contains("palmsland")))
        {
            var network = SavedNetwork.Load(palmsland, assets);
            jobs.Add(("palmsland", network.Roads, network.Terrain.Sample, network.LeftHandTraffic));
        }
        foreach (var job in jobs)
        {
            if (selected.Count > 0 && !selected.Contains(job.Id)) continue;
            var started = DateTime.UtcNow;
            var specs = perturbSeed < 0 ? job.Specs : Perturb(job.Specs, perturbSeed);
            Export(assets, job.Id, specs, job.Terrain, job.LeftHand, outDir, withMesh, job.Id != "palmsland" && perturbSeed < 0);
            Console.WriteLine($"{job.Id}: {job.Specs.Length} roads, {(DateTime.UtcNow - started).TotalSeconds:F1}s");
        }
        File.WriteAllText(Path.Combine(outDir, "layouts.json"), JsonSerializer.Serialize(Layouts(), new JsonSerializerOptions { WriteIndented = true }));
        return 0;
    }

    static RoadSpec[] Perturb(RoadSpec[] specs, int seed)
    {
        var random = new System.Random(seed);
        float Nudge(float v)
        {
            int k = random.Next(-1, 2);
            return k == 0 ? v : k > 0 ? MathF.BitIncrement(v) : MathF.BitDecrement(v);
        }
        float3 P(float3 p) => new float3(Nudge(p.x), p.y, Nudge(p.z));
        return specs.Select(s => s with { Curve = new Colossal.Mathematics.Bezier4x3(P(s.Curve.a), P(s.Curve.b), P(s.Curve.c), P(s.Curve.d)) }).ToArray();
    }

    static Dictionary<string, string[]> Layouts()
    {
        var types = new[] { typeof(NodeGeometry), typeof(EdgeGeometry), typeof(EdgeNodeGeometry), typeof(NetCompositionData),
            typeof(NetCompositionPiece), typeof(NetCompositionLane), typeof(NetCompositionCrosswalk), typeof(NetCompositionCarriageway),
            typeof(BatchDataHelpers.CompositionParameters), typeof(BatchMeshHelpers.VertexData), typeof(NetGeometryData), typeof(NetData),
            typeof(RoadData), typeof(CompositionFlags) };
        return types.ToDictionary(t => t.Name, t => { var names = new List<string>(); Names(t, "", names); return names.ToArray(); });
    }

    static IEnumerable<FieldInfo> Fields(Type t) =>
        t.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic).OrderBy(f => f.MetadataToken);

    static bool Leaf(Type t) => t.IsPrimitive || t.IsEnum || t == typeof(half) || t == typeof(Entity);

    static void Names(Type t, string prefix, List<string> names)
    {
        if (Leaf(t)) { names.Add(prefix.TrimEnd('.')); return; }
        foreach (var f in Fields(t)) Names(f.FieldType, prefix + Clean(f.Name) + ".", names);
    }

    static string Clean(string name) => name.StartsWith("<") ? name[1..name.IndexOf('>')] : name;

    static void Flatten(object value, List<double> output)
    {
        switch (value)
        {
            case float f: output.Add(f); return;
            case half h: output.Add((float)h); return;
            case Entity e: output.Add(e.Index); return;
            case bool b: output.Add(b ? 1 : 0); return;
        }
        var t = value.GetType();
        if (t.IsEnum) { output.Add(Convert.ToUInt64(value)); return; }
        if (t.IsPrimitive) { output.Add(Convert.ToDouble(value)); return; }
        foreach (var f in Fields(t)) Flatten(f.GetValue(value), output);
    }

    static double[] Flat(object value) { var list = new List<double>(); Flatten(value, list); return list.ToArray(); }

    sealed class Bin
    {
        private readonly BinaryWriter _writer;
        public Bin(string path) { _writer = new BinaryWriter(File.Create(path)); }
        public long Floats(IEnumerable<float> values) { long offset = _writer.BaseStream.Position; foreach (var v in values) _writer.Write(v); return offset; }
        public long Ints(IEnumerable<int> values) { long offset = _writer.BaseStream.Position; foreach (var v in values) _writer.Write(v); return offset; }
        public long Bytes(byte[] values) { long offset = _writer.BaseStream.Position; _writer.Write(values); return offset; }
        public void Close() => _writer.Dispose();
    }

    // withMesh: compile and refine the node meshes (RoadRenderer's surface path); withVertices: also
    // write composition meshes and every deformed vertex (scenarios; Palmsland only gets counts).
    // RoadRenderer.BuildSteps without Godot: every deformed road surface feeds the node receivers
    // (RoadSurfaceProjection) the way RoadRenderer.Add does, then RoadLab's own RoadDecals runs with the
    // lane graph. Writes the lane graph, the crosswalk curves and, per decal surface, every vertex
    // (Unity-space position, normal, uv, uv2, colour: 14 floats) and the indices.
    static object Decals(RoadWorld world, RoadAssets assets, RoadMeshes meshes, Dictionary<(Entity, bool), RoadJunctionSurface> junctions,
        Dictionary<Entity, int> nodeIndex, Bin bin)
    {
        var projections = new Dictionary<Entity, RoadSurfaceProjection>();
        RoadSurfaceProjection Projection(Entity node) { if (!projections.TryGetValue(node, out var p)) projections[node] = p = new(); return p; }
        void Surface(RoadComposition composition, BatchDataHelpers.CompositionParameters parameters, RoadSurfaceProjection projection,
            RoadSurfaceProjection startProjection, RoadSurfaceProjection endProjection, float3 start, float3 end, RoadJunctionSurface junction)
        {
            var data = meshes.Compile(composition);
            if (data.Vertices.Length == 0) return;
            if ((composition.Data.m_Flags.m_General & CompositionFlags.General.Node) != 0) data = RoadMeshRefinement.Refine(data, parameters, junction);
            foreach (var material in data.Materials)
            {
                if (material.m_IndexCount == 0) continue;
                var positions = new float3[material.m_VertexCount];
                var normals = new float3[material.m_VertexCount];
                for (int i = 0; i < material.m_VertexCount; i++)
                {
                    var v = RoadMeshes.Deform(data.Vertices[material.m_StartVertex + i], parameters, world.TerrainSample, junction);
                    positions[i] = v.Position; normals[i] = v.Normal;
                }
                for (int i = 0; i < material.m_IndexCount; i += 3)
                {
                    int a = data.Indices[material.m_StartIndex + i] - material.m_StartVertex;
                    int b = data.Indices[material.m_StartIndex + i + 1] - material.m_StartVertex;
                    int c = data.Indices[material.m_StartIndex + i + 2] - material.m_StartVertex;
                    var normal = math.normalizesafe(normals[a] + normals[b] + normals[c]);
                    if (projection != null) { projection.Add(positions[a], positions[b], positions[c], normal); continue; }
                    float radius = composition.Data.m_Width * 2, radius2 = radius * radius;
                    if (math.min(math.distancesq(positions[a].xz, start.xz), math.min(math.distancesq(positions[b].xz, start.xz), math.distancesq(positions[c].xz, start.xz))) < radius2)
                        startProjection.Add(positions[a], positions[b], positions[c], normal);
                    if (math.min(math.distancesq(positions[a].xz, end.xz), math.min(math.distancesq(positions[b].xz, end.xz), math.distancesq(positions[c].xz, end.xz))) < radius2)
                        endProjection.Add(positions[a], positions[b], positions[c], normal);
                }
            }
        }
        foreach (var road in world.Roads)
        {
            var edge = world.Data<Edge>()[road.Entity];
            var geometry = world.Data<EdgeGeometry>()[road.Entity];
            BatchDataHelpers.CalculateEdgeParameters(geometry, false, out var parameters);
            Surface(road.EdgeComposition, parameters, null, Projection(edge.m_Start), Projection(edge.m_End),
                (geometry.m_Start.m_Left.a + geometry.m_Start.m_Right.a) * .5f, (geometry.m_End.m_Left.d + geometry.m_End.m_Right.d) * .5f, null);
            BatchDataHelpers.CalculateNodeParameters(world.Data<StartNodeGeometry>()[road.Entity].m_Geometry, road.StartComposition.Data, out parameters);
            Surface(road.StartComposition, parameters, Projection(edge.m_Start), null, null, default, default, junctions[(road.Entity, true)]);
            BatchDataHelpers.CalculateNodeParameters(world.Data<EndNodeGeometry>()[road.Entity].m_Geometry, road.EndComposition.Data, out parameters);
            Surface(road.EndComposition, parameters, Projection(edge.m_End), null, null, default, default, junctions[(road.Entity, false)]);
        }

        var graph = new RoadLanes(world);
        var builder = new RoadDecals(assets);
        var root = new Godot.Node3D();
        builder.Begin(world.LeftHandTraffic, graph);
        // Per surface the vertex and index count after each road's Add and each AddConnection, so a
        // comparison can resynchronise when one block differs. RoadDecals keeps its batches private.
        var batchesField = typeof(RoadDecals).GetField("_batches", BindingFlags.NonPublic | BindingFlags.Instance);
        var blocks = new List<Dictionary<string, int[]>>();
        void Block()
        {
            var counts = new Dictionary<string, int[]>();
            foreach (System.Collections.DictionaryEntry entry in (System.Collections.IDictionary)batchesField.GetValue(builder))
            {
                var tool = (Godot.SurfaceTool)entry.Value.GetType().GetField("Item1").GetValue(entry.Value);
                counts[(string)entry.Key] = new[] { tool.VertexCount, tool.IndexCount };
            }
            blocks.Add(counts);
        }
        foreach (var road in world.Roads)
        {
            var edge = world.Data<Edge>()[road.Entity];
            builder.Add(root, world, road, world.Data<EdgeGeometry>()[road.Entity], Projection(edge.m_Start), Projection(edge.m_End));
            Block();
        }
        foreach (var lane in graph.Connections)
        {
            if (projections.TryGetValue(lane.Junction, out var projection)) builder.AddConnection(lane, projection);
            Block();
        }
        builder.Finish(root);

        var roadIndex = world.Roads.Select((r, i) => (r.Entity, i)).ToDictionary(p => p.Entity, p => p.i);
        var lanes = graph.Lanes.Select(l => new
        {
            road = roadIndex[l.Road],
            definition = (int)l.Definition.m_Index,
            connector = l.IsConnector,
            reverse = l.Reverse,
            start = nodeIndex[l.StartNode],
            end = nodeIndex[l.EndNode],
            junction = l.IsConnector ? nodeIndex[l.Junction] : -1,
            from = l.From,
            to = l.To,
            curves = l.Curves.Select(c => Flat(c)).ToArray(),
            shared = l.IsConnector ? new[] { (int)l.NodeData.m_SharedStartCount, (int)l.NodeData.m_SharedEndCount } : null,
            connected = l.IsConnector ? null : new[] { (int)l.EdgeData.m_ConnectedStartCount, (int)l.EdgeData.m_ConnectedEndCount },
        }).ToArray();
        var crosswalks = world.Roads.Select(road => new[] { true, false }.Select(start => RoadCrosswalks.Create(world, road, start)
            .Select(c => new { lane = c.Lane.Index, curve = Flat(c.Curve), marking = Flat(c.MarkingCurve) }).ToArray()).ToArray()).ToArray();
        var surfaces = root.Children.OfType<Godot.MeshInstance3D>().Select(instance =>
        {
            var mesh = (Godot.ArrayMesh)instance.Mesh;
            // RoadRenderer.V flips z into Godot space; undo it.
            long offset = bin.Floats(Enumerable.Range(0, mesh.Positions.Count).SelectMany(i => new[]
            {
                mesh.Positions[i].X, mesh.Positions[i].Y, -mesh.Positions[i].Z, mesh.Normals[i].X, mesh.Normals[i].Y, -mesh.Normals[i].Z,
                mesh.UVs[i].X, mesh.UVs[i].Y, mesh.UV2s[i].X, mesh.UV2s[i].Y, mesh.Colors[i].R, mesh.Colors[i].G, mesh.Colors[i].B, mesh.Colors[i].A,
            }));
            long indices = bin.Ints(mesh.Indices);
            return new { surface = instance.Name["Lane decals ".Length..], vertexCount = mesh.Positions.Count, indexCount = mesh.Indices.Count, vertices = offset, indices };
        }).ToArray();
        return new { lanes, crosswalks, surfaces, blocks };
    }

    static void Export(RoadAssets assets, string id, RoadSpec[] specs, Func<float3, float> terrain, bool leftHand, string outDir, bool withMesh, bool withVertices)
    {
        var world = new RoadWorld(assets, terrain, leftHand);
        world.Build(specs);
        var bin = new Bin(Path.Combine(outDir, id + ".bin"));
        var compositionIds = new Dictionary<RoadComposition, int>(ReferenceEqualityComparer.Instance);
        var compositions = new List<object>();
        var meshes = withMesh ? new RoadMeshes(assets) : null;
        withVertices &= withMesh;
        var prefabNames = assets.RoadNames.ToDictionary(n => assets.RoadId(n).Index, n => n);

        // RoadJunctionSurface keeps its seam partners private: read them by reflection and name each
        // side by its parameter slot (road * 3 + 1 start, + 2 end) * 2 + side, the GPU's numbering.
        var junctions = RoadJunctionSurface.Create(world);
        var surfaceSlots = new Dictionary<RoadJunctionSurface, int>(ReferenceEqualityComparer.Instance);
        for (int r = 0; r < world.Roads.Count; r++)
        {
            surfaceSlots[junctions[(world.Roads[r].Entity, true)]] = r * 3 + 1;
            surfaceSlots[junctions[(world.Roads[r].Entity, false)]] = r * 3 + 2;
        }
        var sidesField = typeof(RoadJunctionSurface).GetField("_sides", BindingFlags.NonPublic | BindingFlags.Instance);
        int[] Partners(RoadJunctionSurface surface)
        {
            var sides = (Array)sidesField.GetValue(surface);
            var result = new int[sides.Length];
            for (int i = 0; i < sides.Length; i++)
            {
                object side = sides.GetValue(i);
                var type = side.GetType();
                object partner = type.GetField("Partner").GetValue(side);
                result[i] = partner == null ? -1 : surfaceSlots[(RoadJunctionSurface)type.GetField("Surface").GetValue(partner)] * 2 + (int)type.GetField("Index").GetValue(partner);
            }
            return result;
        }

        int Composition(RoadComposition c, string prefab)
        {
            if (compositionIds.TryGetValue(c, out int index)) return index;
            index = compositions.Count; compositionIds[c] = index;
            object mesh = null;
            if (withVertices)
            {
                var compiled = meshes.Compile(c);
                mesh = new
                {
                    vertexCount = compiled.Vertices.Length,
                    indexCount = compiled.Indices.Length,
                    materials = compiled.Materials.Select(m => new { start = m.m_StartIndex, count = m.m_IndexCount, startVertex = m.m_StartVertex, vertexCount = m.m_VertexCount, atlas = meshes.Atlases[m.m_MaterialIndex] }).ToArray(),
                    // Raw 32-byte VertexData (float3, packed normal, packed tangent, Color32, half4).
                    vertices = bin.Bytes(System.Runtime.InteropServices.MemoryMarshal.AsBytes(compiled.Vertices.AsSpan()).ToArray()),
                    indices = bin.Ints(compiled.Indices),
                };
            }
            compositions.Add(new
            {
                prefab,
                data = Flat(c.Data),
                pieces = c.Pieces.Select(p => Flat(p)).ToArray(),
                lanes = c.Lanes.Select(l => Flat(l)).ToArray(),
                carriageways = c.Carriageways.Select(w => Flat(w)).ToArray(),
                crosswalks = c.Crosswalks.Select(x => Flat(x)).ToArray(),
                mesh,
            });
            return index;
        }

        // As RoadRenderer.Add draws it: node instances deform through their junction surface, and node
        // compositions are refined first. deformed: the unrefined composition mesh (8 floats per vertex);
        // refined.corners: the refined triangles' corners in index order (8 floats each).
        static IEnumerable<float> Floats((float3 Position, float3 Normal, float2 UV) d) =>
            new[] { d.Position.x, d.Position.y, d.Position.z, d.Normal.x, d.Normal.y, d.Normal.z, d.UV.x, d.UV.y };
        object Instance(RoadComposition composition, string prefab, BatchDataHelpers.CompositionParameters parameters, RoadJunctionSurface junction)
        {
            int index = Composition(composition, prefab);
            int[] partners = junction == null ? null : Partners(junction);
            if (meshes == null) return new { composition = index, parameters = Flat(parameters), junction = partners };
            var compiled = meshes.Compile(composition);
            object refined = null;
            if ((composition.Data.m_Flags.m_General & CompositionFlags.General.Node) != 0 && compiled.Vertices.Length > 0)
            {
                var mesh = RoadMeshRefinement.Refine(compiled, parameters, junction);
                long corners = -1;
                if (withVertices)
                {
                    var d = mesh.Vertices.Select(v => RoadMeshes.Deform(v, parameters, terrain, junction)).ToArray();
                    corners = bin.Floats(mesh.Indices.SelectMany(i => Floats(d[i])));
                }
                refined = new { vertices = mesh.Vertices.Length, triangles = mesh.Indices.Length / 3, corners };
            }
            if (!withVertices) return new { composition = index, parameters = Flat(parameters), junction = partners, refined };
            long offset = bin.Floats(compiled.Vertices.SelectMany(v => Floats(RoadMeshes.Deform(v, parameters, terrain, junction))));
            return new { composition = index, parameters = Flat(parameters), junction = partners, deformed = offset, refined };
        }

        var nodeIndex = world.Nodes.Select((n, i) => (n, i)).ToDictionary(p => p.n, p => p.i);
        var nodes = world.Nodes.Select(n => new
        {
            entity = n.Index,
            prefab = prefabNames.GetValueOrDefault(world.Data<PrefabRef>()[n].m_Prefab.Index),
            position = Flat(world.Data<Game.Net.Node>()[n].m_Position),
            rotation = Flat(world.Data<Game.Net.Node>()[n].m_Rotation),
            elevation = world.Data<Elevation>().TryGetComponent(n, out var e) ? Flat(e.m_Elevation) : null,
            upgraded = world.Data<Upgraded>().TryGetComponent(n, out var u) ? Flat(u.m_Flags) : null,
            outside = world.Data<OutsideConnection>().HasComponent(n),
            owner = world.Data<Game.Common.Owner>().TryGetComponent(n, out var o) ? o.m_Owner.Index : (int?)null,
            connected = world.Buffers<ConnectedEdge>()[n].ToArray().Select(c => c.m_Edge.Index).ToArray(),
            geometry = Flat(world.Data<NodeGeometry>()[n]),
        }).ToArray();
        var edges = world.Roads.Select(r =>
        {
            var edge = world.Data<Edge>()[r.Entity];
            var geometry = world.Data<EdgeGeometry>()[r.Entity];
            var start = world.Data<StartNodeGeometry>()[r.Entity].m_Geometry;
            var end = world.Data<EndNodeGeometry>()[r.Entity].m_Geometry;
            BatchDataHelpers.CalculateEdgeParameters(geometry, false, out var edgeParameters);
            BatchDataHelpers.CalculateNodeParameters(start, r.StartComposition.Data, out var startParameters);
            BatchDataHelpers.CalculateNodeParameters(end, r.EndComposition.Data, out var endParameters);
            return new
            {
                entity = r.Entity.Index,
                prefab = r.Spec.Prefab,
                start = nodeIndex[edge.m_Start],
                end = nodeIndex[edge.m_End],
                curve = Flat(world.Data<Curve>()[r.Entity]),
                elevation = world.Data<Elevation>().TryGetComponent(r.Entity, out var ee) ? Flat(ee.m_Elevation) : null,
                upgraded = world.Data<Upgraded>().TryGetComponent(r.Entity, out var uu) ? Flat(uu.m_Flags) : null,
                owner = world.Data<Game.Common.Owner>().TryGetComponent(r.Entity, out var oo) ? oo.m_Owner.Index : (int?)null,
                edgeFlags = Flat(r.EdgeComposition.Data.m_Flags),
                startFlags = Flat(r.StartComposition.Data.m_Flags),
                endFlags = Flat(r.EndComposition.Data.m_Flags),
                edgeGeometry = Flat(geometry),
                startGeometry = Flat(start),
                endGeometry = Flat(end),
                edgeInstance = Instance(r.EdgeComposition, r.Spec.Prefab, edgeParameters, null),
                startInstance = Instance(r.StartComposition, r.Spec.Prefab, startParameters, junctions[(r.Entity, true)]),
                endInstance = Instance(r.EndComposition, r.Spec.Prefab, endParameters, junctions[(r.Entity, false)]),
            };
        }).ToArray();
        object decals = withVertices ? Decals(world, assets, meshes, junctions, nodeIndex, bin) : null;
        bin.Close();
        var document = new { id, leftHand, roads = specs.Length, nodes, edges, compositions, decals };
        File.WriteAllText(Path.Combine(outDir, id + ".json"), JsonSerializer.Serialize(document));
    }
}
