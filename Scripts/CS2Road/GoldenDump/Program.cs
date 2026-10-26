// Golden-data exporter for the UE GPU road port.
// Runs the C# RoadLab port (verbatim CS2 algorithms + managed adapters) and writes
// composition, geometry, parameter and deformed-mesh reference data.
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
        if (perturbArg != null) { selected.Remove(perturbArg); withMesh = false; }
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
            Export(assets, job.Id, specs, job.Terrain, job.LeftHand, outDir, withMesh && job.Id != "palmsland");
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

    static void Export(RoadAssets assets, string id, RoadSpec[] specs, Func<float3, float> terrain, bool leftHand, string outDir, bool withMesh)
    {
        var world = new RoadWorld(assets, terrain, leftHand);
        world.Build(specs);
        var bin = new Bin(Path.Combine(outDir, id + ".bin"));
        var compositionIds = new Dictionary<RoadComposition, int>(ReferenceEqualityComparer.Instance);
        var compositions = new List<object>();
        var meshes = withMesh ? new RoadMeshes(assets) : null;
        var prefabNames = assets.RoadNames.ToDictionary(n => assets.RoadId(n).Index, n => n);

        int Composition(RoadComposition c, string prefab)
        {
            if (compositionIds.TryGetValue(c, out int index)) return index;
            index = compositions.Count; compositionIds[c] = index;
            object mesh = null;
            if (meshes != null)
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

        object Instance(RoadComposition composition, string prefab, BatchDataHelpers.CompositionParameters parameters)
        {
            int index = Composition(composition, prefab);
            if (meshes == null) return new { composition = index, parameters = Flat(parameters) };
            var compiled = meshes.Compile(composition);
            var deformed = compiled.Vertices.Select(v => RoadMeshes.Deform(v, parameters, terrain, null)).ToArray();
            long offset = bin.Floats(deformed.SelectMany(d => new[] { d.Position.x, d.Position.y, d.Position.z, d.Normal.x, d.Normal.y, d.Normal.z, d.UV.x, d.UV.y }));
            return new { composition = index, parameters = Flat(parameters), deformed = offset };
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
                edgeInstance = Instance(r.EdgeComposition, r.Spec.Prefab, edgeParameters),
                startInstance = Instance(r.StartComposition, r.Spec.Prefab, startParameters),
                endInstance = Instance(r.EndComposition, r.Spec.Prefab, endParameters),
            };
        }).ToArray();
        bin.Close();
        var document = new { id, leftHand, roads = specs.Length, nodes, edges, compositions };
        File.WriteAllText(Path.Combine(outDir, id + ".json"), JsonSerializer.Serialize(document));
    }
}
