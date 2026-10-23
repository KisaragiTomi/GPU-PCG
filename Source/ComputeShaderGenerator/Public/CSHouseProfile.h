#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"   // FCSOpeningSite::Openings —— CoreMinimal 不带它
#include "CSHouseProfile.generated.h"

/**
 * 洞的 2D 剖面求值器（Plan_House.md D4/D6/D8）。
 *
 * **开洞不走布尔**（用户裁决 2026-08-29「尽可能不用 MeshBoolean」）：洞的形状本来就是有限的
 * 原型集合（拱 / 矩形 / 圆…），每种配一条手写 2D 剖面，墙板生成时按剖面直接砌出带洞几何，
 * 洞缘精度只受分段数控制、洞口内壁一并产出。洞不携带、不生成、不回读任何 per-opening 几何。
 *
 * **这是正确性关键路径**：同一条剖面同时供三处使用 ——
 *   ① 墙板砌洞（上下过梁带 / 窗台带）
 *   ② 洞口内壁扫掠（reveal）
 *   ③ QueryFeaturePlacement 的重叠谓词
 * 三处不同式就会出现"谓词说能放、几何砌出来穿帮"。所以它必须是唯一的纯函数，且进单测。
 *
 * 墙空间口径：(S, Z) —— S 是沿边缘线段的弧长（从线段起点算），Z 是从房底起算的高度。
 * 与 FCSHouseEdgeFrame 的 (Start, U, In) 一一对应。
 */

UENUM(BlueprintType)
enum class ECSOpeningType : uint8
{
	/** 道路推导：地面顶点色 R 过阈的子段自动点亮（D6）。 */
	Door,
	/** 特征标记注册：ACSHouseFeatureMarker 提诉求、房子裁决（D8）。 */
	Window,
	/** 第三方注入（楼梯穿墙等）。**当前不生产，只保证格式容纳**，见计划开放问题。 */
	Stair,
};

UENUM(BlueprintType)
enum class ECSOpeningShape : uint8
{
	/** 矩形下身 + 半圆顶。门拱的原型。 */
	Arch,
	/** 纯矩形。 */
	Rect,
	/** 正圆（Width 同时决定直径，Z0/Z1 被居中的圆覆盖）。 */
	Circle,
};

/**
 * 一个洞 = 原型剖面 + 沿边的摆位。不存任何切出的几何。
 *
 * **为什么现在就要 Z0/Z1/AxisUS/Skew**（用户指令 2026-08-29：楼梯暂不做，但洞逻辑必须容纳它）：
 * 早先的"Height 从墙基起算"+"切轴恒为墙法线"这两条硬编码，把三类洞挡在门外 ——
 *   ① 楼梯穿墙：洞底在半空（踏面高度）、洞顶随坡倾斜、切轴是楼梯行进方向而非墙法线；
 *   ② 窗台高度：窗只能从墙基起算的话，窗台压根表达不出来；
 *   ③ 多边形 footprint 的转角洞：切轴需要偏离单一边的法线。
 * 三者共用这一个判决点，所以字段必须在 D8 冻结 openings 格式**之前**加，不能等楼梯排期。
 */
USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSWallOpening
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	ECSOpeningType Type = ECSOpeningType::Door;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	ECSOpeningShape Shape = ECSOpeningShape::Arch;

	/** 边缘线段索引（矩形房 = 0..3；多边形 footprint 后为折线段索引）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	int32 EdgeIndex = 0;

	/** 沿边参数位置（从边起点算的弧长），洞的中心。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	float CenterS = 0.0f;

	/** 洞宽（沿边）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	float Width = 0.0f;

	/** 洞底，墙空间高度。门恒 0；窗台 / 楼梯口 > 0。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	float Z0 = 0.0f;

	/** 洞顶，墙空间高度（取代早先那个"从墙基算的 Height"）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	float Z1 = 0.0f;

	/**
	 * 切轴在 (沿边 U, 墙内法线 In) 平面内的方向。(0,1) = 垂直墙面（绝大多数洞）；
	 * 楼梯斜穿时非 (0,1)，洞口内壁沿它扫掠而不是沿法线挤出。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	FVector2D AxisUS = FVector2D(0.0, 1.0);

	/** Z1 沿 S 的线性斜率。楼梯洞顶随坡倾斜；门窗恒 0。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	float Skew = 0.0f;

	/** (线段, 子段) 或 特征标记 GUID / 注入方 id。滞回与哈希用。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	FGuid SourceId;

	/** 悬停高亮用的 8 位标签（进房体顶点色 G 通道，见 ACSHouseActor 的通道字典）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	uint8 Tag = 0;

	/**
	 * 与左右两侧残料跨度有关的样式位（`CSHouse_StylePier*`）。
	 *
	 * **为什么把它挂在洞上而不是另起一张表**：墩的判定带双阈迟滞 ⇒ 它是路径依赖的，
	 * 记忆只能活在 actor 里（同 `DoorRunMemory`）；而铺墙板的 `CSHouse_BuildBodySoup` 是纯函数，
	 * 只吃一份 desc。中间要么再传一张按跨度序号索引的表（两边各自枚举跨度、序号一旦对不上就
	 * 静默错位），要么把结论粘在洞本身上——洞是唯一同时流过两边、且顺序天然一致的东西。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	uint8 StyleFlags = 0;

	/**
	 * 拱顶相对**起拱线**的升高 cm（`Shape == Arch` 才有意义）。**0 = 用半宽**，即正半圆。
	 *
	 * 加它是为了把**拱高与洞宽解耦**（2026-09-04，用户"门上升过高"）。半圆下拱高恒等于半宽，
	 * 洞一宽拱就顶到很高；TG 不是这样 —— `img/tiny-glade-ref-twin-arch-pier.jpg` 里那两个拱
	 * 跨约 200 cm、起拱线以上只升约 80 cm，明显**扁于半圆**（TG 侧的载体是
	 * `create_stone_arch_profile` + `ArchFunction::remap_t` 的剖面曲线）。
	 *
	 * 归一化之后判据一个字都不用改：`q = ((S−Cs)/HW, (Z−拱脚)/Rise)`，`dot(q,q) < 1` 从正圆
	 * 变成**椭圆**。⇒ **材质里那段 HLSL 不需要跟着改** —— 它只看已经归一化的 q，
	 * 缩放全在 `FCSOpeningClipField` 里。这条是这次改动能这么小的原因。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	float ArchRise = 0.0f;

	/**
	 * 附属物的**门形态**（2026-09-16）：窗被拖到贴近墙脚时吸附成门，洞底落在墙脚（`Z0 = 0`）。
	 * 从 `FCSHouseWindow::bDoorForm` 原样带过来。
	 *
	 * ⚠️ **`Type` 仍是 `Window`，别改成 `Door`**：`Type` 记的是**谁产的洞**（道路 / 附属物 / 注入方），
	 * 不是洞的形状。TG 同构 —— 存档里的 `DecoratorType` 始终是 `CottageWindow` / `GothicWindow`，
	 * 只有每帧派生的 `DecoratorSubtype` 变成 `*BalconyDoor`（附录 E §3.1）。改成 `Door` 的话，
	 * 门扇（`RebuildDoorLeaves`）与门框砖（`CSHouseFrame`）都会按道路门再给它补一份，
	 * 而附属物自己已经带着门的网格 —— 双份几何，一条断言都不会红。
	 *
	 * 房子这边**只有谓词**读它：不再拿 `WindowMinSillZ` 卡它（门本来就落地）。门侧挂件（门铃 / 花环）
	 * 归附属物自己带，**不**进道路门那家摆件（TG 那边也是两套，附录 E §7）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Opening")
	bool bDoorForm = false;

	float HalfWidth() const { return Width * 0.5f; }
	/** 实际拱高：`ArchRise` 为 0 时退回半宽（正半圆），并夹在洞高之内。 */
	float Rise() const
	{
		const float Full = FMath::Max(Z1 - Z0, UE_KINDA_SMALL_NUMBER);
		const float Want = (ArchRise > UE_KINDA_SMALL_NUMBER) ? ArchRise : HalfWidth();
		return FMath::Clamp(Want, UE_KINDA_SMALL_NUMBER, Full);
	}
	float SpringZ() const { return Z1 - Rise(); }
	float S0() const { return CenterS - Width * 0.5f; }
	float S1() const { return CenterS + Width * 0.5f; }
	bool IsValid() const { return Width > UE_KINDA_SMALL_NUMBER && Z1 > Z0 + UE_KINDA_SMALL_NUMBER; }
};

/** 剖面在某个横向位置上的上下边界。ZLow == ZHigh 表示洞在此闭合（圆的两端）。 */
struct FCSOpeningProfileSample
{
	float S = 0.0f;
	float ZLow = 0.0f;
	float ZHigh = 0.0f;
};

/**
 * 曲线段数按弦高容差自适应，而不是写死一个常数。
 *
 * 半圆按 N 段折线近似时弦高 = R·(1 − cos(π/2N))，反解得
 *   N = ceil( π / (2·acos(1 − Tol/R)) )
 * **注意分母那个 2** —— 漏掉它会在 Tol=0.2、R=55 时给出 37 段而不是正确的 19 段。
 *
 * 写死 12 段的问题是它把绝对误差绑在了半径上：R=55 时折角 0.47 cm，房子放大即线性放大。
 */
inline int32 CSHouse_ProfileSegments(float Radius, float ChordTolerance)
{
	if (Radius <= UE_KINDA_SMALL_NUMBER) return 6;
	const float Tol = FMath::Max(ChordTolerance, 1.0e-3f);
	const float CosHalf = FMath::Clamp(1.0f - Tol / Radius, -1.0f, 1.0f - 1.0e-6f);
	const float HalfAngle = FMath::Acos(CosHalf);
	if (HalfAngle <= 1.0e-6f) return 48;
	return FMath::Clamp(FMath::CeilToInt(PI / (2.0f * HalfAngle)), 6, 48);
}

/**
 * 采样一个洞的剖面，产出沿 S 单调递增的上下边界折线。
 *
 * **圆弧按外接多边形建，不是内接**（计划「洞口内壁的接缝精度」）：折线内接会比精确曲线
 * 缩进最多一个弦高，与按精确曲线摆放的洞缘装饰之间就会漏缝；外接则是微覆盖，宁可覆盖不可漏。
 * 顶点半径因此乘 1/cos(π/2N)，折线的边恰好切在真圆上。
 *
 * Skew 把洞顶沿 S 线性倾斜（楼梯口）；Z0/Z1 是未倾斜时的洞底与洞顶。
 */
inline void CSHouse_SampleOpeningProfile(const FCSWallOpening& Opening, float ChordTolerance, TArray<FCSOpeningProfileSample>& OutSamples)
{
	OutSamples.Reset();
	if (!Opening.IsValid()) return;

	const float R = Opening.HalfWidth();
	const float Skew = Opening.Skew;
	auto Emit = [&OutSamples, &Opening, Skew](float S, float ZLow, float ZHigh)
	{
		FCSOpeningProfileSample Sample;
		Sample.S = S;
		const float Tilt = Skew * (S - Opening.CenterS);
		Sample.ZLow = ZLow;
		Sample.ZHigh = ZHigh + Tilt;
		OutSamples.Add(Sample);
	};

	switch (Opening.Shape)
	{
	case ECSOpeningShape::Rect:
		// 上下边界都是常数：两个样本就够，法线不会因为多插几个点而变好。
		Emit(Opening.S0(), Opening.Z0, Opening.Z1);
		Emit(Opening.S1(), Opening.Z0, Opening.Z1);
		break;

	case ECSOpeningShape::Circle:
	{
		// 洞高服从洞宽（正圆），Z0/Z1 只用来定圆心。
		const float ZCentre = (Opening.Z0 + Opening.Z1) * 0.5f;
		const int32 N = CSHouse_ProfileSegments(R, ChordTolerance) * 2;   // 整圆，半圆的两倍
		const float Circum = R / FMath::Cos(PI / (2.0f * N));
		for (int32 K = 0; K <= N; ++K)
		{
			const float A = PI * K / N;   // 0..π 扫过 S，上下边界对称
			Emit(Opening.CenterS - Circum * FMath::Cos(A), ZCentre - Circum * FMath::Sin(A), ZCentre + Circum * FMath::Sin(A));
		}
		break;
	}

	case ECSOpeningShape::Arch:
	default:
	{
		// 矩形下身（Z0 → 拱脚）+ **椭圆**顶：半轴 = (半宽, Rise)。`ArchRise` 为 0 时 Rise = 半宽，
		// 退化成原来的正半圆，逐位不变。拱脚高 = 洞顶 − Rise。
		const float Rise = Opening.Rise();
		const float SpringZ = Opening.SpringZ();
		// 段数按**较长**那根半轴取：扁拱的 S 向仍然很长，按 Rise 取会在两肩上欠采样。
		const int32 N = CSHouse_ProfileSegments(FMath::Max(R, Rise), ChordTolerance);
		// 外接补偿逐轴各做一次（弦高补偿是等比缩放，两轴不同尺度时不能共用一个系数）。
		const float Inflate = 1.0f / FMath::Cos(PI / (2.0f * N));
		const float CircumS = R * Inflate;
		const float CircumZ = Rise * Inflate;
		for (int32 K = 0; K <= N; ++K)
		{
			const float A = PI * K / N;   // 0..π，从 S0 扫到 S1
			Emit(Opening.CenterS - CircumS * FMath::Cos(A), Opening.Z0, SpringZ + CircumZ * FMath::Sin(A));
		}
		break;
	}
	}
}

/**
 * 洞的**解析裁剪场**：把墙空间 (S, Z) 归一化成 q，材质按 q 逐像素判 discard。
 *
 * 这是 Tiny Glade 原版的开洞方式（逆向报告第一轮【确凿，GLSL 直接给出】）——CPU 只提供解析
 * 参数，洞形在像素阶段成立，零几何运算、洞缘是解析精确曲线而非折线。TG 的载体是逐砖实例的
 * `vec3 global_arch_height_vals`；本项目的墙是三角汤，对位物是**逐顶点 UV1**（UV0 留给墙面
 * 贴图，靠 FCSMeshStreamLayout::NumTexCoordSets 的逐 mesh 变体腾出来的）。
 *
 * **每种形状自带一套归一化，判据因此都是自封闭的**（不需要额外传洞底/洞高）：
 *   Arch   q = ((S−Cs)/HW, (Z−SpringZ)/Rise)    判据 q.y ≤ 0 ? |q.x| < 1 : dot(q,q) < 1
 *          （Rise 缺省 = HW ⇒ 正半圆；给了别的值就是**椭圆拱**，见 `FCSWallOpening::ArchRise`）
 *   Rect   q = ((S−Cs)/HW, (Z−Zmid)/HH)         判据 max(|q.x|, |q.y|) < 1
 *   Circle q = ((S−Cs)/HW, (Z−Zc)/HW)           判据 dot(q,q) < 1
 *
 * Arch 的判据在拱脚线以下是**无下界**的 —— 这不是疏漏：洞面板本身从 Z0 起建，面板底就是洞底，
 * 窗台那一截是下面另一块实心盒。这样两个 float 就够，不必再挤一个归一化洞底进 8-bit 通道
 * （直线的量化不可见，曲线的量化贴脸可见，所以宁可让几何承担直线那一半）。
 *
 * **判据必须与材质里那份逐字对应** —— 同一形状两处写不一样，就会出现"CPU 说能放、画面上切穿帮"。
 * CSHouse_ClipKeeps() 是 CPU 侧的那一份，单测拿它与几何交叉验证。
 */
USTRUCT()
struct COMPUTESHADERGENERATOR_API FCSOpeningClipField
{
	GENERATED_BODY()

	/** 沿边弧长上的洞心。 */
	UPROPERTY() float CenterS = 0.0f;
	/** 1 / 半宽。 */
	UPROPERTY() float InvHalfWidth = 0.0f;
	/** 竖直参考高（Arch = 拱脚, Rect = 洞中, Circle = 圆心）。 */
	UPROPERTY() float RefZ = 0.0f;
	/** 1 / 竖直半尺度。 */
	UPROPERTY() float InvScaleZ = 0.0f;
	/** 形状 id，写进顶点色 B 通道（/255）。 */
	UPROPERTY() ECSOpeningShape Shape = ECSOpeningShape::Arch;
	/** false = 这块面板上没有洞，UV1 写哨兵、B 写 255。 */
	UPROPERTY() bool bValid = false;

	FVector2f Eval(float S, float Z) const
	{
		if (!bValid) return FVector2f(8.0f, 8.0f);   // 哨兵：任何判据下都在洞外，整块面板保留
		return FVector2f((S - CenterS) * InvHalfWidth, (Z - RefZ) * InvScaleZ);
	}
};

/** 从一个洞算出它的裁剪场。与 CSHouse_SampleOpeningProfile 同源，两者描述的是同一条曲线。 */
inline FCSOpeningClipField CSHouse_ComputeClipField(const FCSWallOpening& Opening)
{
	FCSOpeningClipField Field;
	if (!Opening.IsValid()) return Field;

	const float HW = Opening.HalfWidth();
	Field.CenterS = Opening.CenterS;
	Field.InvHalfWidth = 1.0f / HW;
	Field.Shape = Opening.Shape;
	Field.bValid = true;

	switch (Opening.Shape)
	{
	case ECSOpeningShape::Rect:
	{
		const float HH = FMath::Max((Opening.Z1 - Opening.Z0) * 0.5f, UE_KINDA_SMALL_NUMBER);
		Field.RefZ = (Opening.Z0 + Opening.Z1) * 0.5f;
		Field.InvScaleZ = 1.0f / HH;
		break;
	}
	case ECSOpeningShape::Circle:
		Field.RefZ = (Opening.Z0 + Opening.Z1) * 0.5f;
		Field.InvScaleZ = 1.0f / HW;   // 正圆：竖直尺度服从洞宽
		break;
	case ECSOpeningShape::Arch:
	default:
	{
		// 竖直尺度是 **Rise 而不是半宽**（2026-09-04）：两者相等时就是原来的正半圆。
		// 这一行是"拱高与洞宽解耦"的全部实现 —— 判据与材质 HLSL 都只看归一化后的 q，不必改。
		const float Rise = Opening.Rise();
		Field.RefZ = Opening.SpringZ();  // 拱脚
		Field.InvScaleZ = 1.0f / Rise;
		break;
	}
	}
	return Field;
}

/**
 * CPU 侧的裁剪判据。**返回 true = 这一点保留（在洞外）**，与材质 OpacityMask 的语义一致。
 * 材质里那份是它的逐字翻译；两处必须一起改。
 */
inline bool CSHouse_ClipKeeps(const FCSOpeningClipField& Field, const FVector2f& Q)
{
	if (!Field.bValid) return true;
	switch (Field.Shape)
	{
	case ECSOpeningShape::Rect:   return !(FMath::Max(FMath::Abs(Q.X), FMath::Abs(Q.Y)) < 1.0f);
	case ECSOpeningShape::Circle: return !((Q.X * Q.X + Q.Y * Q.Y) < 1.0f);
	case ECSOpeningShape::Arch:
	default:
		return !(Q.Y <= 0.0f ? FMath::Abs(Q.X) < 1.0f : (Q.X * Q.X + Q.Y * Q.Y) < 1.0f);
	}
}

/**
 * 洞在高度 `Z` 处的**半宽**（0 = 这个高度上没有洞）。`CSHouse_ClipKeeps` 的逆解。
 *
 * 砖层（D4 两层之 A）的①删实例要它：按洞的**包围盒**裁行，拱洞会被切成一个矩形缺口 ——
 * 拱顶两侧本该有砖的地方全空了。按剪影裁才是对的，而剪影就是"每个高度上洞有多宽"。
 *
 * ⚠️ **两处边界不能只看 clip 场**：
 * - 拱的 clip 场在**拱脚以下无下界**（`FCSOpeningClipField` 注释里写明是故意的，窗台那一截由
 *   实心盒承担）。只问 clip 场的话，一扇 `Z0 = 90` 的拱窗会一路裁到地面。所以先用洞自己的
 *   `[Z0, Z1]` 卡一道。
 * - 判据用 **`<=` 而不是 `<`**：`|q| == 1` 恰好落在洞缘上（矩形洞的 `Z == Z0` 就是这种情况）。
 *   取闭区间会把洞算大一丝丝 ⇒ 多删一块砖而不是留一块砖在洞里，**朝安全的方向错**。
 */
inline float CSHouse_OpeningHalfWidthAtZ(const FCSWallOpening& Opening, float Z)
{
	if (!Opening.IsValid()) return 0.0f;
	if (Z < Opening.Z0 || Z > Opening.Z1) return 0.0f;

	const FCSOpeningClipField Field = CSHouse_ComputeClipField(Opening);
	if (!Field.bValid || Field.InvHalfWidth <= UE_KINDA_SMALL_NUMBER) return 0.0f;

	const float HalfWidth = 1.0f / Field.InvHalfWidth;
	const float QY = (Z - Field.RefZ) * Field.InvScaleZ;

	switch (Field.Shape)
	{
	case ECSOpeningShape::Rect:
		return FMath::Abs(QY) <= 1.0f ? HalfWidth : 0.0f;
	case ECSOpeningShape::Circle:
		return FMath::Abs(QY) <= 1.0f ? HalfWidth * FMath::Sqrt(FMath::Max(1.0f - QY * QY, 0.0f)) : 0.0f;
	case ECSOpeningShape::Arch:
	default:
		// 拱脚以下是矩形下身（满宽），以上是椭圆。
		if (QY <= 0.0f) return HalfWidth;
		return QY <= 1.0f ? HalfWidth * FMath::Sqrt(FMath::Max(1.0f - QY * QY, 0.0f)) : 0.0f;
	}
}

/**
 * 洞在一条**横带** `[LowZ, HighZ]` 上挡掉的 S 区间（返回 false = 这一带它挡不住）。
 *
 * 取带内**最宽**的那个高度 —— 少取一点就会在洞里留下半块砖。三个采样点足够覆盖三种形状：
 * 两个端点，加上 `RefZ`（圆洞的最宽处在中间，拱与矩形则单调或恒定，端点已经包住）。
 */
inline bool CSHouse_OpeningSpanForBand(const FCSWallOpening& Opening, float LowZ, float HighZ,
	float& OutS0, float& OutS1)
{
	if (!Opening.IsValid()) return false;

	// 带与洞在 Z 上的交集。空 ⇒ 挡不住（高窗不切勒脚、落地门不切压顶就靠这一句）。
	const float Lo = FMath::Max(LowZ, Opening.Z0);
	const float Hi = FMath::Min(HighZ, Opening.Z1);
	if (Hi < Lo) return false;

	const FCSOpeningClipField Field = CSHouse_ComputeClipField(Opening);
	const float Peak = FMath::Clamp(Field.RefZ, Lo, Hi);

	float HalfWidth = 0.0f;
	HalfWidth = FMath::Max(HalfWidth, CSHouse_OpeningHalfWidthAtZ(Opening, Lo));
	HalfWidth = FMath::Max(HalfWidth, CSHouse_OpeningHalfWidthAtZ(Opening, Hi));
	HalfWidth = FMath::Max(HalfWidth, CSHouse_OpeningHalfWidthAtZ(Opening, Peak));
	if (HalfWidth <= UE_KINDA_SMALL_NUMBER) return false;

	OutS0 = Opening.CenterS - HalfWidth;
	OutS1 = Opening.CenterS + HalfWidth;
	return true;
}

/**
 * 一条横带上，洞缘从**带底到带顶**收进去多少（≥ 0）。砖层洞缘四级的③要它。
 *
 * ①按剪影裁行时，切口取的是带内**最宽**的那个高度 —— 于是洞窄下去的那一头（拱圈往上）
 * 会在砖与洞之间留一条缝，最宽处 = 本函数的返回值。③把端头那块砖**剪切**成上宽下窄，
 * 缝就没了：TG 对位物是 `flags & 32` 那条"局部 z 按拱高曲线缩放"，做的是同一件事。
 *
 * ⚠️ 只处理**往上收窄**（拱；矩形恒 0）。往上变宽的洞（正圆的下半）返回 0 ⇒ 保持①的保守
 * 阶梯：剪切是**平行四边形**，一个自由度救不了上下两头都要动的梯形，硬凑会把砖推进洞里。
 */
inline float CSHouse_OpeningTopShear(const FCSWallOpening& Opening, float LowZ, float HighZ)
{
	float S0 = 0.0f, S1 = 0.0f;
	if (!CSHouse_OpeningSpanForBand(Opening, LowZ, HighZ, S0, S1)) return 0.0f;

	const float WidestHalf = (S1 - S0) * 0.5f;
	const float TopHalf = CSHouse_OpeningHalfWidthAtZ(Opening, FMath::Min(HighZ, Opening.Z1));
	return FMath::Max(WidestHalf - TopHalf, 0.0f);
}

/** 洞在墙上实际占掉的沿边区间（面板宽度）：半宽再向两侧各让出半个墩，端盖因此 |q.x| > 1 恒保留。 */
inline void CSHouse_OpeningCell(const FCSWallOpening& Opening, float PierWidth, float& OutMinS, float& OutMaxS)
{
	const float CellHalf = Opening.HalfWidth() + FMath::Max(PierWidth, 1.0f) * 0.5f;
	OutMinS = Opening.CenterS - CellHalf;
	OutMaxS = Opening.CenterS + CellHalf;
}

// -----------------------------------------------------------------------------
// 拱间墩（计划 D6「拱间墩与转角墩」，2026-08-30 的用户实拍裁决）
//
// 实拍（`Docs/TinyGlade/img/TG_continuous_arches.png`）读出的三件事：连续拱之间**没有"墙"这个表面**，
// 起拱线以下只有一根细墩、两侧直接透到草地；墩是**砖**不是残料墙，拱圈的楔石不间断砌到地面；
// 墙面本身是灰泥。
//
// ⚠️ **落点在 2026-08-30「裁决三」之后改了一次**：计划原话是「窄跨度上**不生成**灰泥面板」，
// 而裁决三把「避免所有真几何洞、一律渲染层挖」升格成了架构不变量 —— 不生成面板就是一个真几何洞。
// 所以现在的做法是：**面板照常砌成实心盒，起拱线以下那截由一块矩形裁剪场在像素阶段裁掉**
// （`CSHouse_PierClipField`），与门拱走同一套设施。观感等价，几何实心，烘成 StaticMesh 之后
// 洞仍由材质切出（裁决六）。
//
// ⚠️ **残料跨度按洞剖面之间量，不是按面板格之间量。** 默认参数下拱宽 = 段距 − 墩宽，
// 于是两个**格**恰好首尾相接、格之间那块"实心段"面板宽度为 0 —— 只跳过它等于什么都没做。
// 真正的那片灰泥是两个格各自伸进跨度里的**端盖**（各半个 PierWidth），所以墩侧必须把格收回到
// 洞缘（见 CSHouse_BuildBodySoup）。
// -----------------------------------------------------------------------------

/** `FCSWallOpening::StyleFlags` 的位：我**左**（S 小）/ **右**（S 大）那段残料按墩处理。 */
constexpr uint8 CSHouse_StylePierBefore = 1 << 0;
constexpr uint8 CSHouse_StylePierAfter = 1 << 1;
// `1 << 2` 曾是 `CSHouse_StyleCornerDoor`（跨转角段被切出的那片），**2026-09-06 用户裁决删除**：
// 转角就是一个墩 —— 两道拱只是不在同一条边上 —— 所以它走上面那两位，配对在
// `ResolvePierSpans` 里跨角做，中间的柱子由 `BuildCornerPierBricks` 出。这一位空着不复用，
// 免得旧序列化数据里残留的位被读成别的意思。
/**
 * 这个洞是**一条路被切成多拱**（拱廊）里的一个子拱（2026-09-04 傍晚）。
 *
 * 用户在 TG 里的实测："两道门并排时只有拱没有木门" —— 木门只装在**单拱**上，
 * 一排连续拱是敞开的过道（实拍 `img/tiny-glade-ref-twin-arch-pier.jpg` 里两道拱都空着）。
 * ⚠️ `ResolvePierSpans` 只清墩那两位，这一位能活到门扇那一步。
 */
constexpr uint8 CSHouse_StyleArcade = 1 << 3;
/**
 * 门扇看这一组：任一位为真 ⇒ 敞开过道，不装门扇。
 *
 * 墩位进这里是 2026-09-06 删掉转角位的直接后果：转角一对靠墩位相认，"转角不装门扇"就得从墩位读。
 * 附带的行为变化：**两扇独立的门挨得够近被判成墩时也不装门扇了** —— 与 TG 实测
 * "两道门并排时只有拱没有木门"一致，此前那种情况反而是例外。
 */
constexpr uint8 CSHouse_StyleNoLeafMask = CSHouse_StylePierBefore | CSHouse_StylePierAfter | CSHouse_StyleArcade;

/** 墩样式的两个阈值。双阈迟回与门拱点亮同一条纪律（计划 D6）。 */
struct FCSHousePierStyle
{
	bool bEnabled = true;
	/** 跨度 ≤ 此值 ⇒ 转墩（低阈）。 */
	float MaxWidth = 60.0f;
	/** 已是墩时跨度 ≥ 此值才转回墙（高阈）。 */
	float RestoreWidth = 75.0f;
};

/**
 * 双阈迟回：跨度是**连续量**，单阈会让跨度在阈值附近抖动时样式来回切换（整面墙的灰泥
 * 忽有忽无，比"选错一种样式"难看得多）。与门洞的区间滞回（`DoorKeepWidthRatio`）同型。
 */
inline bool CSHouse_SpanIsPier(const FCSHousePierStyle& Style, float SpanWidth, bool bWasPier)
{
	if (!Style.bEnabled) return false;
	// 高阈被填得比低阈还小时退化成单阈，不至于出现"转墩了就再也转不回来"。
	const float High = FMath::Max(Style.RestoreWidth, Style.MaxWidth);
	return bWasPier ? (SpanWidth < High) : (SpanWidth <= Style.MaxWidth);
}

/**
 * 相邻两个洞之间那段残料**能不能**按墩处理；能的话给出跨度与墩顶高。
 *
 * **只认"两侧都是落地的拱"**，理由是墩顶只有对拱才定义得出来：拱在起拱线以下是等宽矩形，
 * 起拱线正是"两拱的砖脚并排竖直往下走"的那条线，也就是墩该砌到多高。矩形洞没有起拱线
 * （洞是整块矩形），圆洞的对位物是圆心 —— 拿它们当墩顶会在窗台下方挖掉一段本该有的墙。
 * 窗（Z0 > 0）更直接：它下面那截墙是窗台，不是墩。
 *
 * 墩顶取两拱起拱线的**较低者**：取较高者会在两条起拱线之间留一条横向缝（低的那一拱在那里
 * 已经把洞开满了，而墩上的灰泥还没开始砌）。
 */
inline bool CSHouse_PierSpanBetween(const FCSWallOpening& Left, const FCSWallOpening& Right,
	float& OutSpan, float& OutTopZ)
{
	OutSpan = 0.0f;
	OutTopZ = 0.0f;
	if (!Left.IsValid() || !Right.IsValid()) return false;
	if (Left.EdgeIndex != Right.EdgeIndex) return false;
	if (Left.Shape != ECSOpeningShape::Arch || Right.Shape != ECSOpeningShape::Arch) return false;
	if (Left.Z0 > UE_KINDA_SMALL_NUMBER || Right.Z0 > UE_KINDA_SMALL_NUMBER) return false;

	const float Span = Right.S0() - Left.S1();
	if (Span < 0.0f) return false;   // 洞重叠（谓词本该挡住）：不是跨度，别当墩

	OutSpan = Span;
	OutTopZ = FMath::Max(FMath::Min(Left.SpringZ(), Right.SpringZ()), 0.0f);
	return true;
}

/**
 * 墩跨度的裁剪矩形要比跨度本身宽出来的余量 cm。
 *
 * 与普通洞**恰好相反**：普通面板的端盖必须落在洞**外**（`|q.x| > 1`）才不会把墩切掉；
 * 墩跨度这块面板的两片端盖必须落在洞**内**才会被裁掉 —— 否则跨度两端会各立起一片贯穿墙厚的
 * 灰泥薄片，正好卡在墩的位置上，从拱洞里一眼看见。
 */
constexpr float CSHouse_PierCutMargin = 1.0f;

/**
 * 一段矩形裁剪场的**唯一真源**（墩跨度与 D7 接缝共用）。
 *
 * 抽出来不是为了省行数，是因为"端盖要落在裁剪矩形**内**"这条反直觉的约束现在有两个消费者：
 * 各写一份的症状是其中一处忘了加余量，于是那一处的跨度两端各立起一片贯穿墙厚的薄片 ——
 * 而两处都自洽，谁都不报错。余量由调用方给，因为两处的取值理由不同（见各自的常量注释）。
 *
 * 竖向是**闭区间** [BottomZ, TopZ]：矩形判据 `|q.y| < 1` 上下都有界。想要"下边界在地面以下、
 * 只裁上界"就传一个负的 BottomZ（墩传的是 −TopZ，对称矩形）。
 */
inline FCSOpeningClipField CSHouse_RectCutField(float MinS, float MaxS, float BottomZ, float TopZ, float Margin)
{
	FCSWallOpening Cut;
	Cut.Shape = ECSOpeningShape::Rect;
	Cut.CenterS = (MinS + MaxS) * 0.5f;
	Cut.Width = FMath::Max(MaxS - MinS, 0.0f) + 2.0f * FMath::Max(Margin, 0.0f);
	Cut.Z0 = BottomZ;
	Cut.Z1 = FMath::Max(TopZ, BottomZ + 1.0f);   // 退化输入不许把 InvScaleZ 除成 inf
	return CSHouse_ComputeClipField(Cut);
}

/**
 * 墩跨度的裁剪场：把起拱线以下那一整片灰泥裁掉。
 *
 * **不是"不生成面板"**（2026-08-30 裁决三：避免所有真几何洞，一律用顶点色或门洞那套 clip 场在
 * 渲染层挖）—— 面板照常砌成实心盒，只是这块矩形在像素阶段被 discard。代价与门拱逐字相同：
 * 距离场 / Lumen 把 masked 洞当实心墙，墩两侧在 GI 里不透光（用户已知情接受）。
 *
 * 竖向取 [−PierTopZ, +PierTopZ] 的对称矩形：墙空间 Z ≥ 0，所以 `|q.y| < 1` 就等于"在起拱线以下"，
 * 不必给矩形另造一套"下边界在地面以下"的表达。
 */
inline FCSOpeningClipField CSHouse_PierClipField(float SpanMinS, float SpanMaxS, float PierTopZ)
{
	const float Top = FMath::Max(PierTopZ, 1.0f);
	return CSHouse_RectCutField(SpanMinS, SpanMaxS, -Top, Top, CSHouse_PierCutMargin);
}

/**
 * 接缝裁剪矩形的余量 cm。理由与 `CSHouse_PierCutMargin` 同型（端盖必须落在矩形**内**），
 * 但**取值不能共用一个常量**：墩的余量是跨度尺度（几十 cm）上的事，接缝这一刀横跨的是
 * 邻居整个 footprint（几百 cm），将来谁想调其中一个都不该顺手动到另一个。
 */
constexpr float CSHouse_SeamCutMargin = 1.0f;

/**
 * D7 接缝在墙上切的那一段：**纯裁剪，不是洞**。
 *
 * 不进 `CurrentOpenings`、不过 `CSHouse_QueryOpening`、不长门框砖 —— 洞是"这面墙上有个开口"
 * 这件设计意图，接缝裁剪是"这截墙插进邻居房间里了，别画它"这件事实。混进 openings 表的症状
 * 有三：谓词会拿它去挤掉真正的窗、`ResolvePierSpans` 会拿它当拱去配墩、`MakeOpeningPath`
 * 会绕着它铺一圈门框砖（而接缝砖只该立在轮廓交点上）。
 */
struct FCSWallCut
{
	int32 EdgeIndex = 0;
	/** 沿边弧长区间（与 `CSHouse_GetEdge` 的 S 同口径）。 */
	float MinS = 0.0f;
	float MaxS = 0.0f;
	/** 墙空间高度区间。BottomZ ≤ 0 表示"一直裁到墙底"。 */
	float BottomZ = 0.0f;
	float TopZ = 0.0f;

	bool IsValid() const { return MaxS - MinS > UE_KINDA_SMALL_NUMBER && TopZ > BottomZ + UE_KINDA_SMALL_NUMBER; }
};

/** 接缝裁剪场。与墩共用 `CSHouse_RectCutField`，只是余量与竖向区间的来源不同。 */
inline FCSOpeningClipField CSHouse_SeamClipField(float MinS, float MaxS, float BottomZ, float TopZ)
{
	// 下边界 ≤ 0 时改用对称矩形：墙空间 Z ≥ 0，对称矩形在墙内没有下边界，省得在墙底附近
	// 因为 `|q.y|` 的浮点 1.0 而留下一条一像素宽的墙脚（同 `CSHouse_PierClipField` 的理由）。
	const float Bottom = BottomZ > UE_KINDA_SMALL_NUMBER ? BottomZ : -FMath::Max(TopZ, 1.0f);
	return CSHouse_RectCutField(MinS, MaxS, Bottom, TopZ, CSHouse_SeamCutMargin);
}

/**
 * 两个洞是否冲突。
 *
 * 判据是**同边一维 S 区间**：两个洞的**面板格**（CSHouse_OpeningCell —— 半宽再向两侧各让半个墩）
 * 按 Clearance 膨胀后是否相交，Z 一律不参与。
 *
 * **用户裁决 2026-08-30（C1 选甲）：永久放弃"门上开窗"。** 此前判的是 (S, Z) 二维矩形，允许高窗
 * 压在低门正上方 —— 但 CSHouse_BuildBodySoup 铺墙板是沿 S 的单游标扫掠、每块面板只带一个 clip
 * 场，S 上重叠的第二个洞根本砌不出来（轻则被前一块无 clip 的面板咬掉半边，重则面板装不下而
 * 静默 continue）。谓词与几何必须同维，D8「谓词说能放就一定砌得出」才成立。
 *
 * 比的是**格**而不是洞本身：格才是扫掠真正消费的那个区间 —— 两格互不相交 ⇒ 游标永远顶不到洞。
 * 墩宽因此只有 CSHouse_OpeningCell 一处真源，将来按洞型分流墩宽时谓词自动跟随。
 */
inline bool CSHouse_OpeningsOverlap(const FCSWallOpening& A, const FCSWallOpening& B, float PierWidth, float Clearance)
{
	if (A.EdgeIndex != B.EdgeIndex) return false;

	float AMin = 0, AMax = 0, BMin = 0, BMax = 0;
	CSHouse_OpeningCell(A, PierWidth, AMin, AMax);
	CSHouse_OpeningCell(B, PierWidth, BMin, BMax);
	return !(AMin - Clearance > BMax || BMin > AMax + Clearance);
}

// -----------------------------------------------------------------------------
// 墙面框架与可放置性谓词（D8「谓词是唯一真源」的执行面）
// -----------------------------------------------------------------------------

/**
 * 一面墙的局部框架。U×In = +Z（已验）。
 *
 * **2026-09-13 起是斜接口径**（footprint 折线化 3a-2）：`Start` 就是外角点、`Len` 是外皮线段
 * 全长，两端的转角方块沿角平分线一分为二、两条边各拿一半 —— 让出来的那半由 `InsetStart` /
 * `InsetEnd` 描述。在这之前是直角对接：偶数边吃下整个转角方块、奇数边两端各缩 `T`，它只在
 * 偶数条边且全直角的多边形上成立（旧口径的存量锚点见 `FCSWallAnchor::SConvention`）。
 */
struct FCSHouseEdgeFrame
{
	FVector2D Start = FVector2D::ZeroVector;   // 外皮线段起点 = 外角点
	FVector2D U = FVector2D::ZeroVector;       // 沿墙的单位向量
	FVector2D In = FVector2D::ZeroVector;      // 指向房内的单位法线（逆时针 ⇒ Perp(U)）
	float Len = 0;                              // 外皮线段全长
	float InsetStart = 0;                       // 起点角：内皮沿 +U 让出的距离；直角 = T，凹角 < 0
	float InsetEnd = 0;                         // 终点角：内皮沿 −U 让出的距离

	/**
	 * 墙体内深度 `Depth`（0 = 外皮，`T` = 内皮）处本边实体占的 S 区间。
	 *
	 * 斜接面是一条从外角点斜到内角点的直线，所以两端被切掉的量随深度**线性**增长：外皮上
	 * 一点都不切（`[0, Len]`），内皮上各切一个 `Inset`。凹角的 `Inset` 为负 ⇒ 区间反而向外
	 * 伸出去，补上凹角内侧那块本来没有墙的三角。
	 */
	void SpanAtDepth(float Depth, float T, float& OutS0, float& OutS1) const
	{
		const float K = T > UE_KINDA_SMALL_NUMBER ? FMath::Clamp(Depth / T, 0.0f, 1.0f) : 0.0f;
		OutS0 = InsetStart * K;
		OutS1 = Len - InsetEnd * K;
	}
};

/** `FCSHouseFootprint::FromShape` 对输入形状做了什么。诊断用（告警 / 单测），**结果本身不看它**。 */
enum class ECSFootprintShapeFix : uint8
{
	/** 形状少于 3 个点：矩形（空形状 = 矩形，旧存档与新放的房子都是这样）。 */
	Rect,
	/** 输入本身严格凸：原样使用。顺时针的翻成逆时针、首顶点不动 —— 一个顶点都没丢，不算改动。 */
	Kept,
	/** 凹角 / 共线点 / 重复点 / 乱序（自交）：按**凸包**使用，被包进去的点丢弃（2026-09-14 用户裁决）。 */
	Hulled,
	/** 包围盒退化，或凸包不足 3 个顶点 / 面积约为零（点全共线）：退回矩形。 */
	Degenerate,
};

/**
 * 房屋 footprint：**闭合折线**（2026-09-12 用户裁决，取代原来的 `FVector2D` 矩形）。
 *
 * 顶点按**逆时针**排列；边 `i` 从 `Verts[i]` 走到 `Verts[(i + 1) % N]`，`0` 号边的起点就是
 * `Verts[0]`。矩形由 `MakeRect` 造出来时与老口径同序（0 南 1 东 2 北 3 西），所以边号的含义
 * 一个字都没变。
 *
 * ⚠️ **逆时针不是风格选择，是 `In` 的来源**：逆时针多边形的内部恒在行进方向的**左**手侧，
 * 于是 `In = Perp(U) = (-U.y, U.x)` 对任意边都成立 —— 这一条取代了原来四条边各自硬编码的
 * `In`，也是折线化之所以能不碰下游那一整类纯函数（clip 场、谓词、门段、包边、砖层、藤条、
 * 摆件锚点、门扇）的原因：它们只认 `(Start, U, In, Len)`，不认边数。
 *
 * 存盘的是**形状 + 尺寸**两个量（`ACSHouseActor::FootprintShape` / `FootprintSize`），本类型由
 * `FromShape` 现组 —— 形状只管"长什么样"、尺寸只管包围盒多大，所以写 `FootprintSize` 的每一条
 * 旧路径（详情面板、蓝图、脚本、拖边）在异形房子上照样成立，旧存档的空形状就是矩形、不需要迁移。
 * 消费者一律只认本类型（3b–3e 改完），不认那两个存盘量。
 */
USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSHouseFootprint
{
	GENERATED_BODY()

	/** 逆时针闭合折线的顶点（局部空间，房心为原点）。末顶点与首顶点自动相连，不要重复首点。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House")
	TArray<FVector2D> Verts;

	int32 NumEdges() const { return Verts.Num(); }
	bool IsValidFootprint() const { return Verts.Num() >= 3; }

	/** 边 `i` 的两个端点（外皮线上）。越界或退化时返回 false，出参不动。 */
	bool GetEdgeVerts(int32 EdgeIndex, FVector2D& OutA, FVector2D& OutB) const
	{
		const int32 N = Verts.Num();
		if (N < 3 || EdgeIndex < 0 || EdgeIndex >= N) return false;
		OutA = Verts[EdgeIndex];
		OutB = Verts[(EdgeIndex + 1) % N];
		return true;
	}

	/** 轴对齐包围盒（局部空间）。空折线返回无效盒。 */
	FBox2D GetBounds() const
	{
		FBox2D Box(ForceInit);
		for (const FVector2D& V : Verts) Box += V;
		return Box;
	}

	/**
	 * 以**局部原点**为心、能盖住整条折线的轴对齐方盒的**边长**：`2 · max(|x|, |y|)`。
	 *
	 * 各家实例组件的交接包围盒原来写 `max(FootprintSize.X, FootprintSize.Y) · 0.6` —— 那是「居中
	 * 矩形的边长乘 1.2 倍余量再取半」。对 `MakeRect` 造的居中矩形，这个函数**逐位**等于
	 * `max(X, Y)`（`2 · (X · 0.5)` 是精确的），所以换过去之后矩形上的盒子一厘米都不变；对不居中
	 * 或非矩形的折线它仍是保守的（取的是离原点最远的那一维，不是包围盒自己的边长）。
	 */
	double GetCenteredSpan() const
	{
		double Half = 0.0;
		for (const FVector2D& V : Verts) Half = FMath::Max(Half, FMath::Max(FMath::Abs(V.X), FMath::Abs(V.Y)));
		return Half * 2.0;
	}

	/** 外皮周长（逐边长度之和）。 */
	double GetPerimeter() const
	{
		double Sum = 0.0;
		const int32 N = Verts.Num();
		for (int32 i = 0; i < N && N >= 2; ++i) Sum += FVector2D::Distance(Verts[i], Verts[(i + 1) % N]);
		return Sum;
	}

	/**
	 * 点在折线围成的区域内，或离任一条边不超过 `Tolerance`（边界算在内）。
	 *
	 * 奇偶规则判内部；边界单独按距离判 —— 纯奇偶规则在边上的点结果取决于舍入，而落座采样、
	 * 支撑点这些调用方恰恰会把采样点放在边上（矩形的采样网格含四条边），边界必须稳定地算在内。
	 */
	bool ContainsPoint(const FVector2D& P, double Tolerance = 0.01) const
	{
		const int32 N = Verts.Num();
		if (N < 3) return false;
		bool bInside = false;
		for (int32 i = 0, j = N - 1; i < N; j = i++)
		{
			const FVector2D& A = Verts[i];
			const FVector2D& B = Verts[j];
			const FVector2D AB = B - A;
			const double LenSq = AB.SizeSquared();
			const double T = LenSq > UE_DOUBLE_SMALL_NUMBER ? FMath::Clamp(FVector2D::DotProduct(P - A, AB) / LenSq, 0.0, 1.0) : 0.0;
			if (FVector2D::DistSquared(P, A + AB * T) <= Tolerance * Tolerance) return true;
			if (((A.Y > P.Y) != (B.Y > P.Y)) && (P.X < (B.X - A.X) * (P.Y - A.Y) / (B.Y - A.Y) + A.X)) bInside = !bInside;
		}
		return bInside;
	}

	/** 有向面积：逆时针为正。 */
	double GetSignedArea() const
	{
		double Twice = 0.0;
		const int32 N = Verts.Num();
		for (int32 i = 0; i < N; ++i)
		{
			const FVector2D& A = Verts[i];
			const FVector2D& B = Verts[(i + 1) % N];
			Twice += A.X * B.Y - A.Y * B.X;
		}
		return Twice * 0.5;
	}

	/**
	 * 严格凸且逆时针：每个角都是左转（叉积 > `MinTurnSin`）。共线角（叉积≈0）不算凸 ——
	 * 那种顶点让一条墙被无谓地切成两段，屋面直骨架还会在那里出一条零长的脊。
	 */
	bool IsStrictlyConvexCCW(double MinTurnSin = 1.0e-4) const
	{
		const int32 N = Verts.Num();
		if (N < 3) return false;
		for (int32 i = 0; i < N; ++i)
		{
			const FVector2D UIn = (Verts[i] - Verts[(i + N - 1) % N]).GetSafeNormal();
			const FVector2D UOut = (Verts[(i + 1) % N] - Verts[i]).GetSafeNormal();
			if (UIn.X * UOut.Y - UIn.Y * UOut.X <= MinTurnSin) return false;
		}
		return true;
	}

	/**
	 * 形状是否一致（顶点数相同且逐顶点在容差内）。参照系比较用它 —— 顶点数变了就是变了，
	 * 不比坐标：加一个顶点而其余不动，墙的段划分已经换了一套。
	 */
	bool EqualsApprox(const FCSHouseFootprint& Other, double Tolerance) const
	{
		if (Verts.Num() != Other.Verts.Num()) return false;
		for (int32 i = 0; i < Verts.Num(); ++i)
		{
			if (!Verts[i].Equals(Other.Verts[i], Tolerance)) return false;
		}
		return true;
	}

	/**
	 * 轴对齐矩形折线，与 `CSHouse_GetEdge` 的老口径逐位同序：
	 * 0 号边从 (−HX, −HY) 走到 (+HX, −HY)（南，+X 向），其余按逆时针。
	 */
	static FCSHouseFootprint MakeRect(const FVector2D& Size)
	{
		const double HX = Size.X * 0.5, HY = Size.Y * 0.5;
		FCSHouseFootprint FP;
		FP.Verts.Reserve(4);
		FP.Verts.Add({ -HX, -HY });
		FP.Verts.Add({ HX, -HY });
		FP.Verts.Add({ HX, HY });
		FP.Verts.Add({ -HX, HY });
		return FP;
	}

	/**
	 * 形状哈希的量化输入：顶点数 + 逐顶点量化到 **0.5 cm**。房屋各家 desc 哈希、快扫跟踪哈希都拼它。
	 *
	 * 顶点数必须进去：加一个顶点而位置不动的折线，量化后的坐标序列可能与原来的前缀相同。
	 *
	 * ⚠️ **量子是 0.5 不是 1**（2026-09-14）：矩形的顶点是 `±Size/2`，半边长量化到 1 cm 等于把尺寸
	 * 量化粗到 **2 cm** —— 折线化之前各家哈希写的是 `CSHouse_Q(FootprintSize.X, 1)`，粒度是 1 cm。
	 * 顶点量到 0.5 cm 恰好还原那个粒度（`House.FootprintHashGranularity` 钉着）。
	 */
	void AppendQuantizedHash(TArray<int32>& Out) const
	{
		Out.Add(NumEdges());
		for (const FVector2D& V : Verts)
		{
			Out.Append({ int32(FMath::RoundToInt(V.X / 0.5)), int32(FMath::RoundToInt(V.Y / 0.5)) });
		}
	}

	/**
	 * **形状 × 尺寸 → footprint**（`ACSHouseActor::FootprintShape` × `FootprintSize`）。
	 *
	 * 形状是任意坐标系下的一组顶点：这里把它的**包围盒**拉伸到 `Size`、居中到原点。于是形状只描述
	 * "长什么样"，大小永远由 `Size` 说了算 —— 拖尺寸、蓝图、脚本里写 `FootprintSize` 的每一条旧路径
	 * 在异形房子上照样成立。
	 *
	 * - **严格凸**的输入原样使用：顺时针的翻成逆时针、首顶点不动（`Kept`）。
	 * - **不是严格凸的**（凹角、共线点、重复点、乱序 / 自交）按**凸包**使用（`Hulled`，2026-09-14 用户裁决
	 *   "让折线取最后的凸包就行了"；凹 footprint 不做）。凸包逆时针、去掉共线点，从**原下标最小**的那个
	 *   凸包顶点起算 —— 输入已凸时它就是 `Verts[0]`，所以凸包与原样使用两条路的边号口径一致。
	 *   ⚠️ **只在使用时取，调用方别回写 `FootprintShape`**：用户在细节面板里逐个加顶点，中间态常常是凹的，
	 *   回写会把刚加的点当场删掉，面板里就根本加不了点。
	 * - 退回矩形：少于 3 个点（空 = 矩形，旧存档就是这样，`Rect`）；包围盒退化、或凸包不足 3 个顶点 /
	 *   面积约为零（点全共线，`Degenerate`）。退化兜底取矩形而不是"最后一个合法形状"：本函数是纯函数，
	 *   没有历史可退；矩形也是折线化之前这条路唯一的兜底口径。
	 */
	static FCSHouseFootprint FromShape(TConstArrayView<FVector2D> Shape, const FVector2D& Size,
		ECSFootprintShapeFix* OutFix = nullptr)
	{
		auto Report = [OutFix](ECSFootprintShapeFix Fix) { if (OutFix) *OutFix = Fix; };
		const int32 N = Shape.Num();
		if (N < 3)
		{
			Report(ECSFootprintShapeFix::Rect);
			return MakeRect(Size);
		}

		FBox2D Box(ForceInit);
		for (const FVector2D& V : Shape) Box += V;
		const FVector2D Extent = Box.GetSize();
		if (Extent.X <= UE_KINDA_SMALL_NUMBER || Extent.Y <= UE_KINDA_SMALL_NUMBER)
		{
			Report(ECSFootprintShapeFix::Degenerate);
			return MakeRect(Size);
		}
		const FVector2D Centre = Box.GetCenter();

		// 正缩放是保凸、保共线、保绕向的仿射变换，所以"先拉伸再判凸 / 取凸包"与"先取再拉伸"是同一个结果；
		// 在拉伸后的厘米空间里判，容差才有物理意义。
		FCSHouseFootprint FP;
		FP.Verts.Reserve(N);
		for (const FVector2D& V : Shape)
		{
			FP.Verts.Add(FVector2D((V.X - Centre.X) / Extent.X * Size.X, (V.Y - Centre.Y) / Extent.Y * Size.Y));
		}

		// 快路：已经严格凸（任一绕向）。与折线化 3f 以来的输出**逐位相同** —— 已有的异形存档一个 bit 都不许变。
		{
			FCSHouseFootprint Oriented = FP;
			if (Oriented.GetSignedArea() < 0.0)
			{
				for (int32 Index = 0; Index < N; ++Index) Oriented.Verts[Index] = FP.Verts[(N - Index) % N];
			}
			if (Oriented.IsStrictlyConvexCCW())
			{
				Report(ECSFootprintShapeFix::Kept);
				return Oriented;
			}
		}

		FCSHouseFootprint Hull;
		Hull.Verts = ConvexHullCCW(FP.Verts);
		if (Hull.Verts.Num() < 3 || Hull.GetSignedArea() <= 1.0e-6 * FMath::Max(Size.X * Size.Y, 1.0) || !Hull.IsStrictlyConvexCCW())
		{
			Report(ECSFootprintShapeFix::Degenerate);
			return MakeRect(Size);
		}
		Report(ECSFootprintShapeFix::Hulled);
		return Hull;
	}

	/**
	 * 点集的**严格凸包**：逆时针、不含共线点与重复点（Andrew 单调链）；从**原下标最小**的凸包顶点起算。
	 *
	 * 共线判据与 `IsStrictlyConvexCCW` 同一个口径（转角正弦 ≤ `MinTurnSin` 不算凸角）：单调链先按叉积
	 * 严格剔一遍，再把近共线的顶点逐个摘掉，保证结果一定过得了 `IsStrictlyConvexCCW`。
	 * 不足 3 个点时原样返回剩下的（调用方按退化处理）。
	 */
	static TArray<FVector2D> ConvexHullCCW(TConstArrayView<FVector2D> Points, double MinTurnSin = 1.0e-4)
	{
		const int32 N = Points.Num();
		TArray<int32> Order;
		Order.Reserve(N);
		for (int32 Index = 0; Index < N; ++Index) Order.Add(Index);
		// 按 (x, y, 原下标) 排：重复点里下标小的排前面，单调链会留下它。
		Order.Sort([&Points](int32 A, int32 B)
		{
			if (Points[A].X != Points[B].X) return Points[A].X < Points[B].X;
			if (Points[A].Y != Points[B].Y) return Points[A].Y < Points[B].Y;
			return A < B;
		});

		auto Cross = [&Points](int32 O, int32 A, int32 B)
		{
			return (Points[A].X - Points[O].X) * (Points[B].Y - Points[O].Y) - (Points[A].Y - Points[O].Y) * (Points[B].X - Points[O].X);
		};

		TArray<int32> Chain;
		Chain.Reserve(2 * N);
		for (int32 K = 0; K < N; ++K)   // 下链
		{
			while (Chain.Num() >= 2 && Cross(Chain[Chain.Num() - 2], Chain.Last(), Order[K]) <= 0.0) Chain.Pop(EAllowShrinking::No);
			Chain.Add(Order[K]);
		}
		const int32 LowerSize = Chain.Num() + 1;
		for (int32 K = N - 2; K >= 0; --K)   // 上链
		{
			while (Chain.Num() >= LowerSize && Cross(Chain[Chain.Num() - 2], Chain.Last(), Order[K]) <= 0.0) Chain.Pop(EAllowShrinking::No);
			Chain.Add(Order[K]);
		}
		if (Chain.Num() > 1) Chain.Pop(EAllowShrinking::No);   // 末点 = 起点

		// 近共线的顶点逐个摘掉（每摘一个，它两侧的转角都变了，所以要重扫）。
		auto TurnSin = [&Points](int32 Prev, int32 Here, int32 Next)
		{
			const FVector2D UIn = (Points[Here] - Points[Prev]).GetSafeNormal();
			const FVector2D UOut = (Points[Next] - Points[Here]).GetSafeNormal();
			return UIn.X * UOut.Y - UIn.Y * UOut.X;
		};
		for (bool bRemoved = true; bRemoved && Chain.Num() >= 3; )
		{
			bRemoved = false;
			for (int32 K = 0; K < Chain.Num(); ++K)
			{
				const int32 M = Chain.Num();
				if (TurnSin(Chain[(K + M - 1) % M], Chain[K], Chain[(K + 1) % M]) > MinTurnSin) continue;
				Chain.RemoveAt(K);
				bRemoved = true;
				break;
			}
		}

		// 从原下标最小的顶点起算（保持逆时针）。
		int32 Start = 0;
		for (int32 K = 1; K < Chain.Num(); ++K)
		{
			if (Chain[K] < Chain[Start]) Start = K;
		}
		TArray<FVector2D> Out;
		Out.Reserve(Chain.Num());
		for (int32 K = 0; K < Chain.Num(); ++K) Out.Add(Points[Chain[(Start + K) % Chain.Num()]]);
		return Out;
	}
};

/**
 * 转角斜接的让出量：内皮沿 `UOut` 方向缩进（或伸出）多少，才能与上一条边的内皮在角平分线上
 * 相交。`UIn` 是走进这个角的方向、`UOut` 是走出去的方向，两者都是单位向量。
 *
 * 推导：两条内皮线各自沿 `In` 平移 `T`，交点投到出边上的弧长就是 `T · tan(转角/2)`，转角是
 * 从 `UIn` 转到 `UOut` 的有向角（逆时针多边形的凸角是左转、为正）。用半角公式
 * `tan(φ/2) = sin φ / (1 + cos φ)` 直接拿叉积 / 点积算，不走三角函数：
 *
 *   直角（左转 90°）：sin = 1、cos = 0 ⇒ 让出 **恰好** T（这是矩形逐位可测的那个值）
 *   直线（不转）：    sin = 0          ⇒ 0
 *   凹角（右转）：    sin < 0          ⇒ 负数，内皮向外伸出
 *
 * 近 180° 的折返（尖刺）分母趋零、让出量发散 —— 那种折线本该在编辑入口被拒绝，这里只防除零。
 */
inline float CSHouse_CornerInset(const FVector2D& UIn, const FVector2D& UOut, float T)
{
	const double SinTurn = UIn.X * UOut.Y - UIn.Y * UOut.X;
	const double CosTurn = FVector2D::DotProduct(UIn, UOut);
	const double Denom = 1.0 + CosTurn;
	if (Denom <= 1.0e-6) return float((SinTurn >= 0.0 ? 1.0e3 : -1.0e3) * double(T));
	return float(double(T) * SinTurn / Denom);
}

/**
 * 一条边的斜接框架：外皮线段 `A → B`，`Prev` 是 `A` 之前的顶点、`Next` 是 `B` 之后的顶点。
 *
 * 所有 `CSHouse_GetEdge` 重载共用这一份 —— 矩形那条也走它，所以「矩形是折线的特例」不是
 * 说法而是代码事实。`In` 由逆时针性质导出（内部在行进方向的左手侧）。
 */
inline FCSHouseEdgeFrame CSHouse_MakeEdgeFrame(
	const FVector2D& Prev, const FVector2D& A, const FVector2D& B, const FVector2D& Next, float T)
{
	FCSHouseEdgeFrame F;
	const FVector2D D = B - A;
	const double Len0 = D.Size();
	if (Len0 <= UE_DOUBLE_SMALL_NUMBER) return F;   // 退化边：零长框架，调用方按 Len <= 0 跳过

	F.U = D / Len0;
	F.In = FVector2D(-F.U.Y, F.U.X);
	F.Start = A;
	F.Len = float(Len0);
	F.InsetStart = CSHouse_CornerInset((A - Prev).GetSafeNormal(), F.U, T);
	F.InsetEnd = CSHouse_CornerInset(F.U, (Next - B).GetSafeNormal(), T);
	return F;
}

/**
 * 周界某一面墙（斜接口径）。边号即顶点号：边 i 从 `Verts[i]` 走到 `Verts[i+1]`。
 *
 * **住在头文件里而不是 CSHouseActor.cpp 的匿名命名空间里**：墙板、门框砖、藤蔓、摆件、
 * 以及下面那条谓词全都要问"这面墙在哪、有多长"，各抄一份的症状是"藤悬在离墙半个墙厚的
 * 空中"这类只在改过 WallThickness 之后才显形的错位。单测也要拿它算墙长。
 */
inline FCSHouseEdgeFrame CSHouse_GetEdge(int32 EdgeIndex, const FCSHouseFootprint& Footprint, float T)
{
	FVector2D A, B;
	if (!Footprint.GetEdgeVerts(EdgeIndex, A, B)) return FCSHouseEdgeFrame();
	const int32 N = Footprint.NumEdges();
	const FVector2D& Prev = Footprint.Verts[(EdgeIndex + N - 1) % N];
	const FVector2D& Next = Footprint.Verts[(EdgeIndex + 2) % N];
	return CSHouse_MakeEdgeFrame(Prev, A, B, Next, T);
}

/**
 * 矩形口径的便捷重载：0 南(+X 向) 1 东(+Y 向) 2 北(-X 向) 3 西(-Y 向)。
 *
 * 与折线版**逐位等价**（`House.FootprintMitreCorners` 钉住）：这里不重排顶点、不走
 * `TArray`，只是把 `MakeRect` 的四个顶点就地算出来喂同一个核，免掉热路径上的堆分配
 * （门框砖 / 藤条 / 摆件都在逐实例的循环里问边框架）。
 */
inline FCSHouseEdgeFrame CSHouse_GetEdge(int32 EdgeIndex, const FVector2D& Footprint, float T)
{
	const double HX = Footprint.X * 0.5, HY = Footprint.Y * 0.5;
	const FVector2D C[4] = { { -HX, -HY }, { HX, -HY }, { HX, HY }, { -HX, HY } };
	const int32 E = EdgeIndex & 3;
	return CSHouse_MakeEdgeFrame(C[(E + 3) & 3], C[E], C[(E + 1) & 3], C[(E + 2) & 3], T);
}

/**
 * 周界上的一个角。角 k 夹在「k 号边的远端」与「k+1 号边的近端」之间，位置 = `Verts[k+1]`。
 *
 * 角石（`CSHouseQuoin::BuildQuoins`）、转角墩（`ACSHouseActor::BuildCornerPierBricks`）、
 * 跨角配墩（`ResolvePierSpans`）问的都是它 —— 角点与角平分线只能有一个真源。各算各的症状是
 * 「角石包的角与墩立的角在非直角上错开」，而矩形上几种写法逐位相同，单测一条都不会红。
 */
struct FCSHouseCornerFrame
{
	/** 外皮角点（局部空间）。 */
	FVector2D Point = FVector2D::ZeroVector;
	/** 角平分线，指向房外（单位）= 两面墙外法线之和的方向。凹角同样成立（指向凹口）。 */
	FVector2D Outward = FVector2D(1.0, 0.0);
	/** 转角（远边 U 转到近边 U 的有向角）的正弦：> 0 凸角（逆时针左转），< 0 凹角，≈ 0 共线。 */
	double SinTurn = 0.0;
	/** `cos(转角 / 2)` = 平分线与任一面墙外法线的夹角余弦。直角 = 1/√2，共线 = 1，折返 → 0。 */
	double HalfTurnCos = 1.0;

	bool IsConvex(double MinSin = 1.0e-4) const { return SinTurn > MinSin; }

	/**
	 * 两面墙各向内偏 `Depth` 之后的交点 = 沿平分线内缩 `Depth / cos(转角/2)`，凸角凹角同一个式子。
	 * `Depth = T/2` 就是墙厚中线的角点（门樘砖、拱廊墩、转角墩都立在那条线上）；直角上是 `T/√2`。
	 */
	FVector2D PointAtDepth(double Depth) const
	{
		return Point - Outward * (Depth / FMath::Max(HalfTurnCos, 1.0e-3));
	}
};

/** 角 `CornerIndex` 的框架。越界或退化折线返回默认值（`HalfTurnCos = 1`、`SinTurn = 0`，不算凸角）。 */
inline FCSHouseCornerFrame CSHouse_GetCorner(int32 CornerIndex, const FCSHouseFootprint& Footprint)
{
	FCSHouseCornerFrame C;
	const int32 N = Footprint.NumEdges();
	if (N < 3 || CornerIndex < 0 || CornerIndex >= N) return C;
	const FVector2D& A = Footprint.Verts[CornerIndex];
	const FVector2D& P = Footprint.Verts[(CornerIndex + 1) % N];
	const FVector2D& B = Footprint.Verts[(CornerIndex + 2) % N];
	const FVector2D UFar = (P - A).GetSafeNormal();
	const FVector2D UNear = (B - P).GetSafeNormal();
	// 外法线 = −In = (U.y, −U.x)（逆时针折线的内部在行进方向左手侧）。
	const FVector2D NFar(UFar.Y, -UFar.X);
	const FVector2D NNear(UNear.Y, -UNear.X);
	C.Point = P;
	C.SinTurn = UFar.X * UNear.Y - UFar.Y * UNear.X;
	const FVector2D Sum = NFar + NNear;
	const double SumLen = Sum.Size();
	if (SumLen <= 1.0e-6)
	{
		// 折返（尖刺）：两面外法线相消，没有平分线。那种折线本该在编辑入口被拒绝，这里只防除零。
		C.Outward = NFar;
		C.HalfTurnCos = 0.0;
		return C;
	}
	C.Outward = Sum / SumLen;
	C.HalfTurnCos = SumLen * 0.5;   // |NFar + NNear| = 2·cos(转角/2)
	return C;
}

/**
 * 射线（或一个点）落在哪面外墙上。**房子局部空间** —— 世界变换由调用方先解掉。
 *
 * D8 特征标记的宿主解析用它：`S` / `Z` 恰好就是 `FCSHouseWindow` 要的 `CenterS` / 窗台高，
 * 所以标记从"我在世界的哪里"到"我是这面墙上的哪一扇窗"是一步换算，不需要第二套坐标。
 */
struct FCSWallHit
{
	bool  bHit = false;
	/** 边号，与 `CSHouse_GetEdge` 同号（矩形 0..3）。 */
	int32 EdgeIndex = -1;
	/** 沿边弧长，与 `CSHouse_GetEdge` 的 S 同口径。 */
	float S = 0.0f;
	/** 墙空间高度，房底为 0。 */
	float Z = 0.0f;
	/** 射线版是沿射线的距离；就近版是到墙面的垂距。 */
	float Distance = 0.0f;
};

/**
 * 射线 × 每一面外墙（折线有几条边就几面），取最近的一次命中（**解析求交，不是引擎 trace**）。
 *
 * ⚠️ **必须解析求交**：房子的 gpumesh 全线 `NoCollision`（`CSGpuMeshComponent.cpp`），
 * `LineTraceSingle` 永远打不到 —— 计划 D8 明写这条，照 trace 写会得到"窗户一放就没"，
 * 而且不会有任何断言报红（标记自毁是"合法"行为）。
 *
 * 只判**外表面**（`dot(Dir, 外法线) < 0`）：从墙里往外打不算命中，否则站在房内乱挥鼠标
 * 会把窗贴到背面那堵墙上。
 */
inline FCSWallHit CSHouse_RayHitWall(
	const FVector& LocalOrigin, const FVector& LocalDir,
	const FCSHouseFootprint& Footprint, float T, float WallHeight, float MaxDistance)
{
	FCSWallHit Best;
	Best.Distance = MaxDistance;

	const FVector2D Dir2(LocalDir.X, LocalDir.Y);
	for (int32 Edge = 0; Edge < Footprint.NumEdges(); ++Edge)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Footprint, T);
		if (F.Len <= UE_KINDA_SMALL_NUMBER) continue;

		const FVector2D N(-F.In.X, -F.In.Y);          // 外法线
		const double Denom = FVector2D::DotProduct(Dir2, N);
		if (Denom > -UE_KINDA_SMALL_NUMBER) continue;  // 平行、或从背面打过来

		const double Dist = FVector2D::DotProduct(F.Start - FVector2D(LocalOrigin.X, LocalOrigin.Y), N) / Denom;
		if (Dist <= 0.0 || Dist >= double(Best.Distance)) continue;

		const FVector P = LocalOrigin + LocalDir * Dist;
		const double S = FVector2D::DotProduct(FVector2D(P.X, P.Y) - F.Start, F.U);
		if (S < 0.0 || S > double(F.Len)) continue;
		if (P.Z < 0.0 || P.Z > double(WallHeight)) continue;

		Best.bHit = true;
		Best.EdgeIndex = Edge;
		Best.S = float(S);
		Best.Z = float(P.Z);
		Best.Distance = float(Dist);
	}
	return Best;
}

/**
 * 就近找墙：把点投到每一面外墙上取最近的一面（计划 D8 的 `csh.WindowSnapDist` 退路）。
 *
 * 射线版是"朝向摆正了"的通路；这条兜的是"贴着墙但朝向没摆正"——两条都空才判无宿主。
 * S / Z 都夹到墙面内，所以它返回的永远是墙上一个**合法**位置，谓词那关照旧另判。
 */
inline FCSWallHit CSHouse_NearestWall(
	const FVector& LocalPoint, const FCSHouseFootprint& Footprint, float T, float WallHeight, float MaxDistance)
{
	FCSWallHit Best;
	Best.Distance = MaxDistance;

	const FVector2D P2(LocalPoint.X, LocalPoint.Y);
	for (int32 Edge = 0; Edge < Footprint.NumEdges(); ++Edge)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Footprint, T);
		if (F.Len <= UE_KINDA_SMALL_NUMBER) continue;

		const FVector2D N(-F.In.X, -F.In.Y);
		const double S = FMath::Clamp(FVector2D::DotProduct(P2 - F.Start, F.U), 0.0, double(F.Len));
		const double Z = FMath::Clamp(double(LocalPoint.Z), 0.0, double(WallHeight));
		// 到墙面的距离取三维：面内偏移已经被夹住，剩下的就是法向 + 被夹掉的那两段。
		const FVector2D OnFace = F.Start + F.U * S;
		const FVector Delta(P2.X - OnFace.X, P2.Y - OnFace.Y, LocalPoint.Z - Z);
		// 背面（法向为负）不算 —— 与射线版同一条纪律。
		if (FVector2D::DotProduct(P2 - F.Start, N) < 0.0) continue;

		const double Dist = Delta.Size();
		if (Dist >= double(Best.Distance)) continue;

		Best.bHit = true;
		Best.EdgeIndex = Edge;
		Best.S = float(S);
		Best.Z = float(Z);
		Best.Distance = float(Dist);
	}
	return Best;
}

/**
 * 附属物挂在墙上的**锚点**（D8，用户裁决 2026-09-06「位置的存储方式与 TG 一致」）。
 *
 * ## 为什么是「离某个角多远」而不是绝对弧长
 *
 * `FCSHouseWindow::CenterS` 的原点是 `CSHouse_GetEdge` 的 `F.Start`，而 `ACSHouseActor::PushEdge(e)`
 * 会把 e 那一侧的**两个角**各挪 `Δ` —— 它们恰好是第 `e` 与第 `(e+1)%4` 条边的 S 原点。后果实测过
 * （2026-09-05 四条边逐一推算）：**推第 e 条边，第 e 与第 e+1 条边上的窗错位 Δ，另外两条完全正确**，
 * 其中 e+1 那面墙**根本没动**、窗却沿着它滑了 Δ —— 最难解释的一种。
 *
 * 锚到「最近的那个角」之后：被推那一侧的窗随墙走、另一侧的窗原地不动，行为可预期。
 * 这与 TG `wall_attachment::WallCornerAttachment::{from_rectangle_corner_id, to_rectangle_corner_id}`
 * 用**拓扑角编号**而不是坐标是同一个思路 `[PDB]`。
 *
 * ⚠️ **锚点是唯一权威，actor 变换是派生量**（TG `CachedDecoratorTransforms` + `move_decorators_following_anchors`）。
 * 序列化这份结构，不要序列化世界变换 —— 反过来做就是 2026-09-05 分析出的那条"重开关卡窗又挪一次位"。
 */
USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSWallAnchor
{
	GENERATED_BODY()

	/** 挂在哪条边，与 `CSHouse_GetEdge` 同号（矩形 0..3）。`-1` = 尚未锚定。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Wall Anchor")
	int32 EdgeIndex = -1;

	/** `false` = 从边的**起点角**量起，`true` = 从**终点角**量起。取离命中点近的那个。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Wall Anchor")
	bool bFromEndCorner = false;

	/** 到那个角的沿墙距离 cm（恒 ≥ 0）。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Wall Anchor")
	float DistFromCorner = 0.0f;

	/** 洞底离墙基的高度 cm（= `FCSHouseWindow::SillZ`）。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Wall Anchor")
	float SillZ = 0.0f;

	/**
	 * `DistFromCorner` 是按哪种转角口径量的。**0 = 旧口径，1 = 斜接**（2026-09-13 起）。
	 *
	 * 旧口径下奇数边（矩形的东西墙）两端各缩 `T`，距离从**缩进去的那一点**量起；斜接下所有边
	 * 都从外角点量起。同一个物理点在两种口径下差恰好一个 `T`，换算只在 `CSHouse_AnchorS` 一处做。
	 *
	 * ⚠️ **默认值必须是 0，不能是 1**：这个字段是后加的，旧存档里没有它，读进来拿到的是这里的
	 * 默认值 —— 默认成 1 就会把所有旧锚点当新口径解释，东西墙上的窗整体平移一个墙厚。
	 * 新锚点由 `CSHouse_MakeWallAnchor` 显式写 1。
	 */
	UPROPERTY()
	uint8 SConvention = 0;

	/**
	 * **落地门**（2026-09-16）：窗被拖到贴近墙脚时吸附成门，此时 `SillZ` 恒 0。
	 *
	 * 对位 TG `WallAttachmentAnchor.is_bottom_door` —— TG 把门/窗的分界**存在锚点里**，每帧的
	 * `derive_decorator_subtypes` 只读这个字段、不重判高度（附录 E §3.2 / §3.3）。本项目照抄这个
	 * 形态：形态是锚点的一部分，所以撤销、拉尺寸时的重新表达、松手回退（`LastAcceptedAnchor`）
	 * 全都自动带着它走，一行额外代码都不用写。
	 *
	 * ⚠️ **别改成从 `SillZ` 现推**（"洞底低于阈值就是门"）：阈值是标记上的可调参数，改一下阈值
	 * 已经摆好的窗就会集体变门 —— 而且变过去之后派生变换挪了位置，再改回阈值也回不来。
	 * 旧存档里没有这个字段，读进来是 `false`（窗），与这个特性之前的行为逐位相同。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Wall Anchor")
	bool bDoorForm = false;

	/** 边号只查下界：折线化之后边数不再是 4，上界由调用方拿 footprint 自己判。 */
	bool IsValidAnchor() const { return EdgeIndex >= 0 && DistFromCorner >= 0.0f; }

	/** 逐字段相等。用来在重登记前挡掉"没动"的那些，口径同 `FCSHouseWindow::operator==`。 */
	bool operator==(const FCSWallAnchor& O) const
	{
		return EdgeIndex == O.EdgeIndex && bFromEndCorner == O.bFromEndCorner
			&& DistFromCorner == O.DistFromCorner && SillZ == O.SillZ && SConvention == O.SConvention
			&& bDoorForm == O.bDoorForm;
	}
	bool operator!=(const FCSWallAnchor& O) const { return !(*this == O); }
};

/**
 * 一次墙面命中 → 锚点。**沿边弧长离哪个角近就锚哪个角**（见 `FCSWallAnchor` 的注释）。
 *
 * `SillZ` 由调用方给：命中点的 `Z` 是窗**心**高度，洞底要再减半个窗高（口径与
 * `ACSWindowMarker::MakeDemand` 一致，两处必须同源，否则窗会整体偏高半扇）。
 */
inline FCSWallAnchor CSHouse_MakeWallAnchor(const FCSWallHit& Hit, const FCSHouseFootprint& Footprint, float T, float SillZ)
{
	FCSWallAnchor A;
	if (!Hit.bHit) return A;

	const FCSHouseEdgeFrame F = CSHouse_GetEdge(Hit.EdgeIndex, Footprint, T);
	const float S = FMath::Clamp(Hit.S, 0.0f, FMath::Max(F.Len, 0.0f));

	// 边号原样收下（原来是 `& 3`）：命中本来就来自逐边遍历，折线化之后边数不再是 4。
	A.EdgeIndex = Hit.EdgeIndex;
	A.SConvention = 1;   // 斜接口径：从外角点量起（见 `FCSWallAnchor::SConvention`）
	// 正中间（S == Len/2）归**起点角**：判据要给出确定的一侧，`>` 而不是 `>=` 就是这个用意。
	A.bFromEndCorner = (S > F.Len * 0.5f);
	A.DistFromCorner = A.bFromEndCorner ? (F.Len - S) : S;
	A.SillZ = SillZ;
	return A;
}

/**
 * **窗 ↔ 门的形态判据**（2026-09-16，TG `decorator_interaction_intent::snap_balcony_door` 的对位物）。
 * 纯函数：拖 gizmo 与笔刷落笔都经 `ACSWindowMarker::MakeAnchorFromHit` 调到这里，单测直接调。
 *
 * TG 的判据（附录 E §1.3，确凿）：**窗碰撞盒的底边低于墙脚** ⇒ 变门，门心吸到墙脚 + 半个门高。
 * 阈值就是墙脚本身（容差 0）。本函数的 `SnapHeight` 就是"墙脚往上多高算墙脚"。
 *
 * ⚠️ **门形态按门的半高判，不是恒按窗判** —— 这是与 TG 唯一的有意差异。TG 鼠标模式每帧只看光标、
 * 进出门同一条判据（附录 E §1.4），代价是"抓起一扇已落地的门、原地不动，它立刻变回窗"
 * （门心在 `门高/2`，按窗的半高算底边 = (门高 − 窗高)/2 = 51 cm，高过墙脚）。TG 只在拖拽时解析，
 * 这个回弹藏在手感里；**本项目改一下属性面板就会重新解析一次**（`PostEditChangeProperty`），
 * 照抄的话门一碰参数就变回窗。按当前形态判之后，门的底边恒为 0、窗的底边恒为窗台高，
 * 两个形态各自都是解析的不动点（同 `WallStandoff` 那条纪律）；顺带得到一段迟滞，拖过阈值附近不闪。
 *
 * @param CenterZ       命中点高度 = 本体中心（墙空间 cm）
 * @param WindowHeight  窗形态的洞高
 * @param DoorHeight    门形态的洞高；≤ 0 = 这扇窗变不成门
 * @param SnapHeight    底边低于它即吸附成门（TG = 0）
 * @param bWasDoor      当前锚点的形态
 */
inline bool CSHouse_ResolveDoorForm(float CenterZ, float WindowHeight, float DoorHeight, float SnapHeight, bool bWasDoor)
{
	if (DoorHeight <= UE_KINDA_SMALL_NUMBER) return false;
	if (bWasDoor)
	{
		// 吸附后的门底**恰好**是 0。阈值取 TG 原值 0 时 `0 < 0` 不成立 ⇒ 刚吸附的门下一次解析就翻回窗，
		// 所以门这一侧至少留 1 cm 的"还贴着墙脚"。
		return CenterZ - DoorHeight * 0.5f < FMath::Max(SnapHeight, 1.0f);
	}
	return CenterZ - WindowHeight * 0.5f < SnapHeight;
}

/**
 * 锚点 → 沿边弧长 `S`（`FCSHouseWindow::CenterS` 要的那个量）。
 *
 * 墙缩短到锚点越界时**夹到 `[0, Len]`** 而不是留在墙外：谓词那关照旧会判 `NearCorner` 把它拒掉，
 * 但夹住能保证派生变换永远落在墙面上 —— 标记不会飞到墙外的空中去。
 */
inline float CSHouse_AnchorS(const FCSWallAnchor& A, const FCSHouseFootprint& Footprint, float T)
{
	const FCSHouseEdgeFrame F = CSHouse_GetEdge(A.EdgeIndex, Footprint, T);
	const float Len = FMath::Max(F.Len, 0.0f);
	// 旧口径的奇数边距离是从缩进 `T` 的那一点量的：起点角那一侧外角点再往里 T，终点角那一侧
	// 同理 ⇒ 两侧都是「距离 + T」。旧口径只可能出现在矩形（折线化之前存的档），所以奇偶判据
	// 在这里仍然成立，不需要知道边数。
	const float Dist = (A.SConvention == 0 && (A.EdgeIndex & 1) != 0) ? A.DistFromCorner + T : A.DistFromCorner;
	const float S = A.bFromEndCorner ? (Len - Dist) : Dist;
	return FMath::Clamp(S, 0.0f, Len);
}

/**
 * 锚点 → **房子构建空间**的变换（`ACSHouseActor::GetBuildTransform()` 那个只取 yaw 的空间）。
 * 这就是 TG `CachedDecoratorTransforms` 的对位物：变换是从锚点算出来的，不是存下来的。
 *
 * 约定（与 `ACSHouseFeatureMarker` 的探针同向，**别改**）：
 * - **+X = 墙的内法线 `In`**（标记站在墙外看着墙）。`ResolveHostAndRegister` 沿 `GetActorForwardVector()`
 *   打探针，反过来的话标记一放上去就找不到宿主 —— 而找不到宿主是"合法"的自毁，不会有断言报红。
 * - 原点在墙的外表面**再往外 `Standoff`** 处。窗框资产按 −X 朝外来摆。
 *
 * ⚠️ **`Standoff` 必须 ≥ 0，不能把原点推进墙里**（否则派生出来的位置会让探针失效，而且两条通路
 * 一起失效：射线版从外皮内侧往内打得到**负**距离被 `Dist <= 0` 挡掉，就近版的
 * `dot(P − Start, 外法线) < 0` 判成背面直接 `continue`）。症状是"窗吸附一次之后就再也解析不到宿主"，
 * 而无宿主会自毁 —— 又是一次不报红的静默失效。派生变换必须是解析的**不动点**：
 * `SnapToAnchor()` 之后再 `ResolveHostAndRegister()` 必须拿回同一个锚点，单测 `House.WallAnchor` 钉这条。
 *
 * `HalfHeight` = 窗高的一半：锚点存的是洞底，而标记本体锚在窗**心**（口径同 `MakeDemand`）。
 */
inline FTransform CSHouse_AnchorToLocal(
	const FCSWallAnchor& A, const FCSHouseFootprint& Footprint, float T, float HalfHeight, float Standoff)
{
	const FCSHouseEdgeFrame F = CSHouse_GetEdge(A.EdgeIndex, Footprint, T);
	const float S = CSHouse_AnchorS(A, Footprint, T);

	// 外表面上的点 + 沿**外**法线站开一点。`F.Start` 本来就是外皮上的外角点（斜接口径：转角让出的
	// 是内皮沿边的长度，不是法向），所以这里不需要再补半个墙厚。
	const FVector2D P2 = F.Start + F.U * double(S) - F.In * double(FMath::Max(Standoff, 0.0f));
	const FVector Pos(P2.X, P2.Y, double(A.SillZ + HalfHeight));

	const FVector Fwd(F.In.X, F.In.Y, 0.0);
	return FTransform(FRotationMatrix::MakeFromXZ(Fwd, FVector::UpVector).Rotator(), Pos);
}

/** 洞被拒的原因（计划 D8 的 `FCSFeaturePlacement::Reason`）。编辑器/脚本据此区分"没生成"与"生成了但看不见"。 */
UENUM(BlueprintType)
enum class ECSFeatureReject : uint8
{
	/** 放得下。 */
	None,
	/** 尺寸退化（宽 ≤ 0 或 Z1 ≤ Z0），根本不是一个洞。 */
	Degenerate,
	/** 边号越界（不在 0..NumEdges−1）。 */
	NotOnWall,
	/** 会吃掉墙两端的护角。 */
	NearCorner,
	/** 洞底在地面以下，或低于窗台下限（几何合法但观感荒唐）。 */
	SillTooLow,
	/** 洞顶会吃掉墙顶那条连续砖带。 */
	AboveEave,
	/** 与已有洞的**面板格**按 Clearance 膨胀后相交。 */
	OverlapsOpening,
	/** 落在判为墩的跨度上 —— 那截墙起拱线以下整片被裁掉，开在上面就是一扇悬空的洞。 */
	OnPierSpan,
};

/** 谓词的全部输入。房子只是把自己的属性填进来 —— 抽出来是为了让纯 CPU 单测能调**同一条**判据。 */
struct FCSOpeningSite
{
	FCSHouseFootprint Footprint = FCSHouseFootprint::MakeRect(FVector2D(600.0, 400.0));
	float WallThickness = 24.0f;
	float WallHeight = 300.0f;
	float LintelBand = 40.0f;
	float CornerMargin = 60.0f;
	float PierWidth = 20.0f;
	float OpeningClearance = 10.0f;

	/** 窗台下限 cm。只对**非门**生效：门恒 Z0 = 0，拿这条卡它等于把所有门都拒了。 */
	float MinSillZ = 0.0f;

	/** 墩样式开关与它的**高阈**（判据为什么取高阈见 CSHouse_QueryOpening 里那段）。 */
	bool bPierStyleEnabled = true;
	float PierRestoreWidth = 75.0f;

	/**
	 * 已生效的洞。**必须按 (EdgeIndex, CenterS) 升序** —— 墩跨度那一段靠"相邻两项即同边相邻
	 * 两洞"来找跨度，乱序会让它拿隔了一个洞的两端当跨度，把中间那一整块墙误判成墩。
	 */
	TArrayView<const FCSWallOpening> Openings;
};

/**
 * 这个洞放得下吗（D8 的可行性谓词，纯参数判定、零 GPU 回读）。
 *
 * **判据是同边一维 S 区间**：两洞的面板格（`CSHouse_OpeningCell`）按 Clearance 膨胀后是否
 * 相交，Z 一律不参与 —— 用户裁决 2026-08-30（C1 选甲），永久放弃"门上开窗"。谓词与
 * `CSHouse_BuildBodySoup` 的单游标扫掠同维，"谓词说能放就一定砌得出"才成立。**别把它改回二维。**
 *
 * ⚠️ **墩跨度那一条只能按高阈保守判**：墩的双阈迟回（`ResolvePierSpans`）跑在窗过完谓词
 * **之后**（墩是最终洞集合的函数），所以谓词跑的时候 `StyleFlags` 还没算、也不该读。这里改判
 * "这条跨度**有没有可能**被判成墩"——跨度 ≤ 高阈就一律不接受窗。宁可多拒一扇，也不能放出
 * 一扇悬在被裁空的墩上的窗。
 */
inline ECSFeatureReject CSHouse_QueryOpening(const FCSOpeningSite& Site, const FCSWallOpening& Candidate)
{
	if (!Candidate.IsValid()) return ECSFeatureReject::Degenerate;
	if (Candidate.EdgeIndex < 0 || Candidate.EdgeIndex >= Site.Footprint.NumEdges()) return ECSFeatureReject::NotOnWall;

	// 必须整个落在这面墙的可用段内（护角照留），且顶部不能吃掉墙顶的连续砖带。
	//
	// 护角从外角点量起，且**至少让过斜接面**：锐角处的斜接让出量会超过 `CornerMargin`
	// （内角 30° 时是 3.7 个墙厚），洞要是伸进斜接面，那一截面板端面是斜的、裁剪场照样切，
	// 画面上就是一块斜着缺了角的墙。直角下让出量 = T < 默认护角，取 max 之后与只看护角逐位相同。
	const FCSHouseEdgeFrame F = CSHouse_GetEdge(Candidate.EdgeIndex, Site.Footprint, Site.WallThickness);
	const float MarginStart = FMath::Max(Site.CornerMargin, F.InsetStart);
	const float MarginEnd = FMath::Max(Site.CornerMargin, F.InsetEnd);
	if (Candidate.S0() < MarginStart || Candidate.S1() > F.Len - MarginEnd) return ECSFeatureReject::NearCorner;
	if (Candidate.Z0 < 0.0f) return ECSFeatureReject::SillTooLow;
	// 窗台下限只卡**窗**：道路门与附属物的门形态本来就落地，拿它卡门等于把所有门都拒了。
	if (Candidate.Type != ECSOpeningType::Door && !Candidate.bDoorForm && Candidate.Z0 < Site.MinSillZ) return ECSFeatureReject::SillTooLow;
	if (Candidate.Z1 + FMath::Abs(Candidate.Skew) * Candidate.HalfWidth() > Site.WallHeight - Site.LintelBand) return ECSFeatureReject::AboveEave;

	for (const FCSWallOpening& Existing : Site.Openings)
	{
		if (Existing.SourceId.IsValid() && Existing.SourceId == Candidate.SourceId) continue;   // 自己不与自己冲突
		if (CSHouse_OpeningsOverlap(Existing, Candidate, Site.PierWidth, Site.OpeningClearance)) return ECSFeatureReject::OverlapsOpening;
	}

	// 墩跨度不接受窗（计划 D6「拱间墩」）。默认参数下这条不可达 —— 墩跨度 ≈ PierWidth，而格谓词
	// 本来就要求两洞的格（各含半个墩宽）不相交 ⇒ 窗压根挤不进两个拱之间。把 PierStyleMaxWidth
	// 调到远超 PierWidth 时才会露出来：那时墩上会挖出一扇悬在半空的窗。
	if (Site.bPierStyleEnabled && Site.Openings.Num() >= 2)
	{
		float CellMin = 0, CellMax = 0;
		CSHouse_OpeningCell(Candidate, Site.PierWidth, CellMin, CellMax);
		for (int32 K = 0; K + 1 < Site.Openings.Num(); ++K)
		{
			const FCSWallOpening& Left = Site.Openings[K];
			const FCSWallOpening& Right = Site.Openings[K + 1];
			if (Left.EdgeIndex != Candidate.EdgeIndex) continue;
			float Span = 0.0f, TopZ = 0.0f;
			// 跨度与墩顶只认这一份判据（"两侧都是落地的拱"），与房体那块墩裁剪场同源。
			if (!CSHouse_PierSpanBetween(Left, Right, Span, TopZ)) continue;
			if (Span > FMath::Max(Site.PierRestoreWidth, 0.0f)) continue;   // 宽到永远判不成墩
			if (CellMin < Right.S0() && CellMax > Left.S1()) return ECSFeatureReject::OnPierSpan;
		}
	}
	return ECSFeatureReject::None;
}

/** 拒绝原因 → 一句人话。编辑器线框颜色与脚本日志共用，别在调用点各写一份。 */
inline const TCHAR* CSHouse_FeatureRejectText(ECSFeatureReject Reason)
{
	switch (Reason)
	{
	case ECSFeatureReject::None:            return TEXT("");
	case ECSFeatureReject::Degenerate:      return TEXT("尺寸退化（宽或高 ≤ 0）");
	case ECSFeatureReject::NotOnWall:       return TEXT("边号越界");
	case ECSFeatureReject::NearCorner:      return TEXT("吃掉了墙端的护角");
	case ECSFeatureReject::SillTooLow:      return TEXT("窗台太低（低于 WindowMinSillZ 或在地面以下）");
	case ECSFeatureReject::AboveEave:       return TEXT("洞顶吃掉了墙顶的连续砖带");
	case ECSFeatureReject::OverlapsOpening: return TEXT("与已有洞的面板格相交（门拱优先）");
	case ECSFeatureReject::OnPierSpan:      return TEXT("落在拱间墩的跨度上");
	default:                                return TEXT("未知");
	}
}
