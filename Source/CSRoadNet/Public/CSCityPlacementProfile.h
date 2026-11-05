#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "Engine/DataAsset.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CSCityPlacementProfile.generated.h"

class UStaticMesh;

UENUM(BlueprintType)
enum class ECSCityPlacementState : uint8 { PendingReview, Ready, Disabled };

/** A planar installation patch in the source mesh's built local space (cm), with its own topology. */
USTRUCT(BlueprintType)
struct CSROADNET_API FCSCityPlacementRegion
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	FGuid RegionId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	FName Label;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	TArray<FVector> Positions;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	TArray<FIntVector> Triangles;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	FVector OutwardNormal = FVector(0, 1, 0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	FVector PreferredUp = FVector(0, 0, 1);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	TArray<FName> AllowedCategories;
};

/** Shared semantic surface data, independent of render LODs. Ready means reviewed for this source revision,
 * not that every device fits: callers still verify device footprints, geometry, and neighbouring buildings. */
UCLASS(BlueprintType)
class CSROADNET_API UCSCityPlacementProfile : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	FGuid ProfileId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	int32 SchemaVersion = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	int32 DataRevision = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	TSoftObjectPtr<UStaticMesh> SourceMesh;

	/** SHA-256 of the exported built source geometry and material references, not render LOD indices. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	FString SourceGeometryHash;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	ECSCityPlacementState State = ECSCityPlacementState::PendingReview;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement", meta = (MultiLine = "true"))
	FString ReviewNotes;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	TArray<FCSCityPlacementRegion> Regions;

	/** Structural validation. Source-revision checking is the asset builder's responsibility. */
	UFUNCTION(BlueprintCallable, Category = "Placement")
	bool ValidateData(FString& Error) const;
};

/** A persistent default association on a source StaticMesh; the geometry belongs to the profile asset. */
UCLASS()
class CSROADNET_API UCSCityPlacementBinding : public UAssetUserData
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement")
	TObjectPtr<UCSCityPlacementProfile> Profile;
};

UCLASS()
class CSROADNET_API UCSCityPlacementLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Returns the association including PendingReview/Disabled states. Does not imply placement is permitted. */
	UFUNCTION(BlueprintPure, Category = "Placement")
	static UCSCityPlacementProfile* GetDefaultProfile(UStaticMesh* SourceMesh);

	/** Editor-only mutation. Validates ownership and data; saves neither asset. */
	UFUNCTION(BlueprintCallable, Category = "Placement")
	static bool SetDefaultProfile(UStaticMesh* SourceMesh, UCSCityPlacementProfile* Profile, FString& Error);
};
