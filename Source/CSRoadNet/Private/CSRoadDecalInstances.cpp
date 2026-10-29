#include "CSRoadDecalInstances.h"

#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

namespace CSRoad
{
namespace
{
// The network's space (Unity metres, y up) to the actor's (cm, z up).
FVector DecalInstances_ToUnreal(const float3& V) { return FVector(V.x, -V.z, V.y) * 100.0; }

FVector DecalInstances_Point(const FVector (&P)[4], double T)
{
	const double S = 1.0 - T;
	return P[0] * (S * S * S) + P[1] * (3.0 * S * S * T) + P[2] * (3.0 * S * T * T) + P[3] * (T * T * T);
}

FVector DecalInstances_Derivative(const FVector (&P)[4], double T)
{
	const double S = 1.0 - T;
	return ((P[1] - P[0]) * (S * S) + (P[2] - P[1]) * (2.0 * S * T) + (P[3] - P[2]) * (T * T)) * 3.0;
}

// The height (Unity y) DecalFitCS projects a grid row from: the curve's, a crosswalk's straight footprint's.
double DecalInstances_CurveY(const Bezier4x3& Curve, bool bStraight, double T)
{
	const double S = 1.0 - T;
	if (bStraight) return FMath::Lerp(double(Curve.a.y), double(Curve.d.y), T);
	return Curve.a.y * (S * S * S) + Curve.b.y * (3.0 * S * S * T) + Curve.c.y * (3.0 * S * T * T) + Curve.d.y * (T * T * T);
}
}

void BuildDecalInstances(const FGpuDecalInput& Decals, const FTransform& NetworkToWorld, TArray<FDecalInstances>& Out, const TArray<float>* FitHeights)
{
	Out.Reset();
	TMap<FIntVector, int32> BatchOf;
	const double Scale = NetworkToWorld.GetScale3D().GetAbsMax();
	constexpr int32 Stride = DecalFitColumns + 1;
	for (const FGpuDecalItem& Item : Decals.Items)
	{
		const bool bStraight = Item.Kind == uint32(EDecalKind::Clipped);
		// What RoadLab projects onto the surface is fitted to it; a strip lies on its curve.
		const bool bFitted = Item.Kind != uint32(EDecalKind::Strip);
		const int32 Rows = FMath::Max(int32(Item.Rows), 1);
		if (bFitted && (!FitHeights || int64(Item.VertexOffset) + int64(Rows + 1) * Stride > int64(FitHeights->Num()))) continue;
		FVector P[4] = { NetworkToWorld.TransformPosition(DecalInstances_ToUnreal(Item.Curve.a)), NetworkToWorld.TransformPosition(DecalInstances_ToUnreal(Item.Curve.b)),
			NetworkToWorld.TransformPosition(DecalInstances_ToUnreal(Item.Curve.c)), NetworkToWorld.TransformPosition(DecalInstances_ToUnreal(Item.Curve.d)) };
		// A crosswalk's footprint runs straight between its curve's ends.
		if (bStraight)
		{
			P[1] = FMath::Lerp(P[0], P[3], 1.0 / 3.0);
			P[2] = FMath::Lerp(P[0], P[3], 2.0 / 3.0);
		}
		const double Width = double(Item.Width) * 100.0 * Scale;
		const double Lift = double(Item.Lift) * 100.0 * Scale;
		const double HeightScale = 100.0 * NetworkToWorld.GetScale3D().Z;
		// The heights at RoadLab's points of a fitted grid's row, and the columns it draws (DecalFitColumnChoices).
		auto Surface = [&](int32 GridRow, int32 Column) { return (*FitHeights)[int32(Item.VertexOffset) + GridRow * Stride + Column]; };
		int32 Columns = DecalFitColumns;
		if (bFitted)
		{
			bool bAllOn = true;
			for (int32 GridRow = 0; GridRow <= Rows && bAllOn; GridRow++) for (int32 Column = 0; Column < Stride; Column++) bAllOn &= !FMath::IsNaN(Surface(GridRow, Column));
			for (const int32 Choice : DecalFitColumnChoices)
			{
				if (!bAllOn || Choice >= DecalFitColumns) break;
				const int32 Step = DecalFitColumns / Choice;
				bool bFits = true;
				for (int32 GridRow = 0; GridRow <= Rows && bFits; GridRow++)
				{
					for (int32 Column = 0; Column < Stride; Column++)
					{
						const int32 Low = FMath::Min(Column / Step * Step, DecalFitColumns - Step);
						const double Between = FMath::Lerp(double(Surface(GridRow, Low)), double(Surface(GridRow, Low + Step)), double(Column - Low) / Step);
						bFits &= FMath::Abs(Between - double(Surface(GridRow, Column))) * HeightScale <= DecalFitTolerance;
					}
				}
				if (!bFits) continue;
				Columns = Choice;
				break;
			}
		}
		// RoadDecals' renderer priority, from the lift it gave: .018 + priority * .0003 m.
		const int32 Priority = FMath::RoundToInt32((Item.Lift - 0.018f) / 0.0003f);
		int32& Batch = BatchOf.FindOrAdd(FIntVector(int32(Item.MaterialSlot), Priority, bFitted ? Columns : 0), INDEX_NONE);
		if (Batch == INDEX_NONE)
		{
			Batch = Out.Num();
			FDecalInstances& Added = Out.AddDefaulted_GetRef();
			Added.MaterialSlot = int32(Item.MaterialSlot);
			Added.Priority = Priority;
			Added.bFitted = bFitted;
			Added.Columns = bFitted ? Columns : 1;
			Added.Floats = bFitted ? DecalData::FittedCount(Columns) : DecalData::Count;
		}
		FDecalInstances& Into = Out[Batch];
		// A crosswalk's texture distance grows linearly along it (Distance.w * t, as RoadLab clips it).
		const float4 Distance = bStraight ? float4(0.0f, Item.Distance.w / 3.0f, Item.Distance.w * 2.0f / 3.0f, Item.Distance.w) : Item.Distance;
		const float Height = Item.UVArea.w - Item.UVArea.y;
		const int32 Pieces = FMath::DivideAndRoundUp(Rows, DecalStripRows);
		for (int32 Piece = 0; Piece < Pieces; Piece++)
		{
			// A strip's pieces share its curve evenly. A fitted grid's take DecalStripRows of its rows
			// each, so its points are the item's grid's; the last one's rows past the end fold onto it.
			const double T0 = bFitted ? double(Piece * DecalStripRows) / Rows : double(Piece) / Pieces;
			const double T1 = bFitted ? double((Piece + 1) * DecalStripRows) / Rows : double(Piece + 1) / Pieces;
			const double TEnd = FMath::Min(T1, 1.0);
			// The piece's stretch of the curve as a Bezier of its own, inside the hull of its control
			// points; then half the width across and the lift, whichever way they turn.
			const double Span = (TEnd - T0) / 3.0;
			const FVector Start = DecalInstances_Point(P, T0);
			const FVector End = DecalInstances_Point(P, TEnd);
			FBox Box(ForceInit);
			for (const FVector& Point : { Start, Start + DecalInstances_Derivative(P, T0) * Span, End - DecalInstances_Derivative(P, TEnd) * Span, End }) Box += Point;
			Box = Box.ExpandBy(Width * 0.5 + Lift + 1.0);
			// A fitted grid's points stand where the surface is: their heights above the curve, to the
			// millimetre; the box reaches them.
			int32 Quantized[(DecalStripRows + 1) * Stride + 1];
			const int32 Points = (DecalStripRows + 1) * (Columns + 1);
			if (bFitted)
			{
				for (int32 Row = 0; Row <= DecalStripRows; Row++)
				{
					const int32 GridRow = FMath::Min(Piece * DecalStripRows + Row, Rows);
					const double T = double(GridRow) / Rows;
					const double CurveY = DecalInstances_CurveY(Item.Curve, bStraight, T);
					const FVector CurvePoint = DecalInstances_Point(P, T);
					for (int32 Column = 0; Column <= Columns; Column++)
					{
						const float Landed = Surface(GridRow, Column * (DecalFitColumns / Columns));
						int32& Q = Quantized[Row * (Columns + 1) + Column];
						if (FMath::IsNaN(Landed))
						{
							Q = DecalData::FitHeightOff;
							continue;
						}
						const double Above = (double(Landed) - CurveY) * HeightScale;
						Q = FMath::Clamp(int32(FMath::RoundToDouble(Above * 10.0)) + DecalData::FitHeightZero, 0, DecalData::FitHeightOff - 1);
						Box += FVector(CurvePoint.X, CurvePoint.Y, CurvePoint.Z + Above - 1.0);
						Box += FVector(CurvePoint.X, CurvePoint.Y, CurvePoint.Z + Above + Lift + 1.0);
					}
				}
				Quantized[Points] = DecalData::FitHeightOff;
			}
			const FVector Origin = Box.Min;
			const FVector Size = Box.GetSize();
			Into.Transforms.Add(FTransform(FQuat::Identity, Origin, Size));
			float* Data = &Into.Data[Into.Data.AddZeroed(Into.Floats)];
			if (bFitted)
			{
				Data[DecalData::Columns] = float(Columns);
				for (int32 K = 0; K * 2 < Points; K++) Data[DecalData::Heights + K] = float(Quantized[K * 2] + Quantized[K * 2 + 1] * 4096);
			}
			auto Put = [Data](int32 At, const FVector& V)
			{
				Data[At + 0] = float(V.X);
				Data[At + 1] = float(V.Y);
				Data[At + 2] = float(V.Z);
			};
			Put(DecalData::A, P[0] - Origin);
			Put(DecalData::B, P[1] - Origin);
			Put(DecalData::C, P[2] - Origin);
			Put(DecalData::D, P[3] - Origin);
			Put(DecalData::Scale, Size);
			Data[DecalData::T0] = float(T0);
			Data[DecalData::T1] = float(T1);
			Data[DecalData::Width] = float(Width);
			Data[DecalData::Lift] = float(Lift);
			Data[DecalData::Straight] = bStraight ? 1.0f : 0.0f;
			Data[DecalData::UMin] = Item.UVArea.x;
			Data[DecalData::UMax] = Item.UVArea.z;
			Data[DecalData::VMin] = Item.UVArea.y;
			Data[DecalData::VCurve + 0] = Distance.x * Height;
			Data[DecalData::VCurve + 1] = Distance.y * Height;
			Data[DecalData::VCurve + 2] = Distance.z * Height;
			Data[DecalData::VCurve + 3] = Distance.w * Height;
			Data[DecalData::Length] = Item.Length;
			Data[DecalData::Weights + 0] = Item.Weights.x;
			Data[DecalData::Weights + 1] = Item.Weights.y;
			Data[DecalData::Connected + 0] = Item.Connected.x;
			Data[DecalData::Connected + 1] = Item.Connected.y;
			Data[DecalData::Smoothing] = Item.Smoothing;
		}
	}
}

namespace
{
// DecalStripRows rows of Columns + 1 points: x along, y across, z = y (the unit box bounds it); UV0
// (across, along).
UStaticMesh* DecalInstances_UnitMesh(UObject* Outer, int32 Columns)
{
	FMeshDescription Description;
	FStaticMeshAttributes Attributes(Description);
	Attributes.Register();
	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> Signs = Attributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	UVs.SetNumChannels(1);
	const FPolygonGroupID Group = Description.CreatePolygonGroup();
	Attributes.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Decal");
	TArray<FVertexInstanceID> Corners;
	for (int32 Row = 0; Row <= DecalStripRows; Row++)
	{
		for (int32 Column = 0; Column <= Columns; Column++)
		{
			const float Along = float(Row) / float(DecalStripRows);
			const float Across = float(Column) / float(Columns);
			const FVertexID Vertex = Description.CreateVertex();
			Positions[Vertex] = FVector3f(Along, Across, Across);
			const FVertexInstanceID Corner = Description.CreateVertexInstance(Vertex);
			Normals[Corner] = FVector3f(0.0f, 0.0f, 1.0f);
			Tangents[Corner] = FVector3f(1.0f, 0.0f, 0.0f);
			Signs[Corner] = 1.0f;
			UVs.Set(Corner, 0, FVector2f(Across, Along));
			Corners.Add(Corner);
		}
	}
	for (int32 Row = 0; Row < DecalStripRows; Row++)
	{
		for (int32 Column = 0; Column < Columns; Column++)
		{
			const int32 I = Row * (Columns + 1) + Column;
			const int32 Next = I + Columns + 1;
			Description.CreateTriangle(Group, { Corners[I], Corners[Next], Corners[I + 1] });
			Description.CreateTriangle(Group, { Corners[I + 1], Corners[Next], Corners[Next + 1] });
		}
	}
	UStaticMesh* Mesh = NewObject<UStaticMesh>(Outer, NAME_None, RF_Transient);
	Mesh->GetStaticMaterials().Add(FStaticMaterial(nullptr, TEXT("Decal"), TEXT("Decal")));
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bFastBuild = true;
	Params.bCommitMeshDescription = false;
	Params.bMarkPackageDirty = false;
	Params.bBuildSimpleCollision = false;
	Mesh->BuildFromMeshDescriptions({ &Description }, Params);
	return Mesh;
}
}

UStaticMesh* MakeDecalStripMesh(UObject* Outer)
{
	return DecalInstances_UnitMesh(Outer, 1);
}

UStaticMesh* MakeDecalGridMesh(UObject* Outer, int32 Columns)
{
	return DecalInstances_UnitMesh(Outer, Columns);
}
}
