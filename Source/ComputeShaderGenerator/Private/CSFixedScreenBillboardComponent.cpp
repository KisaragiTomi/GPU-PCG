#include "CSFixedScreenBillboardComponent.h"

#include "Engine/Texture2D.h"
#include "MeshElementCollector.h"
#include "PrimitiveSceneProxy.h"
#include "PrimitiveViewRelevance.h"
#include "SceneView.h"
#include "TextureResource.h"

class FCSFixedScreenSpriteSceneProxy final : public FPrimitiveSceneProxy
{
public:
	SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	explicit FCSFixedScreenSpriteSceneProxy(const UCSFixedScreenBillboardComponent* Component)
		: FPrimitiveSceneProxy(Component)
		, Texture(Component->Sprite)
		, ScreenPixelSize(Component->ScreenPixelSize)
		, OpacityMaskRefVal(Component->OpacityMaskRefVal)
	{
		bWillEverBeLit = false;
	}

	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap, FMeshElementCollector& Collector) const override
	{
		const FTexture* TextureResource = Texture ? Texture->GetResource() : nullptr;
		if (!TextureResource) return;

		const FVector Origin = GetLocalToWorld().GetOrigin();
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			if (!(VisibilityMap & (1u << ViewIndex))) continue;
			const FSceneView* View = Views[ViewIndex];
			const FMatrix& Projection = View->ViewMatrices.GetProjectionMatrix();
			const float Width = View->UnscaledViewRect.Width();
			const float Height = View->UnscaledViewRect.Height();
			const float W = View->WorldToScreen(Origin).W;
			if (W <= 0.0f || Width <= 0.0f || Height <= 0.0f ||
				FMath::IsNearlyZero(Projection.M[0][0]) || FMath::IsNearlyZero(Projection.M[1][1])) continue;

			// DrawSprite takes a world-space half-size. Projection converts this to ScreenPixelSize pixels.
			const float SizeX = ScreenPixelSize * W / (FMath::Abs(Projection.M[0][0]) * Width);
			const float SizeY = ScreenPixelSize * W / (FMath::Abs(Projection.M[1][1]) * Height);
			Collector.GetPDI(ViewIndex)->DrawSprite(Origin, SizeX, SizeY, TextureResource, FLinearColor::White,
				GetDepthPriorityGroup(View), 0.0f, 0.0f, 0.0f, 0.0f, SE_BLEND_Masked, OpacityMaskRefVal);
		}
	}

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Result;
		Result.bDrawRelevance = IsShown(View) && View->Family->EngineShowFlags.BillboardSprites;
		Result.bOpaque = true;
		Result.bDynamicRelevance = true;
		Result.bEditorPrimitiveRelevance = UseEditorCompositing(View);
		return Result;
	}

	virtual uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }

private:
	const UTexture2D* Texture;
	const float ScreenPixelSize;
	const float OpacityMaskRefVal;
};

FPrimitiveSceneProxy* UCSFixedScreenBillboardComponent::CreateSceneProxy()
{
	return new FCSFixedScreenSpriteSceneProxy(this);
}
