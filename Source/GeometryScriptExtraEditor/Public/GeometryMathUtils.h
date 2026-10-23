#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "GeometryMathUtils.generated.h"

// =====================================================================
// UNoise — Curl / Perlin noise helpers
// =====================================================================
UCLASS()
class GEOMETRYSCRIPTEXTRAEDITOR_API UNoise : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = Noise)
	static FVector CurlNoise(FVector Pos, FVector& Out_AddedPos, FVector Offset = FVector(0, 0, 0), float Strength = 1, float Frequency = 1);

	UFUNCTION(BlueprintCallable, Category = Noise)
	static FVector PerlinNoise3D(FVector Pos, FVector& Out_AddedPos, FVector Offset = FVector(0, 0, 0), float Strength = 1, float Frequency = 1, int32 RandomSeed = 0);
};

// =====================================================================
// UPointFunction — nearest-point search
// =====================================================================
UCLASS()
class GEOMETRYSCRIPTEXTRAEDITOR_API UPointFunction : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	static int32 FindNearPointIteration(TArray<FVector> TarLocations, FVector SourceLocation);
	static int32 FindNearPointIteration(TArray<FVector> TarLocations, FVector SourceLocation, TFunction<bool(int32)> Func);
};

// =====================================================================
// UPolyLine — polyline smooth / resample / query
// =====================================================================
UCLASS()
class GEOMETRYSCRIPTEXTRAEDITOR_API UPolyLine : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = PolyLine)
	static FGeometryScriptPolyPath SmoothLine(FGeometryScriptPolyPath PolyPath, int NumIterations);

	UFUNCTION(BlueprintPure, Category = PolyLine)
	static FGeometryScriptPolyPath ResampleByCount(FGeometryScriptPolyPath PolyPath, int32 NumIterations = 50);

	UFUNCTION(BlueprintPure, Category = PolyLine)
	static FGeometryScriptPolyPath ResampleByLength(FGeometryScriptPolyPath PolyPath, float Interval = 50);

	UFUNCTION(BlueprintPure, Category = PolyLine)
	static TArray<FTransform> ConvertPolyPathToTransforms(FGeometryScriptPolyPath PolyPath, bool GenerateRotator);

	UFUNCTION(BlueprintPure, Category = PolyLine)
	static TArray<float> CurveU(FGeometryScriptPolyPath PolyPath, bool Normalize);
};

// =====================================================================
// UGeneralMath — generic reduction / math utilities
// =====================================================================

UENUM(BlueprintType)
enum class EReductionCustomOp : uint8
{
	Add            UMETA(DisplayName = "Add (Acc + V)"),
	Subtract       UMETA(DisplayName = "Subtract (Acc - V)"),
	Multiply       UMETA(DisplayName = "Multiply (Acc * V)"),
	Divide         UMETA(DisplayName = "Divide (Acc / V)"),
	Modulo         UMETA(DisplayName = "Modulo (Acc % V)"),
	BitAnd         UMETA(DisplayName = "BitAnd (Acc & V)"),
	BitOr          UMETA(DisplayName = "BitOr (Acc | V)"),
	Xor            UMETA(DisplayName = "Xor (Acc ^ V)"),
	LeftShift      UMETA(DisplayName = "LeftShift (Acc << V)"),
	RightShift     UMETA(DisplayName = "RightShift (Acc >> V)"),
	Max            UMETA(DisplayName = "Max (Acc > V ? Acc : V)"),
	Min            UMETA(DisplayName = "Min (Acc < V ? Acc : V)"),
	Conditional    UMETA(DisplayName = "Conditional (V != 0 ? V : Acc)"),
};

UCLASS()
class GEOMETRYSCRIPTEXTRAEDITOR_API UGeneralMath : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = Noise)
	static void Reduction();

	template<typename T, typename R>
	static R Reduce(TArrayView<const T> Input, TFunctionRef<R(R, T)> Lambda, R Init)
	{
		R Acc = MoveTemp(Init);
		for (const T& Elem : Input)
			Acc = Lambda(Acc, Elem);
		return Acc;
	}

	UFUNCTION(BlueprintCallable, Category = "Math|Reduction", meta = (DisplayName = "Reduce Int32 Sum"))
	static int32 Reduce_Int32Sum(const TArray<int32>& Values);

	UFUNCTION(BlueprintCallable, Category = "Math|Reduction", meta = (DisplayName = "Reduce Float Sum"))
	static float Reduce_FloatSum(const TArray<float>& Values);

	UFUNCTION(BlueprintCallable, Category = "Math|Reduction", meta = (DisplayName = "Reduce Double Sum"))
	static double Reduce_DoubleSum(const TArray<double>& Values);

	UFUNCTION(BlueprintCallable, Category = "Math|Reduction", meta = (DisplayName = "Reduce Int32 Max"))
	static int32 Reduce_Int32Max(const TArray<int32>& Values);

	UFUNCTION(BlueprintCallable, Category = "Math|Reduction", meta = (DisplayName = "Reduce Float Max"))
	static float Reduce_FloatMax(const TArray<float>& Values);

	UFUNCTION(BlueprintCallable, Category = "Math|Reduction", meta = (DisplayName = "Reduce Int32 Min"))
	static int32 Reduce_Int32Min(const TArray<int32>& Values);

	UFUNCTION(BlueprintCallable, Category = "Math|Reduction", meta = (DisplayName = "Reduce Float Min"))
	static float Reduce_FloatMin(const TArray<float>& Values);

	UFUNCTION(BlueprintCallable, Category = "Math|Reduction", meta = (DisplayName = "Reduce Int32 Custom"))
	static int32 Reduce_Int32Custom(
		const TArray<int32>& Values,
		EReductionCustomOp Operation,
		int32 InitValue = 0
	);

	template<typename T>
	static int32 CountIf(TArrayView<const T> Input, TFunctionRef<bool(T)> Predicate)
	{
		return Reduce<T, int32>(Input, [&Predicate](int32 Acc, T V) {
			return Acc + (Predicate(V) ? 1 : 0);
		}, 0);
	}

	template<typename T, typename R>
	static R Reduce_Custom(TArrayView<const T> Input, TFunctionRef<R(R, T)> Lambda, R Init)
	{
		return Reduce<T, R>(Input, Lambda, MoveTemp(Init));
	}

	template<typename T, typename R>
	static R Reduce_Custom(const TArray<T>& Input, TFunctionRef<R(R, T)> Lambda, R Init)
	{
		return Reduce_Custom<T, R>(TArrayView<const T>(Input), Lambda, MoveTemp(Init));
	}
};
