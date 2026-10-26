#include "CSRoadAssets.h"

#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace CSRoad
{
namespace
{
float CSRoadAssets_F(const TSharedPtr<FJsonObject>& N, const TCHAR* K, float Fallback = 0.0f)
{
	double Value = 0.0;
	return N.IsValid() && N->TryGetNumberField(K, Value) ? static_cast<float>(Value) : Fallback;
}

int32 CSRoadAssets_I(const TSharedPtr<FJsonObject>& N, const TCHAR* K, int32 Fallback = 0)
{
	double Value = 0.0;
	return N.IsValid() && N->TryGetNumberField(K, Value) ? static_cast<int32>(Value) : Fallback;
}

bool CSRoadAssets_B(const TSharedPtr<FJsonObject>& N, const TCHAR* K)
{
	if (!N.IsValid()) return false;
	const TSharedPtr<FJsonValue> V = N->TryGetField(K);
	if (!V.IsValid()) return false;
	if (V->Type == EJson::Boolean) return V->AsBool();
	if (V->Type == EJson::Number) return static_cast<int32>(V->AsNumber()) != 0;
	return false;
}

TSharedPtr<FJsonObject> CSRoadAssets_Obj(const TSharedPtr<FJsonObject>& N, const TCHAR* K)
{
	const TSharedPtr<FJsonObject>* Out = nullptr;
	return N.IsValid() && N->TryGetObjectField(K, Out) ? *Out : nullptr;
}

const TArray<TSharedPtr<FJsonValue>>& CSRoadAssets_Arr(const TSharedPtr<FJsonObject>& N, const TCHAR* K)
{
	static const TArray<TSharedPtr<FJsonValue>> Empty;
	const TArray<TSharedPtr<FJsonValue>>* Out = nullptr;
	return N.IsValid() && N->TryGetArrayField(K, Out) ? *Out : Empty;
}

float3 CSRoadAssets_V(const TSharedPtr<FJsonValue>& N)
{
	if (!N.IsValid()) return float3(0.0f);
	if (N->Type == EJson::Array)
	{
		const TArray<TSharedPtr<FJsonValue>>& A = N->AsArray();
		return float3(static_cast<float>(A[0]->AsNumber()), static_cast<float>(A[1]->AsNumber()), static_cast<float>(A[2]->AsNumber()));
	}
	if (N->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> O = N->AsObject();
		return float3(CSRoadAssets_F(O, TEXT("x")), CSRoadAssets_F(O, TEXT("y")), CSRoadAssets_F(O, TEXT("z")));
	}
	return float3(0.0f);
}

int32 CSRoadAssets_Ref(const TSharedPtr<FJsonObject>& N, const TCHAR* K)
{
	return CSRoadAssets_I(CSRoadAssets_Obj(N, K), TEXT("m_PathID"));
}

TSharedPtr<FJsonObject> CSRoadAssets_Parse(const TArray<uint8>& Bytes)
{
	FString Text;
	FFileHelper::BufferToString(Text, Bytes.GetData(), Bytes.Num());
	TSharedPtr<FJsonObject> Out;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	return FJsonSerializer::Deserialize(Reader, Out) ? Out : nullptr;
}

// .NET Array.Sort for the short NetPieceLane arrays: 2 and 3 elements use the
// SwapIfGreater network, up to 16 an insertion sort. Equal keys move exactly as there.
template<typename T, typename FCompare>
void CSRoadAssets_DotNetSort(TArray<T>& Keys, FCompare Compare)
{
	const int32 N = Keys.Num();
	auto SwapIfGreater = [&](int32 I, int32 J) { if (I != J && Compare(Keys[I], Keys[J]) > 0) Swap(Keys[I], Keys[J]); };
	if (N <= 1) return;
	if (N == 2)
	{
		SwapIfGreater(0, 1);
		return;
	}
	if (N == 3)
	{
		SwapIfGreater(0, 1);
		SwapIfGreater(0, 2);
		SwapIfGreater(1, 2);
		return;
	}
	ensureMsgf(N <= 16, TEXT("CSRoad: introsort partitioning for %d elements is not replicated"), N);
	for (int32 I = 0; I < N - 1; I++)
	{
		T Key = Keys[I + 1];
		int32 J = I;
		while (J >= 0 && Compare(Key, Keys[J]) < 0)
		{
			Keys[J + 1] = Keys[J];
			J--;
		}
		Keys[J + 1] = Key;
	}
}
}

bool FAssets::Load(const TArray<uint8>& PrefabsJson, const TArray<uint8>& RawPrefabsJson, const TArray64<uint8>* PiecesBin, FString& OutError)
{
	Catalog = CSRoadAssets_Parse(PrefabsJson);
	Raw = CSRoadAssets_Parse(RawPrefabsJson);
	Pieces = PiecesBin;
	if (!Catalog.IsValid() || !Raw.IsValid())
	{
		OutError = TEXT("prefabs.json or raw-prefabs.json is not valid JSON");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Name : CSRoadAssets_Arr(Catalog, TEXT("requirements"))) RequirementNames.Add(Name->AsString());

	// pieces.bin: "R2MESH01", count, then {key, vertex/index/submesh counts, payload}.
	if (Pieces)
	{
		const uint8* Data = Pieces->GetData();
		const int64 Size = Pieces->Num();
		if (Size < 12 || FMemory::Memcmp(Data, "R2MESH01", 8) != 0)
		{
			OutError = TEXT("pieces.bin has an unknown header");
			return false;
		}
		int64 Offset = 8;
		auto ReadInt = [&]() { int32 V = 0; FMemory::Memcpy(&V, Data + Offset, 4); Offset += 4; return V; };
		const int32 Count = ReadInt();
		for (int32 I = 0; I < Count; I++)
		{
			const int32 KeyLength = ReadInt();
			const FString Key = FString(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Data + Offset), KeyLength));
			Offset += KeyLength;
			const int32 Vertices = ReadInt();
			const int32 Indices = ReadInt();
			const int32 Submeshes = ReadInt();
			MeshEntries.Add(Key, TPair<int64, FIntVector>(Offset, FIntVector(Vertices, Indices, Submeshes)));
			Offset += int64(Vertices) * 48 + int64(Indices) * 4 + int64(Submeshes) * 16;
		}
		if (Offset != Size)
		{
			OutError = TEXT("pieces.bin size mismatch");
			return false;
		}
	}

	const TSharedPtr<FJsonObject> PieceCatalog = CSRoadAssets_Obj(Catalog, TEXT("pieces"));
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : PieceCatalog->Values)
	{
		const int32 Entity = FCString::Atoi(*Pair.Key);
		const TSharedPtr<FJsonObject> P = Pair.Value->AsObject();
		const TArray<TSharedPtr<FJsonValue>>& Heights = CSRoadAssets_Arr(P, TEXT("surface_heights"));
		const TArray<TSharedPtr<FJsonValue>>& Range = CSRoadAssets_Arr(P, TEXT("height_range"));
		NetPieceData Data{};
		Data.m_Width = CSRoadAssets_F(P, TEXT("width"));
		Data.m_Length = CSRoadAssets_F(P, TEXT("length"));
		Data.m_HeightRange = Bounds1(static_cast<float>(Range[0]->AsNumber()), static_cast<float>(Range[1]->AsNumber()));
		Data.m_SurfaceHeights = float4(static_cast<float>(Heights[0]->AsNumber()), static_cast<float>(Heights[1]->AsNumber()), static_cast<float>(Heights[2]->AsNumber()), static_cast<float>(Heights[3]->AsNumber()));
		Data.m_WidthOffset = CSRoadAssets_F(P, TEXT("width_offset"));
		Data.m_NodeOffset = CSRoadAssets_F(P, TEXT("node_offset"));
		Data.m_SideConnectionOffset = CSRoadAssets_F(P, TEXT("side_connection_offset"));
		PieceData.Add(Entity, Data);
		for (const TSharedPtr<FJsonObject>& C : Components(RawObject(Entity)))
		{
			const FString Type = C->GetStringField(TEXT("_type"));
			if (Type == TEXT("NetPieceCrosswalk"))
			{
				NetCrosswalkData Crosswalk{};
				Crosswalk.m_Lane = CSRoadAssets_Ref(C, TEXT("m_Lane"));
				Crosswalk.m_Start = CSRoadAssets_V(C->TryGetField(TEXT("m_Start")));
				Crosswalk.m_End = CSRoadAssets_V(C->TryGetField(TEXT("m_End")));
				CrosswalkData.Add(Entity, Crosswalk);
			}
			if (Type == TEXT("MatchPieceVertices"))
			{
				float3 Offsets(NAN);
				const TArray<TSharedPtr<FJsonValue>>& Values = CSRoadAssets_Arr(C, TEXT("m_Offsets"));
				for (int32 I = 0; I < FMath::Min(3, Values.Num()); I++) Offsets[I] = static_cast<float>(Values[I]->AsNumber());
				NetVertexMatchData Match{};
				Match.m_Offsets = Offsets;
				VertexMatches.Add(Entity, Match);
			}
			if (Type == TEXT("NetPieceLanes"))
			{
				TArray<NetPieceLane> Lanes;
				for (const TSharedPtr<FJsonValue>& L : CSRoadAssets_Arr(C, TEXT("m_Lanes")))
				{
					const TSharedPtr<FJsonObject> LO = L->AsObject();
					NetPieceLane Lane{};
					Lane.m_Lane = CSRoadAssets_Ref(LO, TEXT("m_Lane"));
					Lane.m_Position = CSRoadAssets_V(LO->TryGetField(TEXT("m_Position")));
					Lane.m_ExtraFlags = CSRoadAssets_B(LO, TEXT("m_FindAnchor")) ? LaneFlags_FindAnchor : 0u;
					Lanes.Add(Lane);
				}
				// NetPieceLane.CompareTo: by m_Position.x.
				CSRoadAssets_DotNetSort(Lanes, [](const NetPieceLane& A, const NetPieceLane& B)
				{
					return A.m_Position.x != B.m_Position.x ? (A.m_Position.x > B.m_Position.x ? 1 : -1) : 0;
				});
				PieceLanes.Add(Entity, MoveTemp(Lanes));
			}
		}
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Raw->Values)
	{
		const TSharedPtr<FJsonObject> P = Pair.Value->AsObject();
		const FString Type = P->GetStringField(TEXT("_type"));
		if (Type == TEXT("NetLanePrefab") || Type == TEXT("NetLaneGeometryPrefab")) LaneData.Add(FCString::Atoi(*Pair.Key), InitializeLane(P));
	}
	const TSharedPtr<FJsonObject> SectionCatalog = CSRoadAssets_Obj(Catalog, TEXT("sections"));
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : SectionCatalog->Values)
	{
		const int32 Entity = FCString::Atoi(*Pair.Key);
		const TSharedPtr<FJsonObject> S = Pair.Value->AsObject();
		TArray<NetSubSection> Subs;
		for (const TSharedPtr<FJsonValue>& SubValue : CSRoadAssets_Arr(S, TEXT("sub_sections")))
		{
			const TSharedPtr<FJsonObject> Sub = SubValue->AsObject();
			NetSubSection Item{};
			Item.m_SubSection = FCString::Atoi(*Sub->GetStringField(TEXT("section")));
			Requirements(Sub, TEXT("all"), Item.m_CompositionAll, Item.m_SectionAll);
			Requirements(Sub, TEXT("any"), Item.m_CompositionAny, Item.m_SectionAny);
			Requirements(Sub, TEXT("none"), Item.m_CompositionNone, Item.m_SectionNone);
			Subs.Add(Item);
		}
		SubSections.Add(Entity, MoveTemp(Subs));
		TArray<NetSectionPiece> SectionPieceList;
		for (const TSharedPtr<FJsonValue>& PieceValue : CSRoadAssets_Arr(S, TEXT("pieces")))
		{
			const TSharedPtr<FJsonObject> PO = PieceValue->AsObject();
			NetSectionPiece Item{};
			Item.m_Piece = FCString::Atoi(*PO->GetStringField(TEXT("piece")));
			Item.m_Offset = CSRoadAssets_V(PO->TryGetField(TEXT("offset")));
			Requirements(PO, TEXT("all"), Item.m_CompositionAll, Item.m_SectionAll);
			Requirements(PO, TEXT("any"), Item.m_CompositionAny, Item.m_SectionAny);
			Requirements(PO, TEXT("none"), Item.m_CompositionNone, Item.m_SectionNone);
			Item.m_Flags = InitializePieceFlags(Item.m_Piece);
			SectionPieceList.Add(Item);
		}
		SectionPieces.Add(Entity, MoveTemp(SectionPieceList));
	}
	const TSharedPtr<FJsonObject> RoadCatalog = CSRoadAssets_Obj(Catalog, TEXT("roads"));
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : RoadCatalog->Values)
	{
		const FString& Name = Pair.Key;
		const TSharedPtr<FJsonObject> Road = Pair.Value->AsObject();
		const int32 Id = CSRoadAssets_I(Road, TEXT("path_id"));
		Names.Add(Name);
		RoadIds.Add(Name, Id);
		IdNames.Add(Id, Name);
		const TSharedPtr<FJsonObject> RawRoad = RoadRaw(Name);
		auto RawFlags = [](const TSharedPtr<FJsonObject>& State, const TCHAR* Key)
		{
			TArray<uint> Values;
			for (const TSharedPtr<FJsonValue>& V : CSRoadAssets_Arr(State, Key)) Values.Add(static_cast<uint>(V->AsNumber()));
			CompositionFlags Flags;
			uint SectionsUnused = 0;
			NetCompositionHelpers::GetRequirementFlags(Values, Flags, SectionsUnused);
			return Flags;
		};
		TArray<NetGeometryEdgeState> Edge;
		for (const TSharedPtr<FJsonValue>& V : CSRoadAssets_Arr(RawRoad, TEXT("m_EdgeStates")))
		{
			const TSharedPtr<FJsonObject> S = V->AsObject();
			NetGeometryEdgeState State{};
			State.m_CompositionAll = RawFlags(S, TEXT("m_RequireAll"));
			State.m_CompositionAny = RawFlags(S, TEXT("m_RequireAny"));
			State.m_CompositionNone = RawFlags(S, TEXT("m_RequireNone"));
			State.m_State = RawFlags(S, TEXT("m_SetState"));
			Edge.Add(State);
		}
		EdgeStateData.Add(Id, MoveTemp(Edge));
		TArray<NetGeometryNodeState> Node;
		for (const TSharedPtr<FJsonValue>& V : CSRoadAssets_Arr(RawRoad, TEXT("m_NodeStates")))
		{
			const TSharedPtr<FJsonObject> S = V->AsObject();
			NetGeometryNodeState State{};
			State.m_CompositionAll = RawFlags(S, TEXT("m_RequireAll"));
			State.m_CompositionAny = RawFlags(S, TEXT("m_RequireAny"));
			State.m_CompositionNone = RawFlags(S, TEXT("m_RequireNone"));
			State.m_State = RawFlags(S, TEXT("m_SetState"));
			State.m_MatchType = static_cast<uint>(CSRoadAssets_I(S, TEXT("m_MatchType")));
			Node.Add(State);
		}
		NodeStateData.Add(Id, MoveTemp(Node));
		TArray<NetGeometrySection> RoadSections;
		AddSections(CSRoadAssets_Arr(Road, TEXT("sections")), RoadSections, 0);
		if (Road->HasTypedField<EJson::Array>(TEXT("underground"))) AddSections(CSRoadAssets_Arr(Road, TEXT("underground")), RoadSections, NetSectionFlags_Underground);
		if (Road->HasTypedField<EJson::Array>(TEXT("overhead"))) AddSections(CSRoadAssets_Arr(Road, TEXT("overhead")), RoadSections, NetSectionFlags_Overhead);
		Sections.Add(Name, MoveTemp(RoadSections));
	}
	return true;
}

TSharedPtr<FJsonObject> FAssets::RawObject(int32 Id) const
{
	return CSRoadAssets_Obj(Raw, *FString::FromInt(Id));
}

TArray<TSharedPtr<FJsonObject>> FAssets::Components(const TSharedPtr<FJsonObject>& Prefab) const
{
	TArray<TSharedPtr<FJsonObject>> Result;
	for (const TSharedPtr<FJsonValue>& Reference : CSRoadAssets_Arr(Prefab, TEXT("components")))
	{
		const TSharedPtr<FJsonObject> C = RawObject(CSRoadAssets_I(Reference->AsObject(), TEXT("m_PathID")));
		if (C.IsValid() && CSRoadAssets_B(C, TEXT("active"))) Result.Add(C);
	}
	return Result;
}

TSharedPtr<FJsonObject> FAssets::RoadRaw(const FString& Name) const
{
	const TSharedPtr<FJsonObject> Road = CSRoadAssets_Obj(CSRoadAssets_Obj(Catalog, TEXT("roads")), *Name);
	return RawObject(CSRoadAssets_I(Road, TEXT("path_id")));
}

void FAssets::Requirements(const TSharedPtr<FJsonObject>& Item, const TCHAR* Key, CompositionFlags& OutFlags, uint& OutSections) const
{
	TArray<uint> Values;
	for (const TSharedPtr<FJsonValue>& V : CSRoadAssets_Arr(Item, Key))
	{
		const int32 Index = RequirementNames.IndexOfByKey(V->AsString());
		checkf(Index != INDEX_NONE, TEXT("Unknown NetPieceRequirements %s"), *V->AsString());
		Values.Add(static_cast<uint>(Index));
	}
	NetCompositionHelpers::GetRequirementFlags(Values, OutFlags, OutSections);
}

void FAssets::AddSections(const TArray<TSharedPtr<FJsonValue>>& Source, TArray<NetGeometrySection>& Target, uint Flags) const
{
	TArray<int32> Median;
	for (int32 I = 0; I < Source.Num(); I++)
	{
		if (CSRoadAssets_B(Source[I]->AsObject(), TEXT("median"))) Median.Add(I * 2);
	}
	const int32 Left = Median.Num() > 0 ? FMath::Min(Median) : Source.Num() - 1;
	const int32 Right = Median.Num() > 0 ? FMath::Max(Median) : Source.Num() - 1;
	if (Median.Num() == 0) Flags |= NetSectionFlags_AlignCenter;
	for (int32 I = 0; I < Source.Num(); I++)
	{
		const TSharedPtr<FJsonObject> S = Source[I]->AsObject();
		uint F = Flags;
		if (CSRoadAssets_B(S, TEXT("invert"))) F |= NetSectionFlags_Invert;
		if (CSRoadAssets_B(S, TEXT("flip"))) F |= NetSectionFlags_FlipLanes | NetSectionFlags_FlipMesh;
		if (CSRoadAssets_B(S, TEXT("half_length"))) F |= NetSectionFlags_HalfLength;
		const int32 H = CSRoadAssets_I(S, TEXT("hidden_layers"));
		if ((H & 15) == 15) F |= NetSectionFlags_Hidden;
		if ((H & 1) != 0) F |= NetSectionFlags_HiddenSurface;
		if ((H & 2) != 0) F |= NetSectionFlags_HiddenBottom;
		if ((H & 4) != 0) F |= NetSectionFlags_HiddenTop;
		if ((H & 8) != 0) F |= NetSectionFlags_HiddenSide;
		F |= I * 2 < Left ? NetSectionFlags_Left : (I * 2 > Right ? NetSectionFlags_Right : NetSectionFlags_Median);
		NetGeometrySection Item{};
		Item.m_Section = FCString::Atoi(*S->GetStringField(TEXT("section")));
		Item.m_Offset = CSRoadAssets_V(S->TryGetField(TEXT("offset")));
		Item.m_Flags = F;
		uint Unused = 0;
		Requirements(S, TEXT("all"), Item.m_CompositionAll, Unused);
		Requirements(S, TEXT("any"), Item.m_CompositionAny, Unused);
		Requirements(S, TEXT("none"), Item.m_CompositionNone, Unused);
		Target.Add(Item);
	}
}

uint FAssets::InitializePieceFlags(int32 Piece) const
{
	const TSharedPtr<FJsonObject> P = CSRoadAssets_Obj(CSRoadAssets_Obj(Catalog, TEXT("pieces")), *FString::FromInt(Piece));
	const FString Layer = P->GetStringField(TEXT("layer"));
	uint F = Layer == TEXT("surface") ? NetPieceFlags_Surface : Layer == TEXT("bottom") ? NetPieceFlags_Bottom : Layer == TEXT("top") ? NetPieceFlags_Top : NetPieceFlags_Side;
	if (CSRoadAssets_I(P, TEXT("mesh_count")) > 0) F |= NetPieceFlags_HasMesh;
	for (const TSharedPtr<FJsonObject>& C : Components(RawObject(Piece)))
	{
		const FString Type = C->GetStringField(TEXT("_type"));
		if (Type == TEXT("NetDividerPiece"))
		{
			if (CSRoadAssets_B(C, TEXT("m_PreserveShape"))) F |= NetPieceFlags_PreserveShape | NetPieceFlags_DisableTiling;
			if (CSRoadAssets_B(C, TEXT("m_BlockTraffic"))) F |= NetPieceFlags_BlockTraffic;
			if (CSRoadAssets_B(C, TEXT("m_BlockCrosswalk"))) F |= NetPieceFlags_BlockCrosswalk;
		}
		else if (Type == TEXT("AsymmetricPieceMesh"))
		{
			if (CSRoadAssets_B(C, TEXT("m_Sideways"))) F |= NetPieceFlags_AsymmetricMeshX;
			if (CSRoadAssets_B(C, TEXT("m_Lengthwise"))) F |= NetPieceFlags_AsymmetricMeshZ;
		}
		else if (Type == TEXT("NetPieceTiling"))
		{
			if (CSRoadAssets_B(C, TEXT("m_DisableTextureTiling"))) F |= NetPieceFlags_DisableTiling;
		}
		else if (Type == TEXT("MovePieceVertices"))
		{
			if (CSRoadAssets_B(C, TEXT("m_LowerBottomToTerrain"))) F |= NetPieceFlags_LowerBottomToTerrain;
			if (CSRoadAssets_B(C, TEXT("m_RaiseTopToTerrain"))) F |= NetPieceFlags_RaiseTopToTerrain;
			if (CSRoadAssets_B(C, TEXT("m_SmoothTopNormal"))) F |= NetPieceFlags_SmoothTopNormal;
		}
	}
	return F;
}

NetLaneData FAssets::InitializeLane(const TSharedPtr<FJsonObject>& Prefab) const
{
	NetLaneData Result{};
	const int32 Pathfind = CSRoadAssets_Ref(Prefab, TEXT("m_PathfindPrefab"));
	if (Pathfind != 0)
	{
		Result.m_PathfindPrefab = Pathfind;
		if (CSRoadAssets_B(RawObject(Pathfind), TEXT("m_TrackTrafficFlow"))) Result.m_Flags |= LaneFlags_TrackFlow;
	}
	for (const TSharedPtr<FJsonObject>& C : Components(Prefab))
	{
		const FString Type = C->GetStringField(TEXT("_type"));
		if (Type == TEXT("CarLane"))
		{
			Result.m_Flags |= LaneFlags_Road;
			Result.m_Width = CSRoadAssets_F(C, TEXT("m_Width"));
			if (CSRoadAssets_B(C, TEXT("m_StartingLane"))) Result.m_Flags |= LaneFlags_DisconnectedStart;
			if (CSRoadAssets_B(C, TEXT("m_EndingLane"))) Result.m_Flags |= LaneFlags_DisconnectedEnd;
			if (CSRoadAssets_B(C, TEXT("m_Twoway"))) Result.m_Flags |= LaneFlags_Twoway;
			if (CSRoadAssets_B(C, TEXT("m_BusLane"))) Result.m_Flags |= LaneFlags_PublicOnly;
			const int32 RoadType = CSRoadAssets_I(C, TEXT("m_RoadType"));
			if (RoadType == static_cast<int32>(RoadTypes_Watercraft)) Result.m_Flags |= LaneFlags_OnWater;
			if (RoadType == static_cast<int32>(RoadTypes_Bicycle)) Result.m_Flags |= LaneFlags_BicyclesOnly;
		}
		else if (Type == TEXT("TrackLane"))
		{
			Result.m_Flags |= LaneFlags_Track;
			Result.m_Width = CSRoadAssets_F(C, TEXT("m_Width"));
			if (CSRoadAssets_B(C, TEXT("m_Twoway"))) Result.m_Flags |= LaneFlags_Twoway;
		}
		else if (Type == TEXT("UtilityLane"))
		{
			Result.m_Flags |= LaneFlags_Utility;
			Result.m_Width = CSRoadAssets_F(C, TEXT("m_Width"));
			if (CSRoadAssets_B(C, TEXT("m_Underground"))) Result.m_Flags |= LaneFlags_Underground;
		}
		else if (Type == TEXT("PedestrianLane"))
		{
			Result.m_Flags |= LaneFlags_Pedestrian | LaneFlags_Twoway;
			Result.m_Width = CSRoadAssets_F(C, TEXT("m_Width"));
			if (CSRoadAssets_B(C, TEXT("m_OnWater"))) Result.m_Flags |= LaneFlags_OnWater;
		}
		else if (Type == TEXT("ParkingLane"))
		{
			Result.m_Flags |= LaneFlags_Parking;
			const TSharedPtr<FJsonObject> SlotSize = CSRoadAssets_Obj(C, TEXT("m_SlotSize"));
			float2 Size(CSRoadAssets_F(SlotSize, TEXT("x")), CSRoadAssets_F(SlotSize, TEXT("y")));
			Size = math::select(Size, float2(0.0f), Size < float2(0.001f));
			const float Angle = math::radians(math::clamp(CSRoadAssets_F(C, TEXT("m_SlotAngle")), 0.0f, 90.0f));
			const float2 Direction(math::cos(Angle), math::sin(Angle));
			Result.m_Width = math::dot(Size, Direction);
			if (Size.y != 0.0f && Size.y < Result.m_Width) Result.m_Width = Size.y;
			if (Direction.x < 0.001f) Result.m_Flags |= LaneFlags_Twoway;
			if (Size.x == 0.0f) Result.m_Flags |= LaneFlags_Virtual;
			if (CSRoadAssets_B(C, TEXT("m_SpecialVehicles"))) Result.m_Flags |= LaneFlags_PublicOnly;
		}
	}
	return Result;
}

const FComposition& FAssets::Compose(const FString& Name, CompositionFlags Flags, bool bResolveStates)
{
	const bool bNode = (Flags.m_General & CompositionFlags_General_Node) != 0;
	const int32 Id = RoadId(Name);
	if (bResolveStates)
	{
		// RoadAssets.Compose resolves states through CompositionSelectSystem; equivalent here.
		CompositionFlags States{};
		if (bNode)
		{
			for (const NetGeometryNodeState& NodeState : NodeStateData.FindChecked(Id))
			{
				// GetNodeStates(states, flags, flags, true, true) with both sides kept.
				const bool2 Match(NetCompositionHelpers::TestEdgeFlags(NodeState, Flags), NetCompositionHelpers::TestEdgeFlags(NodeState, Flags));
				if (NetCompositionHelpers::TestEdgeMatch(NodeState, Match)) States |= NodeState.m_State;
			}
		}
		else
		{
			for (const NetGeometryEdgeState& EdgeState : EdgeStateData.FindChecked(Id))
			{
				if (NetCompositionHelpers::TestEdgeFlags(EdgeState, Flags)) States |= EdgeState.m_State;
			}
		}
		Flags |= States;
	}
	const TPair<FString, CompositionFlags> Key(Name, Flags);
	if (const TSharedPtr<FComposition>* Cached = CompositionCache.Find(Key)) return **Cached;
	TSharedPtr<FComposition> Result = MakeShared<FComposition>();
	Result->Prefab = Id;
	TArray<NetCompositionPiece>& PieceList = Result->Pieces;
	NetCompositionHelpers::GetCompositionPieces(PieceList, Sections.FindChecked(Name), Flags, SubSections, SectionPieces);
	NetCompositionData Data{};
	Data.m_Flags = Flags;
	NetCompositionHelpers::CalculateCompositionData(Data, PieceList, PieceData, VertexMatches);
	NetCompositionHelpers::AddCompositionLanes(Id, Data, PieceList, Result->Lanes, &Result->Carriageways, LaneData, PieceLanes);
	if (bNode) NetCompositionSystem::AddCompositionCrosswalks(Data, PieceList, Result->Crosswalks, CrosswalkData, LaneData, PieceLanes);
	// NetCompositionSystem.InitializeCompositionJob: roads carry GeometryFlags.BlockZone.
	Data.m_State |= CompositionState_BlockZone;
	Result->Data = Data;
	CompositionCache.Add(Key, Result);
	return *Result;
}

NetGeometryData FAssets::Geometry(const FString& Name)
{
	if (const NetGeometryData* Cached = GeometryCache.Find(Name)) return *Cached;
	const TSharedPtr<FJsonObject> RawRoad = RoadRaw(Name);
	const FComposition& Edge = Compose(Name, CompositionFlags{}, false);
	const FComposition& Elevated = Compose(Name, MakeFlags(CompositionFlags_General_Elevated, 0, 0), false);
	NetGeometryData Data{};
	Data.m_DefaultWidth = Edge.Data.m_Width;
	Data.m_ElevatedWidth = Elevated.Data.m_Width;
	Data.m_DefaultHeightRange = Edge.Data.m_HeightRange;
	Data.m_DefaultSurfaceHeight = Edge.Data.m_SurfaceHeight;
	Data.m_ElevatedHeightRange = Elevated.Data.m_HeightRange;
	Data.m_EdgeLengthRange = Bounds1(0.0f, 200.0f);
	Data.m_ElevatedLength = 80.0f;
	Data.m_ElevationLimit = 4.0f;
	const bool bHighway = CSRoadAssets_B(RawRoad, TEXT("m_HighwayRules"));
	Data.m_MinNodeOffset = bHighway ? 2.0f + Edge.Data.m_Width * 0.5f : 0.0f;
	const float Steepness = CSRoadAssets_F(RawRoad, TEXT("m_MaxSlopeSteepness"));
	Data.m_MaxSlopeSteepness = Steepness < 0.001f ? 0.0f : Steepness;
	Data.m_AggregateType = CSRoadAssets_Ref(RawRoad, TEXT("m_AggregateType"));
	Data.m_StyleType = CSRoadAssets_Ref(RawRoad, TEXT("m_StyleType"));
	Data.m_MergeLayers = Layer_Road | Layer_TramTrack | Layer_PublicTransportRoad;
	Data.m_IntersectLayers = Layer_Road | Layer_TrainTrack | Layer_Pathway | Layer_TramTrack | Layer_PublicTransportRoad;
	Data.m_Flags = GeometryFlags_SupportRoundabout | GeometryFlags_BlockZone | GeometryFlags_Directional | GeometryFlags_FlattenTerrain | GeometryFlags_ClipTerrain | (bHighway ? GeometryFlags_SmoothElevation : 0u);
	switch (CSRoadAssets_I(RawRoad, TEXT("m_InvertMode")))
	{
	case 0: Data.m_Flags |= GeometryFlags_InvertCompositionHandedness; break;
	case 1: Data.m_Flags |= GeometryFlags_FlipCompositionHandedness; break;
	case 2: Data.m_Flags |= GeometryFlags_IsLefthanded | GeometryFlags_InvertCompositionHandedness; break;
	case 3: Data.m_Flags |= GeometryFlags_IsLefthanded | GeometryFlags_FlipCompositionHandedness; break;
	default: break;
	}
	if (CSRoadAssets_Ref(RawRoad, TEXT("m_ZoneBlock")) != 0) Data.m_Flags |= GeometryFlags_SnapCellSize;
	const bool bForward = (Edge.Data.m_State & (CompositionState_HasForwardRoadLanes | CompositionState_HasForwardTrackLanes)) != 0;
	const bool bBackward = (Edge.Data.m_State & (CompositionState_HasBackwardRoadLanes | CompositionState_HasBackwardTrackLanes)) != 0;
	if (bForward != bBackward) Data.m_Flags |= GeometryFlags_FlipTrafficHandedness;
	if ((Edge.Data.m_State & CompositionState_Asymmetric) != 0) Data.m_Flags |= GeometryFlags_Asymmetric;
	if ((Edge.Data.m_State & CompositionState_ExclusiveGround) != 0) Data.m_Flags |= GeometryFlags_ExclusiveGround;
	for (const TSharedPtr<FJsonObject>& C : Components(RawRoad))
	{
		if (C->GetStringField(TEXT("_type")) != TEXT("Bridge")) continue;
		const float Length = CSRoadAssets_F(C, TEXT("m_SegmentLength"));
		if (Length > 0.1f)
		{
			Data.m_EdgeLengthRange.min = CSRoadAssets_B(C, TEXT("m_AllowMinimalLength")) ? 0.0f : Length * 0.6f;
			Data.m_EdgeLengthRange.max = Length * 1.4f;
		}
		Data.m_ElevatedLength = Data.m_EdgeLengthRange.max;
		Data.m_Hanging = CSRoadAssets_F(C, TEXT("m_Hanging"));
		Data.m_Flags |= GeometryFlags_StraightEdges | GeometryFlags_StraightEnds | GeometryFlags_RequireElevated | GeometryFlags_SymmetricalEdges | GeometryFlags_SmoothSlopes;
		if (CSRoadAssets_B(C, TEXT("m_CanCurve"))) Data.m_Flags &= ~GeometryFlags_StraightEdges;
		const int32 Style = CSRoadAssets_I(C, TEXT("m_BuildStyle"));
		if (Style == 1 || Style == 2) Data.m_Flags |= GeometryFlags_ElevatedIsRaised;
		if (Style == 2) Data.m_Flags &= ~GeometryFlags_RequireElevated;
	}
	GeometryCache.Add(Name, Data);
	return Data;
}

NetData FAssets::Network(const FString& Name)
{
	const TSharedPtr<FJsonObject> RawRoad = RoadRaw(Name);
	bool bBridge = false;
	for (const TSharedPtr<FJsonObject>& C : Components(RawRoad)) bBridge |= C->GetStringField(TEXT("_type")) == TEXT("Bridge");
	NetData Data{};
	Data.m_RequiredLayers = CSRoadAssets_I(RawRoad, TEXT("m_RoadType")) == 1 ? Layer_PublicTransportRoad : Layer_Road;
	Data.m_ConnectLayers = Layer_Road | Layer_TrainTrack | Layer_Pathway | Layer_TramTrack | Layer_Fence | Layer_PublicTransportRoad;
	Data.m_LocalConnectLayers = Layer_Pathway | Layer_MarkerPathway;
	Data.m_NodePriority = 2000.0f + Geometry(Name).m_DefaultWidth + (bBridge ? 1000.0f : 0.0f);
	return Data;
}

RoadData FAssets::Road(const FString& Name)
{
	const TSharedPtr<FJsonObject> RawRoad = RoadRaw(Name);
	uint F = 0;
	if (CSRoadAssets_B(RawRoad, TEXT("m_HighwayRules"))) F |= RoadFlags_UseHighwayRules;
	if (CSRoadAssets_B(RawRoad, TEXT("m_TrafficLights"))) F |= RoadFlags_PreferTrafficLights;
	if (CSRoadAssets_Ref(RawRoad, TEXT("m_ZoneBlock")) != 0) F |= RoadFlags_EnableZoning;
	const uint State = Compose(Name, CompositionFlags{}, false).Data.m_State;
	const uint Lanes = State & (CompositionState_HasForwardRoadLanes | CompositionState_HasBackwardRoadLanes);
	if (Lanes == CompositionState_HasForwardRoadLanes) F |= RoadFlags_DefaultIsForward;
	if (Lanes == CompositionState_HasBackwardRoadLanes) F |= RoadFlags_DefaultIsBackward;
	RoadData Data{};
	Data.m_Flags = F;
	Data.m_SpeedLimit = CSRoadAssets_F(RawRoad, TEXT("m_SpeedLimit")) / 3.6f;
	return Data;
}

PlaceableNetData FAssets::Placeable(const FString& Name)
{
	PlaceableNetData Data{};
	Data.m_SnapDistance = 8.0f;
	Data.m_MinWaterElevation = 5.0f;
	Data.m_ElevationRange = Bounds1(-50.0f, 50.0f);
	Data.m_PlacementFlags = PlacementFlags_OnGround;
	// NetInitializeSystem: AuxiliaryNets.m_LinkEndOffsets sets LinkAuxOffsets.
	for (const TSharedPtr<FJsonObject>& C : Components(RoadRaw(Name)))
	{
		if (C->GetStringField(TEXT("_type")) == TEXT("AuxiliaryNets") && CSRoadAssets_B(C, TEXT("m_LinkEndOffsets"))) Data.m_PlacementFlags |= PlacementFlags_LinkAuxOffsets;
	}
	return Data;
}

FString FAssets::AtlasBaseColor(const FString& Atlas) const
{
	const TSharedPtr<FJsonObject>* Bindings = nullptr;
	const TSharedPtr<FJsonObject>* Binding = nullptr;
	FString BaseColor;
	if (Catalog.IsValid() && Catalog->TryGetObjectField(TEXT("material_bindings"), Bindings) && (*Bindings)->TryGetObjectField(Atlas, Binding)
		&& (*Binding)->TryGetStringField(TEXT("BaseColor"), BaseColor)) return BaseColor;
	return Atlas + TEXT("_BaseColor");
}

const FPieceMesh* FAssets::PieceMesh(int32 Piece)
{
	if (const TSharedPtr<FPieceMesh>* Cached = MeshCache.Find(Piece)) return Cached->Get();
	const TSharedPtr<FJsonObject> P = CSRoadAssets_Obj(CSRoadAssets_Obj(Catalog, TEXT("pieces")), *FString::FromInt(Piece));
	FString Key;
	if (!Pieces || !P.IsValid() || !P->TryGetStringField(TEXT("geometry"), Key))
	{
		MeshCache.Add(Piece, nullptr);
		return nullptr;
	}
	TSharedPtr<FPieceMesh> Mesh = MakeShared<FPieceMesh>();
	const uint8* Data = Pieces->GetData();
	const TArray<TSharedPtr<FJsonValue>>& Surfaces = CSRoadAssets_Arr(P, TEXT("surfaces"));
	const int32 Parts = FMath::Max(1, CSRoadAssets_I(P, TEXT("mesh_count")));
	for (int32 Part = 0; Part < Parts; Part++)
	{
		const TPair<int64, FIntVector>* Entry = MeshEntries.Find(Part == 0 ? Key : FString::Printf(TEXT("%s#%d"), *Key, Part));
		checkf(Entry, TEXT("CSRoad: pieces.bin is missing %s#%d"), *Key, Part);
		int64 Offset = Entry->Key;
		auto ReadInt = [&]() { int32 V = 0; FMemory::Memcpy(&V, Data + Offset, 4); Offset += 4; return V; };
		const int32 VertexOffset = Mesh->Vertices.Num();
		const int32 IndexOffset = Mesh->Indices.Num();
		for (int32 I = 0; I < Entry->Value.X; I++)
		{
			// 48 bytes per vertex: position, normal, tangent (xyzw), uv0. Read in place; argument
			// evaluation order inside one constructor call is unspecified.
			float V[12];
			FMemory::Memcpy(V, Data + Offset, sizeof(V));
			Offset += sizeof(V);
			Mesh->Vertices.Add(float3(V[0], V[1], V[2]));
			Mesh->Normals.Add(float3(V[3], V[4], V[5]));
			Mesh->Tangents.Add(float4(V[6], V[7], V[8], V[9]));
			Mesh->UV0s.Add(float2(V[10], V[11]));
		}
		for (int32 I = 0; I < Entry->Value.Y; I++) Mesh->Indices.Add(ReadInt() + VertexOffset);
		const FString Atlas = Surfaces.IsValidIndex(Part) ? Surfaces[Part]->AsString() : FString(TEXT("__missing"));
		int32 MaterialIndex = AtlasNames.IndexOfByKey(Atlas);
		if (MaterialIndex == INDEX_NONE) MaterialIndex = AtlasNames.Add(Atlas);
		for (int32 I = 0; I < Entry->Value.Z; I++)
		{
			MeshMaterial Material{};
			Material.m_StartIndex = ReadInt() + IndexOffset;
			Material.m_IndexCount = ReadInt();
			Material.m_StartVertex = ReadInt() + VertexOffset;
			Material.m_VertexCount = ReadInt();
			Material.m_MaterialIndex = MaterialIndex;
			Mesh->Materials.Add(Material);
		}
	}
	MeshCache.Add(Piece, Mesh);
	return Mesh.Get();
}
}
