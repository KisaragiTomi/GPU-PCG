#include "CSCityAttachmentProfile.h"

namespace
{
bool IsRigidFrame(const FTransform& Frame)
{
	return !Frame.ContainsNaN() && Frame.GetRotation().IsNormalized() && Frame.GetScale3D().Equals(FVector::OneVector, 1.e-6);
}

const FCSCityAttachmentAnchor* FindAnchor(const UCSCityAttachmentProfile& Profile, const FGuid& Id)
{
	return Profile.Anchors.FindByPredicate([&Id](const FCSCityAttachmentAnchor& Anchor) { return Anchor.AnchorId == Id; });
}
}

bool UCSCityAttachmentProfile::ValidateData(FString& Error) const
{
	Error.Reset();
	auto Fail = [&Error](const TCHAR* Message) { Error = Message; return false; };
	if (!ProfileId.IsValid() || SchemaVersion != 1 || DataRevision < 1) return Fail(TEXT("Invalid attachment profile identity or version."));
	if (SourceMesh.IsNull() || SourceGeometryHash.Len() != 64) return Fail(TEXT("Source mesh and SHA-256 source fingerprint are required."));
	for (TCHAR Ch : SourceGeometryHash) if (!FChar::IsHexDigit(Ch)) return Fail(TEXT("Source fingerprint must be hexadecimal."));
	if (State != ECSCityAttachmentState::PendingReview && State != ECSCityAttachmentState::Ready && State != ECSCityAttachmentState::Disabled) return Fail(TEXT("Unknown attachment profile state."));
	if (State == ECSCityAttachmentState::Ready && Anchors.IsEmpty()) return Fail(TEXT("A ready profile needs at least one anchor."));
	TSet<FGuid> Ids;
	TSet<FName> Labels;
	for (const FCSCityAttachmentAnchor& Anchor : Anchors)
	{
		if (!Anchor.AnchorId.IsValid() || Ids.Contains(Anchor.AnchorId)) return Fail(TEXT("Anchor identities must be valid and unique within the profile."));
		if (Anchor.Label.IsNone() || Labels.Contains(Anchor.Label)) return Fail(TEXT("Anchor labels must be nonempty and unique within the profile."));
		if (Anchor.InterfaceType.IsNone() || Anchor.MaxConnections < 1) return Fail(TEXT("An anchor needs an interface and a positive connection capacity."));
		if (!IsRigidFrame(Anchor.LocalFrame)) return Fail(TEXT("Anchor frames must be finite, with normalized rotation and unit scale."));
		switch (Anchor.Role)
		{
		case ECSCityAttachmentAnchorRole::SurfaceMount:
		case ECSCityAttachmentAnchorRole::Plug:
		case ECSCityAttachmentAnchorRole::Socket:
		case ECSCityAttachmentAnchorRole::Peer: break;
		default: return Fail(TEXT("Unknown anchor role."));
		}
		Ids.Add(Anchor.AnchorId);
		Labels.Add(Anchor.Label);
	}
	return true;
}

bool UCSCityAttachmentLibrary::AreAnchorsCompatible(const UCSCityAttachmentProfile* HostProfile, FGuid HostAnchorId,
	const UCSCityAttachmentProfile* GuestProfile, FGuid GuestAnchorId, FString& Error)
{
	Error.Reset();
	auto Fail = [&Error](const TCHAR* Message) { Error = Message; return false; };
	if (!HostProfile || !GuestProfile) return Fail(TEXT("Both attachment profiles are required."));
	if (!HostProfile->ValidateData(Error) || !GuestProfile->ValidateData(Error)) return false;
	if (HostProfile->State != ECSCityAttachmentState::Ready || GuestProfile->State != ECSCityAttachmentState::Ready) return Fail(TEXT("Only reviewed Ready profiles may be connected."));
	const auto* Host = FindAnchor(*HostProfile, HostAnchorId);
	const auto* Guest = FindAnchor(*GuestProfile, GuestAnchorId);
	if (!Host || !Guest) return Fail(TEXT("Anchor ID not found in its profile."));
	if (Host->InterfaceType != Guest->InterfaceType) return Fail(TEXT("Anchor interface types differ."));
	using Role = ECSCityAttachmentAnchorRole;
	const bool bMatches = (Host->Role == Role::Socket && Guest->Role == Role::Plug)
		|| (Host->Role == Role::Plug && Guest->Role == Role::Socket)
		|| (Host->Role == Role::Peer && Guest->Role == Role::Peer);
	if (!bMatches) return Fail(TEXT("Roles must be Plug/Socket or Peer/Peer. SurfaceMount requires a verified building region."));
	return true;
}

bool UCSCityAttachmentLibrary::ComputeConnectedTransform(const UCSCityAttachmentProfile* HostProfile, FGuid HostAnchorId,
	const FTransform& HostWorld, const UCSCityAttachmentProfile* GuestProfile, FGuid GuestAnchorId,
	FTransform& GuestWorld, FString& Error)
{
	GuestWorld = FTransform::Identity;
	if (!AreAnchorsCompatible(HostProfile, HostAnchorId, GuestProfile, GuestAnchorId, Error)) return false;
	if (!IsRigidFrame(HostWorld))
	{
		Error = TEXT("Host world transform must be finite, with normalized rotation and unit scale; scaled devices need separate authored profiles.");
		return false;
	}
	const auto* Host = FindAnchor(*HostProfile, HostAnchorId);
	const auto* Guest = FindAnchor(*GuestProfile, GuestAnchorId);
	// Exact 180 degrees about local Z, avoiding float-pi drift on repeated/reversed connections.
	const FTransform OpposedFrame(FQuat(0, 0, 1, 0));
	// UE composes A * B by applying A first. The guest anchor must land at the opposed host anchor.
	const FTransform Candidate = Guest->LocalFrame.Inverse() * OpposedFrame * Host->LocalFrame * HostWorld;
	if (!IsRigidFrame(Candidate))
	{
		Error = TEXT("Connected transform is not a finite rigid frame.");
		return false;
	}
	GuestWorld = Candidate;
	return true;
}
