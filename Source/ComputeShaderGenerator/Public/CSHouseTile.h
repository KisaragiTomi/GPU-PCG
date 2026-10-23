#pragma once

#include "CoreMinimal.h"
#include "CSGroundShaperSteps.h"   // FPaletteBuffers —— 实例源的容器，与石阶/藤/摆件同一份
#include "CSHouseRoof.h"           // FCSRoofDesc —— 屋面方程只有这一份真源

/**
 * 屋面瓦片（TinyGladeHouse D4 的后半：**屋面本体**）。
 *
 * -----------------------------------------------------------------------------
 * 为什么屋面是瓦而不是实体板
 * -----------------------------------------------------------------------------
 * TG 的屋顶**整面由瓦铺成**（实拍俯视 + `system_roof::visual::assemble_roof_tiles`），四个坡面、
 * 四条角斜脊、中间一条短脊。此前那套"两块实体坡板 + 山墙 + 檐口封口"已随双坡结构一起删除
 * （见 `CSHouseRoof.h` 的抬头）。房体三角汤里因此**一片屋面都没有**，屋面全部活在这条实例路上。
 *
 * -----------------------------------------------------------------------------
 * 排布：一条方程管四个面
 * -----------------------------------------------------------------------------
 * 四坡的高度场是矩形的内距（`CSHouseRoof.h`），于是**每个坡面在平面上都是同一个形状**：
 * 一条底边 + 两条 45° 斜边收进去的梯形。用「内距 d」参数化，那个梯形的半宽是
 *
 *     w(d) = L − d，   d ∈ [−Overhang, HalfSpan]，  L = 这条边的半长
 *
 * 檐口外挑段 d < 0 ⇒ w > L，正是四坡屋檐在转角处继续外扩的那一截；
 * 长边的面到 d = HalfSpan 时剩下 w = L − HalfSpan（那条短脊），短边的面剩下 0（角上的尖）。
 * **脊、角斜脊、金字塔尖全都是这一条式子的边界情形**，不需要为它们各写一段。
 *
 * 排距与列距一律走"可用长 / 份数"的**等分**（与门洞分段、摆件间距、石阶铺装同一条纪律）：
 * 目标间距只决定份数，实际间距由等分给出，瓦再按实际间距缩放 —— 屋面因此永远铺满，
 * 不会在尺寸连续变化时出现"最后一列忽有忽无"的跳变。
 *
 * -----------------------------------------------------------------------------
 * 2026-10-02：脊长可调之后（`FCSRoofDesc::EdgeSteep`，见 `CSHouseRoof.h` 抬头）
 * -----------------------------------------------------------------------------
 * 上面那条式子是**等坡度**的口径（倍率全为 1），现在仍然逐位成立 —— `House.TileEqualPitchMatchesLegacy`
 * 拿冻结的旧实现守着。脊长调过的矩形屋面只多三件事，都落在同一趟循环里：
 *  · 两端的坡面有自己的坡度与坡长 ⇒ 排数、排距逐面算（TG 同），它们与两侧的坡在角斜脊上不再逐排对齐，
 *    交线由逐瓦裁剪面收口；裁剪面取「两面等高」的那条线，不再是 45°。
 *  · 山墙边（脊长 = 房长）没有坡面：那一面不铺瓦，两侧坡面的排一样长，一直通到山墙外 `GableOverhang` 处，
 *    裁剪面是那里的竖直面。
 *  · 骨架里山墙侧的斜边（`FCSRoofSkeletonArc::bVerge`）照铺斜脊那种窄板 = 封边板，法线取那一个坡面的法线。
 *
 * -----------------------------------------------------------------------------
 * 纯函数 + 记录：GPU 那一侧只做打包
 * -----------------------------------------------------------------------------
 * `BuildPlan` 无 GPU、无 world 依赖，可直接进 automation 测试 —— 屋面最容易错的两件事
 * （瓦有没有铺满、身份稳不稳）都只能在这一层断言，到了 GPU 那一侧就只剩一个实例计数。
 */
namespace CSHouseTile
{
/**
 * 瓦片网格的三条**局部轴**各自承担什么。由 `ACSHouseActor` 从网格包围盒自动判定后填进来。
 *
 * ⚠️ **不改网格、只换基**：藤蔓那条路是导入期换轴（`CSHouseVine::BuildBaseMesh` 把长度轴搬到
 * +Z），因为它的 kernel 只认一种基。瓦这里反过来 —— 记录自带整组基，于是"这块网格哪根轴朝上"
 * 只是 CPU 侧的一个排列，既不动顶点也不进 kernel。换一张轴向不同的瓦资产时改这里就够了。
 */
struct FMeshAxes
{
	/** 局部轴号 0/1/2：指向屋脊的那根（瓦沿坡向的长度轴）。 */
	int32 UpSlope = 0;
	/** 沿排（水平，绕着房子走）的那根。 */
	int32 AlongRow = 1;
	/** 屋面外法线那根（瓦最薄的那根轴）。 */
	int32 Normal = 2;
	/** 法线轴的朝向：网格的这根轴指向瓦的**背面**时填 −1。 */
	float NormalSign = 1.0f;
	/** 网格自身三轴尺寸 cm（**局部轴序**，不是重排后的）。 */
	FVector3f NativeSize = FVector3f(1.0f, 1.0f, 1.0f);
	/**
	 * 网格自身包围盒中心（局部轴序）。**枢轴不在中心的资产靠它对齐** ——
	 * 实例变换是"原点 + Σ 顶点分量 × 缩放基"，枢轴在角上的瓦不减掉这一项就会整片偏半块，
	 * 而位置断言、瓦数、剔除盒全都正常，只有出图看得见。
	 */
	FVector3f NativeCentre = FVector3f::ZeroVector;

	float NativeAlongSlope() const { return NativeSize[FMath::Clamp(UpSlope, 0, 2)]; }
	float NativeAcrossRow() const { return NativeSize[FMath::Clamp(AlongRow, 0, 2)]; }
	float NativeThickness() const { return NativeSize[FMath::Clamp(Normal, 0, 2)]; }
};

/** 铺瓦的形态参数。全部由 `ACSHouseActor` 的属性喂进来，这里不留默认策略。 */
struct FParams
{
	/** 沿坡向的排距 cm（**沿坡面量**，不是竖直投影）。≤ 0 = 由网格尺寸与 RowOverlap 反解。 */
	float RowPitch = 0.0f;
	/** 排内相邻瓦的间距 cm。≤ 0 = 由网格尺寸与 ColumnOverlap 反解。 */
	float ColumnPitch = 0.0f;
	/**
	 * 瓦画多大 = 间距 × 这个系数。> 1 就是**故意让相邻瓦互相压住** —— 与门框砖那条负缝
	 * 同一条纪律（TG 的砖本来就是故意胀大互穿的）。正缝会在每一道接缝上露出屋面底下的天空。
	 * 上下两排的压叠远比左右深，所以两个方向各有一个系数。
	 */
	float RowOverlap = 1.6f;
	float ColumnOverlap = 1.06f;
	/**
	 * 瓦沿屋面法线的**厚度** cm。≤ 0 = 用网格原生尺寸（旧行为）。
	 *
	 * ⚠️ **这一项不加的话瓦是一米厚的方块。** TG 的 `roof_tile` 原件实测就是
	 * **1.188 × 1.0 × 1.0 m** —— 它是个**单位块**，真实厚度由 TG 的逐实例 `scale_t` 压出来
	 * （`_nani_instanced_roof` 的 VS）。我们这边平面内两轴按排距缩了，法线轴却一直取原生
	 * 100 cm，于是屋顶看着像堆了一层砖。
	 */
	float Thickness = 0.0f;
	/** 瓦画多大的**总系数**，乘在平面内两轴上（排距一步不动 ⇒ 瓦数不变，只是每片变大变小）。 */
	float SizeScale = 1.0f;
	/**
	 * 角斜脊 / 屋脊上盖瓦的尺寸系数。≤ 0 = 不铺脊瓦。
	 *
	 * 角斜脊：TG 的单排窄板，宽 25 cm、板厚 18 cm，长轴沿脊；坡面瓦按两坡等距平面裁剪。
	 * 水平屋脊（2026-09-29 起照 TG）：分段平躺厚板，宽 = 40 cm × 本系数，见 `CSHouseTile.cpp` 的 `CSHouseTile_Ridge*`。
	 * roof_tile_lod1 / backface 是原版的简化/背面通道，不能当作随机瓦型或专用脊瓦。
	 */
	float RidgeCapScale = 1.15f;
	/** 沿屋面法线抬起 cm。 */
	float StandOff = 0.5f;
	/** 逐瓦的尺寸抖动（比例）。 */
	float ScaleJitter = 0.05f;
	/**
	 * 逐瓦的**瓦长**抖动（比例，只乘上坡那一轴）。TG `assemble_rectangular_roof`：瓦长 = 排距 / 0.7 × (0.8 + 0.4 · rand)，
	 * 即 0.2；瓦以排线为中心，所以同一排的下沿参差。
	 */
	float LengthJitter = 0.0f;
	/** 绕屋面法线的朝向抖动（弧度）。 */
	float YawJitter = 0.04f;
	/** 沿屋面法线的高度抖动 cm（TG 的瓦是一片起伏的鳞，不是一张平面）。 */
	float LiftJitter = 0.6f;
	int32 Seed = 1;
	FMeshAxes Axes;
};

/**
 * 一块瓦。三条轴**按网格的局部轴序**存（`AxisX` 就是喂给网格 +X 的那条世界方向），
 * 排列已经在 CPU 侧折进来了 —— kernel 因此一步正交化都不做，见 `CSHousePack.usf` 里 PackHouseTileCS 的说明。
 */
struct FRecord
{
	FVector3f WorldPos = FVector3f::ZeroVector;
	float Random01 = 0.0f;
	FVector3f AxisX = FVector3f(1.0f, 0.0f, 0.0f);
	float SizeX = 1.0f;                              // 世界 cm
	FVector3f AxisY = FVector3f(0.0f, 1.0f, 0.0f);
	float SizeY = 1.0f;
	FVector3f AxisZ = FVector3f(0.0f, 0.0f, 1.0f);
	float SizeZ = 1.0f;
	/** 材质 custom data[0..1]：裁剪平面水平法线角 + 4、世界空间平面常数，保留 n.xy·P.xy + d <= 0。
	 * 角编码 > 0 为坡面瓦；0 为不裁剪/不抬端的脊板；-1 为只抬端的水平脊首尾板（TG is_main_roof_body）。 */
	FVector2f ClipPlane = FVector2f::ZeroVector;
};

/**
 * 纯函数：屋面 desc + 摆位 + 参数 → 瓦。**不碰任何 GPU 资源，可以在纯 CPU 单测里跑。**
 *
 * `World` 与房体 / 藤 / 摆件同一份 `ACSHouseActor::GetBuildTransform()`（只取 yaw + 位置）。
 */
COMPUTESHADERGENERATOR_API void BuildPlan(const FCSRoofDesc& Roof, const FTransform& World,
	const FParams& Params, TArray<FRecord>& OutTiles);

/**
 * 容量上限：**只依赖配置与尺寸，不依赖这一次真排了几块**。
 *
 * 零阻塞纪律的执行面 —— 容量按它一次付清、之后永不扩容。真排超了就在 kernel 里截断：
 * 少铺几块瓦，远好过在拖动的某一帧上付一次设备同步。
 */
COMPUTESHADERGENERATOR_API int32 MaxTilesBound(const FCSRoofDesc& Roof, const FParams& Params);

/**
 * 录一趟打包 pass：上传记录 → 一个 dispatch 写满 packed 行 + counter。
 *
 * **录完直接返回，不阻塞**。前置条件是 `CSShaperSteps::ReserveCapacity` 已经把容量备好 ——
 * 这里只用现有容量，一个字节都不分配。返回是否真的录了 pass。
 */
COMPUTESHADERGENERATOR_API bool Pack(const TArray<FRecord>& Records,
	const CSShaperSteps::FPaletteBuffers& Buffers, const FMatrix44f& WorldToComponent);
}
