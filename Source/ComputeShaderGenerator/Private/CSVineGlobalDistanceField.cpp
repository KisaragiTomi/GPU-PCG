#include "CSVineGlobalDistanceField.h"

#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "Math/DoubleFloat.h"
#include "SceneView.h"
#include <atomic>

FCSVineGDFParameters MakeVineGDFParameters(const FSceneView& View,
	const FGlobalDistanceFieldParameterData& Data, float MaxDistance, float Tolerance, int32 Iterations)
{
	FCSVineGDFParameters P{};
	P.Field = SetupGlobalDistanceFieldParameters_Minimal(Data);
	P.Field.GlobalDistanceFieldCoverageAtlasTexture = Data.CoverageAtlasTexture;
	P.Field.GlobalDistanceFieldCoverageAtlasTextureSampler = TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();
	P.Field.GlobalDistanceFieldPageAtlasTextureSampler = TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();
	P.Field.GlobalDistanceFieldMipTextureSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	const FDFVector3 Translation(View.ViewMatrices.GetPreViewTranslation());
	P.VineGDFTranslationHigh = Translation.High;
	P.VineGDFTranslationLow = Translation.Low;
	P.VineGDFMaxDistance = FMath::Max(MaxDistance, 1.0f);
	P.VineGDFTolerance = FMath::Clamp(Tolerance, 0.01f, P.VineGDFMaxDistance);
	P.VineGDFIterations = uint32(FMath::Clamp(Iterations, 1, 32));
	return P;
}

bool EnqueueVineGDFJob(UWorld* World, UObject* Owner, FGDFJobRequest&& Request,
	TFunction<void()> OnUnavailable, TFunction<bool()> IsCurrent)
{
	if (!World || !World->Scene || !Owner || !Request.Build || !Request.OnComplete) return false;
	// 0 waiting, 1 submitted to the renderer, 2 cancelled, 3 complete.
	auto State = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	Request.Scene = World->Scene;
	Request.IsCancelled = [State]() { return State->load() == 2; };
	Request.Build = [State, Build = MoveTemp(Request.Build)](FRDGBuilder& GraphBuilder, const FSceneView& View,
		const FGlobalDistanceFieldParameterData& Data, FGDFJobOutputs& Outputs) mutable
	{
		if (Data.NumGlobalSDFClipmaps <= 0 || Data.GlobalDFResolution <= 0.0f || !Data.PageAtlasTexture || !Data.PageTableTexture) return;
		int32 Waiting = 0;
		if (!State->compare_exchange_strong(Waiting, 1)) return;
		Build(GraphBuilder, View, Data, Outputs);
	};
	Request.OnComplete = [State, Complete = MoveTemp(Request.OnComplete)](FGDFJobOutputs& Outputs) mutable
	{
		int32 Submitted = 1;
		if (!State->compare_exchange_strong(Submitted, 3)) return;
		Complete(Outputs);
	};
	const double Deadline = FPlatformTime::Seconds() + 10.0;
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[State, Deadline, WeakOwner = TWeakObjectPtr<UObject>(Owner), IsCurrent = MoveTemp(IsCurrent), OnUnavailable = MoveTemp(OnUnavailable)](float) mutable
	{
		if (State->load() >= 2) return false;
		if (!WeakOwner.IsValid() || (IsCurrent && !IsCurrent()) || FPlatformTime::Seconds() >= Deadline)
		{
			int32 Waiting = 0;
			if (State->compare_exchange_strong(Waiting, 2))
			{
				UE_LOG(LogTemp, Warning, TEXT("[VineGDF] Request cancelled/owner destroyed, or no usable view/global distance field within 10 seconds."));
				if (OnUnavailable) OnUnavailable();
				return false;
			}
		}
		return true;
	}));
	FGDFSampleService::Get().EnqueueGDFJob(MoveTemp(Request));
	return true;
}
