#include "CSRangeGeneratorActor.h"

#include "ComputeShaderCliffGenerate.h"
#include "Landscape.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Kismet/KismetSystemLibrary.h"


ACSRangeGenerator::ACSRangeGenerator()
{
	SceneComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	//SceneComponent->SetupAttachment(GetRootComponent(), TEXT("CaptureRoot"));
	SetRootComponent(SceneComponent);
	
	Box = CreateDefaultSubobject<UBoxComponent>(TEXT("Box"));
	Box->SetupAttachment(SceneComponent, TEXT("Box"));
	Box->SetBoxExtent(FVector(50,50,50));
	Box->SetCollisionEnabled(ECollisionEnabled::Type::NoCollision);
	Box->SetCollisionObjectType(ECC_Vehicle);

	CollisionBox = CreateDefaultSubobject<UBoxComponent>(TEXT("CollisionBox"));
	CollisionBox->SetupAttachment(SceneComponent, TEXT("CollisionBox"));
	CollisionBox->SetBoxExtent(FVector(50,50,50));
	CollisionBox->SetCollisionEnabled(ECollisionEnabled::Type::NoCollision);
	CollisionBox->SetCollisionObjectType(ECC_WorldStatic);
		
	Instances = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Instances"));
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	Instances->SetStaticMesh(Mesh);
	Instances->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Overlap);
	Instances->SetCollisionObjectType(ECollisionChannel::ECC_WorldDynamic);
	Instances->SetHiddenInGame(true);
	Instances->SetupAttachment(GetRootComponent(), TEXT("Instances"));
	Instances->SetupAttachment(SceneComponent, TEXT("Instances"));

	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}

void ACSRangeGenerator::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	Box->SetCollisionEnabled(ECollisionEnabled::Type::NoCollision);
	Box->SetCollisionObjectType(ECC_Vehicle);
}

void ACSRangeGenerator::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

void ACSRangeGenerator::GenerateInternal()
{
	if (CaptureSceneGenerators.Num() == 0) return;
	if (CaptureSceneGenerators.Num() <= GeneratorCount) return;
	if (StoreCaptureTransforms.Num() <= MultGenerateCount)
	{
		GeneratorCount += 1;
		MultGenerateCount = 0;
		if (CaptureSceneGenerators.Num() <= GeneratorCount) return;
	}

	CaptureSceneGenerators[GeneratorCount]->SetActorLocation(StoreCaptureTransforms[MultGenerateCount].GetLocation());
	CaptureSceneGenerators[GeneratorCount]->Generate(5, 1);
	MultGenerateCount += 1;
}

void ACSRangeGenerator::Generate()
{
	GenerateInternal();
}

ACSBoxRangeGenerator::ACSBoxRangeGenerator()
{
}

TArray<FTransform> ACSBoxRangeGenerator::GenerateTransformsInternal()
{
	FVector Center;
	FVector BoxExtent;
	GetActorBounds(false, Center, BoxExtent);
	const float ExtentMult = 1.0f;
	FVector Size = BoxExtent * ExtentMult;

	TArray<FVector> Dirs;
	TSet<FVector> DirSet;
	FVector DirTest = FVector::ZeroVector;
	float DirDivide = 2;
	
	for (int32 i = 0; i < DirDivide * 2 + 1; i++)
	{
		for (int32 n = 0; n < DirDivide * 2 + 1; n++)
		{
			for (int32 c = 0; c < DirDivide * 2 + 1; c++)
			{
				if (c / DirDivide - 1 > 0.2f)
				{
					continue;
				}
				float RandomAngleRangeMult = FMath::FRandRange(0.1f, 0.2f);
				FVector RandomVector = FMath::VRandCone(FVector(0, 0, 1), 360, 360);
				FVector Dir = (FVector(i / DirDivide - 1, n / DirDivide - 1, 0) + RandomVector * RandomAngleRangeMult).GetSafeNormal();
				DirSet.Add(Dir);
				DirTest += Dir;
			}
		}
	}
	Dirs = DirSet.Array();
	DirTest = DirTest.GetSafeNormal();

	StoreCaptureTransforms.Empty();

	if (StoreCaptureTransforms.Num() != 0)
	{
		return StoreCaptureTransforms;
	}



	

	
	TArray<TEnumAsByte<EObjectTypeQuery>> ObjectTypes{ UEngineTypes::ConvertToObjectType(ECollisionChannel::ECC_WorldDynamic), UEngineTypes::ConvertToObjectType(ECollisionChannel::ECC_WorldStatic)};
	TArray<AActor*> ActorsToIgnore;
	TArray<AActor*> OverlapOutActors;
	UKismetSystemLibrary::ComponentOverlapActors(Box, Box->GetComponentTransform(), ObjectTypes, nullptr, ActorsToIgnore, ActorsInBox);
	if (ActorsInBox.Num() == 0) return StoreCaptureTransforms;

	
	
	CollisionBox->SetCollisionEnabled(ECollisionEnabled::Type::QueryOnly);

	FTransform ActorTransform = GetActorTransform();
	FVector l = FVector::OneVector / ((FVector)DivideCount - FVector::OneVector);

	
	// 计算采样点
	for (int32 i = 0; i < DivideCount.X; i++)
	{
		for (int32 n = 0; n < DivideCount.Y; n++)
		{
			for (int32 c = 0; c < DivideCount.Z; c++)
			{
				FVector Pos = ActorTransform.TransformPosition((FVector(i * l.X, n * l.Y, c * l.Z) - FVector::OneVector * 0.5f) * 100);
				for (FVector Dir : Dirs)
				{
					FVector RandomVector = FMath::VRandCone(FVector(0, 0, 1), 360, 360);
					FVector RandomDir = FMath::VRandCone(FVector(0, 0, 1), 360, 360);
					FRotator Rot = FRotationMatrix::MakeFromX((Dir + RandomDir * 0.3f).GetSafeNormal()).Rotator();
					bool bOverlapPickActor = false;

					TArray<FHitResult> OutHits;
					UKismetSystemLibrary::BoxTraceMultiForObjects(
						GetWorld(), Pos, Pos, FVector(0, CaptureSize * 0.5f, CaptureSize * 0.5f), Rot, ObjectTypes, true,
						ActorsToIgnore, EDrawDebugTrace::None, OutHits, true);
					for (FHitResult OutHit : OutHits)
					{
						AActor* HitActor = OutHit.GetActor();
						if (ActorsInBox.Contains(HitActor))
						{
							bOverlapPickActor = true;
							break;
						}
					}
					if (bOverlapPickActor) continue;

				FVector Start = Pos;
					FVector End = Start + Dir * 10000;
					FHitResult OutHit;
					if (UKismetSystemLibrary::LineTraceSingle(GetWorld(), Start, End, ETraceTypeQuery::TraceTypeQuery1, true, ActorsToIgnore, EDrawDebugTrace::None, OutHit, true))
					{
						if (!(ActorsInBox.Contains(OutHit.GetActor()) && OutHit.Distance > 200))
						{
							continue;
						}
					}
					else
					{
						continue;
					}
				// 检查上方是否为地表
				End = Start + FVector(0, 0, 10000);
					if (UKismetSystemLibrary::LineTraceSingle(GetWorld(), Start, End, ETraceTypeQuery::TraceTypeQuery1, true, ActorsToIgnore, EDrawDebugTrace::None, OutHit, true))
					{
						if (Cast<ALandscape>(OutHit.GetActor()))
						{
							continue;
						}
					}
					StoreCaptureTransforms.Add(FTransform(Rot, Pos, FVector::OneVector));
				}
			}
		}
	}
	CollisionBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	return StoreCaptureTransforms;
}


ACSPlaneRangeGenerator::ACSPlaneRangeGenerator()
{
}

void ACSPlaneRangeGenerator::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

}

TArray<FTransform> ACSPlaneRangeGenerator::GenerateTransformsInternal()
{
	FVector Center = Box->Bounds.Origin;
	FVector BoxExtent = Box->Bounds.BoxExtent;
	const float ExtentMult = 1.0f;
	FVector Size = BoxExtent * ExtentMult * 2;
	const int32 XCount = FMath::CeilToInt(Size.X / CaptureSize) + 1;
	const int32 YCount = FMath::CeilToInt(Size.Y / CaptureSize) + 1;
	const float XLength = 1.0f / (XCount - 1);
	const float YLength = 1.0f / (YCount - 1);

	StoreCaptureTransforms.Empty();
	StoreCaptureTransforms.Reserve(XCount * YCount);
	FTransform ActorTransform = GetActorTransform();
	for (int32 Y = 0; Y < YCount; Y++)
	{
		for (int32 X = 0; X < XCount; X++)
		{
			FVector Pos = ActorTransform.TransformPosition((FVector(X * XLength, Y * YLength, 0) - FVector::OneVector * 0.5f) * 100);
			StoreCaptureTransforms.Add(FTransform(FRotator::ZeroRotator, Pos, FVector::OneVector));
		}
	}

	for (int32 i = 0; i < StoreCaptureTransforms.Num(); i++)
	{
		StoreCaptureTransforms.Swap(i, FMath::RandRange(0, StoreCaptureTransforms.Num() - 1));
	}
	return StoreCaptureTransforms;
}

TArray<FTransform> ACSRangeGenerator::GenerateTransformsCount()
{
	return GenerateTransformsInternal();
}

TArray<FTransform> ACSRangeGenerator::GenerateTransformsInternal()
{
	TArray<FTransform> Transforms;
	return Transforms;
}
