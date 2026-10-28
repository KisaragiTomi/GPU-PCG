#include "CSRoadNetworkFile.h"

#include <atomic>

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace CSRoad
{
namespace
{
float2 CSRoadNetworkFile_Pair(const TArray<TSharedPtr<FJsonValue>>& A)
{
	return float2(static_cast<float>(A[0]->AsNumber()), static_cast<float>(A[1]->AsNumber()));
}

float3 CSRoadNetworkFile_Vector(const TArray<TSharedPtr<FJsonValue>>& A, int32 Offset = 0)
{
	return float3(static_cast<float>(A[Offset]->AsNumber()), static_cast<float>(A[Offset + 1]->AsNumber()), static_cast<float>(A[Offset + 2]->AsNumber()));
}

bool CSRoadNetworkFile_Elevation(const TSharedPtr<FJsonObject>& O, float2& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
	if (!O->TryGetArrayField(TEXT("elevation"), A) || A->Num() < 2) return false;
	Out = CSRoadNetworkFile_Pair(*A);
	return true;
}

CompositionFlags CSRoadNetworkFile_Upgrade(const TSharedPtr<FJsonObject>& O)
{
	const TArray<TSharedPtr<FJsonValue>>& A = O->GetArrayField(TEXT("upgrade"));
	return MakeFlags(static_cast<uint>(A[0]->AsNumber()), static_cast<uint>(A[1]->AsNumber()), static_cast<uint>(A[2]->AsNumber()));
}

int32 CSRoadNetworkFile_Owner(const TSharedPtr<FJsonObject>& O)
{
	double Value = 0.0;
	return O->TryGetNumberField(TEXT("owner"), Value) ? static_cast<int32>(Value) : 0;
}

bool CSRoadNetworkFile_LoadMap(const TSharedPtr<FJsonObject>& Terrain, const FString& Directory, const TCHAR* Key, const TCHAR* OffsetKey, const TCHAR* SizeKey, FSavedTerrain::FMap& Out, FString& OutError)
{
	const TSharedPtr<FJsonObject> M = Terrain->GetObjectField(Key);
	Out.Width = static_cast<int32>(M->GetNumberField(TEXT("width")));
	Out.Height = static_cast<int32>(M->GetNumberField(TEXT("height")));
	const FString File = M->GetStringField(TEXT("file"));
	if (FPaths::GetCleanFilename(File) != File)
	{
		OutError = TEXT("Heightmap must be in the snapshot directory");
		return false;
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *FPaths::Combine(Directory, File)) || Out.Width < 2 || Out.Height < 2 || Bytes.Num() != Out.Width * Out.Height * 2)
	{
		OutError = FString::Printf(TEXT("Heightmap %s is missing or has the wrong size"), *File);
		return false;
	}
	Out.Data.SetNumUninitialized(Out.Width * Out.Height);
	FMemory::Memcpy(Out.Data.GetData(), Bytes.GetData(), Bytes.Num());
	Out.Offset = CSRoadNetworkFile_Pair(Terrain->GetArrayField(OffsetKey));
	Out.Size = CSRoadNetworkFile_Pair(Terrain->GetArrayField(SizeKey));
	return true;
}
}

float FSavedTerrain::Sample(float3 Point) const
{
	const float2 PointXZ = Point.xz();
	float2 P = (PointXZ - Playable.Offset) / Playable.Size * float2(static_cast<float>(Playable.Width), static_cast<float>(Playable.Height)) - 0.5f;
	const bool bOutside = math::any(P < float2(0.0f)) || math::any(P >= float2(static_cast<float>(Playable.Width - 1), static_cast<float>(Playable.Height - 1)));
	const FMap& Map = bOutside ? World : Playable;
	P = (PointXZ - Map.Offset) / Map.Size * float2(static_cast<float>(Map.Width), static_cast<float>(Map.Height)) - 0.5f;
	const int2 Floor(static_cast<int32>(math::floor(P.x)), static_cast<int32>(math::floor(P.y)));
	const int2 A = int2(math::clamp(Floor.x, 0, Map.Width - 1), math::clamp(Floor.y, 0, Map.Height - 1));
	const int2 B = int2(math::clamp(Floor.x + 1, 0, Map.Width - 1), math::clamp(Floor.y + 1, 0, Map.Height - 1));
	const float2 T = math::saturate(P - float2(static_cast<float>(A.x), static_cast<float>(A.y)));
	const float Row0 = math::lerp(static_cast<float>(Map.Data[A.y * Map.Width + A.x]), static_cast<float>(Map.Data[A.y * Map.Width + B.x]), T.x);
	const float Row1 = math::lerp(static_cast<float>(Map.Data[B.y * Map.Width + A.x]), static_cast<float>(Map.Data[B.y * Map.Width + B.x]), T.x);
	return math::lerp(Row0, Row1, T.y) * HeightScaleOffset.x / 65535.0f + HeightScaleOffset.y;
}

bool FSavedNetwork::Load(const FString& Path, const FAssets* Assets, bool bLoadTerrain, FString& OutError)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		OutError = FString::Printf(TEXT("Cannot read %s"), *Path);
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid() || Root->GetStringField(TEXT("format")) != TEXT("cs2-road-network-v1"))
	{
		OutError = TEXT("Unsupported road snapshot format");
		return false;
	}
	const TSharedPtr<FJsonObject>* Metadata = nullptr;
	const TSharedPtr<FJsonObject> Source = Root->GetObjectField(TEXT("source"));
	if (Source->TryGetObjectField(TEXT("metadata"), Metadata))
	{
		const TSharedPtr<FJsonObject>* Options = nullptr;
		if ((*Metadata)->TryGetObjectField(TEXT("options"), Options)) (*Options)->TryGetBoolField(TEXT("leftHandTraffic"), bLeftHandTraffic);
	}
	TMap<int32, int32> NodeIndices;
	for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("nodes")))
	{
		const TSharedPtr<FJsonObject> N = Value->AsObject();
		FSavedNode Node;
		Node.Id = static_cast<int32>(N->GetNumberField(TEXT("id")));
		Node.Prefab = N->GetStringField(TEXT("prefab"));
		Node.Position = CSRoadNetworkFile_Vector(N->GetArrayField(TEXT("position")));
		const TArray<TSharedPtr<FJsonValue>>& R = N->GetArrayField(TEXT("rotation"));
		Node.Rotation = float4(static_cast<float>(R[0]->AsNumber()), static_cast<float>(R[1]->AsNumber()), static_cast<float>(R[2]->AsNumber()), static_cast<float>(R[3]->AsNumber()));
		Node.bHasElevation = CSRoadNetworkFile_Elevation(N, Node.Elevation);
		Node.Upgrade = CSRoadNetworkFile_Upgrade(N);
		Node.bOutside = N->GetBoolField(TEXT("outside"));
		Node.Owner = CSRoadNetworkFile_Owner(N);
		NodeIndices.Add(Node.Id, Nodes.Add(Node));
	}
	TSet<int32> EdgeIds;
	for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("roads")))
	{
		const TSharedPtr<FJsonObject> E = Value->AsObject();
		FRoadSpec Spec;
		Spec.Prefab = E->GetStringField(TEXT("prefab"));
		const TArray<TSharedPtr<FJsonValue>>& C = E->GetArrayField(TEXT("curve"));
		Spec.Curve = Bezier4x3(CSRoadNetworkFile_Vector(C, 0), CSRoadNetworkFile_Vector(C, 3), CSRoadNetworkFile_Vector(C, 6), CSRoadNetworkFile_Vector(C, 9));
		Spec.Upgrade = CSRoadNetworkFile_Upgrade(E);
		Spec.bHasSource = true;
		Spec.SourceId = static_cast<int32>(E->GetNumberField(TEXT("id")));
		const int32* Start = NodeIndices.Find(static_cast<int32>(E->GetNumberField(TEXT("start"))));
		const int32* End = NodeIndices.Find(static_cast<int32>(E->GetNumberField(TEXT("end"))));
		if (!Start || !End)
		{
			OutError = FString::Printf(TEXT("Edge %d references a missing node"), Spec.SourceId);
			return false;
		}
		Spec.SourceStart = *Start;
		Spec.SourceEnd = *End;
		Spec.bHasElevation = CSRoadNetworkFile_Elevation(E, Spec.Elevation);
		Spec.Owner = CSRoadNetworkFile_Owner(E);
		if (EdgeIds.Contains(Spec.SourceId))
		{
			OutError = TEXT("Duplicate source edge");
			return false;
		}
		EdgeIds.Add(Spec.SourceId);
		for (const float3& P : { Spec.Curve.a, Spec.Curve.b, Spec.Curve.c, Spec.Curve.d })
		{
			if (!math::all(math::isfinite(P)))
			{
				OutError = FString::Printf(TEXT("Nonfinite source curve %d"), Spec.SourceId);
				return false;
			}
		}
		Roads.Add(Spec);
	}
	if (Assets)
	{
		for (const FRoadSpec& Road : Roads)
		{
			if (!Assets->HasRoad(Road.Prefab))
			{
				OutError = FString::Printf(TEXT("Unmapped prefab: %s"), *Road.Prefab);
				return false;
			}
		}
		for (const FSavedNode& Node : Nodes)
		{
			if (!Assets->HasRoad(Node.Prefab))
			{
				OutError = FString::Printf(TEXT("Unmapped prefab: %s"), *Node.Prefab);
				return false;
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* ViewArray = nullptr;
	if (Root->TryGetArrayField(TEXT("views"), ViewArray))
	{
		for (const TSharedPtr<FJsonValue>& Value : *ViewArray)
		{
			const TSharedPtr<FJsonObject> V = Value->AsObject();
			Views.Add({ V->GetStringField(TEXT("name")), CSRoadNetworkFile_Vector(V->GetArrayField(TEXT("position"))), static_cast<float>(V->GetNumberField(TEXT("distance"))) });
		}
	}
	if (bLoadTerrain)
	{
		const TSharedPtr<FJsonObject> TerrainJson = Root->GetObjectField(TEXT("terrain"));
		const FString Directory = FPaths::GetPath(FPaths::ConvertRelativePathToFull(Path));
		Terrain.HeightScaleOffset = CSRoadNetworkFile_Pair(TerrainJson->GetArrayField(TEXT("heightScaleOffset")));
		if (!CSRoadNetworkFile_LoadMap(TerrainJson, Directory, TEXT("playable"), TEXT("playableOffset"), TEXT("playableArea"), Terrain.Playable, OutError)) return false;
		if (!CSRoadNetworkFile_LoadMap(TerrainJson, Directory, TEXT("world"), TEXT("worldOffset"), TEXT("worldSize"), Terrain.World, OutError)) return false;
		static std::atomic<uint64> Loads{ 0 };
		Terrain.Serial = ++Loads;
	}
	return true;
}
}
