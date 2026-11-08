#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSCityAttachmentProfile.h"
#include "Engine/StaticMesh.h"

namespace
{
UCSCityAttachmentProfile* AttachmentTestProfile(ECSCityAttachmentAnchorRole Role)
{
	auto* Profile = NewObject<UCSCityAttachmentProfile>();
	Profile->ProfileId = FGuid::NewGuid();
	Profile->SourceMesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube")));
	Profile->SourceGeometryHash = FString::ChrN(64, TCHAR('a'));
	Profile->State = ECSCityAttachmentState::Ready;
	FCSCityAttachmentAnchor& Anchor = Profile->Anchors.AddDefaulted_GetRef();
	Anchor.AnchorId = FGuid::NewGuid();
	Anchor.Label = TEXT("Mount");
	Anchor.InterfaceType = TEXT("Test.RigidMount");
	Anchor.Role = Role;
	return Profile;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityAttachmentValidationTest, "PCGPlugins.CSRoadNet.City.Attachment.Validation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSCityAttachmentValidationTest::RunTest(const FString& Parameters)
{
	auto* Profile = AttachmentTestProfile(ECSCityAttachmentAnchorRole::Plug);
	FString Error;
	TestTrue(TEXT("Complete profile"), Profile->ValidateData(Error));
	const auto Original = Profile->Anchors[0];
	Profile->Anchors.Add(Original);
	TestFalse(TEXT("Duplicate anchor ID"), Profile->ValidateData(Error));
	Profile->Anchors[1].AnchorId = FGuid::NewGuid();
	TestFalse(TEXT("Duplicate label"), Profile->ValidateData(Error));
	Profile->Anchors.SetNum(1);
	Profile->Anchors[0].MaxConnections = 0;
	TestFalse(TEXT("Zero capacity"), Profile->ValidateData(Error));
	Profile->Anchors[0] = Original;
	Profile->Anchors[0].LocalFrame.SetScale3D(FVector(-1, 1, 1));
	TestFalse(TEXT("Mirrored anchor"), Profile->ValidateData(Error));
	Profile->Anchors[0].LocalFrame.SetScale3D(FVector(1, 2, 1));
	TestFalse(TEXT("Stretched anchor"), Profile->ValidateData(Error));
	Profile->Anchors[0] = Original;
	Profile->Anchors[0].LocalFrame.SetRotation(FQuat(0, 0, 0, 0));
	TestFalse(TEXT("Invalid rotation"), Profile->ValidateData(Error));
	Profile->Anchors[0] = Original;
	Profile->SourceGeometryHash = FString::ChrN(64, TCHAR('z'));
	TestFalse(TEXT("Invalid fingerprint"), Profile->ValidateData(Error));
	Profile->SourceGeometryHash = FString::ChrN(64, TCHAR('a'));
	Profile->Anchors.Empty();
	TestFalse(TEXT("Empty ready profile"), Profile->ValidateData(Error));
	Profile->State = ECSCityAttachmentState::PendingReview;
	TestTrue(TEXT("Empty draft may be authored"), Profile->ValidateData(Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityAttachmentCompatibilityTest, "PCGPlugins.CSRoadNet.City.Attachment.Compatibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSCityAttachmentCompatibilityTest::RunTest(const FString& Parameters)
{
	auto* Host = AttachmentTestProfile(ECSCityAttachmentAnchorRole::Socket);
	auto* Guest = AttachmentTestProfile(ECSCityAttachmentAnchorRole::Plug);
	const FGuid HostId = Host->Anchors[0].AnchorId;
	const FGuid GuestId = Guest->Anchors[0].AnchorId;
	FString Error;
	auto Compatible = [&]() { return UCSCityAttachmentLibrary::AreAnchorsCompatible(Host, HostId, Guest, GuestId, Error); };
	TestTrue(TEXT("Socket and plug"), Compatible());
	TestTrue(TEXT("Pair matching is symmetric"), UCSCityAttachmentLibrary::AreAnchorsCompatible(Guest, GuestId, Host, HostId, Error));
	Guest->State = ECSCityAttachmentState::PendingReview;
	TestFalse(TEXT("Draft cannot connect"), Compatible());
	Guest->State = ECSCityAttachmentState::Disabled;
	TestFalse(TEXT("Disabled cannot connect"), Compatible());
	Guest->State = ECSCityAttachmentState::Ready;
	Guest->Anchors[0].InterfaceType = TEXT("Test.OtherMount");
	TestFalse(TEXT("Mismatched family"), Compatible());
	Guest->Anchors[0].InterfaceType = Host->Anchors[0].InterfaceType;
	Guest->Anchors[0].Role = ECSCityAttachmentAnchorRole::Socket;
	TestFalse(TEXT("Socket cannot mate to socket"), Compatible());
	Host->Anchors[0].Role = ECSCityAttachmentAnchorRole::Peer;
	Guest->Anchors[0].Role = ECSCityAttachmentAnchorRole::Peer;
	TestTrue(TEXT("Peer pair"), Compatible());
	Host->Anchors[0].Role = ECSCityAttachmentAnchorRole::SurfaceMount;
	TestFalse(TEXT("Surface needs exterior validation"), Compatible());
	Host->Anchors[0].Role = ECSCityAttachmentAnchorRole::Peer;
	TestFalse(TEXT("Unknown anchor"), UCSCityAttachmentLibrary::AreAnchorsCompatible(Host, FGuid::NewGuid(), Guest, GuestId, Error));
	TestFalse(TEXT("Missing profile"), UCSCityAttachmentLibrary::AreAnchorsCompatible(nullptr, HostId, Guest, GuestId, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityAttachmentPoseTest, "PCGPlugins.CSRoadNet.City.Attachment.Pose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSCityAttachmentPoseTest::RunTest(const FString& Parameters)
{
	auto* Host = AttachmentTestProfile(ECSCityAttachmentAnchorRole::Socket);
	auto* Guest = AttachmentTestProfile(ECSCityAttachmentAnchorRole::Plug);
	Host->Anchors[0].LocalFrame = FTransform(FRotator(23, 51, -9), FVector(17, -33, 48));
	Guest->Anchors[0].LocalFrame = FTransform(FRotator(-17, 101, 38), FVector(-21, 52, 7));
	const FGuid HostId = Host->Anchors[0].AnchorId;
	const FGuid GuestId = Guest->Anchors[0].AnchorId;
	const FTransform HostWorld(FRotator(-31, 127, 18), FVector(-192400, 20000, 1800));
	FTransform GuestWorld;
	FString Error;
	if (!TestTrue(TEXT("Compute a candidate pose"), UCSCityAttachmentLibrary::ComputeConnectedTransform(Host, HostId, HostWorld, Guest, GuestId, GuestWorld, Error))) return false;
	const auto& A = Host->Anchors[0].LocalFrame;
	const auto& B = Guest->Anchors[0].LocalFrame;
	TestTrue(TEXT("World anchor origins coincide"), HostWorld.TransformPosition(A.GetLocation()).Equals(GuestWorld.TransformPosition(B.GetLocation()), 1.e-6));
	const FVector HostX = HostWorld.TransformVectorNoScale(A.TransformVectorNoScale(FVector::ForwardVector));
	const FVector GuestX = GuestWorld.TransformVectorNoScale(B.TransformVectorNoScale(FVector::ForwardVector));
	const FVector HostZ = HostWorld.TransformVectorNoScale(A.TransformVectorNoScale(FVector::UpVector));
	const FVector GuestZ = GuestWorld.TransformVectorNoScale(B.TransformVectorNoScale(FVector::UpVector));
	TestTrue(TEXT("Outward axes oppose"), HostX.Equals(-GuestX, 1.e-6));
	TestTrue(TEXT("Up axes agree"), HostZ.Equals(GuestZ, 1.e-6));
	FTransform RecoveredHost;
	TestTrue(TEXT("Reverse connection"), UCSCityAttachmentLibrary::ComputeConnectedTransform(Guest, GuestId, GuestWorld, Host, HostId, RecoveredHost, Error));
	TestTrue(TEXT("Recovers original host"), RecoveredHost.Equals(HostWorld, 1.e-6));
	FTransform ScaledHost = HostWorld;
	ScaledHost.SetScale3D(FVector(2));
	TestFalse(TEXT("Reject device scaling"), UCSCityAttachmentLibrary::ComputeConnectedTransform(Host, HostId, ScaledHost, Guest, GuestId, GuestWorld, Error));
	TestTrue(TEXT("Failure clears stale output"), GuestWorld.Equals(FTransform::Identity));
	// A repeated copy of one asset may connect to another instance of that asset: identities live in the scene graph.
	Host->Anchors[0].Role = ECSCityAttachmentAnchorRole::Peer;
	TestTrue(TEXT("Reusable profile is not an instance identity"), UCSCityAttachmentLibrary::AreAnchorsCompatible(Host, HostId, Host, HostId, Error));
	return true;
}

#endif
