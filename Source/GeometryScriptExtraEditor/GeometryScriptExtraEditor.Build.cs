using System.IO;
using UnrealBuildTool;

public class GeometryScriptExtraEditor : ModuleRules
{
    public GeometryScriptExtraEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        // Avoid shared-PCH symbol/link mismatches in this editor-only module.
        PCHUsage = ModuleRules.PCHUsageMode.NoPCHs;
        bUseUnity = false;
        bUseRTTI = true;

        // Ensure our own Public headers are found before engine headers with
        // colliding names (e.g. PolyLine.h vs GeometryCore's Polyline.h).
        PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Public"));

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "ComputeShaderGenerator",
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "AssetRegistry",
                "ModelingOperators",
                "CoreUObject",
                "DeveloperSettings",
                "Engine",
                "EditorScriptingUtilities",
                "GeometryAlgorithms",
                "GeometryCore",
                "GeometryFramework",
                "GeometryScriptingCore",
                "Projects",
                "Slate",
                "ProxyLODMeshReduction",
                "SlateCore", 
                "Landscape",
                "Foliage",
                "DynamicMesh",
                "ModelingComponents",
                "ModelingComponentsEditorOnly",
                "MeshDescription",
                "StaticMeshDescription",
                "MeshConversion",
                "Renderer",
                "RenderCore",
                "RHI",
                "UnrealEd",
            }
        );

        AddEngineThirdPartyPrivateStaticDependencies(Target,
            "IntelTBB",
            "UVAtlas",
            "DirectXMesh",
            "OpenVDB",
            "Blosc",
            "zlib"
        );
    }
}
