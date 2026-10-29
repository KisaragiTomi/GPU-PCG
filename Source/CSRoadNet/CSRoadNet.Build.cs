using UnrealBuildTool;

// Cities: Skylines II road logic ported to UE. Composition selection and composition
// meshes run in C++; edge/node geometry and mesh deformation run as compute shaders
// that write straight into UCSMesh buffers (ComputeShaderGenerator).
public class CSRoadNet : ModuleRules
{
	public CSRoadNet(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		// The CPU half of the port is compared with the C# original value for value;
		// /fp:fast would let MSVC reorder or contract the float expressions.
		FPSemantics = FPSemanticsMode.Precise;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"RenderCore",
				"RHI",
				"ComputeShaderGenerator",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Projects",
				"Renderer",
				"Json",
				"AssetRegistry",
				"MeshDescription",
				"StaticMeshDescription",
			}
			);

		if (Target.Type == TargetType.Editor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}
	}
}
