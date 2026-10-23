#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "ComputeShaderShallowWater.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Tests/AutomationEditorCommon.h"
#include "TextureResource.h"

namespace
{
	/** 与 NaniteCapture 测试同一个高处拍摄中心：新关卡里就算有什么东西，也进不了高度图。 */
	const FVector CSSWSolverTest_Origin(0.0, 0.0, 20000.0);
	/** WorldPixelSize：CaptureSize 2560 ⇒ 128²，640 ⇒ 32²。 */
	constexpr double CSSWSolverTest_TexelSize = 20.0;
	/** ShallowWater.usf 把深度换回高度用的是编译期 MAX_HEIGHT，actor 的 MaxHeight 不等于它时整张地形会平移。 */
	constexpr float CSSWSolverTest_ShaderMaxHeight = 10000.0f;

	/** 带 CSSW 标签的引擎 Cube（居中的 100cm 立方体）。Offset 相对拍摄中心。 */
	AStaticMeshActor* CSSWSolverTest_SpawnBox(UWorld* World, const FVector& Offset, const FRotator& Rotation, const FVector& Scale)
	{
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (!Cube) return nullptr;

		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags = RF_Transient;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(),
			FTransform(Rotation, CSSWSolverTest_Origin + Offset, Scale), SpawnParameters);
		if (!Actor) return nullptr;

		UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetStaticMesh(Cube);
		// 引擎默认材质为所有用途都编好了着色器，不会在测试里等一轮材质编译。
		Component->SetMaterial(0, UMaterial::GetDefaultMaterial(MD_Surface));
		Actor->Tags.Add(FName(TEXT("CSSW")));
		return Actor;
	}

	/** 地板：30 m 见方，顶面正好在拍摄中心高度（相对高度 0），两种捕获尺寸都盖得住。 */
	AStaticMeshActor* CSSWSolverTest_SpawnFloor(UWorld* World)
	{
		return CSSWSolverTest_SpawnBox(World, FVector(0.0, 0.0, -50.0), FRotator::ZeroRotator, FVector(30.0, 30.0, 1.0));
	}

	/** 求解器把 ActorScale.X × 500cm 当注水半径。 */
	FVector CSSWSolverTest_SourceScale(double RadiusTexels)
	{
		return FVector(RadiusTexels * CSSWSolverTest_TexelSize / 500.0);
	}

	/** 水源。C++ 类本身不带组件（蓝图 BP_CSSW_Source 才有根和圆盘）：没有根时 GetActorLocation 恒为原点、
	 *  缩放恒为 1，所以这里补一个根。 */
	ACSShallowWaterSource* CSSWSolverTest_SpawnSource(UWorld* World, const FVector& Offset, double RadiusTexels)
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags = RF_Transient;
		ACSShallowWaterSource* Source = World->SpawnActor<ACSShallowWaterSource>(ACSShallowWaterSource::StaticClass(), FTransform::Identity, SpawnParameters);
		if (!Source) return nullptr;

		USceneComponent* Root = NewObject<USceneComponent>(Source, TEXT("Root"), RF_Transient);
		Source->SetRootComponent(Root);
		Root->RegisterComponent();
		Source->SetActorLocation(CSSWSolverTest_Origin + Offset);
		Source->SetActorScale3D(CSSWSolverTest_SourceScale(RadiusTexels));
		return Source;
	}

	ACSShallowWaterCapture* CSSWSolverTest_SpawnWater(UWorld* World, double CaptureSize, bool bCloseBound)
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags = RF_Transient;
		ACSShallowWaterCapture* Water = World->SpawnActor<ACSShallowWaterCapture>(ACSShallowWaterCapture::StaticClass(), FTransform(CSSWSolverTest_Origin), SpawnParameters);
		if (!Water) return nullptr;

		Water->CaptureSize = float(CaptureSize);
		Water->WorldPixelSize = float(CSSWSolverTest_TexelSize);
		Water->CloseBound = bCloseBound;
		// 构造脚本那条路是 0.01s 的防抖定时器，测试体里没人推定时器，直接调它的落点。
		Water->ConstructionComponent();
		return Water;
	}

	/** 清空水体并重拍地形。Clean 会把 RT_SceneDepth 一并清成哨兵，所以先 Clean 再 CaptureAll。 */
	void CSSWSolverTest_Reset(ACSShallowWaterCapture* Water)
	{
		Water->Clean();
		Water->CaptureAll();
	}

	/** 逐步推进，每步一次迭代。求解入口按引擎帧去重（同一帧只解一次），测试体在同一帧里连续推进，每步先清掉去重标记。 */
	void CSSWSolverTest_Step(ACSShallowWaterCapture* Water, int32 Steps)
	{
		for (int32 Step = 0; Step < Steps; ++Step)
		{
			Water->LastSolverFrameNumber = 0;
			Water->ShallowWaterSolverSourcePoint(1);
		}
	}

	/** texel (X, Y) 中心相对拍摄中心的世界 XY。 */
	FVector2D CSSWSolverTest_TexelCenterOffset(double CaptureSize, int32 X, int32 Y)
	{
		const double Half = CaptureSize * 0.5;
		return FVector2D(-Half + (X + 0.5) * CSSWSolverTest_TexelSize, -Half + (Y + 0.5) * CSSWSolverTest_TexelSize);
	}

	/** RT_SceneDepth 换回地面世界高度（depth = ActorZ + MaxHeight − WorldZ），行优先。回读自带 flush。 */
	TArray<float> CSSWSolverTest_ReadGroundZ(ACSShallowWaterCapture* Water)
	{
		TArray<FLinearColor> Pixels;
		Water->RT_SceneDepth->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels);

		const float CameraHeight = float(Water->GetActorLocation().Z + Water->MaxHeight);
		TArray<float> Ground;
		Ground.Reserve(Pixels.Num());
		for (const FLinearColor& Pixel : Pixels) Ground.Add(CameraHeight - Pixel.R);
		return Ground;
	}

	/** RT_VelocityHeight 的一次回读：每 texel (vel.x, vel.y, 水深, foam)，行优先。回读自带 flush。 */
	struct FCSSWSolverState
	{
		TArray<FLinearColor> Texels;
		int32 Size = 0;

		float Depth(int32 X, int32 Y) const { return Texels[Y * Size + X].B; }
	};

	FCSSWSolverState CSSWSolverTest_Read(ACSShallowWaterCapture* Water)
	{
		FCSSWSolverState State;
		State.Size = Water->RT_VelocityHeight->SizeX;
		Water->RT_VelocityHeight->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(State.Texels);
		return State;
	}

	struct FCSSWSolverStats
	{
		int32 NonFinite = 0;
		int32 NegativeDepth = 0;
		int32 OverSpeed = 0;
		int32 Wet = 0;
		/** Σ 水深，cm·texel²。 */
		double Volume = 0.0;
		/** 按水深加权的 texel 坐标（texel 中心 = 整数 + 0.5）。 */
		FVector2D Centroid = FVector2D::ZeroVector;
		/** 有水的 texel 到 Reference 的最远距离，texel。 */
		double MaxWetDistance = 0.0;
		float MaxSpeed = 0.0f;
	};

	FCSSWSolverStats CSSWSolverTest_Stats(const FCSSWSolverState& State, const FVector2D& Reference)
	{
		FCSSWSolverStats Stats;
		FVector2D WeightedSum = FVector2D::ZeroVector;
		for (int32 Y = 0; Y < State.Size; ++Y)
		{
			for (int32 X = 0; X < State.Size; ++X)
			{
				const FLinearColor& Texel = State.Texels[Y * State.Size + X];
				if (!FMath::IsFinite(Texel.R) || !FMath::IsFinite(Texel.G) || !FMath::IsFinite(Texel.B) || !FMath::IsFinite(Texel.A))
				{
					++Stats.NonFinite;
					continue;
				}

				const float Speed = FVector2f(Texel.R, Texel.G).Size();
				Stats.MaxSpeed = FMath::Max(Stats.MaxSpeed, Speed);
				if (Speed > CSSW_VELOCITY_CLAMP + 1e-3f) ++Stats.OverSpeed;
				if (Texel.B < 0.0f) ++Stats.NegativeDepth;
				if (Texel.B <= 0.0f) continue;

				const FVector2D Center(X + 0.5, Y + 0.5);
				++Stats.Wet;
				Stats.Volume += Texel.B;
				WeightedSum += Center * Texel.B;
				Stats.MaxWetDistance = FMath::Max(Stats.MaxWetDistance, FVector2D::Distance(Center, Reference));
			}
		}
		if (Stats.Volume > 0.0) Stats.Centroid = WeightedSum / Stats.Volume;
		return Stats;
	}

	/** 任何时刻都必须成立：数值有限、水深非负、速度不超过 shader 的 VELOCITY_CLAMP。 */
	void CSSWSolverTest_TestSane(FAutomationTestBase& Test, const TCHAR* When, const FCSSWSolverStats& Stats)
	{
		Test.TestEqual(*FString::Printf(TEXT("%s：没有 NaN / Inf"), When), Stats.NonFinite, 0);
		Test.TestEqual(*FString::Printf(TEXT("%s：水深不为负"), When), Stats.NegativeDepth, 0);
		Test.TestEqual(*FString::Printf(TEXT("%s：速度不超过 VELOCITY_CLAMP"), When), Stats.OverSpeed, 0);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSShallowWaterSolverSourceSpreadTest,
	"PCGPlugins.ComputeShaderGenerator.ShallowWater.Solver.SourceSpread",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

/**
 * 平地、一个水源，真 GPU 求解 + 回读 RT_VelocityHeight：
 *   ① 落点：源落在自己世界坐标对应的 texel 上，X / Y 镜像处没有水，源心两侧对称（没有半个 texel 的偏置）；
 *   ② 波前：每步只和四邻域交换水量，N 步后有水的 texel 离源心不超过 半径 + N；
 *   ③ 漫流：水面确实摊开，平地上质心不跑偏；
 *   ④ 双向：源低于当前水面时把水面往下拉（排水），而不只是注水。
 */
bool FCSShallowWaterSolverSourceSpreadTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	if (!TestNotNull(TEXT("地板"), CSSWSolverTest_SpawnFloor(World))) return false;

	constexpr double CaptureSize = 2560.0;
	ACSShallowWaterCapture* Water = CSSWSolverTest_SpawnWater(World, CaptureSize, /*bCloseBound*/ false);
	if (!TestNotNull(TEXT("CSSW actor"), Water)) return false;
	if (!TestEqual(TEXT("MaxHeight 等于 shader 的 MAX_HEIGHT"), Water->MaxHeight, CSSWSolverTest_ShaderMaxHeight)) return false;
	CSSWSolverTest_Reset(Water);
	const int32 Size = Water->RT_VelocityHeight->SizeX;
	if (!TestEqual(TEXT("128² 模拟网格"), Size, 128)) return false;

	// 源放在 texel (44, 74) 中心、离地 100cm、半径 10 texel：偏离中心，X / Y 镜像和 XY 互换的位置都落在远处的空地上。
	constexpr int32 SX = 44;
	constexpr int32 SY = 74;
	constexpr double Radius = 10.0;
	const FVector2D SourceTexel(SX + 0.5, SY + 0.5);
	if (!TestEqual(TEXT("地板拍进了高度图"), CSSWSolverTest_ReadGroundZ(Water)[SY * Size + SX], float(CSSWSolverTest_Origin.Z), 0.5f)) return false;

	ACSShallowWaterSource* Source = CSSWSolverTest_SpawnSource(World, FVector(CSSWSolverTest_TexelCenterOffset(CaptureSize, SX, SY), 100.0), Radius);
	if (!TestNotNull(TEXT("水源"), Source)) return false;

	// ① 落点
	CSSWSolverTest_Step(Water, 1);
	const FCSSWSolverState S1 = CSSWSolverTest_Read(Water);
	const FCSSWSolverStats Stats1 = CSSWSolverTest_Stats(S1, SourceTexel);
	CSSWSolverTest_TestSane(*this, TEXT("第 1 步"), Stats1);
	const float CenterDepth1 = S1.Depth(SX, SY);
	TestTrue(*FString::Printf(TEXT("源心被灌到源高度附近（100cm，实际 %.2f）"), CenterDepth1), CenterDepth1 >= 80.0f && CenterDepth1 <= 100.5f);
	TestEqual(TEXT("X 镜像处没有水"), S1.Depth(Size - 1 - SX, SY), 0.0f);
	TestEqual(TEXT("Y 镜像处没有水"), S1.Depth(SX, Size - 1 - SY), 0.0f);
	TestEqual(TEXT("XY 互换处没有水"), S1.Depth(SY, SX), 0.0f);
	// 半个 texel 的偏置会让两侧差出约 10cm（半径 10 texel、源高 100cm 的锥面上半个 texel 就是 5cm 的混合差，两侧相反）。
	TestEqual(TEXT("源心左右对称"), S1.Depth(SX - 1, SY), S1.Depth(SX + 1, SY), 2.0f);
	TestEqual(TEXT("源心上下对称"), S1.Depth(SX, SY - 1), S1.Depth(SX, SY + 1), 2.0f);

	// ② 波前
	CSSWSolverTest_Step(Water, 19);
	const FCSSWSolverStats Stats20 = CSSWSolverTest_Stats(CSSWSolverTest_Read(Water), SourceTexel);
	CSSWSolverTest_TestSane(*this, TEXT("第 20 步"), Stats20);
	const double FrontLimit = Radius + 20.0 + 1.0;
	TestTrue(*FString::Printf(TEXT("20 步后波前不超过 半径 + 步数（实际 %.2f，上限 %.0f texel）"), Stats20.MaxWetDistance, FrontLimit), Stats20.MaxWetDistance <= FrontLimit);

	// ③ 漫流。质心与波前对称守的是方向性偏差：速度 pass 只存每格的右 / 下面，任何只作用在一侧的项
	// （例如注水只改本格、梯度却对着邻居注水前的高度）都会给每个右 / 下面多一份推力，水体一路往 +X+Y 漂。
	CSSWSolverTest_Step(Water, 100);
	const FCSSWSolverState S120 = CSSWSolverTest_Read(Water);
	const FCSSWSolverStats Stats120 = CSSWSolverTest_Stats(S120, SourceTexel);
	CSSWSolverTest_TestSane(*this, TEXT("第 120 步"), Stats120);
	TestTrue(*FString::Printf(TEXT("水面摊开了（湿 texel %d → %d）"), Stats1.Wet, Stats120.Wet), Stats120.Wet > Stats1.Wet * 1.3);
	const FVector2D CentroidShift = Stats120.Centroid - SourceTexel;
	const double CentroidDrift = CentroidShift.Size();
	TestTrue(*FString::Printf(TEXT("平地上质心不跑偏（偏 (%.3f, %.3f) texel）"), CentroidShift.X, CentroidShift.Y), CentroidDrift < 1.5);
	// 过源心的行 / 列上，四个方向各自最远的湿 texel：波前是否对称。
	auto FrontReach = [&S120, Size](int32 DX, int32 DY)
	{
		int32 Reach = 0;
		for (int32 K = 1; K < Size; ++K)
		{
			const int32 X = SX + DX * K;
			const int32 Y = SY + DY * K;
			if (X < 0 || Y < 0 || X >= Size || Y >= Size) break;
			if (S120.Depth(X, Y) > 0.0f) Reach = K;
		}
		return Reach;
	};
	const int32 ReachPosX = FrontReach(1, 0), ReachNegX = FrontReach(-1, 0), ReachPosY = FrontReach(0, 1), ReachNegY = FrontReach(0, -1);
	TestTrue(*FString::Printf(TEXT("波前对称（+X %d / -X %d / +Y %d / -Y %d texel）"), ReachPosX, ReachNegX, ReachPosY, ReachNegY),
		FMath::Abs(ReachPosX - ReachNegX) <= 1 && FMath::Abs(ReachPosY - ReachNegY) <= 1);

	// ④ 源降到离地 30cm：半径内的水面被往下拉，没有别的地方会凭空产水，总水量只能减少。
	const float CenterDepth120 = S120.Depth(SX, SY);
	Source->SetActorLocation(FVector(Source->GetActorLocation().X, Source->GetActorLocation().Y, CSSWSolverTest_Origin.Z + 30.0));
	CSSWSolverTest_Step(Water, 20);
	const FCSSWSolverState S140 = CSSWSolverTest_Read(Water);
	const FCSSWSolverStats Stats140 = CSSWSolverTest_Stats(S140, SourceTexel);
	CSSWSolverTest_TestSane(*this, TEXT("降源后第 20 步"), Stats140);
	TestEqual(TEXT("源低于水面时把源心水深拉到源高度附近"), S140.Depth(SX, SY), 30.0f, 15.0f);
	TestTrue(TEXT("排水：总水量减少"), Stats140.Volume < Stats120.Volume);

	AddInfo(FString::Printf(TEXT("第 1 步：源心 %.2f cm，湿 %d；第 20 步：波前 %.2f texel；第 120 步：源心 %.2f cm，湿 %d，水量 %.0f，质心偏 %.3f texel，最大速度 %.3f；降源 20 步：源心 %.2f cm，水量 %.0f"),
		CenterDepth1, Stats1.Wet, Stats20.MaxWetDistance, CenterDepth120, Stats120.Wet, Stats120.Volume, CentroidDrift, Stats120.MaxSpeed, S140.Depth(SX, SY), Stats140.Volume));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSShallowWaterSolverWallTest,
	"PCGPlugins.ComputeShaderGenerator.ShallowWater.Solver.WallBlocks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

/** 一堵横贯整张图、比源高得多的墙：水流到墙脚为止，墙顶和墙后一滴都没有。 */
bool FCSShallowWaterSolverWallTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	if (!TestNotNull(TEXT("地板"), CSSWSolverTest_SpawnFloor(World))) return false;
	// 墙：X ∈ [175, 235]，横贯整个 Y，高 500。texel 73 / 74 / 75 的中心（190 / 210 / 230）在墙里，72 / 76 离墙面 5cm。
	if (!TestNotNull(TEXT("墙"), CSSWSolverTest_SpawnBox(World, FVector(205.0, 0.0, 250.0), FRotator::ZeroRotator, FVector(0.6, 30.0, 5.0)))) return false;

	constexpr double CaptureSize = 2560.0;
	ACSShallowWaterCapture* Water = CSSWSolverTest_SpawnWater(World, CaptureSize, /*bCloseBound*/ false);
	if (!TestNotNull(TEXT("CSSW actor"), Water)) return false;
	CSSWSolverTest_Reset(Water);
	const int32 Size = Water->RT_VelocityHeight->SizeX;
	if (!TestEqual(TEXT("128² 模拟网格"), Size, 128)) return false;

	constexpr int32 WallFirstX = 73;
	const TArray<float> Ground = CSSWSolverTest_ReadGroundZ(Water);
	const float Base = float(CSSWSolverTest_Origin.Z);
	if (!TestEqual(TEXT("墙拍进了高度图"), Ground[64 * Size + 74], Base + 500.0f, 0.5f)) return false;
	TestEqual(TEXT("墙前一格是地板"), Ground[64 * Size + WallFirstX - 1], Base, 0.5f);
	TestEqual(TEXT("墙后一格是地板"), Ground[64 * Size + WallFirstX + 3], Base, 0.5f);

	// 源贴着墙：半径 5 texel 的注水盘止于 texel 70，离墙面两格。
	constexpr int32 SX = 66;
	constexpr int32 SY = 64;
	if (!TestNotNull(TEXT("水源"), CSSWSolverTest_SpawnSource(World, FVector(CSSWSolverTest_TexelCenterOffset(CaptureSize, SX, SY), 100.0), 5.0))) return false;

	CSSWSolverTest_Step(Water, 60);
	const FCSSWSolverState State = CSSWSolverTest_Read(Water);
	const FCSSWSolverStats Stats = CSSWSolverTest_Stats(State, FVector2D(SX + 0.5, SY + 0.5));
	CSSWSolverTest_TestSane(*this, TEXT("第 60 步"), Stats);

	int32 WetBeyond = 0;
	float MaxBeyond = 0.0f;
	for (int32 Y = 0; Y < Size; ++Y)
	{
		for (int32 X = WallFirstX; X < Size; ++X)
		{
			const float Depth = State.Depth(X, Y);
			if (Depth <= 0.0f) continue;
			++WetBeyond;
			MaxBeyond = FMath::Max(MaxBeyond, Depth);
		}
	}
	TestEqual(*FString::Printf(TEXT("墙顶与墙后没有水（最深 %.3f cm）"), MaxBeyond), WetBeyond, 0);
	// 不然上一条是空话：水根本没流到墙跟前。
	const float FootDepth = State.Depth(WallFirstX - 1, SY);
	TestTrue(*FString::Printf(TEXT("水流到了墙脚（%.2f cm）"), FootDepth), FootDepth > 1.0f);

	AddInfo(FString::Printf(TEXT("第 60 步：墙脚 %.2f cm，湿 %d，水量 %.0f，墙后湿 texel %d"), FootDepth, Stats.Wet, Stats.Volume, WetBeyond));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSShallowWaterSolverDownhillTest,
	"PCGPlugins.ComputeShaderGenerator.ShallowWater.Solver.Downhill",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

/** 斜坡上的水源：水体质心顺着拍到的地面坡度往低处走，不横向跑偏。坡沿对角线，X / Y 两个轴的符号都验到。 */
bool FCSShallowWaterSolverDownhillTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	// 40 m 见方的板，俯仰 10° 再偏航 45°：坡面沿对角线抬升，每个轴向约 2.5cm / texel。
	if (!TestNotNull(TEXT("斜坡"), CSSWSolverTest_SpawnBox(World, FVector(0.0, 0.0, -51.0), FRotator(10.0, 45.0, 0.0), FVector(40.0, 40.0, 1.0)))) return false;

	constexpr double CaptureSize = 2560.0;
	ACSShallowWaterCapture* Water = CSSWSolverTest_SpawnWater(World, CaptureSize, /*bCloseBound*/ false);
	if (!TestNotNull(TEXT("CSSW actor"), Water)) return false;
	CSSWSolverTest_Reset(Water);
	const int32 Size = Water->RT_VelocityHeight->SizeX;
	if (!TestEqual(TEXT("128² 模拟网格"), Size, 128)) return false;

	// 下坡方向取自拍到的地面而不是按旋转推算：这里验的是求解器"水往低处流"，拍摄朝向由 NaniteCapture 测试守着。
	constexpr int32 SX = 80;
	constexpr int32 SY = 80;
	const TArray<float> Ground = CSSWSolverTest_ReadGroundZ(Water);
	auto GroundAt = [&Ground, Size](int32 X, int32 Y) { return Ground[Y * Size + X]; };
	const FVector2D Gradient(
		(GroundAt(SX + 10, SY) - GroundAt(SX - 10, SY)) / 20.0f,
		(GroundAt(SX, SY + 10) - GroundAt(SX, SY - 10)) / 20.0f);
	if (!TestTrue(*FString::Printf(TEXT("两个轴向都有坡度（%.2f, %.2f cm / texel）"), Gradient.X, Gradient.Y), FMath::Abs(Gradient.X) > 1.0 && FMath::Abs(Gradient.Y) > 1.0)) return false;
	const FVector2D Downhill = -Gradient.GetSafeNormal();

	const FVector2D SourceTexel(SX + 0.5, SY + 0.5);
	const double SourceHeight = GroundAt(SX, SY) - CSSWSolverTest_Origin.Z + 100.0;
	if (!TestNotNull(TEXT("水源"), CSSWSolverTest_SpawnSource(World, FVector(CSSWSolverTest_TexelCenterOffset(CaptureSize, SX, SY), SourceHeight), 5.0))) return false;

	CSSWSolverTest_Step(Water, 150);
	const FCSSWSolverStats Stats = CSSWSolverTest_Stats(CSSWSolverTest_Read(Water), SourceTexel);
	CSSWSolverTest_TestSane(*this, TEXT("第 150 步"), Stats);

	const FVector2D Shift = Stats.Centroid - SourceTexel;
	const double Along = Shift | Downhill;
	const double Across = FMath::Abs(Shift ^ Downhill);
	TestTrue(*FString::Printf(TEXT("质心往下坡走了（%.3f texel）"), Along), Along > 1.0);
	TestTrue(*FString::Printf(TEXT("质心没有横向跑偏（%.3f texel）"), Across), Across < 1.5);

	AddInfo(FString::Printf(TEXT("坡度 (%.2f, %.2f) cm/texel；第 150 步：质心位移 (%.3f, %.3f)，顺坡 %.3f、横向 %.3f texel，湿 %d，水量 %.0f"),
		Gradient.X, Gradient.Y, Shift.X, Shift.Y, Along, Across, Stats.Wet, Stats.Volume));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSShallowWaterSolverClosedBasinTest,
	"PCGPlugins.ComputeShaderGenerator.ShallowWater.Solver.ClosedBasin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

/**
 * 32² 的小池子，水源灌到水碰壁后停注：
 *   CloseBound：水量守恒，边界带一滴不进；
 *   敞开边界：水从边缘流失，最后剩得比封闭时少。
 */
bool FCSShallowWaterSolverClosedBasinTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	if (!TestNotNull(TEXT("地板"), CSSWSolverTest_SpawnFloor(World))) return false;

	constexpr double CaptureSize = 640.0;
	ACSShallowWaterCapture* Water = CSSWSolverTest_SpawnWater(World, CaptureSize, /*bCloseBound*/ true);
	if (!TestNotNull(TEXT("CSSW actor"), Water)) return false;
	CSSWSolverTest_Reset(Water);
	const int32 Size = Water->RT_VelocityHeight->SizeX;
	if (!TestEqual(TEXT("32² 模拟网格"), Size, 32)) return false;

	constexpr int32 SX = 16;
	constexpr int32 SY = 16;
	constexpr double Radius = 4.0;
	const FVector2D SourceTexel(SX + 0.5, SY + 0.5);
	ACSShallowWaterSource* Source = CSSWSolverTest_SpawnSource(World, FVector(CSSWSolverTest_TexelCenterOffset(CaptureSize, SX, SY), 60.0), Radius);
	if (!TestNotNull(TEXT("水源"), Source)) return false;

	// 速度 pass 的边界带：离边 ≤ 2 格（WATERBOUNDSIZE + 1）。
	auto InBoundStrip = [Size](int32 X, int32 Y) { return X <= 2 || Y <= 2 || X >= Size - 3 || Y >= Size - 3; };

	struct FRun
	{
		double VolumeParked = 0.0;
		double VolumeEnd = 0.0;
		float RingMaxDepth = 0.0f;
		int32 WetInStrip = 0;
	};
	auto Run = [&](bool bCloseBound) -> FRun
	{
		FRun Result;
		Water->CloseBound = bCloseBound;
		CSSWSolverTest_Reset(Water);
		Source->SetActorScale3D(CSSWSolverTest_SourceScale(Radius));
		CSSWSolverTest_Step(Water, 200);

		const FCSSWSolverState Filled = CSSWSolverTest_Read(Water);
		const FCSSWSolverStats FilledStats = CSSWSolverTest_Stats(Filled, SourceTexel);
		CSSWSolverTest_TestSane(*this, bCloseBound ? TEXT("封闭·注水 200 步") : TEXT("敞开·注水 200 步"), FilledStats);
		Result.VolumeParked = FilledStats.Volume;
		for (int32 I = 3; I <= Size - 4; ++I)
		{
			Result.RingMaxDepth = FMath::Max(Result.RingMaxDepth, FMath::Max(
				FMath::Max(Filled.Depth(I, 3), Filled.Depth(I, Size - 4)),
				FMath::Max(Filled.Depth(3, I), Filled.Depth(Size - 4, I))));
		}

		// 停注水：半径为 0 的源既不注水也不激活 tile。不直接删源——没有源时求解入口整个跳过，水会冻在原地。
		Source->SetActorScale3D(FVector(0.0, 1.0, 1.0));
		CSSWSolverTest_Step(Water, 100);

		const FCSSWSolverState End = CSSWSolverTest_Read(Water);
		const FCSSWSolverStats EndStats = CSSWSolverTest_Stats(End, SourceTexel);
		CSSWSolverTest_TestSane(*this, bCloseBound ? TEXT("封闭·停注 100 步") : TEXT("敞开·停注 100 步"), EndStats);
		Result.VolumeEnd = EndStats.Volume;
		for (int32 Y = 0; Y < Size; ++Y)
		{
			for (int32 X = 0; X < Size; ++X)
			{
				if (InBoundStrip(X, Y) && End.Depth(X, Y) > 0.0f) ++Result.WetInStrip;
			}
		}
		return Result;
	};

	const FRun Closed = Run(true);
	// 不然守恒是空话：水根本没碰到边界带。
	TestTrue(*FString::Printf(TEXT("封闭：注水阶段水流到了边界带内侧（%.2f cm）"), Closed.RingMaxDepth), Closed.RingMaxDepth > 0.5f);
	const double ClosedDrift = Closed.VolumeParked > 0.0 ? (Closed.VolumeEnd - Closed.VolumeParked) / Closed.VolumeParked : 0.0;
	TestTrue(*FString::Printf(TEXT("封闭：停注后 100 步水量守恒（变化 %+.3f%%）"), ClosedDrift * 100.0), Closed.VolumeParked > 0.0 && FMath::Abs(ClosedDrift) < 0.03);
	TestEqual(TEXT("封闭：边界带里没有水"), Closed.WetInStrip, 0);

	const FRun Open = Run(false);
	TestTrue(*FString::Printf(TEXT("敞开：停注后水从边缘流失（%.0f → %.0f）"), Open.VolumeParked, Open.VolumeEnd), Open.VolumeEnd < Open.VolumeParked);
	TestTrue(*FString::Printf(TEXT("敞开：最后剩的水比封闭时少（%.0f vs %.0f）"), Open.VolumeEnd, Closed.VolumeEnd), Open.VolumeEnd < Closed.VolumeEnd * 0.9);

	AddInfo(FString::Printf(TEXT("封闭：停注时 %.0f → 100 步后 %.0f（%+.3f%%），边界内侧最深 %.2f cm；敞开：%.0f → %.0f"),
		Closed.VolumeParked, Closed.VolumeEnd, ClosedDrift * 100.0, Closed.RingMaxDepth, Open.VolumeParked, Open.VolumeEnd));
	return true;
}

#endif
