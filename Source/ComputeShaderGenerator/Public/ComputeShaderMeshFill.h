#pragma once
//
#include "CoreMinimal.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/BoxComponent.h"
#include "GlobalShader.h"
#include "ComputeShaderSceneCapture.h"
#include "ShaderParameterStruct.h"

#include "ComputeShaderMeshFill.generated.h"
//
//
#define NUM_THREADS_PER_GROUP_DIMENSION_X 32
#define NUM_THREADS_PER_GROUP_DIMENSION_Y 32
#define NUM_THREADS_PER_GROUP_DIMENSION_Z 1
#define NUM_SAMPLE_MESH_THREADS_PER_GROUP_DIMENSION_X 16
#define NUM_SAMPLE_MESH_THREADS_PER_GROUP_DIMENSION_Y 16
#define NUM_SAMPLE_MESH_THREADS_PER_GROUP_DIMENSION_Z 1
#define SHAREGROUP_FINDEXT_SIZE 256

USTRUCT()
struct COMPUTESHADERGENERATOR_API FCSMeshFillData
{
	GENERATED_USTRUCT_BODY()
public:
	
	FVector2D RandomRotate = FVector2D(0, 0);
	FVector2D RandomScale = FVector2D(1, 1);
	FVector2D RandomHeightOffset = FVector2D(0, 0);
	float DrawScale = 1;
	float SpawnScaleMult = 1;
	int32 SelectIndex = -1;

};

class COMPUTESHADERGENERATOR_API FMeshFillMult : public FGlobalShader
{
public:
		
	enum class EMeshFillFunction : uint8
	{
		MF_InitVerticalRockHeight,
		MF_InitFillTarget,
		MF_InitTargetHeight,
		
		MF_General,
		MF_FillVerticalRock,
		MF_FillCappingRock,
		MF_UpdateCurrentHeight,
		MF_FindBestPixel,
		MF_FindBestPixelRW_512,
		MF_FindBestPixelRW_256,
		MF_TargetHeight,
		MF_ExtentGenerateMask,
		MF_UpdateCurrentHeightMult,
		MF_FillMeshMult,
		MF_Test,
		MAX
	};
	class FMeshFillFunction : SHADER_PERMUTATION_ENUM_CLASS("FMESHFILL", EMeshFillFunction);
	class FSceneNormalRotator : SHADER_PERMUTATION_BOOL("USESCENENORMALROTATOR");
	class FSceneLight : SHADER_PERMUTATION_BOOL("USESCENELIGHT");
	class FMask : SHADER_PERMUTATION_BOOL("USEMASK");
	using FPermutationDomain = TShaderPermutationDomain<FMeshFillFunction, FSceneNormalRotator, FSceneLight, FMask>;

	static TShaderMapRef<FMeshFillMult> CreateMeshFillPermutation(EMeshFillFunction Permutation, bool UseSceneNormalRotator = false, bool UseSceneLight = false, bool UseMask = false)
	{
		typename FMeshFillMult::FPermutationDomain PermutationVector;
		PermutationVector.Set<FMeshFillMult::FMeshFillFunction>(Permutation);
		PermutationVector.Set<FSceneNormalRotator>(UseSceneNormalRotator);
		PermutationVector.Set<FSceneLight>(UseSceneLight);
		PermutationVector.Set<FMask>(UseMask);
		TShaderMapRef<FMeshFillMult> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
		return ComputeShader;
	}
	//Declare this class as a global shader
	DECLARE_GLOBAL_SHADER(FMeshFillMult);
	//Tells the engine that this shader uses a structure for its parameters
	SHADER_USE_PARAMETER_STRUCT(FMeshFillMult, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_SceneDepth)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_SceneNormal)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_ObjectNormal)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_TMeshDepth)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_Result)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_TargetHeight)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_CurrentSceneDepth)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_ObjectDepth)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_TestMap)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_HeightNormal)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_SceneLight)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_Mask)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_WorldPosition)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray, TA_MeshHeight)
		
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_CurrentSceneDepth)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_CurrentSceneDepthA)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_CurrentSceneDepthB)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_TargetHeight)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_TempA)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_TempB)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_DebugView)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_Result)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_ResultA)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_ResultB)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_FilterResult)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_SaveRotateScale)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_Deduplication)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_FilterResulteMult)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, RWA_MeshHeight)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_FindPixelBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint2>, RW_FindPixelBufferResult_Number)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_FindPixelBufferResult_NumberCount)

	
		SHADER_PARAMETER(float, CaptureSize)
		SHADER_PARAMETER(float, Size)
		SHADER_PARAMETER(float, HeightOffset)
		SHADER_PARAMETER(float, HeightDiff)
		SHADER_PARAMETER(float, GenerateThreshold)
		SHADER_PARAMETER(float, UnGenerateThreshold)
		SHADER_PARAMETER(FVector4f, RandomRange)
		SHADER_PARAMETER(FVector3f, CameraDir)
		SHADER_PARAMETER(FMatrix44f, ActorTransform)
	
		SHADER_PARAMETER(int, SelectIndex)
		SHADER_PARAMETER_SAMPLER(SamplerState, Sampler)
	END_SHADER_PARAMETER_STRUCT()
public:
	//Called by the engine to determine which permutations to compile for this shader
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}
	//Modifies the compilations environment of the shader
	static inline void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		//We're using it here to add some preprocessor defines. That way we don't have to change both C++ and HLSL code 
		// when we change the value for NUM_THREADS_PER_GROUP_DIMENSION
		
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), NUM_THREADS_PER_GROUP_DIMENSION_X);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), NUM_THREADS_PER_GROUP_DIMENSION_Y);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Z"), NUM_THREADS_PER_GROUP_DIMENSION_Z);
		
		static const TCHAR* ShaderSourceModeDefineName[] =
		{
			TEXT("FMESHFILL_INITVERTICALROCKHEIGHT"),
			TEXT("FMESHFILL_INITFILLTARGET"),
			TEXT("FMESHFILL_INITTARGETHEIGHT"),
			TEXT("FMESHFILL_GENERAL"),
			TEXT("FMESHFILL_FILLVERTICALROCK"),
			TEXT("FMESHFILL_FILLCAPPINGROCK"),
			TEXT("FMESHFILL_UPDATECURRENTHEIGHT"),
			TEXT("FMESHFILL_FINDBESTPIXEL"),
			TEXT("FMESHFILL_FINDBESTPIXELRW_512"),
			TEXT("FMESHFILL_FINDBESTPIXELRW_256"),
			TEXT("FMESHFILL_TARGETHEIGHT"),
			TEXT("FMESHFILL_EXTENTGENERATEMASK"),
			TEXT("FMESHFILL_UPDATECURRENTHEIGHTMULT"),
			TEXT("FMESHFILL_FILLMESHMULT"),
			TEXT("FMESHFILL_TEST")
		}; 
		static_assert(UE_ARRAY_COUNT(ShaderSourceModeDefineName) == (uint32)EMeshFillFunction::MAX, "Enum doesn't match define table.");

		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const uint32 SourceModeIndex = static_cast<uint32>(PermutationVector.Get<FMeshFillFunction>());
		OutEnvironment.SetDefine(ShaderSourceModeDefineName[SourceModeIndex], 1u);

		OutEnvironment.SetDefine(TEXT("MAX_HEIGHT"), 10000);
		//OutEnvironment.SetDefine(TEXT("FINDPIXELTHREADSIZE"), 256);

		if (PermutationVector.Get<FMeshFillFunction>() == EMeshFillFunction::MF_FillVerticalRock || PermutationVector.Get<FMeshFillFunction>() == EMeshFillFunction::MF_FillMeshMult )
		{
			OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), NUM_SAMPLE_MESH_THREADS_PER_GROUP_DIMENSION_X);
			OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), NUM_SAMPLE_MESH_THREADS_PER_GROUP_DIMENSION_Y);
		}
		if (PermutationVector.Get<FMeshFillFunction>() == EMeshFillFunction::MF_FindBestPixel)
		{
			OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), SHAREGROUP_FINDEXT_SIZE);
			OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), 1);
		}
		if (PermutationVector.Get<FMeshFillFunction>() == EMeshFillFunction::MF_FindBestPixelRW_512)
		{
			OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 512 * 512 / SHAREGROUP_FINDEXT_SIZE);
			OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), 1);
		}
		if (PermutationVector.Get<FMeshFillFunction>() == EMeshFillFunction::MF_FindBestPixelRW_256)
		{
			OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), 16);
			OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), 16);
		}
		if (PermutationVector.Get<FMeshFillFunction>() == EMeshFillFunction::MF_UpdateCurrentHeightMult)
		{
			OutEnvironment.SetDefine(TEXT("SHARETEXTURESIZE"), 32);
		}
	}
};

// IMPLEMENT_GLOBAL_SHADER moved to ComputeShaderBasicFunction.cpp

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSGenerateParameter
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	UTexture2D* TMeshDepth;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	UTextureRenderTarget2D* Mask;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	UTextureRenderTarget2D* SceneDepth;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	UTextureRenderTarget2D* DebugView;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	UTextureRenderTarget2D* Result;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	UTextureRenderTarget2D* SceneNormal;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	UTextureRenderTarget2D* CurrentSceneDepth;


	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float Size = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float RandomRotation = 0;

	bool IsValid()
	{
		return TMeshDepth != nullptr && SceneDepth != nullptr &&
			SceneNormal != nullptr && DebugView != nullptr && Result != nullptr;
	}
	bool IsValidMult()
	{
		return TMeshDepth != nullptr && SceneDepth != nullptr &&
			SceneNormal != nullptr && DebugView != nullptr && Result != nullptr ;
	}
};

UCLASS()
class COMPUTESHADERGENERATOR_API ACSFillTarget : public ACSGenerateCaptureScene
{
	GENERATED_BODY()
public:
	
	UPROPERTY(BlueprintReadWrite, Category = "FillTarget")
	UTextureRenderTarget2D* RT_CurrentSceneDepth;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "FillTarget")
	UTextureRenderTarget2D* RT_SceneLight;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "FillTarget")
	UTextureRenderTarget2D* RT_Mask;
	
	UPROPERTY(BlueprintReadWrite, Category = "FillTarget")
	UTextureRenderTarget2D* T_TestMap;

	UPROPERTY(BlueprintReadWrite, Category = "FillTarget")
	USceneCaptureComponent2D* CaptureLight;
	UPROPERTY(BlueprintReadWrite, Category = "FillTarget")
	USceneCaptureComponent2D* CapturePostMask;
	
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "FillTarget")
	float HeightDiff = 200;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "FillTarget")
	float SpawnSize = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "FillTarget")
	float RandomRotation = 0;

	ACSFillTarget();

	virtual void OnConstruction(const FTransform& Transform) override;

	virtual void CheckTexture() override;

	virtual void CaptureMeshsInBox() override;
	
	bool IsParameterValidMult()
	{
		bool Check = true;

		if (MeshDataAssets.Num() == 0) Check = false;
		if (RT_SceneDepth == nullptr) Check = false;
		if (RT_SceneNormal == nullptr) Check = false;
		if (RT_DebugView == nullptr) Check = false;
		if (RT_Result == nullptr) Check = false;
		if (RT_CurrentSceneDepth == nullptr) Check = false;
		return Check;
	}

	void FillTargetCal(TArray<FCSMeshFillData> GenerateDatas);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void FillTarget(int32 NumIteration = 1, float InSpawnSize = 1);

	virtual  void Generate(int32 NumIteration = 1, float InSpawnSize = 1) override;
	
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void CurvaturePostTest();
	
	
};

