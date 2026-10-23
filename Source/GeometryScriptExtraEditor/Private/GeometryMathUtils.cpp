#include "GeometryMathUtils.h"
#include "Curve/CurveUtil.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"

// =====================================================================
// UNoise
// =====================================================================

FVector UNoise::CurlNoise(FVector Pos, FVector& Out_AddedPos, FVector Offset, float Strength, float Frequency)
{
	FVector curl = FVector(0, 0, 0);
	float h = 0.001;
	float n, n1, a, b;
	Frequency /= 100;
	FVector NoisePos = (Pos + Offset) * Frequency;
	n = FMath::PerlinNoise3D(NoisePos);

	n1 = FMath::PerlinNoise3D((NoisePos - FVector(0, h, 0)));
	a = (n - n1) / h;

	n1 = FMath::PerlinNoise3D((NoisePos - FVector(0, 0, h)));
	b = (n - n1) / h;
	curl.X = a - b;

	a = (n - n1) / h;
	
	n1 = FMath::PerlinNoise3D((NoisePos - FVector(h, 0, 0)));
	b = (n - n1) / h;
	curl.Y = a - b;

	a = (n - n1) / h;
	
	n1 = FMath::PerlinNoise3D((NoisePos - FVector(0, h, 0)));
	b = (n - n1) / h;
	curl.Z = a - b;

	Out_AddedPos = Pos + curl * Strength;
	return curl;
}

FVector UNoise::PerlinNoise3D(FVector Pos, FVector& Out_AddedPos, FVector Offset, float Strength, float Frequency, int32 RandomSeed)
{
	FRandomStream Random(RandomSeed);
	
	FVector Displacement;
	FVector Offsets[3];
	for (int32 k = 0; k < 3; ++k)
	{
		const float RandomOffset = 10000.0f * Random.GetFraction();
		Offsets[k] = FVector(RandomOffset, RandomOffset, RandomOffset);
		Offsets[k] += Offset;
		FVector NoisePos = (FVector)(Frequency * (Pos + Offsets[k]));
		Displacement[k] = Strength * FMath::PerlinNoise3D(Frequency * NoisePos);
	}

	Out_AddedPos = Pos + Displacement;
	return Displacement;
}

// =====================================================================
// UPointFunction
// =====================================================================

int32 UPointFunction::FindNearPointIteration(TArray<FVector> TarLocations, FVector SourceLocation)
{
	int32 Index = -1;
	int32 Iteration = TarLocations.Num();
	float Dist = 999999999.0;
	for (int32 i = 0; i < Iteration; i++)
	{
		float TarDist = FVector::Dist(TarLocations[i], SourceLocation);
		if (TarDist < Dist)
		{
			Dist = TarDist;
			Index = i;
		}
	}
	
	return Index;
}

int32 UPointFunction::FindNearPointIteration(TArray<FVector> TarLocations, FVector SourceLocation, TFunction<bool(int32)> Func)
{
	int32 Index = -1;
	int32 Iteration = TarLocations.Num();
	float Dist = 999999999.0;
	for (int32 i = 0; i < Iteration; i++)
	{
		if (!Func(i))
			continue;
		
		float TarDist = FVector::Dist(TarLocations[i], SourceLocation);
		if (TarDist < Dist)
		{
			Dist = TarDist;
			Index = i;
		}
	}
	
	return Index;
}

// =====================================================================
// UPolyLine
// =====================================================================

FGeometryScriptPolyPath UPolyLine::SmoothLine(FGeometryScriptPolyPath PolyPath, int NumIterations)
{
	int32 EndIdx = (PolyPath.Path.IsValid()) ? FMath::Max(PolyPath.Path->Num()-1,0) : 0;
	if (PolyPath.Path.IsValid())
	{
		UE::Geometry::CurveUtil::IterativeSmooth<double, FVector>(*PolyPath.Path, 0, EndIdx, 1, NumIterations, false);
	}
	return PolyPath;
}

FGeometryScriptPolyPath UPolyLine::ResampleByCount(const FGeometryScriptPolyPath PolyPath, int32 NumIterations)
{
	float Sum = 0;
	float CurrentLength = 0;
	FGeometryScriptPolyPath ToReturn;
	ToReturn.Reset();

	TArray<FVector> Vertices = *PolyPath.Path;
	TArray<FVector> PathVertices;
	PathVertices.SetNum(NumIterations);
	int32 NV = Vertices.Num();
	int32 PointCount = 1;
	int32 iterat = NumIterations - 2;

	float ArcLength = UE::Geometry::CurveUtil::ArcLength<float, FVector>(*PolyPath.Path, false);
	float Interval = ArcLength / (NumIterations - 1);
	
	if (Vertices.Num() < 1)
	{
		return ToReturn;
	}

	PathVertices[0] = Vertices[0];
	Sum += FVector::Distance(Vertices[0], Vertices[1]);
	
	FVector Dir = (Vertices[1] - Vertices[0]).GetSafeNormal(.001);
	for (int i = 0; i < iterat; i++)
	{
		CurrentLength += Interval;
		while(CurrentLength > Sum)
		{
			PointCount += 1;
			Sum += FVector::Distance(Vertices[PointCount], Vertices[PointCount-1]);
			Dir = (Vertices[PointCount] - Vertices[PointCount-1]).GetSafeNormal(.001);
		}
		FVector SamplePos = Vertices[PointCount] - Dir * (Sum - CurrentLength);
		PathVertices[i + 1] = SamplePos;
	}
	PathVertices[NumIterations - 1] = Vertices[Vertices.Num()-1];

	ToReturn.Path->Append(PathVertices);
	return ToReturn;
}

FGeometryScriptPolyPath UPolyLine::ResampleByLength(const FGeometryScriptPolyPath PolyPath, float IntervalExp)
{
	float Sum = 0;
	float CurrentLength = 0;
	FGeometryScriptPolyPath ToReturn;
	ToReturn.Reset();

	float ArcLength = UE::Geometry::CurveUtil::ArcLength<float, FVector>(*PolyPath.Path, false);
	int32 NumIterations = int32(ArcLength / IntervalExp);
	if (NumIterations < 2)
		return ToReturn;
	
	float Interval = ArcLength / NumIterations;
	
	TArray<FVector> Vertices = *PolyPath.Path;
	TArray<FVector> PathVertices;
	PathVertices.SetNum(NumIterations);
	int32 NV = Vertices.Num();
	int32 PointCount = 1;
	int32 iterat = NumIterations - 2;
	
	if (Vertices.Num() < 1)
	{
		return ToReturn;
	}

	PathVertices[0] = Vertices[0];
	Sum += FVector::Distance(Vertices[0], Vertices[1]);
	
	FVector Dir = (Vertices[1] - Vertices[0]).GetSafeNormal(.001);
	for (int i = 0; i < iterat; i++)
	{
		CurrentLength += Interval;
		while(CurrentLength > Sum)
		{
			PointCount += 1;
			Sum += FVector::Distance(Vertices[PointCount], Vertices[PointCount-1]);
			Dir = (Vertices[PointCount] - Vertices[PointCount-1]).GetSafeNormal(.001);
		}
		FVector SamplePos = Vertices[PointCount] - Dir * (Sum - CurrentLength);
		PathVertices[i + 1] = SamplePos;
	}
	PathVertices[NumIterations - 1] = Vertices[Vertices.Num()-1];

	ToReturn.Path->Append(PathVertices);
	return ToReturn;
}

TArray<FTransform> UPolyLine::ConvertPolyPathToTransforms(FGeometryScriptPolyPath PolyPath, bool GenerateRotator)
{
	TArray<FVector> Line = *PolyPath.Path;
	int32 LineVertexNum = Line.Num();
	TArray<FTransform> LineTransforms;
	LineTransforms.Reserve(LineVertexNum);

	for (int32 i = 0; i < LineVertexNum; i++)
	{
		FVector Tangent = UE::Geometry::CurveUtil::Tangent<double, FVector>(Line, i);
		LineTransforms.Add(FTransform(FRotationMatrix::MakeFromX(Tangent).Rotator(), Line[i], FVector::OneVector));
	}
	return LineTransforms;
}

TArray<float> UPolyLine::CurveU(FGeometryScriptPolyPath PolyPath, bool Normalize)
{
	float Sum = 0;
	TArray<FVector> Vertices = (*PolyPath.Path);
	int32 NPt = Vertices.Num();
	TArray<float> CurveU;
	CurveU.Reserve(NPt);
	for (int i = 0; i < NPt; ++i)
	{
		Sum += FVector::Dist(Vertices[i], Vertices[FMath::Max(i-1, 0)]);
		CurveU.Add(Sum);
	}
	if (!Normalize)
		return CurveU;

	for (int32 i = 0; i < NPt; ++i)
	{
		CurveU[i] /= CurveU[NPt - 1]; 
	}
	return CurveU;
}

// =====================================================================
// UGeneralMath
// =====================================================================

int32 UGeneralMath::Reduce_Int32Sum(const TArray<int32>& Values)
{
	return Reduce<int32, int32>(Values, [](int32 Acc, int32 V) { return Acc + V; }, 0);
}

float UGeneralMath::Reduce_FloatSum(const TArray<float>& Values)
{
	return Reduce<float, float>(Values, [](float Acc, float V) { return Acc + V; }, 0.0f);
}

double UGeneralMath::Reduce_DoubleSum(const TArray<double>& Values)
{
	return Reduce<double, double>(Values, [](double Acc, double V) { return Acc + V; }, 0.0);
}

int32 UGeneralMath::Reduce_Int32Max(const TArray<int32>& Values)
{
	return Reduce<int32, int32>(Values, [](int32 Acc, int32 V) { return (Acc > V) ? Acc : V; }, TNumericLimits<int32>::Lowest());
}

float UGeneralMath::Reduce_FloatMax(const TArray<float>& Values)
{
	return Reduce<float, float>(Values, [](float Acc, float V) { return (Acc > V) ? Acc : V; }, -TNumericLimits<float>::Max());
}

int32 UGeneralMath::Reduce_Int32Min(const TArray<int32>& Values)
{
	return Reduce<int32, int32>(Values, [](int32 Acc, int32 V) { return (Acc < V) ? Acc : V; }, TNumericLimits<int32>::Max());
}

float UGeneralMath::Reduce_FloatMin(const TArray<float>& Values)
{
	return Reduce<float, float>(Values, [](float Acc, float V) { return (Acc < V) ? Acc : V; }, TNumericLimits<float>::Max());
}

int32 UGeneralMath::Reduce_Int32Custom(const TArray<int32>& Values, EReductionCustomOp Operation, int32 InitValue)
{
	auto Op = [Operation](int32 Acc, int32 V) -> int32 {
		switch (Operation)
		{
		case EReductionCustomOp::Add:         return Acc + V;
		case EReductionCustomOp::Subtract:    return Acc - V;
		case EReductionCustomOp::Multiply:   return Acc * V;
		case EReductionCustomOp::Divide:      return (V != 0) ? Acc / V : Acc;
		case EReductionCustomOp::Modulo:      return (V != 0) ? Acc % V : Acc;
		case EReductionCustomOp::BitAnd:      return Acc & V;
		case EReductionCustomOp::BitOr:       return Acc | V;
		case EReductionCustomOp::Xor:         return Acc ^ V;
		case EReductionCustomOp::LeftShift:   return Acc << V;
		case EReductionCustomOp::RightShift:   return Acc >> V;
		case EReductionCustomOp::Max:         return (Acc > V) ? Acc : V;
		case EReductionCustomOp::Min:         return (Acc < V) ? Acc : V;
		case EReductionCustomOp::Conditional: return (V != 0) ? V : Acc;
		default:                              return Acc;
		}
	};
	return Reduce<int32, int32>(Values, Op, InitValue);
}

void UGeneralMath::Reduction()
{
}
