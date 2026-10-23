#pragma once

#include "CoreMinimal.h"
#include "GlobalDistanceFieldParameters.h"
#include "GDFSampleService.h"

class UWorld;

BEGIN_SHADER_PARAMETER_STRUCT(FCSVineGDFParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FGlobalDistanceFieldParameters2, Field)
	SHADER_PARAMETER(FVector3f, VineGDFTranslationHigh)
	SHADER_PARAMETER(FVector3f, VineGDFTranslationLow)
	SHADER_PARAMETER(float, VineGDFMaxDistance)
	SHADER_PARAMETER(float, VineGDFTolerance)
	SHADER_PARAMETER(uint32, VineGDFIterations)
END_SHADER_PARAMETER_STRUCT()

FCSVineGDFParameters MakeVineGDFParameters(const FSceneView& View,
	const FGlobalDistanceFieldParameterData& Data, float MaxDistance, float Tolerance, int32 Iterations);

// Consumes GDF textures only inside the matching scene's renderer graph. Completion is on the
// game thread; an unavailable/non-rendering view times out without changing the previous vine.
bool EnqueueVineGDFJob(UWorld* World, UObject* Owner, FGDFJobRequest&& Request,
	TFunction<void()> OnUnavailable, TFunction<bool()> IsCurrent);
