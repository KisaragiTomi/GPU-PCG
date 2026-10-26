// The slice of the Godot API that RoadLab's App/RoadDecals.cs touches, so GoldenDump can compile that
// file unchanged: SurfaceTool records what is added, resources resolve res:// against the RoadLab
// checkout, and nothing renders. RoadRenderer.V is the Unity -> Godot conversion RoadDecals calls.
using System.Collections.Generic;

namespace Godot
{
    public struct Vector2 { public float X, Y; public Vector2(float x, float y) { X = x; Y = y; } }
    public struct Vector3 { public float X, Y, Z; public Vector3(float x, float y, float z) { X = x; Y = y; Z = z; } }
    public struct Color { public float R, G, B, A; public Color(float r, float g, float b, float a = 1) { R = r; G = g; B = b; A = a; } }
    public static class Colors { public static readonly Color White = new(1, 1, 1, 1); }

    public class Resource { }
    public class Shader : Resource { }
    public class Texture2D : Resource { }
    public class Material : Resource { }
    public class ShaderMaterial : Material
    {
        public Shader Shader { get; set; }
        public readonly Dictionary<string, object> Parameters = new();
        public void SetShaderParameter(string name, object value) => Parameters[name] = value;
    }

    public class Mesh : Resource { public enum PrimitiveType { Triangles } }

    // One committed surface: per vertex position, normal, uv, uv2 and colour, as SurfaceTool got them.
    public sealed class ArrayMesh : Mesh
    {
        public readonly List<Vector3> Positions = new(), Normals = new();
        public readonly List<Vector2> UVs = new(), UV2s = new();
        public readonly List<Color> Colors = new();
        public readonly List<int> Indices = new();
        public Material Material;
    }

    public sealed class SurfaceTool : System.IDisposable
    {
        private readonly ArrayMesh _mesh = new();
        private Vector3 _normal;
        private Vector2 _uv, _uv2;
        private Color _color;
        public void Begin(Mesh.PrimitiveType type) { }
        public void SetMaterial(Material material) => _mesh.Material = material;
        public void SetNormal(Vector3 normal) => _normal = normal;
        public void SetUV(Vector2 uv) => _uv = uv;
        public void SetUV2(Vector2 uv) => _uv2 = uv;
        public void SetColor(Color color) => _color = color;
        public void AddVertex(Vector3 position)
        {
            _mesh.Positions.Add(position); _mesh.Normals.Add(_normal); _mesh.UVs.Add(_uv); _mesh.UV2s.Add(_uv2); _mesh.Colors.Add(_color);
        }
        public void AddIndex(int index) => _mesh.Indices.Add(index);
        public int VertexCount => _mesh.Positions.Count;
        public int IndexCount => _mesh.Indices.Count;
        public void GenerateTangents() { }
        public ArrayMesh Commit() => _mesh;
        public void Dispose() { }
    }

    public class Node { public string Name { get; set; } public readonly List<Node> Children = new(); public void AddChild(Node child) => Children.Add(child); }
    public class Node3D : Node { }
    public class GeometryInstance3D : Node3D
    {
        public enum ShadowCastingSetting { Off, On }
        public ShadowCastingSetting CastShadow { get; set; }
    }
    public class MeshInstance3D : GeometryInstance3D { public Mesh Mesh { get; set; } }

    public static class GD { public static T Load<T>(string path) where T : new() => new T(); }

    public static class ResourceLoader
    {
        // res:// is the RoadLab checkout (the Godot project root).
        public static string Root = System.Environment.GetEnvironmentVariable("CS2ROADLAB") ?? @"D:\MyProject\CitiesSkylines-Road2-C#";
        public static bool Exists(string path) => System.IO.File.Exists(System.IO.Path.Combine(Root, path.Replace("res://", "")));
    }
}

public static class RoadRenderer
{
    public static Godot.Vector3 V(Unity.Mathematics.float3 p) => new(p.x, p.y, -p.z);
}
