#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
// CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS —— FPaletteBuffers::CustomData 的元素数按它算。
// 显式写出来：unity 构建下漏 include 一声不吭，只在 -SingleFile 或 Live Coding 下炸。
#include "CSGpuInstancedMeshComponent.h"

class AActor;

/**
 * GPU 实例源的**共用容器与容量管理**（TinyGladeHouse D6/D9/D12/D13）。
 *
 * 文件名与命名空间是历史遗留：这里原本是塑形物石阶（`ACSGroundShaperActor::RebuildSteps`）
 * 的 GPU 散布 —— CPU 解等高线与铺装、`RDG_SmoothSpline` 求值 B 样条、`CSGroundSteps.usf`
 * 逐记录组装实例行。**那条路已随 2026-08-30「裁决一」整条删除**（石阶现在只有地面自己那条
 * `CSGroundStairs.usf`，门框砖走 `CSHouseFrame.usf` 的解析推导）。
 *
 * 剩下的是四条 GPU 实例路（门框砖 / 藤枝 / 藤叶 / 摆件）都在用的三样东西：
 *   · `FPaletteBuffers` —— 一个 palette 条目的 packed 实例行 + counter + 基础网格尺寸；
 *   · `ReserveCapacity` —— 注册期一次付清的定容预留（交互期因此永远走不到扩容那次阻塞）；
 *   · `ReleaseOnRenderThread` —— 在渲染命令里放掉这份引用（顺序上的讲究，不是安全要求，见它的注释）。
 *
 * 另外还留着一个 `ResampleUniform`：它**没有产线消费者**，只服务于单测
 * `House.FrameAnalyticMatchesLegacy` 里那份旧路 CPU 镜像（见它自己的注释）。
 */
namespace CSShaperSteps
{
/**
 * 把折线重采样成等弧长间距的点列（首末点逐位保留）。
 *
 * ⚠️ **产线消费者为零，这是有意的**（2026-08-30 裁决一）：它当初是门框旧路
 * （`ACSHouseActor::BuildFramePlan`，B 样条 + 逐砖记录）的必需品 —— 均匀三次 B 样条的参数
 * 按控制点序号走，而排块算的是弧长占比，两者只有在控制点等距时才等价；不重采样的话
 * 「左门樘底 -> 拱缘 -> 右门樘底」那条 U 形路径疏密比 16.9:1，19 块砖有 17 块挤在拱上、
 * 互相穿模成薄鳍片。旧路删掉之后这条约束随之作废：解析推导直接从拱参数算砖心，没有样条。
 *
 * **留着它是因为等价性判据要它**：单测 `House.FrameAnalyticMatchesLegacy` 里那份旧路
 * CPU 镜像（`CSHouseTest_LegacyFrameBricks`，不调任何产线代码）必须逐字复现旧路的组路方式，
 * 而这段重采样正是其中一步。它是"解析推导没跑偏"这条唯一守卫的一部分，**不是死代码**。
 * 另有 `House.FrameCurveUniform` 直接钉它自己的契约。
 *
 * 首末点必须逐位保留 —— 镜像靠"两端各外延一格控制点 + 只取 [1/(N-1), (N-2)/(N-1)]
 * 参数区间"把端点钉在设计位置上，端点一动这条纪律就失效。
 */
inline void ResampleUniform(const TArray<FVector>& InPoints, int32 SegmentCount, TArray<FVector>& OutPoints)
{
	if (InPoints.Num() < 2 || SegmentCount < 1)
	{
		OutPoints = InPoints;
		return;
	}

	TArray<double> Cumulative;
	Cumulative.SetNumUninitialized(InPoints.Num());
	Cumulative[0] = 0.0;
	for (int32 Index = 1; Index < InPoints.Num(); ++Index)
	{
		Cumulative[Index] = Cumulative[Index - 1] + FVector::Dist(InPoints[Index - 1], InPoints[Index]);
	}
	const double Total = Cumulative.Last();
	if (Total <= UE_KINDA_SMALL_NUMBER)
	{
		OutPoints = InPoints;
		return;
	}

	OutPoints.Reset(SegmentCount + 1);
	OutPoints.Add(InPoints[0]);
	// 游标只向前走：目标弧长单调递增，重扫一遍是 O(N·M) 而这里 O(N+M)。
	int32 Segment = 1;
	for (int32 Step = 1; Step < SegmentCount; ++Step)
	{
		const double Target = Total * Step / SegmentCount;
		while (Segment < InPoints.Num() - 1 && Cumulative[Segment] < Target) ++Segment;
		const double Span = Cumulative[Segment] - Cumulative[Segment - 1];
		const double T = Span > UE_KINDA_SMALL_NUMBER
			? FMath::Clamp((Target - Cumulative[Segment - 1]) / Span, 0.0, 1.0) : 0.0;
		OutPoints.Add(FMath::Lerp(InPoints[Segment - 1], InPoints[Segment], T));
	}
	OutPoints.Add(InPoints.Last());
}

// --- 交互期的稳态化：把"随尺寸连续变化的量"吸成阶梯 -------------------------------------
//
// 起因（实测，2026-08-30 拉尺寸那一轮）：门框砖实例源的包围盒直接由 FootprintSize / WallHeight
// 算出，而交接的包围盒阈值只有 1 cm ⇒ 拖动中每一帧都重走一次阻塞的 SetInstanceSourceGPU；
// 房体的 EnsureCapacitySync 按精确顶点数要容量 ⇒ 每一帧重新分配 + 拷贝。两条都是
// "值连续变 ⇒ 资源每帧重来"的同一个失败模式。解法同型：先留余量、再对齐到台阶、并且只涨不缩。
//
// ⚠️ **这两条必须只有一份**（2026-08-31 从 `CSHouseActor.cpp` 的匿名命名空间搬到这里）：
// 地面那一侧的裙边摆件走的是同一条纪律，各抄一份的话两组常数迟早分叉，
// 而症状是"房子那边零阻塞、地面这边每帧一次"——两边的断言各自都绿。
//
// 可陈述的保证是**一段把尺寸涨大不到 25% 的拖动，一次交接都不付**；越过时也只付一次，
// 随后又有一整段余量。代价只是剔除保守一点、显存多留一点，全量重建时会重新收紧。

inline constexpr double BoundsHeadroom = 1.25;   // 25% 余量：够吃下任何一段帧级拖动
inline constexpr double BoundsQuantum = 200.0;   // cm，包围盒台阶
inline constexpr int32 CapacityStep = 4096;      // 顶点/索引/实例容量的台阶

/** 留余量 + 向上对齐到台阶。只用于**保守**量（包围盒、容量），不能用在几何尺寸上。 */
inline double QuantizeUp(double Value)
{
	return FMath::CeilToDouble(FMath::Max(Value, 0.0) * BoundsHeadroom / BoundsQuantum) * BoundsQuantum;
}

/** 同一条纪律的容量版。`UCSMesh::EnsureCapacitySync` 只涨不缩，但"涨"本身就是一次阻塞刷新。 */
inline int32 ReserveCount(int32 Exact)
{
	const int64 Want = int64(FMath::Max(Exact, 1)) * 3 / 2;
	return int32(FMath::Max<int64>(CapacityStep, FMath::DivideAndRoundUp<int64>(Want, CapacityStep) * CapacityStep));
}

/** 一个 palette 条目对应的 GPU 实例源（直接喂 UCSGpuInstancedMeshComponent）。 */
struct FPaletteBuffers
{
	TRefCountPtr<FRDGPooledBuffer> PackedInstances;   // Buffer<float4>，CS_GPU_INSTANCED_ROW_FLOAT4S 个 / 实例
	TRefCountPtr<FRDGPooledBuffer> Counter;           // Buffer<uint>，[0] = 活跃数
	/** Buffer<float>，`CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS` 个 / 实例。**恒分配**：
	 *  8 字节 / 实例，4096 容量也只有 32 KB，为省它多开一条"要不要分配"的分支不划算。
	 *  不写它的生产者留全零，材质读到 0。 */
	TRefCountPtr<FRDGPooledBuffer> CustomData;
	uint32 Capacity = 0;
	FVector3f BaseSphereCentre = FVector3f::ZeroVector;   // 基础网格局部包围球（未缩放）
	float BaseSphereRadius = 0.0f;

	/**
	 * 三轴块尺寸。基础网格自带真实尺寸时留 (1,1,1)；
	 * 喂"单位立方体字典 mesh"时由它给出实际尺寸 —— Tiny Glade 的 brick 正是一块
	 * 100³ 的居中立方体，**非均匀缩放本身就是砖的尺寸**（逆向报告 §1.4：
	 * "Affine3Packed transform，非均匀缩放 = 砖尺寸"）。
	 * 轴向约定同 kernel（`CSHouseFrame.usf`）：X = 面内径向、Y = 沿曲线、Z = 平面法线。
	 */
	FVector3f BlockSize = FVector3f(1.0f, 1.0f, 1.0f);

	bool IsValid() const { return PackedInstances.IsValid() && Counter.IsValid() && Capacity > 0; }
	void Reset() { *this = FPaletteBuffers(); }
};

/**
 * 按下限预留容量。
 *
 * 存在的理由是**时机**：分配必须在渲染线程，因此扩容那一趟一定阻塞。把它挪到注册/加载期
 * 一次付清，交互期（画笔刷、拖房子）就永远走不到扩容分支 —— 那正是"交互热路径零设备同步"
 * 这条纪律要求的。不预留的话，第一次真的长出砖的那一笔会当场付一次 flush，而且它出现在
 * 哪一笔完全取决于用户画到哪里，最难复现也最难归因。
 *
 * 只涨不缩；已经够大就一次 enqueue 都不发。扩容出来的新 buffer（行 / counter / custom data）
 * 三条都清零：池子复用的 buffer 带着上一位租客的字节，交接出去就是幽灵实例。扩容之后组件拿到的
 * 是一份**空**实例源 —— 交接结果 `HandedOver` 就是"必须重打包一次"的信号，各家 Rebuild 的早退门
 * 据此不许早退。
 */
bool ReserveCapacity(TArray<FPaletteBuffers>& Palettes, uint32 MinCapacityPerPalette);

/**
 * 在渲染命令里放掉这批引用并清空表。
 *
 * **不是安全要求**（2026-09-07 审查 B7）：`FRDGPooledBuffer` 是原子引用计数，RDG 池子自己恒持一份、
 * 且要等 buffer 闲置 30 帧才真释放 —— 无论哪个线程放掉引用，在途帧读的显存都不会被抽走。
 * 这一跳只是让"放掉"排在已经入队的命令之后。真正会出事的两件是：复用的 buffer 没清零（GrowTo 已清）、
 * 引用一直没人放 —— 各放各的：组件那一份由组件销毁时自己放（UCSGpuInstancedMeshComponent::
 * OnComponentDestroyed），生产者这一份由生产者放（本函数，ACSTinyGlade 派生类在 ReleaseInstancedBuffers 里调）。
 */
void ReleaseOnRenderThread(TArray<FPaletteBuffers>& Palettes);

/**
 * 把每个 palette 的 counter 清零。**不阻塞**（只录一趟 pass）。
 *
 * 存在的理由是本项目栽过的那一类 bug：生产者判定"这一轮一件都没有"于是**提前返回、
 * 顺手撤掉实例源**，counter 却还留着上一代的数。撤实例源同时会清空交接缓存，于是下一次
 * `EnsureXxxComponent` 把**同一批 buffer** 又交回组件 —— 剔除 pass 照着陈旧计数器再画一遍
 * 上一代的实例，而 CPU 侧的计数、三角形数、零阻塞断言**全部照绿**。
 *
 * 每条散布 kernel 自己的空表分支都已经会显式清零，但那要求 kernel **真的被调到**；
 * 提前返回的那几条路走不到它。所以那些路必须在撤实例源**之前**显式调这一条。
 */
void ZeroCounters(TArray<FPaletteBuffers>& Palettes);

// --- 实例组件的建/补 ---------------------------------------------------------------------

/**
 * 建/补一个 GPU 实例组件（挂在 `Owner` 的根上、`RF_Transient`、立刻注册）。
 *
 * ⚠️ **必须先 `RegisterComponent` 再喂**：未注册时任何变更都会把 GPU 网格释放掉。
 * 而"每一趟都判"是因为蓝图 actor 重跑构造脚本会把实例组件销毁、指针随之失效 ——
 * 只在构造里建一次的话，重跑之后画面永远是空的，且没有任何报错。
 *
 * 返回是否**新建**了。新组件身上什么都没有，走 `SetBaseMeshFromGpuData` 的调用方要据此
 * 作废自己那份"基础网格快照已就绪"；走 `SetBaseMesh` 的不用管（它内部同一张网格早退）。
 */
bool EnsureInstancedComponent(AActor* Owner, TObjectPtr<UCSGpuInstancedMeshComponent>& Component);

/**
 * 数组版：把组件表调到 `Num` 个 —— 多的销毁（组件自己撤源、放显存），缺的补建。
 *
 * 返回**表是否变过**（增、删、补建都算）。调色板与组件按**下标**对齐，表一变基础网格快照
 * 就必须整批重建，少一个就全体错位。
 */
bool EnsureInstancedComponents(AActor* Owner, TArray<TObjectPtr<UCSGpuInstancedMeshComponent>>& Components, int32 Num);

// --- 实例源交接：九处逐字重复的状态机收敛到这里 -----------------------------------------
//
// 每一家产物（门框砖 / 砖石柱 / 藤三调色板 / 屋瓦 / 房子摆件 / 石阶 / 石子 / 裙边摆件 /
// 地被 N 物种）原本都在自己 `Ensure*` 的尾部手写同一段：判容量或包围盒变了没 -> 填
// `FCSGpuInstanceSourceGPU` -> 交接 -> 回写缓存。九份副本已经分叉出过三种**都不报错**的差异：
//   · 漏 `CustomData` => 材质里的 `Per Instance Custom Data` 恒读 0（藤叶不长、地被不弯）；
//   · 漏 `IsValid(组件)` => 蓝图重跑构造脚本之后是空指针解引用；
//   · 漏 `HasInstanceSourceGPU()` 兜底 => 缓存说"交接过了"而组件其实是空的，画面永远空白。
// 三种都只在特定操作序列下显形，而画面之外没有任何信号 —— 正是必须只留一份的那类代码。
//
// **留在调用方的只有真正逐家不同的那件事：包围盒怎么算。** 判据、填充、回写全在这里。

/**
 * 上一次交出去的东西：逐调色板的容量 + 那只包围盒。判"要不要重交"只看这两样。
 *
 * 撤实例源（`ClearInstanceSourceGPU`）与组件重建都必须连它一起 `Reset` —— 缓存说"交接过了"
 * 而组件是空的，下一趟就会被判成稳态而直接跳过，画面永远空白且没有任何报错。
 */
struct FHandoverCache
{
	TArray<uint32> Capacities;
	FBox LocalBounds = FBox(ForceInit);

	void Reset() { Capacities.Reset(); LocalBounds = FBox(ForceInit); }
};

/** 一个调色板在交接面上的样子。三种 buffers 结构都填得出来（见 `MakeHandoverSource`）。 */
struct FHandoverSource
{
	UCSGpuInstancedMeshComponent* Component = nullptr;
	TRefCountPtr<FRDGPooledBuffer> PackedInstances;
	TRefCountPtr<FRDGPooledBuffer> Counter;
	/**
	 * 可空，**只有真写它的生产者才填**（藤叶/花的生长相位、地被的弯曲幅度）。
	 *
	 * 填了的代价是剔除 pass 多跑一趟逐可见槽的拷贝（`bHasCustomData`）；而
	 * `FPaletteBuffers::CustomData` 是**恒分配**的，所以不写它的那几家填了也只是多抄一遍全零，
	 * 材质两条路都读到 0。差别只在那趟拷贝，所以这是个显式开关而不是"有就交"。
	 */
	TRefCountPtr<FRDGPooledBuffer> CustomData;
	uint32 Capacity = 0;

	/** 组件与源都齐了才交得出去。`IsValid` 那一半是给蓝图重跑构造脚本兜底的。 */
	bool IsReady() const
	{
		return ::IsValid(Component) && PackedInstances.IsValid() && Counter.IsValid() && Capacity > 0;
	}
};

enum class EHandoverMode : uint8
{
	/** 任一条没准备好就整批不交 —— 调色板与组件按**下标**对齐，交一半等于错位。 */
	AllOrNothing,
	/** 各判各的、各交各的。调色板之间互不相干时用（地被 N 物种、石阶 + 石子）。 */
	PerPalette,
};

enum class EHandoverResult : uint8
{
	UpToDate,     // 判据说不用交。**稳态下每一趟都该是它** —— 交接是阻塞的
	HandedOver,   // 真交了（至少一个）
	NotReady,     // 该交，但源或组件没准备好
};

struct FHandoverOptions
{
	EHandoverMode Mode = EHandoverMode::AllOrNothing;
	/**
	 * 非空 => 每次真交接打一行，内容是"谁 + 第几个 + 容量"。
	 * 这是阻塞的那一趟的唯一痕迹：稳态下一行都不该有，画路 / 拖尺寸时反复出现就说明上面
	 * 某个"没变"的判据其实每次都在变，而那正是交互期掉帧的来源。
	 */
	const TCHAR* LogLabel = nullptr;
	/**
	 * 这一批源承诺"改了就通知"（FCSGpuInstanceSourceGPU::bNotifiesChanges）：交出去的源带上这面旗子，
	 * 而判定"不用重新交接"的那些源在这里补一次 `NotifyInstanceSourceChanged()` —— 生产者每次重散 / 重打包之后
	 * 都会走到这个函数，所以"调了交接 = 刚改过 buffer"。换来的是 Nanite 路不再每帧重写 GPU-Scene。
	 *
	 * ⚠️ 只有**每一条改写 buffer 的路径后面都跟着这个函数**的生产者才能开。有哪条路径改了 buffer 却不来这里，
	 * Nanite 路的画面就停在上一次的样子，不报错。开之前把那一族所有写这批 buffer 的地方数一遍。
	 */
	bool bNotifiesChanges = false;
};

/**
 * 只涨不缩地把新盒子并进上次那只，返回并好的。
 *
 * 起因见本文件上面那段"交互期的稳态化"：包围盒直接由尺寸算出而交接阈值只有 1 cm，
 * 拖动中每一帧都会重走一次阻塞的 `SetInstanceSourceGPU`。
 *
 * `bFullRebuild` 时**不并**，让它重新收紧（否则一次误拉大就永久留着保守盒）。
 * 盒子由配置算死、本来就不随交互漂的那几家（石阶 / 地被）不需要调这一条。
 */
FBox MergeHandoverBounds(FBox LocalBounds, const FHandoverCache& Cache, bool bFullRebuild = false);

/** 从一个 `FPaletteBuffers` 填出交接面。`bWithCustomData` 的取舍见 `FHandoverSource::CustomData`。 */
FHandoverSource MakeHandoverSource(
	UCSGpuInstancedMeshComponent* Component, const FPaletteBuffers& Buffers, bool bWithCustomData);

/**
 * 判 + 交 + 回写。`Sources` 与 `Cache.Capacities` 按下标对齐（数不一致一律判成"要交"）。
 *
 * 交接本身是阻塞的（组件内部走 `SetStreamLayoutSync` + 立刻重建 render state），所以判据必须
 * 只看**由配置算得出来的量**：一旦掺进实例数据，包围盒就会随落笔漂移，每一笔都成立，
 * "交互期零阻塞"整条纪律当场退化掉。调用方算 `LocalBounds` 时守的就是这一条。
 */
EHandoverResult HandOverInstanceSources(
	TConstArrayView<FHandoverSource> Sources,
	const FBox& LocalBounds,
	FHandoverCache& Cache,
	const FHandoverOptions& Options = FHandoverOptions(),
	const UObject* LogOwner = nullptr);

/** 单调色板的直通壳子（门框砖 / 砖石柱 / 屋瓦 / 石阶 / 石子）。 */
EHandoverResult HandOverInstanceSource(
	const FHandoverSource& Source,
	const FBox& LocalBounds,
	FHandoverCache& Cache,
	const FHandoverOptions& Options = FHandoverOptions(),
	const UObject* LogOwner = nullptr);

}
