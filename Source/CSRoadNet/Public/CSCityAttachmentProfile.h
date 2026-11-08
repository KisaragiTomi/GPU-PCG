#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CSCityAttachmentProfile.generated.h"

class UStaticMesh;

UENUM(BlueprintType)
enum class ECSCityAttachmentState : uint8 { PendingReview, Ready, Disabled };

UENUM(BlueprintType)
enum class ECSCityAttachmentAnchorRole : uint8
{
	/** Attaches to a verified building surface. Never mates with another object's anchor. */
	SurfaceMount,
	Plug,
	Socket,
	/** A symmetric interface, e.g. the endpoints of a cable. */
	Peer
};

/** A reusable connection frame, independent of render triangles and scene instance indices. */
USTRUCT(BlueprintType)
struct CSROADNET_API FCSCityAttachmentAnchor
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attachment")
	FGuid AnchorId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attachment")
	FName Label;

	/** Built source mesh local space, cm. Unit scale; +X faces the mate, +Z is the preferred up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attachment")
	FTransform LocalFrame = FTransform::Identity;

	/** Exact interface family, e.g. AirConditioner.D_F. Does not imply collision clearance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attachment")
	FName InterfaceType;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attachment")
	ECSCityAttachmentAnchorRole Role = ECSCityAttachmentAnchorRole::SurfaceMount;

	/** Capacity per (instance, anchor), enforced by the future scene connection graph. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attachment", meta = (ClampMin = "1"))
	int32 MaxConnections = 1;
};

/** Object-side connections. Building-side placement permissions belong to the assembled building. */
UCLASS(BlueprintType)
class CSROADNET_API UCSCityAttachmentProfile : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attachment")
	FGuid ProfileId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attachment")
	int32 SchemaVersion = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attachment")
	int32 DataRevision = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attachment")
	TSoftObjectPtr<UStaticMesh> SourceMesh;

	/** SHA-256, using the same built-source export as placement profiles. Recheck after reimport. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attachment")
	FString SourceGeometryHash;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attachment")
	ECSCityAttachmentState State = ECSCityAttachmentState::PendingReview;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attachment", meta = (MultiLine = "true"))
	FString ReviewNotes;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attachment")
	TArray<FCSCityAttachmentAnchor> Anchors;

	/** Structural validation only; does not load the source or recheck its current fingerprint. */
	UFUNCTION(BlueprintCallable, Category = "Attachment")
	bool ValidateData(FString& Error) const;
};

UCLASS()
class CSROADNET_API UCSCityAttachmentLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Requires Ready profiles, the same interface and Plug/Socket or Peer/Peer roles.
	 * Does not check occupancy, source revisions, collision, or building exterior permissions. */
	UFUNCTION(BlueprintCallable, Category = "Attachment")
	static bool AreAnchorsCompatible(const UCSCityAttachmentProfile* HostProfile, FGuid HostAnchorId,
		const UCSCityAttachmentProfile* GuestProfile, FGuid GuestAnchorId, FString& Error);

	/** Rigid candidate pose: origins coincide, +X axes oppose, +Z axes agree.
	 * HostWorld must have unit scale. No spawning, reserving, or placement permission is implied. */
	UFUNCTION(BlueprintCallable, Category = "Attachment")
	static bool ComputeConnectedTransform(const UCSCityAttachmentProfile* HostProfile, FGuid HostAnchorId,
		const FTransform& HostWorld, const UCSCityAttachmentProfile* GuestProfile, FGuid GuestAnchorId,
		FTransform& GuestWorld, FString& Error);
};
