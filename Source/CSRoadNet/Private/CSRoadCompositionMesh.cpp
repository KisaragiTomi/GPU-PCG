#include "CSRoadCompositionMesh.h"

namespace CSRoad
{
namespace
{
uint CSRoadMesh_AsUint(float F) { uint U; FMemory::Memcpy(&U, &F, 4); return U; }
float CSRoadMesh_AsFloat(uint U) { float F; FMemory::Memcpy(&F, &U, 4); return F; }

int32 CSRoadMesh_GetMaterial(TArray<MeshMaterial>& Materials, const MeshMaterial& PieceMaterial)
{
	const int32 Length = Materials.Num();
	for (int32 I = 0; I < Length; I++)
	{
		if (Materials[I].m_MaterialIndex == PieceMaterial.m_MaterialIndex) return I;
	}
	MeshMaterial Added{};
	Added.m_MaterialIndex = PieceMaterial.m_MaterialIndex;
	Materials.Add(Added);
	return Length;
}

void CSRoadMesh_AddHeightBounds(TArray<Bounds1>& HeightBounds, float3 A, float3 B, int32 AIndex, int32 BIndex)
{
	if (AIndex <= BIndex)
	{
		const float Num = 1.0f / static_cast<float>(BIndex - AIndex + 1);
		for (int32 I = AIndex; I <= BIndex; I++) HeightBounds[I] |= math::lerp(A.y, B.y, static_cast<float>(I - AIndex) * Num);
	}
	else
	{
		const float Num3 = 1.0f / static_cast<float>(AIndex - BIndex + 1);
		for (int32 J = BIndex; J <= AIndex; J++) HeightBounds[J] |= math::lerp(B.y, A.y, static_cast<float>(J - BIndex) * Num3);
	}
}

void CSRoadMesh_InitializeHeightBounds(TArray<Bounds1>& HeightBounds, const NetCompositionPiece& Piece, const FPieceMesh& Mesh)
{
	const int32 Num = HeightBounds.Num() - 1;
	const float Num2 = static_cast<float>(Num) / Piece.m_Size.z;
	for (int32 I = 0; I <= Num; I++) HeightBounds[I] = Bounds1(FLT_MAX, -FLT_MAX);
	for (int32 J = 0; J < Mesh.Indices.Num(); J += 3)
	{
		const float3 V1 = Mesh.Vertices[Mesh.Indices[J]];
		const float3 V2 = Mesh.Vertices[Mesh.Indices[J + 1]];
		const float3 V3 = Mesh.Vertices[Mesh.Indices[J + 2]];
		const int32 Num3 = math::clamp(RoundToInt(V1.z * Num2) + (Num >> 1), 0, Num);
		const int32 Num4 = math::clamp(RoundToInt(V2.z * Num2) + (Num >> 1), 0, Num);
		const int32 Num5 = math::clamp(RoundToInt(V3.z * Num2) + (Num >> 1), 0, Num);
		CSRoadMesh_AddHeightBounds(HeightBounds, V1, V2, Num3, Num4);
		CSRoadMesh_AddHeightBounds(HeightBounds, V2, V3, Num4, Num5);
		CSRoadMesh_AddHeightBounds(HeightBounds, V3, V1, Num5, Num3);
	}
}

Bounds1 CSRoadMesh_GetHeightBounds(const TArray<Bounds1>& HeightBounds, const NetCompositionPiece& Piece, float Z)
{
	const int32 Num = HeightBounds.Num() - 1;
	const float Num2 = static_cast<float>(Num) / Piece.m_Size.z;
	return HeightBounds[math::clamp(RoundToInt(Z * Num2) + (Num >> 1), 0, Num)];
}
}

// Unity.Mathematics math.f32tof16 / f16tof32, bit for bit.
uint F32ToF16(float X)
{
	const int32 Infinity32 = 255 << 23;
	const uint Mask = 0x7FFFF000u;
	const uint UX = CSRoadMesh_AsUint(X);
	const uint UUX = UX & Mask;
	uint H = (CSRoadMesh_AsUint(math::min(CSRoadMesh_AsFloat(UUX) * 1.92592994e-34f, 260042752.0f)) + 0x1000u) >> 13;
	H = static_cast<int32>(UUX) >= Infinity32 ? (static_cast<int32>(UUX) > Infinity32 ? 0x7e00u : 0x7c00u) : H;
	return H | (UX & ~Mask) >> 16;
}

float F16ToF32(uint X)
{
	const uint ShiftedExp = 0x7c00u << 13;
	uint UF = (X & 0x7fffu) << 13;
	const uint E = UF & ShiftedExp;
	UF += (127u - 15u) << 23;
	UF += E == ShiftedExp ? (128u - 16u) << 23 : 0u;
	UF = E == 0 ? CSRoadMesh_AsUint(CSRoadMesh_AsFloat(UF + (1u << 23)) - 6.10351563e-05f) : UF;
	UF |= (X & 0x8000u) << 16;
	return CSRoadMesh_AsFloat(UF);
}

void GenerateCompositionMesh(FAssets& Assets, const FComposition& Composition, FCompositionMesh& Out)
{
	Out = FCompositionMesh();
	TArray<NetCompositionPiece> Pieces;
	for (const NetCompositionPiece& Piece : Composition.Pieces)
	{
		if ((Piece.m_SectionFlags & NetSectionFlags_Hidden) == 0) Pieces.Add(Piece);
	}
	const NetCompositionData& Data = Composition.Data;
	const bool bFlag = (Data.m_Flags.m_General & CompositionFlags_General_Node) != 0;
	const bool bFlag2 = (Data.m_Flags.m_General & CompositionFlags_General_Roundabout) != 0;
	int32 Num = 0;
	int32 Num2 = 0;
	int32 Num3 = 0;
	const float MiddleOffset = Data.m_MiddleOffset;
	TArray<MeshMaterial>& Materials = Out.Materials;
	float3 F;
	float3 F2;
	for (const NetCompositionPiece& Piece : Pieces)
	{
		const bool bFlag3 = (Piece.m_SectionFlags & NetSectionFlags_Invert) != 0;
		const bool bFlag4 = (Piece.m_SectionFlags & NetSectionFlags_FlipMesh) != 0;
		const bool bFlag5 = (Piece.m_SectionFlags & NetSectionFlags_HalfLength) != 0;
		const bool bFlag6 = (Piece.m_PieceFlags & NetPieceFlags_PreserveShape) != 0;
		const bool bFlag7 = (bFlag && !bFlag2 && !bFlag6) || bFlag5;
		const bool bFlag8 = (Piece.m_PieceFlags & NetPieceFlags_SkipBottomHalf) != 0;
		const FPieceMesh* Mesh = Assets.PieceMesh(Piece.m_Piece);
		if (!Mesh) continue;
		for (const MeshMaterial& PieceMaterial : Mesh->Materials)
		{
			const int32 Material = CSRoadMesh_GetMaterial(Materials, PieceMaterial);
			MeshMaterial Value2 = Materials[Material];
			Value2.m_VertexCount += PieceMaterial.m_VertexCount;
			Value2.m_IndexCount += PieceMaterial.m_IndexCount;
			Num2 = math::max(Num2, PieceMaterial.m_VertexCount);
			const int3 Order = PieceMaterial.m_StartIndex + math::select(int3(0, 1, 2), int3(2, 1, 0), bFlag3 != bFlag4);
			if (bFlag7)
			{
				for (int32 L = 0; L < PieceMaterial.m_VertexCount; L++) Value2.m_VertexCount -= Mesh->Vertices[PieceMaterial.m_StartVertex + L].z >= -0.01f ? 0 : 1;
				for (int32 M = 0; M < PieceMaterial.m_IndexCount; M += 3)
				{
					const int3 T = M + Order;
					F = float3(Mesh->Vertices[Mesh->Indices[T.x]].z, Mesh->Vertices[Mesh->Indices[T.y]].z, Mesh->Vertices[Mesh->Indices[T.z]].z);
					Value2.m_IndexCount -= math::all(F >= float3(-0.01f)) ? 0 : 3;
				}
			}
			else if (bFlag8)
			{
				for (int32 N = 0; N < PieceMaterial.m_VertexCount; N++) Value2.m_VertexCount -= Mesh->Vertices[PieceMaterial.m_StartVertex + N].z <= 0.01f ? 0 : 1;
				for (int32 M = 0; M < PieceMaterial.m_IndexCount; M += 3)
				{
					const int3 T = M + Order;
					F2 = float3(Mesh->Vertices[Mesh->Indices[T.x]].z, Mesh->Vertices[Mesh->Indices[T.y]].z, Mesh->Vertices[Mesh->Indices[T.z]].z);
					// The original tests >= -0.01 here as well (not the <= 0.01 the vertices use).
					Value2.m_IndexCount -= math::all(F2 >= float3(-0.01f)) ? 0 : 3;
				}
			}
			Materials[Material] = Value2;
		}
	}
	for (MeshMaterial& Value3 : Materials)
	{
		Value3.m_StartVertex = Num;
		Value3.m_StartIndex = Num3;
		Num += Value3.m_VertexCount;
		Num3 += Value3.m_IndexCount;
	}
	for (MeshMaterial& Value4 : Materials)
	{
		Value4.m_VertexCount = 0;
		Value4.m_IndexCount = 0;
	}
	Out.Vertices.SetNumZeroed(Num);
	Out.Indices.SetNumZeroed(Num3);
	TArray<int32> LocalToOutput;
	LocalToOutput.SetNumZeroed(Num2);
	TArray<Bounds1> HeightBounds;
	const float Num7 = Data.m_Width * 0.5f + MiddleOffset;
	const float Num8 = Data.m_Width * 0.5f - MiddleOffset;
	bool bOverflowLogged = false;
	for (const NetCompositionPiece& Piece : Pieces)
	{
		const FPieceMesh* Mesh = Assets.PieceMesh(Piece.m_Piece);
		if (!Mesh) continue;
		const bool bFlag10 = (Piece.m_SectionFlags & NetSectionFlags_Invert) != 0;
		const bool bFlag11 = (Piece.m_SectionFlags & NetSectionFlags_FlipMesh) != 0;
		const bool bFlag12 = (Piece.m_SectionFlags & NetSectionFlags_Median) != 0;
		const bool bFlag13 = (Piece.m_SectionFlags & NetSectionFlags_Right) != 0;
		const bool bFlag14 = (Piece.m_SectionFlags & NetSectionFlags_HalfLength) != 0;
		const bool bFlag15 = (Piece.m_PieceFlags & NetPieceFlags_PreserveShape) != 0;
		const bool bFlag16 = (Piece.m_PieceFlags & NetPieceFlags_DisableTiling) != 0;
		const bool bFlag17 = (Piece.m_PieceFlags & NetPieceFlags_LowerBottomToTerrain) != 0;
		const bool bFlag18 = (Piece.m_PieceFlags & NetPieceFlags_RaiseTopToTerrain) != 0;
		const bool bFlag19 = (Piece.m_PieceFlags & NetPieceFlags_SmoothTopNormal) != 0;
		const bool bFlag20 = (bFlag && !bFlag2 && !bFlag15) || bFlag14;
		const bool bFlag21 = (Piece.m_PieceFlags & NetPieceFlags_SkipBottomHalf) != 0;
		const bool bTest = Piece.m_Size.x == 0.0f;
		float4 Scale(bFlag10 ? 1.0f : -1.0f, 1.0f, bFlag11 ? 1.0f : -1.0f, bFlag10 != bFlag11 ? -1.0f : 1.0f);
		float3 Offset = Piece.m_Offset;
		float2 F4 = 1.0f / float2(Data.m_Width, Piece.m_Size.z * 0.5f);
		const float2 F5 = 1.0f / float2(Num7, Piece.m_Size.z * 0.5f);
		const float2 F6 = 1.0f / float2(Num8, Piece.m_Size.z * 0.5f);
		float2 F7(0.5f, 1.0f);
		const float2 F8(1.0f - MiddleOffset / Num7, 1.0f);
		const float2 F9(-MiddleOffset / Num8, 1.0f);
		float Z = Piece.m_Offset.x / Data.m_Width + 0.5f;
		float Z2 = 1.0f + (Piece.m_Offset.x - MiddleOffset) / Num7;
		float Z3 = (Piece.m_Offset.x - MiddleOffset) / Num8;
		if (bFlag && bFlag15)
		{
			const float Num10 = 0.5f * Piece.m_Size.z / Data.m_Width;
			F4.y *= Num10;
			F7.y *= Num10;
			Z = 0.5f;
		}
		else if (bFlag2)
		{
			Z2 = 0.0f;
			Z3 = 1.0f;
		}
		else if (bFlag14)
		{
			Scale.z *= 2.0f;
			Offset.z += 0.5f * Piece.m_Size.z;
		}
		if (bFlag17 || bFlag18 || bFlag19)
		{
			if (HeightBounds.Num() == 0) HeightBounds.SetNumZeroed(257);
			CSRoadMesh_InitializeHeightBounds(HeightBounds, Piece, *Mesh);
		}
		for (const MeshMaterial& PieceMaterial2 : Mesh->Materials)
		{
			const int32 Material2 = CSRoadMesh_GetMaterial(Materials, PieceMaterial2);
			MeshMaterial Value5 = Materials[Material2];
			for (int32 Num12 = 0; Num12 < PieceMaterial2.m_VertexCount; Num12++)
			{
				const float Z4 = Mesh->Vertices[PieceMaterial2.m_StartVertex + Num12].z;
				LocalToOutput[Num12] = (!bFlag20 || Z4 >= -0.01f) && (!bFlag21 || Z4 <= 0.01f) ? Value5.m_StartVertex + Value5.m_VertexCount++ : -1;
			}
			const int3 Order = PieceMaterial2.m_StartIndex + math::select(int3(0, 1, 2), int3(2, 1, 0), bFlag10 != bFlag11);
			float2 F10 = 0.0f;
			float X = 0.0f;
			const bool bFlag22 = !bFlag16;
			for (int32 Num13 = 0; Num13 < PieceMaterial2.m_IndexCount; Num13 += 3)
			{
				int3 T = Num13 + Order;
				T.x = Mesh->Indices[T.x];
				T.y = Mesh->Indices[T.y];
				T.z = Mesh->Indices[T.z];
				int3 Local = T - PieceMaterial2.m_StartVertex;
				Local.x = LocalToOutput[Local.x];
				Local.y = LocalToOutput[Local.y];
				Local.z = LocalToOutput[Local.z];
				if (math::all(Local >= int3(0)))
				{
					const int32 Write = Value5.m_StartIndex + Value5.m_IndexCount;
					if (Write + 3 > Out.Indices.Num())
					{
						// The SkipBottomHalf count/emit mismatch above; the original would overrun its buffer.
						if (!bOverflowLogged) UE_LOG(LogTemp, Warning, TEXT("CSRoad: composition mesh index count overflow (prefab %d)"), Composition.Prefab);
						bOverflowLogged = true;
						Out.Indices.SetNumZeroed(Write + 3);
					}
					Out.Indices[Write] = Local.x;
					Out.Indices[Write + 1] = Local.y;
					Out.Indices[Write + 2] = Local.z;
					Value5.m_IndexCount += 3;
					const float3 F11(Mesh->UV0s[T.x].y, Mesh->UV0s[T.y].y, Mesh->UV0s[T.z].y);
					if (bFlag22 & math::all(F11 >= float3(0.0f)))
					{
						const float3 F12(Mesh->Vertices[T.x].z, Mesh->Vertices[T.y].z, Mesh->Vertices[T.z].z);
						F10 += float2(math::csum(math::abs(F11.yzx() - F11)), math::csum(math::abs(F12.yzx() - F12)));
					}
					else X = math::max(math::max(X, F11.x), math::max(F11.y, F11.z));
				}
			}
			const float TrueValue = F10.x / F10.y;
			X = bFlag22 ? 0.0f : math::ceil(X);
			for (int32 Num14 = 0; Num14 < PieceMaterial2.m_VertexCount; Num14++)
			{
				const int32 Num15 = LocalToOutput[Num14];
				if (Num15 == -1) continue;
				const int32 Index = PieceMaterial2.m_StartVertex + Num14;
				float3 Vertex = Mesh->Vertices[Index];
				float3 F13 = Mesh->Normals[Index];
				float4 Tangent = Mesh->Tangents[Index];
				float4 V(Mesh->UV0s[Index], 0.0f, 0.0f);
				int4 Int8 = 0;
				if (bFlag17)
				{
					const Bounds1 HB = CSRoadMesh_GetHeightBounds(HeightBounds, Piece, Vertex.z);
					Int8.z = Vertex.y <= HB.min + 0.01f ? 1 : Int8.z;
				}
				if (bFlag18)
				{
					const Bounds1 HB = CSRoadMesh_GetHeightBounds(HeightBounds, Piece, Vertex.z);
					Int8.z = Vertex.y >= HB.max - 0.01f ? 2 : Int8.z;
				}
				if (bFlag19)
				{
					const Bounds1 HB = CSRoadMesh_GetHeightBounds(HeightBounds, Piece, Vertex.z);
					F13 = math::select(F13, float3(0.0f, 1.0f, 0.0f), Vertex.y >= HB.max - 0.01f);
				}
				Vertex = Vertex * Scale.xyz() + Offset;
				F13 *= Scale.xyz();
				Tangent *= Scale;
				V.y = bFlag22 & (V.y >= 0.0f) ? TrueValue : V.y - X;
				if (bFlag)
				{
					auto LeftHalf = [&]()
					{
						Int8.set_yw(int2(2, 2));
						V.z = Z2;
						Vertex.set_xz(Vertex.xz() * F5 + F8);
						V.w = Vertex.z * 20.0f;
					};
					auto RightHalf = [&]()
					{
						Int8.set_xyw(int3(1, 3, 4));
						V.z = Z3;
						Vertex.set_xz(Vertex.xz() * F6 + F9);
						V.w = Vertex.z * 20.0f;
					};
					auto RoundaboutLeft = [&]()
					{
						Int8.set_xyw(math::select(int3(0, 4, 2), int3(1, 5, 130), Vertex.z >= 0.0f));
						V.z = Z2;
						Vertex.set_xz(Vertex.xz() * F5 + F8);
						V.w = Vertex.z * 40.0f;
						Vertex.z -= static_cast<float>(Int8.x);
					};
					auto RoundaboutRight = [&]()
					{
						Int8.set_xyw(math::select(int3(4, 2, 4), int3(5, 3, 132), Vertex.z >= 0.0f));
						V.z = Z3;
						Vertex.set_xz(Vertex.xz() * F6 + F9);
						V.w = Vertex.z * 40.0f;
						Vertex.z -= static_cast<float>(Int8.y - 2);
					};
					if (bFlag15)
					{
						Int8.set_xy(int2(6, 7));
						V.z = Z;
						Vertex.set_xz(Vertex.xz() * F4 + F7);
						V.w = Vertex.z * 20.0f;
					}
					else if (bFlag12)
					{
						const float Num16 = Vertex.x - Piece.m_Offset.x;
						Vertex.x = bTest ? Piece.m_Offset.x : Vertex.x;
						if (math::abs(Num16) < 0.01f)
						{
							if (bFlag2)
							{
								Int8.set_xy(math::select(int2(4, 2), int2(5, 3), Vertex.z >= 0.0f));
								Vertex.x = 0.0f;
								Vertex.z = Vertex.z * F4.y + F7.y;
								V.w = Vertex.z * 40.0f;
							}
							else
							{
								Int8.x = 4;
								Vertex.x = 0.0f;
								Vertex.z = Vertex.z * F4.y + F7.y;
								V.w = Vertex.z * 20.0f;
							}
						}
						else if (Num16 > 0.0f)
						{
							if (bFlag2) RoundaboutRight();
							else RightHalf();
						}
						else if (bFlag2) RoundaboutLeft();
						else LeftHalf();
					}
					else if (bFlag13)
					{
						if (bFlag2) RoundaboutRight();
						else RightHalf();
					}
					else if (bFlag2) RoundaboutLeft();
					else LeftHalf();
					Vertex.set_xz(math::saturate(Vertex.xz()));
				}
				else
				{
					Int8.set_xy(math::select(int2(0, 2), int2(1, 3), Vertex.z >= 0.0f));
					V.z = Z;
					Vertex.set_xz(Vertex.xz() * F4 + F7);
					V.w = Vertex.z * 0.5f;
					Vertex.z -= static_cast<float>(Int8.x);
					Vertex.set_xz(math::saturate(Vertex.xz()));
				}
				FCompositionVertex& OutVertex = Out.Vertices[Num15];
				OutVertex.Position = Vertex;
				OutVertex.Color = (uint(uint8(Int8.x))) | (uint(uint8(Int8.y)) << 8) | (uint(uint8(Int8.z)) << 16) | (uint(uint8(Int8.w)) << 24);
				OutVertex.Normal = MathUtils::NormalToOctahedral(F13);
				OutVertex.Tangent = MathUtils::TangentToOctahedral(Tangent);
				OutVertex.UV0 = float4(F16ToF32(F32ToF16(V.x)), F16ToF32(F32ToF16(V.y)), F16ToF32(F32ToF16(V.z)), F16ToF32(F32ToF16(V.w)));
			}
			Materials[Material2] = Value5;
		}
	}
}
}
