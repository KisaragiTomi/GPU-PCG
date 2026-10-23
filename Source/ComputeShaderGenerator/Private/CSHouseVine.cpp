#include "CSHouseVine.h"

#include "CSGpuMeshTypes.h"
#include "CSHouseProfile.h"
#include "ComputeShaderGenerateHelper.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/StaticMesh.h"
#include "GlobalShader.h"
#include "Rendering/ColorVertexBuffer.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"
#include "StaticMeshResources.h"

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSHouseVine_ 前缀。

constexpr int32 CSHouseVine_GroupSize = 64;

/**
 * 障碍处往两侧各扫几档倾角（候选数 = 2 × 这个数 + 1）。
 *
 * 4 的依据是两头都够：一段只有 26 cm，四分之一预算（≈ 8°）已经是 3.6 cm 级的横移差，
 * 再密纯属白扫；而再疏（比如 2 档）会跳过"窄墩之间只有近竖直那一条缝能过去"的情形 ——
 * 症状是两个洞之间那一条墙上的藤成片长不上去，而洞旁边的藤看着都正常。
 */
constexpr int32 CSHouseVine_TurnProbes = 4;

/**
 * 落点**往回收**的档位：先按整步扫一圈倾角，一档都不通就把这一步收短、再扫一圈。
 *
 * 为什么需要它：预算只有 `MaxTurn`（默认 0.55），而 `MaxLean` 是 1.15 ⇒ 一段之内**换不了
 * 横移的方向**。藤贴着洞缘、还带着 1.0 rad 朝洞里的倾角时，九个候选全都仍然朝洞里走 ⇒
 * 整根收尾。实测代价：三拱全开的保留率从 0.663 掉到 0.485（`VineRootEscapesHoles` 自己
 * AddInfo 的数）。收短一步等于给倾角**多争一两段的时间**转过来，而**倾角一个字不改**
 * ⇒ 转向上限逐位不受影响。
 *
 * 这一档是 TG 同构的：`intersect_ivy_growth_w_wall_segment` 撞上别的墙时并**不**改方向，
 * 它把落点挪到被撞那面墙的外皮上（`hit ± normal · 0.315 m`）—— 也就是**步长随障碍变短**。
 * TG 另有一条 `|新点 − 上一点| < 0.42 m = 2 × 步长` 的理智闸，说明它本来就容忍变长的步。
 *
 * ⚠️ 最短一档别低于 1/4：6.5 cm 的段在 `Bloat` 1.15 下几乎整段互穿，再短就只是白付实例。
 */
constexpr float CSHouseVine_StepFracs[] = { 1.0f, 0.5f, 0.25f };

/**
 * 第 `Probe` 个候选倾角。以 `Wish`（游走给出的目标）为圆心按 |偏移| 递增往两侧扫，
 * 全部夹在 `[Lo, Hi]` 内 —— **那对上下界就是"相邻段夹角 ≤ MaxTurn"的构造性保证**，
 * 调用方不必再检查一次。
 *
 * ⚠️ `Probe == 0` 必须**恒等于** `Wish`：无障碍时第 0 个候选就被接受，于是整条无障碍路径
 * 与"只有游走"的旧代码逐位相同。这一条是本轮改动能不碰既有几何断言的全部理由，别顺手改序。
 */
float CSHouseVine_ProbeAngle(int32 Probe, float Wish, float Lo, float Hi, float Step)
{
	const int32 Ring = (Probe + 1) / 2;                          // 0, 1, 1, 2, 2, 3, 3, ...
	const float Side = (Probe & 1) ? 1.0f : -1.0f;               // 先往 +，再往 −
	return FMath::Clamp(Wish + Side * float(Ring) * Step, Lo, Hi);
}

class FCSHouseVinePackCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSHouseVinePackCS);
	SHADER_USE_PARAMETER_STRUCT(FCSHouseVinePackCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, VineRecords)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RWVineInstances)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWVineCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RWVineCustomData)
		SHADER_PARAMETER(FMatrix44f, VineWorldToComponent)
		SHADER_PARAMETER(FVector3f, VineBaseSphereCentre)
		SHADER_PARAMETER(FVector3f, VineBlockSize)
		SHADER_PARAMETER(float, VineBaseSphereRadius)
		SHADER_PARAMETER(uint32, VineRecordCount)
		SHADER_PARAMETER(uint32, VineMaxInstances)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), CSHouseVine_GroupSize);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSHouseVinePackCS, "/Plugin/PCGPlugins/Shaders/Private/CSHousePack.usf", "PackHouseVineCS", SF_Compute);

/** 记录数组 → 上传用的 float4 平铺。布局与 `CSHousePack.usf` 里 PackHouseVineCS 的记录说明逐字对应。 */
void CSHouseVine_Flatten(const TArray<CSHouseVine::FRecord>& In, TArray<FVector4f>& Out)
{
	Out.Reset(In.Num() * 4);
	for (const CSHouseVine::FRecord& R : In)
	{
		Out.Add(FVector4f(R.WorldPos.X, R.WorldPos.Y, R.WorldPos.Z, R.LengthScale));
		Out.Add(FVector4f(R.Dir.X, R.Dir.Y, R.Dir.Z, R.Random01));
		Out.Add(FVector4f(R.Normal.X, R.Normal.Y, R.Normal.Z, R.SizeScale));
		// 第 4 行只用 xy（生长动画）。zw 留白 —— 别顺手塞别的：行宽是 kernel 里
		// `Index * 4u` 那个常量，两边必须一起改。
		Out.Add(FVector4f(R.SpawnTime, R.ArcLength, 0.0f, 0.0f));
	}
}
}

namespace CSHouseVine
{
float Hash01(uint32 H)
{
	H = ((H >> ((H >> 28) + 4u)) ^ H) * 277803737u;
	return float((H >> 22 ^ H) & 0xFFFFFFu) / float(0x1000000u);
}

uint32 IdentityHash(int32 EdgeIndex, int32 Strand, int32 Segment, uint32 Salt, int32 Seed)
{
	// 常数与 `CSGroundStairs.usf::CSStairs_CellSeed` 同一套 —— 两条路的"身份"口径要看得出同源。
	uint32 H = uint32(EdgeIndex + 1048576) * 0x9E3779B1u;
	H ^= uint32(Strand + 1048576) * 0x85EBCA77u;
	H ^= uint32(Segment + 1048576) * 0xC2B2AE3Du;
	H ^= Salt * 0x165667B1u;
	H ^= uint32(Seed) * 0x9E3779B9u;
	return H;
}

bool IsInsideOpening(const TArray<FCSWallOpening>& Openings, int32 EdgeIndex, float S, float Z, float Clearance)
{
	const float C = FMath::Max(Clearance, 0.0f);
	for (const FCSWallOpening& O : Openings)
	{
		if (O.EdgeIndex != EdgeIndex || !O.IsValid()) continue;
		// 拱脚线以下无下界（见头文件），所以洞底这条边界只能自己补。
		if (Z < O.Z0 - C) continue;

		// 把**洞**胀大再算场，而不是把场的结果外扩：拱胀大后拱脚线 Z1−HW 逐位不变、
		// 半圆半径 +C，恰好是那条曲线的等距外偏移；矩形与圆同理。
		FCSWallOpening Fat = O;
		Fat.Width = O.Width + 2.0f * C;
		Fat.Z0 = O.Z0 - C;
		Fat.Z1 = O.Z1 + C;
		const FCSOpeningClipField Field = CSHouse_ComputeClipField(Fat);
		if (!CSHouse_ClipKeeps(Field, Field.Eval(S, Z))) return true;
	}
	return false;
}

/**
 * 藤脚落在洞里时**沿墙脚侧移**到洞外最近的空位，返回是否找到。
 *
 * ⚠️ 第一档是"藤脚在洞里就整根不长"，代价实测：演示房子一开六个拱，两面长墙的根有一大半
 * 直接落在拱底下（拱是**落地**的，Z0 = 0 ⇒ 整个洞宽的墙脚都没了），成片秃掉。
 * 侧移读起来正是真藤在门洞旁边的样子 —— 门口两侧丛生、门洞里干净。
 *
 * 演示房子实测（六拱全开、其余开关按第一档摆平，2026-08-31）：**319 → 158 变成 319 → 198**，
 * 保留率 0.495 → 0.621。这一条是"拱附近变稀"唯一有效的解药 —— 换洞判据（外接矩形 → 解析
 * clip 场）在同一场景里只多出 0~2 段，别把两者搞混（见 `CSHouseVine::IsInsideOpening`）。
 *
 * 搜索半径限成一个藤位（`Slot`）：再远就等于把两根藤挤到同一个位置，视觉上是一簇假的密丛。
 */
static bool CSHouseVine_EscapeRoot(const TArray<FCSWallOpening>& Openings, int32 EdgeIndex,
	float& InOutS, float MinS, float MaxS, float Clearance, float Slot, float FirstStepZ)
{
	// 根部与**第一段的落点**都要在洞外：只查 z=0 的话，贴着拱脚安家的藤第一步就撞回洞里。
	auto Blocked = [&](float S)
	{
		return IsInsideOpening(Openings, EdgeIndex, S, 0.0f, Clearance)
			|| IsInsideOpening(Openings, EdgeIndex, S, FirstStepZ, Clearance);
	};
	if (!Blocked(InOutS)) return true;

	const float Step = FMath::Max(Slot * 0.125f, 2.0f);
	for (float Offset = Step; Offset <= Slot; Offset += Step)
	{
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const float S = FMath::Clamp(InOutS + (Side == 0 ? -Offset : Offset), MinS, MaxS);
			if (!Blocked(S)) { InOutS = S; return true; }
		}
	}
	return false;
}

void BuildPlan(const TArray<FWallStrip>& Strips, const TArray<FCSWallOpening>& Openings,
	const FParams& Params, FPlan& OutPlan)
{
	OutPlan.Reset();

	const float SegLen = FMath::Max(Params.SegmentLength, 1.0f);
	const float Spacing = FMath::Max(Params.StrandSpacing, 10.0f);
	const int32 MaxSeg = FMath::Clamp(Params.MaxSegments, 1, 256);

	// 转角跨墙要能找到"隔壁那面墙"。EdgeIndex 是环形的（`CSHouse_GetEdge` 的 0..3 绕一圈），
	// 模数取实际出现过的最大编号 + 1 —— 矩形房是 4，将来多边形 footprint 也自洽。
	// ⚠️ 线性扫而不是建 TMap：这是纯函数、拖动时每帧都要跑，四面墙的扫描比一次哈希表分配便宜。
	int32 EdgeModulus = 0;
	for (const FWallStrip& Strip : Strips) EdgeModulus = FMath::Max(EdgeModulus, Strip.EdgeIndex + 1);
	auto FindStrip = [&Strips](int32 EdgeIndex) -> const FWallStrip*
	{
		for (const FWallStrip& Strip : Strips) { if (Strip.EdgeIndex == EdgeIndex) return &Strip; }
		return nullptr;
	};
	// 藤脚离墙角留半个段长：贴着转角起藤会有一半的段穿进隔壁那面墙里。
	// ⚠️ 逐墙算 —— 跨墙以后墙长换了，沿用起点那面墙的余量会让藤在短墙上越界。
	auto MarginOf = [SegLen](const FWallStrip& W) { return FMath::Min(SegLen * 0.5f, W.Length * 0.25f); };

	for (const FWallStrip& Root : Strips)
	{
		if (Root.Length <= Spacing * 0.5f || Root.Height <= SegLen) continue;

		const int32 StrandCount = FMath::Max(1, FMath::RoundToInt(Root.Length / Spacing));
		const float Slot = Root.Length / float(StrandCount);
		const float RootMargin = MarginOf(Root);

		for (int32 StrandIdx = 0; StrandIdx < StrandCount; ++StrandIdx)
		{
			const FWallStrip* Wall = &Root;
			float Margin = RootMargin;
			// 起点：格中心 + 半格以内的抖动。**身份里没有位置** —— 见头文件那段。
			const float RootJitter = (Hash01(IdentityHash(Root.EdgeIndex, StrandIdx, -1, 3u, Params.Seed)) - 0.5f) * Slot;
			float S = FMath::Clamp((float(StrandIdx) + 0.5f) * Slot + RootJitter, Margin, Root.Length - Margin);
			float Z = 0.0f;
			// 初始倾角：左右各一半，别让整面墙的藤都朝同一边歪。
			float Angle = (Hash01(IdentityHash(Root.EdgeIndex, StrandIdx, -1, 5u, Params.Seed)) - 0.5f) * 2.0f * Params.MaxLean * 0.5f;

			// 藤脚在洞里（门口正下方）：先试着侧移出去，实在挪不开才放弃这根。
			if (!CSHouseVine_EscapeRoot(Openings, Wall->EdgeIndex, S, Margin, Wall->Length - Margin,
				Params.HoleClearance, Slot, SegLen)) continue;

			// 悬空不长藤（用户裁决 2026-09-06）。判据放在 `EscapeRoot` **之后** —— 侧移会改 S，
			// 按侧移前的位置判等于在问一个藤脚根本不会落的地方。
			if (Wall->SampleGroundGap(S) > Params.MaxGroundGap) continue;

			// 折线：枝的**唯一形状来源**（2026-09-06 裁决 1/2）。点存墙面参数坐标，与 TG 的
			// `WallCoord` 同构 —— 映射到世界是 `PackTubePath` 的最后一步，在这之前一切都天然贴墙。
			// ⚠️ 身份用**起点那面墙**，跨墙不改：与 `IdentityHash` 的调用口径必须逐字一致，
			// 否则 `SpawnTime` 的键会在藤拐弯的那一帧突变，整根藤重新长一遍。
			FStrand Strand;
			Strand.RootEdgeIndex = Root.EdgeIndex;
			Strand.StrandIndex = StrandIdx;
			Strand.RootKey = IdentityHash(Root.EdgeIndex, StrandIdx, -1, 7u, Params.Seed);
			Strand.Points.Add(FStrandPoint{ FVector2f(S, Z), Wall->EdgeIndex });
			Strand.Arc.Add(0.0f);

			// 沿藤累加的世界弧长，喂叶/花的生长相位（见 `FRecord::ArcLength` 的注释）。
			float StrandArc = 0.0f;
			// 这一根在 `OutPlan.Strands` 里的下标。**在这里取而不是在末尾** —— 末尾那句
			// `Add` 之后 Num() 已经加过一了，而且中途 `break` 出去的藤根本走不到那里。
			const int32 ThisStrandIndex = OutPlan.Strands.Num();

			for (int32 Segment = 0; Segment < MaxSeg; ++Segment)
			{
				// ⚠️ **身份一律用"起点那面墙"的编号**，不是当前所在的墙：跨墙以后用当前墙的话，
				// 同一根藤在拐弯前后会拿到两套随机，而且拐不拐弯本身又由随机决定 ⇒ 自指。
				// 身份 = (起点墙, 藤号, 段号, 佐料, 种子)，跨墙对它是透明的。
				const uint32 Id = IdentityHash(Root.EdgeIndex, StrandIdx, Segment, 11u, Params.Seed);

				// ── 这一段的倾角 ─────────────────────────────────────────────────────
				// 目标倾角 = 游走。`Wish` 与旧代码那句"加完扰动再夹一次"的结果**逐位相同**
				// （前提是 `MaxTurn >= Wander`，见 `FParams::MaxTurn`）⇒ 无障碍段的形状没变。
				const float PrevAngle = Angle;
				const float Budget = FMath::Max(Params.MaxTurn, 0.0f);
				const float Wish = FMath::Clamp(
					FMath::Clamp(PrevAngle + (Hash01(Id) - 0.5f) * 2.0f * Params.Wander,
						-Params.MaxLean, Params.MaxLean),
					PrevAngle - Budget, PrevAngle + Budget);

				// 长到墙顶就收。四坡屋顶下四面墙顶一律平在墙高（山墙那条随坡升高的剖面已随
				// 双坡结构一起删除），这条线以上是屋面，不是墙。
				// ⚠️ 判据用**目标**倾角而不是最终被接受的那一个：后者要等扫描跑完才知道，
				// 而 `cos` 在 [−MaxLean, MaxLean] 上恒正 ⇒ 用目标倾角判与旧行为逐位一致。
				if (Z + FMath::Cos(Wish) * SegLen > Wall->Height) break;

				// ── 障碍：在转向预算内**扫**一个能过去的倾角 ─────────────────────────
				// ⚠️ 旧代码在这里**改写**倾角（墙角 `Angle = -Angle`、洞里再 `Angle = 0`），
				// 那正是 2026-09-12 修掉的画面缺陷：倾角是相对竖直的**绝对**偏角，取反一次
				// 相邻段就折过 `2·|Angle|`（MaxLean 1.15 ⇒ 131.8°），藤在洞缘和墙角上"折断"
				// 式急拐；归零同样是一次离散跳变。
				// 反编译实证（`Docs/TinyGlade/Vine_ObstacleTurning.md`）：TG 的
				// `ivy_grower` 里**一次镜像/归零都没有** —— 方向来自
				// `IvyDirectionProposer::get_direction`，是位置的连续函数（两层 FastNoise 在
				// `pos / 2.8 m` 上取样、步长 0.21 m ⇒ 一步只走过 7.5% 个波长）。
				// 这里用"候选倾角**全部**夹在 [PrevAngle ± MaxTurn] 内"把同一条性质补回来：
				// 于是"相邻段夹角 ≤ MaxTurn"是**构造性**的，不靠调参（由 Vine.TurnRate 守着）。
				// 而"取能过去的**最小**转向"在几何上就是**沿障碍边缘滑行** —— 藤贴着洞缘绕上去，
				// 不是被弹开。
				//
				// 扫描是**两层**的：外层收步长（`CSHouseVine_StepFracs`）、内层扫倾角。
				// 顺序是"先整步试完所有倾角，再收短一步重试" —— 宁可少走一步，也不硬折。
				// 只有内层的话，带着 1.0 rad 朝洞里的倾角撞上洞缘时九个候选全都还朝洞里走，
				// 整根当场收尾：实测保留率 0.663 → 0.485。收步长不动倾角，所以上界不受影响。
				const float Lo = FMath::Max(PrevAngle - Budget, -Params.MaxLean);
				const float Hi = FMath::Min(PrevAngle + Budget, Params.MaxLean);
				const float ProbeStep = Budget / float(CSHouseVine_TurnProbes);

				// 跨墙那一掷**提到扫描之外**。`Hash01` 是纯函数、没有流式状态，所以提前掷一个字都
				// 不改；留在循环里反而会让"掷不掷"取决于扫到第几档 —— 那就成了自指
				// （与"身份不许含拐不拐弯"是同一条）。
				const bool bJumpDraw = Hash01(IdentityHash(Root.EdgeIndex, StrandIdx, Segment, 53u, Params.Seed))
					< Params.JumpChance;

				const FWallStrip* NextWall = nullptr;
				float NextS = 0.0f;
				float NextZ = 0.0f;
				for (int32 Frac = 0; Frac < int32(UE_ARRAY_COUNT(CSHouseVine_StepFracs)) && !NextWall; ++Frac)
				{
					const float CandLen = SegLen * CSHouseVine_StepFracs[Frac];
					for (int32 Probe = 0; Probe <= CSHouseVine_TurnProbes * 2 && !NextWall; ++Probe)
					{
						const float Cand = CSHouseVine_ProbeAngle(Probe, Wish, Lo, Hi, ProbeStep);
						const FWallStrip* CandWall = Wall;
						float CandS = S + FMath::Sin(Cand) * CandLen;
						const float CandZ = Z + FMath::Cos(Cand) * CandLen;

						// 墙顶也要逐候选查一次：|Cand| < |Wish| 时 `cos` 更大、落点比目标那一步更高。
						// ⚠️ 第 0 个候选恒等于 `Wish`，所以这一条对无障碍路径是恒真的（逐位不变）。
						if (CandZ > Wall->Height) continue;

						// 撞墙角。掷中跨墙（TG 的 `check_for_wall_jump`）⇒ 拐上相邻那面墙继续长，
						// 这是 TG 里最显眼的一条藤蔓行为；没掷中（或隔壁那面墙不在输入里，比如
						// 单墙单测）⇒ 这个候选算不通过、继续往回扫，一档都扫不出来才收尾。
						// ⚠️ **收尾是有意的，而且与 TG 同构**：TG 的 `ivy_grower` 在
						// `next.along < 0 || > wall.length` 时直接 return，而它的方向是位置的函数
						// ⇒ 下一帧提出来的还是同一个方向，等于永久停在墙端。原来那句镜像才是外加的。
						// 闭合的折线墙（样条墙一面整圈）没有墙端：S 越过两端就绕回来，接着长。
						if (Wall->bLoop && Wall->HasPath())
						{
							CandS = Wall->WrapS(CandS);
						}
						else if (CandS < Margin || CandS > Wall->Length - Margin)
						{
							if (!bJumpDraw) continue;
							const bool bForward = CandS > Wall->Length - Margin;
							const FWallStrip* Neighbour = EdgeModulus > 1
								? FindStrip((Wall->EdgeIndex + (bForward ? 1 : EdgeModulus - 1)) % EdgeModulus)
								: nullptr;
							if (!Neighbour || Neighbour == Wall
								|| Neighbour->Length <= MarginOf(*Neighbour) * 2.0f) continue;

							const float NeighbourMargin = MarginOf(*Neighbour);
							// 越角以后沿墙方向不翻身：原来"往前"到了新墙上仍然是"往前"（从墙头进），
							// 倾角保持不变，藤读起来是**连着**绕过转角的，而不是在转角处折了一下。
							const float Entry = bForward ? NeighbourMargin : Neighbour->Length - NeighbourMargin;
							const float Overshoot = bForward ? (CandS - (Wall->Length - Margin)) : (Margin - CandS);
							CandS = FMath::Clamp(bForward ? Entry + Overshoot : Entry - Overshoot,
								NeighbourMargin, Neighbour->Length - NeighbourMargin);
							CandWall = Neighbour;
						}

						// 墙洞。判据仍是与材质同源的那条 clip 场（见 `IsInsideOpening`）——
						// 换的只是"撞上以后怎么办"，判据本身一个字没动。
						if (IsInsideOpening(Openings, CandWall->EdgeIndex, CandS, CandZ, Params.HoleClearance)) continue;

						NextWall = CandWall;
						NextS = CandS;
						NextZ = CandZ;
						Angle = Cand;
					}
				}

				// 倾角 × 步长两层都扫遍了还是没有一条路 ⇒ 被夹在洞与墙角之间，继续绕就是原地打转。
				// ⚠️ 这一条与 TG 的 `ivy.blocked = true` 同义，但门槛高得多。TG 一撞洞就停，它
				// **有本钱停是因为播种是连续的、不是因为起点撒得高**（2026-09-12 反汇编
				// `ivy_spawner` 核实，此前"起点撒在整面墙上"那条推论是错的：起点取墙曲线上的
				// 点再走 `Vec2 → x0y` swizzle，**高度分量是字面的 0**，和本项目一样全在墙脚，
				// 沿墙的疏密由一道 2D 噪声门决定）。真正的差别是它每帧播 2–4 根、每根 20–70 点、
				// 吃满 40000 点的全局预算才停手，洞集合一变还会清 `blocked` 重试 ⇒ 停一根不要紧，
				// 新的会在墙脚别处长出来。本项目是提交时**一次性确定性**生成整套藤，停一根就是
				// 永久少一根 —— "撞上就停"实测把演示房子从 319 段打到 132、拱之间成片秃掉。
				// 所以是"先在预算内扫，扫不出来才停"。
				if (!NextWall) break;

				// 折线墙（样条墙）走 `PathToWorld`；平面墙保留原式，房子的藤逐位不变。
				const FVector A = Wall->HasPath() ? Wall->PathToWorld(S, Z, Params.StandOff)
					: Wall->Origin + Wall->U * S + Wall->Up * Z + Wall->N * Params.StandOff;
				const FVector B = NextWall->HasPath() ? NextWall->PathToWorld(NextS, NextZ, Params.StandOff)
					: NextWall->Origin + NextWall->U * NextS + NextWall->Up * NextZ + NextWall->N * Params.StandOff;
				const FVector Delta = B - A;
				const float Len = float(Delta.Size());
				// 跨墙那一段横跨两个墙平面，截面基取**两面墙法线的平均**：取任一面都会让那一段
				// 的截面斜插进另一面墙里，而转角恰恰是最显眼的位置。同墙时它逐位等于 Wall->N。
				// 折线墙的法线随 S 变，取两个端点处的那两根。
				const FVector NormalA = Wall->HasPath() ? Wall->PathNormal(S) : Wall->N;
				const FVector NormalB = NextWall->HasPath() ? NextWall->PathNormal(NextS) : NextWall->N;
				const FVector SegNormal = (NormalA + NormalB).GetSafeNormal(UE_SMALL_NUMBER, NormalA);
				if (Len > UE_KINDA_SMALL_NUMBER)
				{
					FRecord Rec;
					Rec.WorldPos = FVector3f(A);
					// ⚠️ 长度轴**故意胀大**（默认 1.15）：与门框砖同一条 TG 实证 ——
					// 相邻两段共用穿越点、缝是负的，段数一变只是穿插量微调；正缝则会在
					// 藤的每个折点露出一条亮缝，而折点恰恰是最显眼的地方。
					Rec.LengthScale = Len * FMath::Max(Params.Bloat, 1.0f);
					Rec.Dir = FVector3f(Delta / Len);
					Rec.Normal = FVector3f(SegNormal);
					Rec.Random01 = Hash01(IdentityHash(Root.EdgeIndex, StrandIdx, Segment, 71u, Params.Seed));
					// 越往上越细，藤才有"长出来"的方向感（TG 的 IvySegment 自带 start/end_thickness）。
					Rec.SizeScale = FMath::Lerp(1.0f, 0.55f, float(Segment) / float(MaxSeg));
					Rec.StrandIndex = ThisStrandIndex;
					Rec.ArcLength = StrandArc;
					OutPlan.Branch.Add(Rec);
				}

				const FVector Along = Delta.GetSafeNormal();
				if (!Along.IsNearlyZero()
					&& Hash01(IdentityHash(Root.EdgeIndex, StrandIdx, Segment, 23u, Params.Seed)) < Params.LeafChance)
				{
					const float Side = Hash01(IdentityHash(Root.EdgeIndex, StrandIdx, Segment, 31u, Params.Seed)) < 0.5f ? -1.0f : 1.0f;
					const float Spread = 0.6f + 0.8f * Hash01(IdentityHash(Root.EdgeIndex, StrandIdx, Segment, 37u, Params.Seed));
					// 叶子从段的中点斜着支出去：方向 = 段方向绕墙面法线转 ±(35°..80°)。
					const float Turn = Side * Spread;
					const FVector Sideways = FVector::CrossProduct(SegNormal, Along).GetSafeNormal();
					const FVector LeafDir = (Along * FMath::Cos(Turn) + Sideways * FMath::Sin(Turn)).GetSafeNormal();
					if (!LeafDir.IsNearlyZero())
					{
						FRecord Leaf;
						Leaf.WorldPos = FVector3f((A + B) * 0.5 + SegNormal * (Params.StandOff * 0.5));
						const float Jitter = 1.0f + (Hash01(IdentityHash(Root.EdgeIndex, StrandIdx, Segment, 43u, Params.Seed)) - 0.5f)
							* 2.0f * Params.LeafSizeJitter;
						Leaf.LengthScale = FMath::Max(Params.LeafSize * Jitter, 1.0f);
						Leaf.Dir = FVector3f(LeafDir);
						Leaf.Normal = FVector3f(SegNormal);
						Leaf.Random01 = Hash01(IdentityHash(Root.EdgeIndex, StrandIdx, Segment, 97u, Params.Seed));
						Leaf.SizeScale = Jitter;
						Leaf.StrandIndex = ThisStrandIndex;
						// 叶挂在段的**中点**上，弧长取半段（记录的 WorldPos 也是中点）。
						Leaf.ArcLength = StrandArc + Len * 0.5f;
						OutPlan.Leaf.Add(Leaf);
					}
				}

				// 花（TG 的 `ivy_flower`）。只开在藤的上半截：TG 的花挂在**长成了的**藤上，
				// 而且贴着地面那一圈会被地形与杂物挡住，纯白付实例。
				const int32 FlowerFrom = FMath::CeilToInt(float(MaxSeg) * FMath::Clamp(Params.FlowerFromFrac, 0.0f, 1.0f));
				if (Segment >= FlowerFrom && !Along.IsNearlyZero()
					&& Hash01(IdentityHash(Root.EdgeIndex, StrandIdx, Segment, 59u, Params.Seed)) < Params.FlowerChance)
				{
					// 花簇朝**外上方**张开（`ivy_flower` 实测底面在 Z=0、簇沿自身 +Z 张开）。
					// ⚠️ 基准向量给 `Along` 而不是墙法线：kernel 用 cross(Normal, Dir) 搭面内轴，
					// 而花的 Dir 本身就以墙法线为主 ⇒ 传墙法线会近似共线，被 kernel 的退化判据丢掉，
					// 症状是"花一朵都不出现"而 counter 却是对的。
					const float Tilt = (Hash01(IdentityHash(Root.EdgeIndex, StrandIdx, Segment, 67u, Params.Seed)) - 0.5f) * 0.7f;
					const FVector Sideways = FVector::CrossProduct(SegNormal, Along).GetSafeNormal();
					const FVector FlowerDir = (SegNormal * 0.85 + FVector(0.0, 0.0, 0.45) + Sideways * Tilt).GetSafeNormal();
					if (!FlowerDir.IsNearlyZero())
					{
						FRecord Flower;
						Flower.WorldPos = FVector3f(B + SegNormal * (Params.StandOff * 0.5));
						const float Jitter = 1.0f + (Hash01(IdentityHash(Root.EdgeIndex, StrandIdx, Segment, 89u, Params.Seed)) - 0.5f)
							* 2.0f * Params.LeafSizeJitter;
						// 高 = 宽 × 实测高宽比：`BlockSize` 只把 xy 钉成 FlowerSize 的方截面，
						// z 得由记录自己说，否则花被拉成柱子（见 FParams::FlowerAspect）。
						Flower.LengthScale = FMath::Max(Params.FlowerSize * Params.FlowerAspect * Jitter, 1.0f);
						Flower.Dir = FVector3f(FlowerDir);
						Flower.Normal = FVector3f(Along);
						Flower.Random01 = Hash01(IdentityHash(Root.EdgeIndex, StrandIdx, Segment, 101u, Params.Seed));
						Flower.SizeScale = Jitter;
						Flower.StrandIndex = ThisStrandIndex;
						// 花挂在段的**终点** B 上，所以吃满整段。
						Flower.ArcLength = StrandArc + Len;
						OutPlan.Flower.Add(Flower);
					}
				}

				StrandArc += Len;

				// 折线推进。判据用 `Len`（**世界**距离）而不是 (S,Z) 的差：跨墙时 S 会跳到隔壁墙的
				// 坐标里，两者的差没有几何意义，拿它判重会把转角那一段误判成退化段丢掉。
				if (Len > UE_KINDA_SMALL_NUMBER)
				{
					Strand.Points.Add(FStrandPoint{ FVector2f(NextS, NextZ), NextWall->EdgeIndex });
					Strand.Arc.Add(StrandArc + Len);
				}

				S = NextS;
				Z = NextZ;
				if (NextWall != Wall)
				{
					Wall = NextWall;
					Margin = MarginOf(*Wall);
				}
			}

			// 一个点连不成管子。**门限是 2 而不是 1**，与 SC 那条路的 `Count < 2u` 同一个理由：
			// 下游按"点数 − 1"算段数，单点线会排出 0 段却仍占着 Meta 的槽位。
			if (Strand.Points.Num() >= 2) OutPlan.Strands.Add(MoveTemp(Strand));
		}
	}
}

void PackTubePath(const TArray<FWallStrip>& Strips, const FPlan& Plan, const FParams& Params,
	int32 Subdivide, float CircleScale, TArrayView<const float> StrandSpawnTimes, FTubePath& OutPath)
{
	OutPath.Reset();
	if (Plan.Strands.IsEmpty()) return;

	auto FindStrip = [&Strips](int32 EdgeIndex) -> const FWallStrip*
	{
		for (const FWallStrip& Strip : Strips) { if (Strip.EdgeIndex == EdgeIndex) return &Strip; }
		return nullptr;
	};
	// (S, Z) + 所在墙 → 世界。与 `BuildPlan` 里算 A / B 的那两行逐字同源 —— 两处写法一旦分岔，
	// 症状是"折线和记录对不上"，而两边各自都自洽。
	auto ToWorld = [&FindStrip, &Params](const FStrandPoint& P) -> FVector
	{
		const FWallStrip* W = FindStrip(P.EdgeIndex);
		if (!W) return FVector::ZeroVector;
		if (W->HasPath()) return W->PathToWorld(P.WallSZ.X, P.WallSZ.Y, Params.StandOff);
		return W->Origin + W->U * double(P.WallSZ.X) + W->Up * double(P.WallSZ.Y) + W->N * Params.StandOff;
	};

	const int32 Sub = FMath::Clamp(Subdivide, 0, 8);
	// Pass C 的环半径 = `10 * CircleScale * Points[i].w`，所以这里反解出 w。
	const float RadiusToScale = 1.0f / FMath::Max(10.0f * CircleScale, UE_KINDA_SMALL_NUMBER);
	const float TipRadius = FMath::Max(Params.Thickness * 0.5f, 0.01f);

	TArray<FVector> World;
	TArray<int32> Edges;
	for (int32 StrandIndex = 0; StrandIndex < Plan.Strands.Num(); ++StrandIndex)
	{
		const FStrand& Strand = Plan.Strands[StrandIndex];
		const int32 RawCount = Strand.Points.Num();
		if (RawCount < 2 || Strand.Arc.Num() != RawCount) continue;

		World.Reset(RawCount);
		Edges.Reset(RawCount);
		for (const FStrandPoint& P : Strand.Points) { World.Add(ToWorld(P)); Edges.Add(P.EdgeIndex); }

		// ── 细分 ────────────────────────────────────────────────────────────────
		// Catmull-Rom 是**仿射组合**（权重和为 1），所以同一面墙上的四个共面控制点插出来的点
		// 必然还在那个平面里 —— 在世界空间做细分不会把线拽离墙面，不必回到 (S,Z) 里算。
		// ⚠️ **但跨墙段必须退回线性**：那四个控制点不共面，混着算会把线甩进墙体内部。
		// 线性插值在转角处就是切一刀，正是想要的圆角。
		const int32 Base = OutPath.Points.Num();
		TArray<FVector> Dense;
		Dense.Reserve(RawCount + (RawCount - 1) * Sub);
		Dense.Add(World[0]);
		// 与 Dense 等长的**生长弧长**：从折线自带的 Arc 线性插值，而不是重新累加细分后的
		// 长度（见 `FStrand::Arc` 的注释 —— 一把尺，别混）。
		TArray<float> GrowArc;
		GrowArc.Reserve(RawCount + (RawCount - 1) * Sub);
		GrowArc.Add(0.0f);
		for (int32 i = 0; i + 1 < RawCount; ++i)
		{
			const FVector& P1 = World[i];
			const FVector& P2 = World[i + 1];
			const float ArcA = Strand.Arc[i];
			const float ArcB = Strand.Arc[i + 1];
			const bool bSameWall = Edges[i] == Edges[i + 1];
			for (int32 s = 1; s <= Sub; ++s)
			{
				const double T = double(s) / double(Sub + 1);
				if (!bSameWall)
				{
					Dense.Add(FMath::Lerp(P1, P2, T));
					GrowArc.Add(FMath::Lerp(ArcA, ArcB, float(T)));
					continue;
				}
				// 端点处把控制点夹回自身（TG 的折线没有环，两端不外推）。同墙才取邻居，
				// 否则 P0/P3 会来自另一面墙、把共面性破坏掉 —— 那正是上面警告的那种线。
				const FVector& P0 = (i > 0 && Edges[i - 1] == Edges[i]) ? World[i - 1] : P1;
				const FVector& P3 = (i + 2 < RawCount && Edges[i + 2] == Edges[i + 1]) ? World[i + 2] : P2;
				const double T2 = T * T, T3 = T2 * T;
				Dense.Add(0.5 * ((2.0 * P1) + (-P0 + P2) * T
					+ (2.0 * P0 - 5.0 * P1 + 4.0 * P2 - P3) * T2
					+ (-P0 + 3.0 * P1 - 3.0 * P2 + P3) * T3));
				GrowArc.Add(FMath::Lerp(ArcA, ArcB, float(T)));
			}
			Dense.Add(P2);
			GrowArc.Add(ArcB);
		}

		// 主体弧长（**收尖之前**量）—— 上面那条锥度曲线的归一化分母。
		float MainArc = 0.0f;
		for (int32 i = 1; i < Dense.Num(); ++i) MainArc += float((Dense[i] - Dense[i - 1]).Size());
		MainArc = FMath::Max(MainArc, UE_KINDA_SMALL_NUMBER);

		// ── 梢部收尖 ────────────────────────────────────────────────────────────
		// 主锥度只收到 0.55，梢仍是一圈**开口**的管腔 —— 从侧上方看正对着那个洞。
		// 沿最后一段的方向再续两环，半径收到 0.45 / 0.10，视觉上就是一个自然的尖。
		//
		// ⚠️ **不收到 0**：整圈环顶点收到同一点会退化成零面积三角形、法线变 NaN
		//（与材质里 `VineThickenStart` 不许取 0 是同一条）。0.10 剩下的孔径不到半毫米。
		// ⚠️ **只封梢不封根**：根埋在墙脚下，封了看不见，白付两环。
		{
			const int32 Last = Dense.Num() - 1;
			if (Last >= 1)
			{
				const FVector Dir = (Dense[Last] - Dense[Last - 1]).GetSafeNormal();
				if (!Dir.IsNearlyZero())
				{
					// 再往前续一小段收口。**收紧的主体不在这两环上**（那是下面按弧长的
					// 平滑衰减干的），这里只是让最末端真的闭合而不是停在一个开口上。
					const double TipR = double(TipRadius) * 0.55;
					Dense.Add(Dense[Last] + Dir * (TipR * 0.8));
					Dense.Add(Dense[Last] + Dir * (TipR * 1.6));
					// 续的两环也要有弧长，否则生长前沿扫到尖上就没数了（读到 0 ⇒ 尖端恒可见）。
					const float EndArc = GrowArc.Last();
					GrowArc.Add(EndArc + float(TipR * 0.8));
					GrowArc.Add(EndArc + float(TipR * 1.6));
				}
			}
		}

		// ── 弧长（细分后的世界折线上累加）───────────────────────────────────────
		const int32 Count = Dense.Num();
		TArray<float> Arc;
		Arc.SetNumUninitialized(Count);
		Arc[0] = 0.0f;
		for (int32 i = 1; i < Count; ++i) Arc[i] = Arc[i - 1] + float((Dense[i] - Dense[i - 1]).Size());
		// 收紧以**最末点**为基准（含上面续的两环），不是主体末点：否则那两环会落在
		// 衰减曲线之外，重新鼓回主锥度，收口白做。
		const float TipEndArc = Arc[Count - 1];

		const float SpawnTime = StrandSpawnTimes.IsValidIndex(StrandIndex) ? StrandSpawnTimes[StrandIndex] : 0.0f;

		for (int32 i = 0; i < Count; ++i)
		{
			// 锥度：与实例路那条 `Lerp(1.0, 0.55, Segment / MaxSeg)` 同一条曲线，只是自变量
			// 换成了归一化弧长（细分之后段号已经不是均匀的了，用它会让锥度随细分次数变）。
			// ⚠️ 归一化**用的是收尖之前的主体长度**：拿含尖的 TotalArc 去归一，主体的锥度会被
			// 那两环稀释（收得比 0.55 早一点点），细分次数一变还会漂。
			const float MainTaper = FMath::Lerp(1.0f, 0.55f, FMath::Min(Arc[i] / MainArc, 1.0f));

			// 梢部收紧：从梢往回 `TipTaperLength` 的一段里，把主锥度平滑压到 `TipTaperMin`。
			// ⚠️ **用 smoothstep 而不是线性**：线性衰减在"开始收"的那一点上是折角，
			// 而那一点正落在藤中段最显眼的位置；smoothstep 两端一阶导为零，接得上。
			// ⚠️ 短藤要夹：`TipTaperLength` 比整根还长时，分母取整根长度，否则整根都在收，
			// 连根部都变细了 —— 症状是"矮墙上的藤整条像根须"。
			const float TipSpan = FMath::Max(FMath::Min(Params.TipTaperLength, MainArc), UE_KINDA_SMALL_NUMBER);
			const float ToTip = FMath::Clamp((TipEndArc - Arc[i]) / TipSpan, 0.0f, 1.0f);
			const float Smooth = ToTip * ToTip * (3.0f - 2.0f * ToTip);
			const float Taper = MainTaper * FMath::Lerp(FMath::Max(Params.TipTaperMin, 0.01f), 1.0f, Smooth);
			OutPath.Points.Add(FVector4f(FVector3f(Dense[i]), TipRadius * Taper * RadiusToScale));
			OutPath.Axes.Add(FVector4f(0.0f, 0.0f, 0.0f, 0.0f));   // 见 FTubePath::Axes 的注释：必须是零
			const int32 Prev = Base + FMath::Max(i - 1, 0);
			const int32 Next = Base + FMath::Min(i + 1, Count - 1);
			OutPath.PointMeta.Add(FIntVector4(Prev, Next, Base, Count));
			OutPath.Growth.Add(FVector2f(SpawnTime, GrowArc.IsValidIndex(i) ? GrowArc[i] : Arc[i]));
			if (i + 1 < Count) OutPath.SegmentMeta.Add(FIntVector4(Base + i, Base + i + 1, 0, 0));
		}
	}
}

bool BuildBaseMesh(UStaticMesh* Source, int32 LengthAxis, FCSGpuMeshCPUData& Out)
{
	Out = FCSGpuMeshCPUData();
	const FStaticMeshRenderData* RenderData = Source ? Source->GetRenderData() : nullptr;
	if (!RenderData || RenderData->LODResources.Num() == 0) return false;

	const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	const uint32 NumVerts = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
	if (NumVerts < 3) return false;

	TArray<uint32> Indices;
	LOD.IndexBuffer.GetCopy(Indices);
	if (Indices.Num() < 3) return false;

	// 换轴：把源网格的长度轴旋到 +Z（kernel 的基约定只认 +Z）。绕单轴 90°，纯置换 + 变号，
	// 所以不引入任何数值误差，也不需要重算绕序（行列式恒 +1）。
	auto ToZ = [LengthAxis](const FVector3f& V)
	{
		switch (LengthAxis)
		{
		case 0: return FVector3f(-V.Z, V.Y, V.X);   // +X → +Z
		case 1: return FVector3f(V.X, -V.Z, V.Y);   // +Y → +Z
		default: return V;                          // 已经是 +Z
		}
	};

	Out.Positions.Reserve(int32(NumVerts));
	for (uint32 V = 0; V < NumVerts; ++V)
	{
		Out.Positions.Add(ToZ(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(V)));
	}
	Out.Indices = MoveTemp(Indices);
	Out.SourceSpace = FCSGpuMeshCPUData::ESpace::ComponentLocal;
	Out.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;
	Out.NumTexCoordChannels = 1;

	const uint32 NumTangentVerts = LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumVertices();
	const bool bHasTangentStream = NumTangentVerts == NumVerts;
	const bool bHasUVStream = bHasTangentStream && LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() > 0;

	// ⚠️ **有流不等于有数据**：`ivy_branch` 的顶点流只有位置，导入器仍然给了一条切线流，
	// 里面全是零长法线；直接用它的后果是整条藤黑成剪影。所以判据是"读出来的东西合不合法"，
	// 不是"这条流在不在"。
	bool bNormalsUsable = bHasTangentStream;
	bool bUVsUsable = bHasUVStream;
	if (bHasTangentStream)
	{
		bool bAnyNonZeroUV = false;
		for (uint32 V = 0; V < NumVerts && bNormalsUsable; ++V)
		{
			if (LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(V).Size() < 0.5f) bNormalsUsable = false;
		}
		for (uint32 V = 0; bHasUVStream && V < NumVerts && !bAnyNonZeroUV; ++V)
		{
			bAnyNonZeroUV |= !LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(V, 0).IsNearlyZero();
		}
		bUVsUsable = bHasUVStream && bAnyNonZeroUV;
	}

	Out.Normals.SetNumZeroed(int32(NumVerts));
	Out.Tangents.SetNumZeroed(int32(NumVerts));
	Out.TexCoords().SetNumZeroed(int32(NumVerts));
	// 顶点色：**有就搬，没有才退白**。藤那两张都没有颜色流，所以这一段对 D13 是恒等的；
	// 它是为 **D12 的 clutter** 加的 —— TG 的杂物把颜色全烘在顶点流里（`Content/HouseTest/TinyGladeAsset/Textures/`
	// 里没有一张 clutter 贴图，459 张贴图与 459 个 MI 一一对应、clutter 一个都不在其中）。
	// 丢掉它的症状是整批摆件变成同一种平色 —— 看着像"贴图没接上"，实际上本就没有贴图。
	// ⚠️ 搬过来的值是**线性域的、而且很暗**（直接从源 GLB 量：`barrel` 均值 .041、
	// `firewood` .233）。消费端把它当 base color 前必须先提亮，否则是一排黑影 ——
	// 这一条的供给侧写在 `Scripts/TinyGladeMakeDecorMaterial.py` 的 `COLOR_BOOST`。
	// ⚙ 归一化用 /255 而不是 `FLinearColor(FColor)`：`SetBaseMeshFromGpuData` 另一端是
	// `ToFColor(false)`（不做 sRGB 转换），两边必须同口径才能逐字节往返，
	// 也才与直接走 `SetBaseMesh` 那条路的结果一致。
	const bool bHasColorStream = LOD.VertexBuffers.ColorVertexBuffer.GetNumVertices() == NumVerts;
	if (bHasColorStream)
	{
		Out.Colors.SetNumUninitialized(int32(NumVerts));
		for (uint32 V = 0; V < NumVerts; ++V)
		{
			const FColor C = LOD.VertexBuffers.ColorVertexBuffer.VertexColor(V);
			Out.Colors[int32(V)] = FVector4f(float(C.R), float(C.G), float(C.B), float(C.A)) / 255.0f;
		}
	}
	else
	{
		Out.Colors.Init(FVector4f(1.0f, 1.0f, 1.0f, 1.0f), int32(NumVerts));
	}

	if (bNormalsUsable)
	{
		for (uint32 V = 0; V < NumVerts; ++V)
		{
			Out.Normals[int32(V)] = ToZ(FVector3f(LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(V)));
			Out.Tangents[int32(V)] = ToZ(LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentX(V));
		}
	}
	else
	{
		// 相邻面法线累加。`ivy_branch` 是 12 顶点 / 6 三角的开口三棱管、顶点不共享 ⇒
		// 每个顶点只被一个面引用，累加出来正好是**逐面平法线**（棱柱的正确答案，不是近似）。
		for (int32 I = 0; I + 2 < Out.Indices.Num(); I += 3)
		{
			const int32 A = int32(Out.Indices[I]), B = int32(Out.Indices[I + 1]), C = int32(Out.Indices[I + 2]);
			if (!Out.Positions.IsValidIndex(A) || !Out.Positions.IsValidIndex(B) || !Out.Positions.IsValidIndex(C)) continue;
			const FVector3f FaceN = FVector3f::CrossProduct(Out.Positions[B] - Out.Positions[A], Out.Positions[C] - Out.Positions[A]);
			Out.Normals[A] += FaceN;
			Out.Normals[B] += FaceN;
			Out.Normals[C] += FaceN;
		}
		for (int32 V = 0; V < Out.Normals.Num(); ++V)
		{
			// 退化面（零面积）留下的零法线退回径向朝外 —— 管壁的外法线本来就是径向。
			const FVector3f Radial = FVector3f(Out.Positions[V].X, Out.Positions[V].Y, 0.0f);
			Out.Normals[V] = Out.Normals[V].GetSafeNormal(UE_SMALL_NUMBER, Radial.GetSafeNormal(UE_SMALL_NUMBER, FVector3f(0, 0, 1)));
			Out.Tangents[V] = FVector3f::CrossProduct(FVector3f(0, 0, 1), Out.Normals[V]).GetSafeNormal(UE_SMALL_NUMBER, FVector3f(1, 0, 0));
		}
	}

	if (bUVsUsable)
	{
		for (uint32 V = 0; V < NumVerts; ++V) Out.TexCoords()[int32(V)] = LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(V, 0);
	}
	else
	{
		// 绕长度轴的柱面展开。**必须是柱面而不是平面投影**：实例被非均匀缩放
		// （截面 9 cm、长度 26 cm 起），平面 UV 会把贴图沿长度轴拉成条。
		FBox3f Bounds(ForceInit);
		for (const FVector3f& P : Out.Positions) Bounds += P;
		const float Span = FMath::Max(Bounds.Max.Z - Bounds.Min.Z, UE_SMALL_NUMBER);
		for (int32 V = 0; V < Out.Positions.Num(); ++V)
		{
			const FVector3f& P = Out.Positions[V];
			Out.TexCoords()[V] = FVector2f(
				FMath::Atan2(P.Y, P.X) / (2.0f * UE_PI) + 0.5f,
				(P.Z - Bounds.Min.Z) / Span);
		}
	}

	Out.BinormalSigns.Init(1.0f, int32(NumVerts));

	// 材质段与材质槽照抄资产 LOD0（2026-09-15）：实例组件没设整体覆盖材质时按它们逐段画资产自己的材质。
	// 以前这里一律槽 0、不带材质表，于是摆件这类多材质资产（`barrel` = 贴图木头 + 顶点色铁箍）只能整张盖一张
	// `M_TinyGladeDecor`。索引没有重排，段在索引里本来就是连续的，`Indices` 与改动前逐位相同。
	Out.TriangleMaterialSlots.Init(0, Out.Indices.Num() / 3);
	for (const FStaticMeshSection& Section : LOD.Sections)
	{
		const int32 FirstTriangle = int32(Section.FirstIndex / 3u);
		const int32 EndTriangle = FMath::Min(FirstTriangle + int32(Section.NumTriangles), Out.TriangleMaterialSlots.Num());
		for (int32 Triangle = FirstTriangle; Triangle < EndTriangle; ++Triangle) Out.TriangleMaterialSlots[Triangle] = FMath::Max(Section.MaterialIndex, 0);
	}
	for (const FStaticMaterial& Material : Source->GetStaticMaterials()) Out.Materials.Add(Material.MaterialInterface);
	return true;
}

bool Pack(const FPlan& Plan, const TArray<CSShaperSteps::FPaletteBuffers>& Palettes, const FMatrix44f& WorldToComponent)
{
	if (Palettes.Num() < Palette_Num) return false;

	TArray<TArray<FVector4f>> Flat;
	Flat.SetNum(Palette_Num);
	CSHouseVine_Flatten(Plan.Branch, Flat[Palette_Branch]);
	CSHouseVine_Flatten(Plan.Leaf, Flat[Palette_Leaf]);
	CSHouseVine_Flatten(Plan.Flower, Flat[Palette_Flower]);

	// ⚠️ 判据是"有没有可写的调色板"，**不是**"有没有记录"：全空时也必须走一趟，
	// 否则 counter 停在上一次的值 —— 症状是"藤已经排不出来了但画面上还在"，
	// 而且只在从有到无那一次出现（空表分支自己会 AddClearUAVPass）。
	bool bAny = false;
	for (int32 Index = 0; Index < Palette_Num; ++Index) bAny |= Palettes[Index].IsValid();
	if (!bAny) return false;

	// 渲染线程一趟做完。Work 按**值**捕获（`TRefCountPtr` 拷贝即加引用），录完直接 return。
	ENQUEUE_RENDER_COMMAND(CSHouseVinePack)(
		[Records = MoveTemp(Flat), Work = Palettes, WorldToComponent]
		(FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSHouseVine.Pack"));

			for (int32 Index = 0; Index < Palette_Num; ++Index)
			{
				// CustomData 也要在 —— 它是 `FPaletteBuffers::IsValid()` 之外新加的一条，
				// 老的常驻集可能没有它，那时 RegisterExternalBuffer 会拿到空指针。
				if (!Work[Index].IsValid() || !Work[Index].CustomData.IsValid()) continue;
				FRDGBufferRef PackedRef = GraphBuilder.RegisterExternalBuffer(Work[Index].PackedInstances, TEXT("CSHouseVine.PackedInstances"));
				FRDGBufferRef CounterRef = GraphBuilder.RegisterExternalBuffer(Work[Index].Counter, TEXT("CSHouseVine.Counter"));
				FRDGBufferRef CustomRef = GraphBuilder.RegisterExternalBuffer(Work[Index].CustomData, TEXT("CSHouseVine.CustomData"));
				FRDGBufferUAVRef PackedUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(PackedRef, PF_A32B32G32R32F));
				FRDGBufferUAVRef CounterUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CounterRef, PF_R32_UINT));
				FRDGBufferUAVRef CustomUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CustomRef, PF_R32_FLOAT));

				const TArray<FVector4f>& Rows = Records[Index];
				const uint32 Count = uint32(Rows.Num() / 4);
				// 空调色板必须显式清零：kernel 一个线程都不跑的话 counter 会留着上一次的值，
				// 症状是"藤已经没了但画面上还在"，而且只在从有到无那一次出现。
				if (Count == 0)
				{
					AddClearUAVPass(GraphBuilder, CounterUAV, 0u);
					GraphBuilder.SetBufferAccessFinal(CustomRef, ERHIAccess::SRVMask);
					GraphBuilder.SetBufferAccessFinal(PackedRef, ERHIAccess::SRVMask);
					GraphBuilder.SetBufferAccessFinal(CounterRef, ERHIAccess::SRVMask);
					continue;
				}

				CSHelper::FRDGStructuredBufferRefs RecordRefs = CSHelper::CreateUploadedStructuredBuffer<FVector4f>(
					GraphBuilder, Rows, TEXT("CSHouseVine.Records"), false, true);
				if (!RecordRefs.SRV) continue;

				FCSHouseVinePackCS::FParameters* PassParams = GraphBuilder.AllocParameters<FCSHouseVinePackCS::FParameters>();
				PassParams->VineRecords = RecordRefs.SRV;
				PassParams->RWVineInstances = PackedUAV;
				PassParams->RWVineCounter = CounterUAV;
				PassParams->RWVineCustomData = CustomUAV;
				PassParams->VineWorldToComponent = WorldToComponent;
				PassParams->VineBaseSphereCentre = Work[Index].BaseSphereCentre;
				PassParams->VineBlockSize = Work[Index].BlockSize;
				PassParams->VineBaseSphereRadius = Work[Index].BaseSphereRadius;
				PassParams->VineRecordCount = Count;
				PassParams->VineMaxInstances = Work[Index].Capacity;

				TShaderMapRef<FCSHouseVinePackCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSHouseVine.Pack"), Shader, PassParams,
					FComputeShaderUtils::GetGroupCount(int32(Count), CSHouseVine_GroupSize));

				// 剔除 pass 只读这两个 buffer，且明说不负责恢复它们的状态 —— producer 自己留在 SRVMask。
				GraphBuilder.SetBufferAccessFinal(PackedRef, ERHIAccess::SRVMask);
				GraphBuilder.SetBufferAccessFinal(CounterRef, ERHIAccess::SRVMask);
				GraphBuilder.SetBufferAccessFinal(CustomRef, ERHIAccess::SRVMask);
			}

			GraphBuilder.Execute();
		});

	return true;
}
void ResolveSpawnTimes(TMap<uint32, FStrandHistory>& History, const FPlan& Plan,
	float Now, float GrowSpeed, bool bGrowOnLoad, TArray<float>& OutSpawnTimes)
{
	OutSpawnTimes.Reset(Plan.Strands.Num());

	const float Speed = FMath::Max(GrowSpeed, 1.0f);
	// 从没长过的哨兵：足够早，材质算出来的前沿远超任何弧长 ⇒ 第一帧就是长成的。
	const float GrownSentinel = -1.0e6f;

	TMap<uint32, FStrandHistory> NextHistory;
	NextHistory.Reserve(Plan.Strands.Num());

	for (const FStrand& Strand : Plan.Strands)
	{
		const FStrandHistory* Prev = History.Find(Strand.RootKey);
		float Spawn;

		if (!Prev)
		{
			// 全新的一根（第一次生成、或跨过藤位间距新增的那根）。
			Spawn = bGrowOnLoad ? Now : GrownSentinel;
		}
		else
		{
			// 第一个不同的点。⚠️ 逐点比较用**墙面参数坐标 + 所在墙**，容差取 0.5 cm ——
			// 比它更严会把浮点噪声判成"变了"，于是每次重求值都重新长一遍。
			int32 Diverge = 0;
			const int32 Common = FMath::Min(Prev->PointsSZ.Num(), Strand.Points.Num());
			while (Diverge < Common)
			{
				const FStrandPoint& P = Strand.Points[Diverge];
				const bool bSame = Prev->Edges.IsValidIndex(Diverge)
					&& Prev->Edges[Diverge] == P.EdgeIndex
					&& (Prev->PointsSZ[Diverge] - P.WallSZ).IsNearlyZero(0.5f);
				if (!bSame) break;
				++Diverge;
			}

			const bool bIdentical = (Diverge == Common)
				&& Prev->PointsSZ.Num() == Strand.Points.Num();
			if (bIdentical)
			{
				Spawn = Prev->SpawnTime;
			}
			else
			{
				// 变化点的弧长。`Diverge` 是"第一个不同的点"，它之前那一截与上一轮逐点相同，
				// 所以那一截已经长出来的部分应当留着。
				const int32 ArcIndex = FMath::Clamp(Diverge, 0, Strand.Arc.Num() - 1);
				const float DivergeArc = Strand.Arc.IsValidIndex(ArcIndex) ? Strand.Arc[ArcIndex] : 0.0f;
				const float Front = (Now - Prev->SpawnTime) * Speed;

				// 前沿还没长到变化点 ⇒ 那段变化对它不可见，相位不用动。
				// 越过了 ⇒ 把前沿拉回变化点（等价于 SpawnTime 往后挪），从那里接着长。
				Spawn = (Front > DivergeArc) ? (Now - DivergeArc / Speed) : Prev->SpawnTime;
			}
		}

		OutSpawnTimes.Add(Spawn);

		FStrandHistory Entry;
		Entry.SpawnTime = Spawn;
		Entry.PointsSZ.Reserve(Strand.Points.Num());
		Entry.Edges.Reserve(Strand.Points.Num());
		for (const FStrandPoint& P : Strand.Points)
		{
			Entry.PointsSZ.Add(P.WallSZ);
			Entry.Edges.Add(P.EdgeIndex);
		}
		NextHistory.Add(Strand.RootKey, MoveTemp(Entry));
	}

	// ⚠️ **整表替换而不是往里塞**：房子反复改尺寸会让键不断变化，只加不删的话这张表
	// 会随编辑次数无界增长，而且泄漏得毫无症状（每根藤还带着一份折线副本）。
	History = MoveTemp(NextHistory);
}
}
