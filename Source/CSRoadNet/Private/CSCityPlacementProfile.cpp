#include "CSCityPlacementProfile.h"
#include "Engine/StaticMesh.h"

bool UCSCityPlacementProfile::ValidateData(FString& Error) const
{
	Error.Reset();
	auto Fail = [&Error](const FString& Message) { Error = Message; return false; };
	if (!ProfileId.IsValid() || SchemaVersion != 1 || DataRevision < 1) return Fail(TEXT("Invalid profile identity or version."));
	if (SourceMesh.IsNull() || SourceGeometryHash.Len() != 64) return Fail(TEXT("Source mesh and SHA-256 source fingerprint are required."));
	for (TCHAR Ch : SourceGeometryHash) if (!FChar::IsHexDigit(Ch)) return Fail(TEXT("Source fingerprint must be hexadecimal."));
	if (State == ECSCityPlacementState::Ready && Regions.IsEmpty()) return Fail(TEXT("A ready profile needs at least one region."));
	TSet<FGuid> Ids;
	for (const FCSCityPlacementRegion& Region : Regions)
	{
		if (!Region.RegionId.IsValid() || Ids.Contains(Region.RegionId)) return Fail(TEXT("Region identities must be valid and unique."));
		Ids.Add(Region.RegionId);
		if (Region.Positions.Num() < 3 || Region.Triangles.IsEmpty() || Region.AllowedCategories.IsEmpty()) return Fail(TEXT("Region geometry and allowed categories are required."));
		for (FName Category : Region.AllowedCategories) if (Category.IsNone()) return Fail(TEXT("Empty attachment category."));
		if (Region.OutwardNormal.ContainsNaN() || !Region.OutwardNormal.IsNormalized() || Region.PreferredUp.ContainsNaN() || !Region.PreferredUp.IsNormalized()) return Fail(TEXT("Region frame vectors must be finite and unit length."));
		if (FMath::Abs(FVector::DotProduct(Region.OutwardNormal, Region.PreferredUp)) > 0.01) return Fail(TEXT("Region up must be tangent to the plane."));
		for (const FVector& P : Region.Positions)
		{
			if (P.ContainsNaN()) return Fail(TEXT("Region has non-finite vertices."));
			if (FMath::Abs(FVector::DotProduct(P - Region.Positions[0], Region.OutwardNormal)) > 0.1) return Fail(TEXT("Region is not planar within 0.1 cm."));
		}
		for (const FIntVector& T : Region.Triangles)
		{
			if (!Region.Positions.IsValidIndex(T.X) || !Region.Positions.IsValidIndex(T.Y) || !Region.Positions.IsValidIndex(T.Z)) return Fail(TEXT("Region triangle index out of range."));
			const FVector N = FVector::CrossProduct(Region.Positions[T.Y] - Region.Positions[T.X], Region.Positions[T.Z] - Region.Positions[T.X]);
			if (N.SizeSquared() < 1.e-8 || FVector::DotProduct(N.GetSafeNormal(), Region.OutwardNormal) < 0.999) return Fail(TEXT("Degenerate triangle or inconsistent region winding."));
		}
	}
	return true;
}

UCSCityPlacementProfile* UCSCityPlacementLibrary::GetDefaultProfile(UStaticMesh* SourceMesh)
{
	if (!SourceMesh) return nullptr;
	const auto* Binding = Cast<UCSCityPlacementBinding>(SourceMesh->GetAssetUserDataOfClass(UCSCityPlacementBinding::StaticClass()));
	return Binding ? Binding->Profile.Get() : nullptr;
}

bool UCSCityPlacementLibrary::SetDefaultProfile(UStaticMesh* SourceMesh, UCSCityPlacementProfile* Profile, FString& Error)
{
	Error.Reset();
#if WITH_EDITOR
	if (!SourceMesh || !Profile) { Error = TEXT("Source mesh and profile are required."); return false; }
	if (Profile->SourceMesh.ToSoftObjectPath() != FSoftObjectPath(SourceMesh)) { Error = TEXT("Profile belongs to another source mesh."); return false; }
	if (!Profile->ValidateData(Error)) return false;
	SourceMesh->Modify();
	auto* Binding = Cast<UCSCityPlacementBinding>(SourceMesh->GetAssetUserDataOfClass(UCSCityPlacementBinding::StaticClass()));
	if (!Binding)
	{
		Binding = NewObject<UCSCityPlacementBinding>(SourceMesh, NAME_None, RF_Transactional);
		SourceMesh->AddAssetUserData(Binding);
	}
	Binding->Modify();
	Binding->Profile = Profile;
	SourceMesh->MarkPackageDirty();
	return true;
#else
	Error = TEXT("Setting a default profile is only supported in the editor.");
	return false;
#endif
}
