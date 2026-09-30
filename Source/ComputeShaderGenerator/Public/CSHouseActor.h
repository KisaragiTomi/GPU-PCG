#pragma once

#include "CoreMinimal.h"
#include "CSGroundShaperSteps.h"
#include "CSHouseDecor.h"
#include "CSHouseDoorRuns.h"
#include "CSHouseFrame.h"
#include "CSHouseProfile.h"
#include "CSHouseQuoin.h"
#include "CSHouseResize.h"
#include "CSHouseRoof.h"
#include "CSHouseSeam.h"
#include "Components/SceneComponent.h"   // EUpdateTransformFlags：根组件 TransformUpdated 的回调签名
#include "CSHouseTile.h"
#include "CSHouseTrim.h"
#include "CSHouseVine.h"
#include "CSTinyGlade.h"
#include "CSWallBase.h"   // 房子 = 围合的墙 + 屋顶：墙的基类（2026-09-22「统一房子和墙的逻辑」）
#include "CSHouseActor.generated.h"

class ACSGroundActor;
struct FCSStairsTerraceSite;
struct FCSHouseContact;
class ACSHouseFeatureMarker;
class ACSHouseHandleActor;
class ACSHouseHeightHandleActor;
class ACSHouseResizeHandleActor;
class UCSGpuInstancedMeshComponent;
class UCSMesh;
class UCSMeshRenderComponent;
class UMaterialInterface;
class UStaticMesh;
struct FCSGpuMeshCPUData;
enum class ECSHouseHeightHandleSide : uint8;   // CSHouseHeightHandleActor.h —— 檐口框 / 房底框

/**
 * 进入 / 退出拉尺寸模式（计划 D5）。`bEntered = false` 表示退出。
 *
 * **静态**多播，沿用 `ACSGroundActor::OnGroundPaintEditorRequest` 那条"runtime 请求、
 * editor 应答"的既有分工：runtime 模块只管广播，编辑器模块（`PCGEditorProcess`）接管
 * 失选监听。房子这一侧因此零编辑器依赖，无头测试也能完整走完整条模式。
 */
DECLARE_MULTICAST_DELEGATE_TwoParams(FCSHouseResizeModeChanged, ACSHouseActor*, bool /*bEntered*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FCSHouseTerraceChanged, ACSHouseActor*);

/**
 * `StartWindowBrush()` 广播它，编辑器模块（`PCGEditorProcess`）应答成激活窗笔刷 EdMode。
 * 与地面那条 `FCSGroundPaintEditorRequest` 同一条接线：运行时模块不认识 EdMode，只发请求。
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FCSHouseWindowBrushRequest, ACSHouseActor*);

/**
 * 房体三角汤的**全部**输入。ACSHouseActor::RebuildBodyMesh 从自己的属性组一份，
 * automation 测试从字面量组一份 —— 后者是这个结构存在的唯一理由：
 * 「洞是不是真的没在几何里挖」「墩跨度砌没砌实」这类判据只能逐三角验，而 actor 进不了纯 CPU
 * 用例（测试不起 world、不碰 RHI，见 Tests/CSHouseLogicTests.cpp 的文件头）。
 *
 * ⚠️ **没有屋面** —— 四坡屋顶由瓦片实例承担，一片屋面三角都不进这份汤（2026-08-31）。
 */
/**
 * 门洞滞回的一条记忆：某条边上一帧的一个洞区间（沿边弧长，与 `CSHouse_GetEdge` 同口径）。
 *
 * 扁平数组而不是 `TMap<边, TArray<区间>>`：UHT 不支持容器套容器，而边数与每条边的洞数
 * 都是个位数，线性扫描比建索引便宜。
 */
USTRUCT()
struct FCSDoorRunMemory
{
	GENERATED_BODY()

	UPROPERTY() int32 EdgeIndex = 0;
	UPROPERTY() float S0 = 0.0f;
	UPROPERTY() float S1 = 0.0f;
};

struct FCSHouseBodyDesc
{
	/** 底面轮廓（闭合折线，局部空间）。墙板数 = 边数，`CSHouse_BuildBodySoup` 不再假定 4。 */
	FCSHouseFootprint Footprint = FCSHouseFootprint::MakeRect(FVector2D(600.0, 400.0));

	float WallThickness = 24.0f;
	/** 墙顶高。四坡屋顶下四面墙顶都平在这个高度上（= 屋面求值器的 EaveZ）。 */
	float WallHeight = 300.0f;
	/** 相邻洞之间保留的墩宽 cm。 */
	float PierWidth = 40.0f;

	/** 这一轮要切出来的洞（几何上不挖，逐像素 clip）。 */
	TArray<FCSWallOpening> Openings;

	/**
	 * D7 接缝在墙上切的段：插进邻居 footprint 里的那截墙（同样几何上不挖，逐像素 clip）。
	 *
	 * **与 Openings 分开是有意的**（理由逐字见 `FCSWallCut`）：洞是设计意图，接缝裁剪是事实。
	 * 混成一张表会让谓词、墩迟回、门框砖三处一起误把它当洞。
	 */
	TArray<FCSWallCut> SeamCuts;

	/** 把局部坐标烘成世界坐标（常驻流口径）。测试传 Identity 即得局部坐标。 */
	FTransform World = FTransform::Identity;

	/** 铺不铺底面（墙内皮围出的那块、朝下的单面，见 `CSHouse_BuildBodySoup`）。 */
	bool bBottomFace = true;
	bool bFlatRoof = false;
};

/**
 * 房体三角汤的**唯一**生成点（计划 D4：屋面方程一旦散开，铺瓦 / 铺梁 / 落窗谓词就会各写一份）。
 * 从 RebuildBodyMesh 里原样抽出来，除了让测试能拿到三角汤以外没有第二个目的。Out 按**追加**写。
 */
COMPUTESHADERGENERATOR_API void CSHouse_BuildBodySoup(const FCSHouseBodyDesc& Desc, FCSGpuMeshCPUData& Out);

/**
 * 房体顶点色 R 通道的构件色号（见下面 ACSHouseActor 的通道字典）。
 * 值直接以 id/255 写进顶点色，8-bit 量化后逐位还原。
 */
UENUM()
enum class ECSHousePart : uint8
{
	Wall = 0,
	Roof = 1,
	// 2 号空着：原为山墙，四坡屋顶没有这个构件（2026-08-31）。别顺手占用 ——
	// 这张表是 P2 冻结的字典，已烘进 StaticMesh 的旧网格里可能还留着 2。
	/** 门框砖：沿洞缘曲线铺的离散块，负责填满 clip 留下的厚度断口（不是扫掠面）。 */
	Frame = 3,
	Pillar = 4,
};

/**
 * 一扇窗的**诉求**（D8）。
 *
 * ⚠️ **这是一份显式列表，不是"按剩余墙面自动填窗"的规则。** Tiny Glade 的窗 100% 是玩家
 * 手放的（`ui_place_decorator` → 光标射线 → `DecoratorDst{Wall|Roof|…}`），全仓找不到任何
 * 读墙长去分配窗位的系统（`Docs/TinyGlade/Progress_ModuleComparison.md（卷二）` 第二节）。本项目还没有放置 UI，
 * 而「门洞触发规则」是唯一没拍板的一条 —— 所以窗的来源就停在这份列表上：属性面板、
 * Blueprint、测试脚本都往这里填，将来的 `ACSWindowMarker` 也只是多一个填表的人。
 * **不要在这里发明触发规则。**
 *
 * 诉求 ≠ 结果：能不能砌出来由 `ACSHouseActor::QueryFeaturePlacement` 说了算（门拱优先），
 * 被拒的诉求留在列表里、只是这一轮不出洞（`GetWindowRejectCount` 数得出来）。
 */
USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSHouseWindow
{
	GENERATED_BODY()

	/** footprint 的边号，与 CSHouse_GetEdge 同号（矩形上 0 南 1 东 2 北 3 西）。越界的边号在谓词里判 NotOnWall。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Window", meta = (ClampMin = "0"))
	int32 EdgeIndex = 0;

	/** 沿边弧长上的窗心（从边起点算）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Window", meta = (ClampMin = "0.0"))
	float CenterS = 150.0f;

	/** 窗宽 cm。默认取 TG `window_cottage_1x1` 的实测宽（78）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Window", meta = (ClampMin = "10.0"))
	float Width = 78.0f;

	/** 窗台高 = 洞底 Z。低于 `WindowMinSillZ` 会被谓词拒（窗台压在地上时几何合法但观感荒唐）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Window", meta = (ClampMin = "0.0"))
	float SillZ = 90.0f;

	/** 窗高 cm；洞顶 = SillZ + Height，超过 墙高 − LintelBand 会被谓词拒。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Window", meta = (ClampMin = "10.0"))
	float Height = 110.0f;

	/**
	 * 洞形。**三种原型全都不用改一行 shader** —— `FCSOpeningClipField` 本来就是二维的，
	 * `Rect`/`Circle` 上下都有界，材质里那段 HLSL 也早已覆盖三个形状 id。
	 * `Arch` 用来做尖顶窗（洞底在 SillZ，下界由洞面板的底承担）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Window")
	ECSOpeningShape Shape = ECSOpeningShape::Rect;

	/**
	 * 门形态（2026-09-16）：这条诉求是一扇**落地门**，不是窗。附属物把窗拖到贴近墙脚时写它
	 * （`ACSWindowMarker::ResolveDoorForm`），此时 `SillZ` 恒 0、宽高取门那一档的网格。
	 * 房子只按它放过窗台下限（`CSHouse_QueryOpening`），洞照旧是附属物的洞：不出门框砖、不装门扇。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Window")
	bool bDoorForm = false;

	/**
	 * 逐字段相等。标记每 tick 都会重登记一次诉求，靠它把"没动"的那些**在标脏之前**挡掉 ——
	 * 少了它，拖动一扇窗会让宿主每帧重求值一次（幂等，所以不会出错，只会白烧）。
	 * 浮点直接比不做容差：诉求是标记算出来的确定值，同一摆位逐位相同。
	 */
	bool operator==(const FCSHouseWindow& Other) const
	{
		return EdgeIndex == Other.EdgeIndex && CenterS == Other.CenterS && Width == Other.Width
			&& SillZ == Other.SillZ && Height == Other.Height && Shape == Other.Shape
			// 漏掉它 = 窗与门恰好同宽同高同位时，变门那一下诉求"没变"、房子不重求值。
			&& bDoorForm == Other.bDoorForm;
	}
	bool operator!=(const FCSHouseWindow& Other) const { return !(*this == Other); }
};

// `ECSVineSeason`（叶子的季节）2026-09-22 挪到了 `CSWallBase.h`：墙的基类要用，房子与样条墙同一个枚举。

/**
 * Tiny Glade 式房屋（Plan_House.md D4/D6/D9 的"房子×地面交互"纵切片）。
 *
 * 声明式重求值：任何唤醒（移动 / 改参 / 地面 OnGroundChanged 直推）都走同一条
 * ReevaluateSite() —— ① 落座：房底 Z = max(footprint 全域地面高度) + HeightOffset，
 * 绝对式、升降对称（地形隆起多高屋顶抬多高，塌陷同理回落）；② 门拱：边缘线段等分
 * 为子段，逐子段采样道路顶点色（双探测线）+ 离地连续收窄 + 滞回点亮，点亮子段生成
 * 拱洞（门是固定拱原型，直接参数化生成——布尔的"纯 mesh 操作数"入口尚不存在，见计划
 * 开放问题）；③ 承重柱：周界支撑点逐点比地面落差，悬空处生柱，独立组件不进房体。
 *
 * 两份产物各自哈希守卫（含量化后的世界变换）：房体 desc 变才重建房体网格，柱 desc
 * 变才重建柱网格 —— 画路只可能动房体，纯地形变化只动柱与落座，互不牵连。
 *
 * 与地面的接线是直推：PostRegisterAllComponents 时订阅 Ground->OnGroundChanged，
 * 收到即 ReevaluateSite（无条件唤醒，幂等哈希把无效唤醒吸收成零成本）。
 *
 * -----------------------------------------------------------------------------
 * 房体顶点色的通道字典（P2 冻结；**全项目唯一仲裁点**，别在别处先到先得地占用）
 * -----------------------------------------------------------------------------
 *   R = 构件色号 ECSHousePart / 255   墙 0 / 屋顶 1 / (2 空) / 门框砖 3 / 柱 4
 *   G = 洞的 Tag / 255                悬停高亮单个拱用；非洞构件恒 0
 *   B = 洞形状 id / 255               ECSOpeningShape；255 = 这块面板没有洞
 *   A = 保留                          预定：季节 t
 *
 * UV 通道（房体声明 2 组，靠 FCSMeshStreamLayout::NumTexCoordSets 的逐 mesh 变体）：
 *   UV0 = 墙面贴图坐标 (沿边弧长 S, 高度 Z) / UVScale
 *   UV1 = 解析裁剪场 q —— 材质按它逐像素 discard 切出洞（Tiny Glade 原版做法）。
 *         判据见 CSHouseProfile.h 的 CSHouse_ClipKeeps()，材质里那份必须逐字对应。
 *
 * 之所以能这么用：房体是**无共享顶点的三角汤**（AddTri 每次新建三个顶点），位域打包是免费的，
 * 也不会被邻接顶点插值污染。**地面正相反** —— 它的顶点是共享的、插值会毁掉位域，而且 R 已经
 * 被道路权重占用，所以地面绝不打包位域。
 *
 * 顶点色这三十二位 + 最多 8 组 UV，是本项目能被材质读到的全部自定义逐顶点语义（对照
 * Tiny Glade 的 41 种）。⚠️ 早先注释里「扩 UV1 在 proxy 绑定处有静默地雷」**已过期**：
 * `CSGpuMeshTypes.cpp:44` 已改成 `ElementsPerUnit = 2 * Clamp(NumTexCoordSets, 1u, MaxTexCoordChannels)`
 * 的单条交错流，`CSGpuMeshSceneProxy.cpp` 的 TexCoord 分支逐组挂 stream component、SRV 只设
 * 一次；上限 `FCSGpuMeshCPUData::MaxTexCoordChannels = 8`（2026-09-04 由 4 抬到引擎天花板
 * `MAX_STATIC_TEXCOORDS`），开关是 `UCSMeshOps::EnsureTexCoordSets`。第 5 组起只走 manual
 * fetch 的 SRV，理由见该分支的注释。
 */
UCLASS(Blueprintable, BlueprintType)
class COMPUTESHADERGENERATOR_API ACSHouseActor : public ACSWallBase
{
	GENERATED_BODY()

public:
	ACSHouseActor();

	// -------------------------------------------------------------------------
	// Footprint / Body
	// -------------------------------------------------------------------------

	/**
	 * 底面**包围盒**尺寸 cm（局部 X / Y）；位置/朝向用 actor transform（支持任意 yaw）。
	 * 矩形房子的包围盒就是它自己；异形房子由 `FootprintShape` 拉伸到这个尺寸。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House", meta = (ClampMin = "100.0"))
	FVector2D FootprintSize = FVector2D(600.0, 400.0);

	/**
	 * footprint 的**形状**（2026-09-12 裁决：矩形不是终局）：一条闭合凸折线，坐标系任意 ——
	 * 它的包围盒会被拉伸到 `FootprintSize`、居中到房心（`FCSHouseFootprint::FromShape`）。
	 * **空 = 矩形**，旧存档与新放的房子都是空的。
	 *
	 * 为什么存"形状 + 尺寸"而不是一条厘米折线：写 `FootprintSize` 的旧路径（详情面板、蓝图、
	 * `Scripts` 里的 `footprint_size`、拖边、测试）一条都不用改就在异形房子上成立，旧存档不需要
	 * 迁移。代价是改尺寸会非等比地拉伸形状 —— 那正是"包围盒尺寸"这个量的本义。
	 *
	 * 顶点按逆时针给（顺时针的会被翻过来）；第 k 条边从第 k 个顶点走到第 k+1 个，门窗锚点、
	 * 拉尺寸抓手都按这个边号。**不是严格凸的形状按凸包使用**（2026-09-14 用户裁决，凹 footprint 不做）：
	 * 凹角处的点、共线点、重复点被包进去丢掉，乱序的点按凸包重排；边号于是按**凸包**数，拉尺寸锥子也
	 * 按凸包的边数生成。凸包只在使用时取（`GetFootprint`），**这个属性本身不回写** —— 在细节面板里
	 * 逐个加顶点时中间态常常是凹的，回写会把刚加的点当场删掉。凸包与输入不一致时重求值打一次
	 * Warning（同一个形状只打一次）。凸包退化（点全共线）时退回矩形。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House")
	TArray<FVector2D> FootprintShape;

	/**
	 * footprint 的**折线视图** = `FootprintShape` × `FootprintSize`。所有几何消费者只认它。
	 *
	 * **按值返回**：几个顶点一次小分配。逐实例的热路径问的是 `CSHouse_GetEdge`，
	 * 不是这个 —— 调用方在循环外取一次即可。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House")
	FCSHouseFootprint GetFootprint() const { return FCSHouseFootprint::FromShape(FootprintShape, FootprintSize); }

	/** 檐口高 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House", meta = (ClampMin = "100.0"))
	float WallHeight = 300.0f;

	/** 墙厚 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House", meta = (ClampMin = "5.0"))
	float WallThickness = 24.0f;

	/**
	 * 房体底面（2026-09-17 用户要求「房子产生底面」）：在房底高度把墙内皮围出的那块封上，朝下的单面。
	 * 墙板自己的底早由墙棱柱封住，所以只铺内皮以内；从屋里往下看是背面、被剔除，不会与地形共面闪烁 ——
	 * 只有从房子底下往上看（摞在别的房上、架在柱子上、悬挑）才看得到它。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House")
	bool bBottomFace = true;

	/** 四坡屋顶坡度（度）；屋顶把手反算此值，低起伏自动切换为露台。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House", meta = (ClampMin = "0.0", ClampMax = "70.0"))
	float RoofPitch = 35.0f;

	/** 屋脊相对檐口低于此高度（cm）时，瓦片替换成石铺露台。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Flat Roof", meta = (ClampMin = "0.0"))
	float FlatRoofThreshold = 20.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Flat Roof", meta = (ClampMin = "0.0", ClampMax = "200.0"))
	float FlatRoofParapetHeight = 55.0f;

	/** 露台铺地材质；未指定时沿用墙面材质。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Flat Roof")
	TObjectPtr<UMaterialInterface> FlatRoofMaterial;

	/**
	 * 屋面整体的**竖直偏移** cm，加在檐口高（= `WallHeight`）上。
	 *
	 * 正值把整个屋顶抬起来（檐口与墙顶之间露出一条缝），负值往下坐进墙里。
	 * 屋面方程只有 `FCSRoofDesc::EaveZ` 这一份真源，所以偏移加在它身上 —— 瓦、脊瓦、
	 * 以及将来任何读屋面的消费者都会一起跟上，不会出现"瓦抬了、别的没抬"。
	 *
	 * ⚠️ 想让瓦离开屋面一点点用 `RoofTileStandOff`（沿**屋面法线**抬），那是另一件事：
	 * 本值是竖直的、且改的是屋面本身。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House", meta = (ClampMin = "-200.0", ClampMax = "200.0"))
	float RoofHeightOffset = 12.0f;

	/** 屋檐外挑 cm。四面都挑同样多（四坡）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House", meta = (ClampMin = "0.0"))
	float RoofOverhang = 25.0f;

	// -------------------------------------------------------------------------
	// 屋面瓦（四坡的屋面**全部**由瓦铺成 —— 房体三角汤里一片屋面都没有）
	//
	// 轴向不需要在这里配：`ACSHouseActor::MakeRoofTileParams` 从网格包围盒自动判定
	// （最薄的一轴 = 屋面法线，剩下两轴按 `bRoofTileSwapAxes` 分配），判定结果会打进日志。
	// -------------------------------------------------------------------------

	/** 屋面瓦。留空 = 不铺瓦（房子就只剩四面墙）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Roof Tile")
	TObjectPtr<UStaticMesh> RoofTileMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Roof Tile")
	bool bRoofTilesEnabled = true;

	/**
	 * 瓦沿屋面法线的**厚度** cm。≤ 0 = 用网格原生尺寸。
	 *
	 * ⚠️ **不给的话瓦是一米厚的方块。** TG 的 `roof_tile` 原件实测 **1.188 × 1.0 × 1.0 m** ——
	 * 它是个**单位块**，真实厚度由 TG 的逐实例缩放压出来（`_nani_instanced_roof` 的 VS 里那个
	 * `scale_t`）。我们这边平面内两轴按排距缩了、法线轴却一直取原生 100 cm，屋顶因此看着像
	 * 堆了一层砖。
	 *
	 * 默认 13 = `SM_TinyGladeRoofTile` 包围盒的法线向尺寸（TG 板厚 0.1 m + 烘焙进去的 3 cm 瓦端抬升），
	 * 即按 1:1 画。0.1 m 是 `assemble_rectangular_roof` 写进 `scale_t.z` 的字面常数（2026-09-29 逆向，
	 * `Roof_TerraceAndRidge.md`「屋面瓦资产」）；此前的 6 cm 配的是 3 cm 板厚的旧烘焙，只有 TG 的三成。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Roof Tile", meta = (ClampMin = "0.0", ClampMax = "100.0"))
	float RoofTileThickness = 13.0f;

	/**
	 * 每片瓦画多大的**总系数**，只乘在平面内两轴上。
	 *
	 * 排距一步不动 ⇒ **瓦数不变**，只是每片变大或变小（< 1 会在瓦之间露出屋面底下的天空，
	 * > 1 让相邻瓦压得更狠）。想改瓦数请调 `RoofTileRowPitch` / `ColumnPitch`，那是另一件事。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Roof Tile", meta = (ClampMin = "0.05", ClampMax = "4.0"))
	float RoofTileSizeScale = 1.0f;

	/**
	 * 角斜脊 / 屋脊上盖瓦的尺寸系数。≤ 0 = 不铺脊瓦。
	 *
	 * 交汇处两坡的瓦是**对切**的，接缝一眼看得见。
	 * · **角斜脊**：TG 的单排窄板，宽 25 cm、厚度为坡面板厚的 1.8 倍；坡面瓦通过逐实例裁剪面切齐。
	 *   本系数只缩放水平脊宽度，≤ 0 时禁用全部脊瓦。
	 * · **水平屋脊**（2026-09-29 起照 TG）：约 62.5 cm 一段、每段一块平躺厚板，宽 40 cm × 本系数（默认 46 cm，
	 *   在 TG 的 0.4–0.55 m 之间）、长 = 段长 × 1.4、厚 = 坡面瓦的 1.5 倍。
	 * roof_tile_lod1 / backface 是原版的简化/背面通道，不能当作随机瓦型或专用脊瓦。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Roof Tile", meta = (ClampMin = "0.0", ClampMax = "3.0"))
	float RoofRidgeCapScale = 1.15f;

	// -------------------------------------------------------------------------
	// 尖顶（TG: `system_roof::visual::place_spires::place_roof_spires`）
	//
	// 逆向侧的三条硬证据（PDB + 反编译 VS，2026-08-31）：
	//  · 组件 `system_roof::utils::components::RoofSpire` 挂在**屋顶实体自己**身上，产出走
	//    `Query<(&Roof, &RoofAnimation), With<RoofSpire>>` 的一次 `filter_map` ⇒
	//    **一座屋顶最多一根**，且有屋顶拿不到（平顶 / 山墙顶那一档）。
	//  · SSBO 只有 `{ vec4 position; float roof_animation_t; float roof_profile; float radius; }`
	//    —— **没有任何旋转**。尖顶恒竖直、绕自身轴对称，摆位只是一个点。
	//  · `_instanced_roof_spire` 的 VS 只对 `roof_profile_mult > -0.001` 的顶点（= 底裙那 504 个）
	//    横向放大 `radius·2 · lerp(.6,1,mult) · lerp(.4,1.5,1-profile)`，竖直分量一动不动，
	//    最后整体 **×0.5**。所以「屋顶越大、裙摆越张，杆子不变粗」，本项目的 `RoofFinialScale`
	//    默认取 0.5 就是那个常数。
	//
	// ⚠️ **本项目摆的是"每个屋脊端点一根"，与 TG 的"每屋顶一根"有意分叉。** 理由：TG 的
	// `roof_tip_offset_xz` 给的是一个点（他们的屋顶形状族里尖顶那一档本来就是锥/金字塔），
	// 而我们的四坡顶脊是一条**线段**，两端各有一处"两条角斜脊 + 一条屋脊"三面交汇的破口 ——
	// 用户 2026-08-31 指的正是那里。正方形时脊长为 0、两端重合 ⇒ 自动退化成金字塔尖上的一根，
	// 不需要为金字塔写特例。
	// -------------------------------------------------------------------------

	/**
	 * 屋脊端点的尖顶。**留空 = 不长**（同 `RoofTileMesh` 的口径）。
	 *
	 * ⚠️ 竖直轴**从包围盒自动判**（最长的一轴 = 尖顶朝上那根），判定结果会打进日志。
	 * TG 的源 `roof_spire.json` 是 y-up 的（y ∈ [−0.858, 2.18]，x/z 对称到 ±1.103），
	 * 本仓库导入后已被转成 Z-up（实测 220.6 × 220.6 × 303.8 cm，判定 up axis=2）。
	 * 自动判轴是为了**两种口径都不必手工转资产** —— 换一张 y-up 直进的资产同样立得起来。
	 * 代价是"矮胖的尖顶"会判错轴，那种资产得自己转正。
	 *
	 * ⚠️ 摆位按**资产自己的枢轴**放在脊端点上。TG 那张的枢轴恰在裙摆与杆子的交界
	 * （包围盒 z ∈ [−85.8, +218.0] cm，0 在底裙上沿），所以裙摆天然垂到瓦面以下、把破口盖住
	 * （默认 0.5 缩放下垂 42.9 cm）。换一张枢轴在底面的资产会整根浮在脊上，用 `RoofFinialSink` 压下去。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Roof Finial")
	TObjectPtr<UStaticMesh> RoofFinialMesh;

	/** 整体覆盖材质。**留空 = 逐槽用网格资产自带的材质**（组件上的覆盖会被清掉）；设了 = 盖住每一个槽。
	 *  尖顶走普通 `UStaticMeshComponent`，**不吃**实例化路径，所以这里不需要 `bUsedWithInstancedStaticMeshes`
	 *  （瓦/藤蔓那条坑在这儿不成立）；资产开了 Nanite 时母材质要有 `bUsedWithNanite`。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Roof Finial")
	TObjectPtr<UMaterialInterface> RoofFinialMaterial;

	/** 整体缩放。默认 0.5 = TG VS 里那个硬编码常数。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Roof Finial", meta = (ClampMin = "0.01", ClampMax = "10.0"))
	float RoofFinialScale = 0.5f;

	/** 沿竖直方向往屋面里压 cm（正值往下）。资产枢轴不在裙摆上沿时用它对齐。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Roof Finial", meta = (ClampMin = "-200.0", ClampMax = "200.0"))
	float RoofFinialSink = 0.0f;

	// -------------------------------------------------------------------------
	// Door Leaf（门扇，2026-09-04）
	//
	// TG 侧：`construct_gates` 按 `segment_length` **现搭** quad 网格
	// （`DoorMeshInProgress::add_quad` + `door_mesh` / `door_mesh_gap`），资产里只有把手
	// （`wooden_gate/door_handle_circle.glb`），**没有门扇网格**。所以那边的门扇天生随洞宽变宽。
	//
	// 本项目：用**现成网格按洞宽缩放**，不现搭。差异是有意的 ——
	// 现搭 quad 要自己管板条排布/UV/法线，而本仓库已经有 `door` 这张提取资产；
	// 代价是宽度差得多时板条比例会被拉伸，用 `DoorLeafMaxStretch` 夹住。
	// -------------------------------------------------------------------------

	/**
	 * 门扇网格**按尺寸分档**（空数组 = 不长门扇，同 `RoofFinialMesh` 的口径）。
	 *
	 * 顺序无所谓 —— 代码按每张网格**自己的包围盒宽度**排序，再挑"native 宽度不超过洞宽的
	 * 最大一档"，挑不到就用最小那档。剩下的差值才交给缩放，所以拉伸量天然被压到最小。
	 *
	 * ⚠️ **这是 TG 自己的做法**：那边的门就是 `balcony_door_rank1/2/3`
	 * （120 / 150 / 180 cm 宽 × 262.5 高，1002 / 1242 / 1716 顶点，带 COLOR_0 + UV），
	 * `DecoratorSubtype` 里按 rank 选档。
	 *
	 * ⚠️ **别用 `decorators/door.glb`（本仓库的 `door` 资产）**：它只有 36 顶点、120×250×75、
	 * 没有 UV —— 是**交互/碰撞代理盒**，不是可见门扇。2026-09-04 第一版错用了它，
	 * 画面上是一块纯色板子。同族的 `*_collision` / `*_interaction` / `*_outline` 同理。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door Leaf")
	TArray<TObjectPtr<UStaticMesh>> DoorLeafMeshes;

	/** 整体覆盖材质。**留空 = 逐槽用网格资产自带的材质**（`balcony_door_rank*` 是木 + 铁两槽）；设了 = 盖住每一个槽。
	 *  门扇走普通 `UStaticMeshComponent`，不吃实例化路径。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door Leaf")
	TObjectPtr<UMaterialInterface> DoorLeafMaterial;

	/** 关掉即所有门扇不出。出图脚本靠它拍"同机位只切开关"的对照图。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door Leaf")
	bool bDoorLeafEnabled = true;

	/**
	 * 门扇占洞宽的比例。**1 = 与洞同宽**；> 1 会被墙裁掉溢出的部分（见 `DoorLeafRise` 那段）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door Leaf", meta = (ClampMin = "0.1", ClampMax = "1.5"))
	float DoorLeafWidthRatio = 1.0f;

	/**
	 * 门扇顶边相对**起拱线**的位置：0 = 齐起拱线，1 = 齐拱顶，> 1 = 越过拱顶。
	 *
	 * ⚠️ **默认 1.05，即门扇故意比洞高一点** —— 这是 TG 的做法，2026-09-04 从实拍
	 * `img/tiny-glade-ref-door-in-arch.png` 反推出来的：那张图里门的轮廓**严丝合缝地贴着
	 * 拱圈石内缘**，一条缝都没有，而 TG 的拱形状随洞宽/拱高连续变化 ——
	 * **一张固定网格不可能每次都对上那条曲线**。所以门扇不是建成拱顶的，它是一块
	 * **比洞更大的矩形**，退在墙面之后，由墙自己的 `OpacityMask` 切出拱形剪影
	 * （实拍里拱圈石还在门上投了一道软阴影，正是"门是凹进去的"的证据）。
	 *
	 * ⇒ 我们**不需要拱形门扇网格**，只要让门扇越过拱顶、并保证它整体退在墙的外表面之后。
	 * 早先默认 0（门顶停在起拱线、拱顶那半圆空着）是反的，画面上是"方门塞在拱下面"。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door Leaf", meta = (ClampMin = "0.0", ClampMax = "1.5"))
	float DoorLeafRise = 1.05f;

	/**
	 * 竖直/水平缩放比的上限。窄洞配宽网格时纵横比会被拉扁，超过这个倍数就**只缩不拉**
	 * （宁可门扇比洞窄一点，也不要把板条拉成面条）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door Leaf", meta = (ClampMin = "1.0", ClampMax = "8.0"))
	float DoorLeafMaxStretch = 2.0f;

	/** 门扇相对墙心沿外法线的偏移 cm（正值朝外）。0 = 门扇中面与墙中面重合。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door Leaf", meta = (ClampMin = "-100.0", ClampMax = "100.0"))
	float DoorLeafInset = 0.0f;

	// ⚠️ 下面这组排布默认值是 TG 的真值（2026-09-29 用户贴 TG 实拍「这是正确的屋顶」后逆向
	// `system_roof::utils::assemble_rectangular_roof`，证据在 `Roof_TerraceAndRidge.md`「屋面瓦资产」）。
	// 此前的 16.9 / 27.75 / 1.6 / 1.06 是 08-31 配旧网格（118.8 × 100 × 100 的单位块）试出来的密度，
	// 排数约为 TG 的 2.6 倍。

	/**
	 * 沿坡向排距 cm。**0 = 由网格自身尺寸与 RowOverlap 反解**（瓦按原尺寸画，排距把它压出重叠）。
	 * TG：排数 = ceil(坡长 / 0.4375 m) − 1，即实际排距 ≈ 44–47 cm；这里按四舍五入等分，45 给出同样的排数。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Roof Tile", meta = (ClampMin = "0.0"))
	float RoofTileRowPitch = 45.0f;

	/** 排内列距 cm。0 = 同上由网格尺寸反解。TG：每排 max(ceil(2 × 排长 m), 3) 片**等宽**瓦，即 ≤ 50 cm、平均约 45。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Roof Tile", meta = (ClampMin = "0.0"))
	float RoofTileColumnPitch = 45.0f;

	/**
	 * 瓦画多大 = 实际排距 × 这个系数。> 1 = 上下两排**故意互相压住**（正缝会露出天空）。
	 * TG：瓦长 = 排距 / 0.7 ⇒ 1.43（再乘 `RoofTileLengthJitter` 的逐片随机）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Roof Tile", meta = (ClampMin = "1.0", ClampMax = "4.0"))
	float RoofTileRowOverlap = 1.43f;

	/**
	 * 同上，排内左右方向。左右压叠远比上下浅。
	 * TG 的瓦宽 = 列距，VS 再按板厚把宽端外扩 3.2 cm/侧、窄端内收（楔形缝就是它）；包围盒宽 / 列距在 45 cm
	 * 时 = 49.7 / 45 ⇒ 1.10，此时 `SM_TinyGladeRoofTile` 恰好按 1:1 画。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Roof Tile", meta = (ClampMin = "1.0", ClampMax = "2.0"))
	float RoofTileColumnOverlap = 1.10f;

	/** 沿屋面法线抬起 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Roof Tile", meta = (ClampMin = "0.0"))
	float RoofTileStandOff = 0.5f;

	/** 逐片等比缩放抖动（比例，平面内两轴一起）。TG 没有这一项（同排瓦等宽），默认 0；长度随机见下一项。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Roof Tile", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float RoofTileScaleJitter = 0.0f;

	/**
	 * 逐片**瓦长**抖动（比例，只乘沿坡那一轴）。TG：瓦长 × (0.8 + 0.4 · rand) ⇒ 0.2。
	 * 瓦以排线为中心，长短不一 ⇒ 同一排的下沿参差不齐 —— 实拍里那种不规整感主要来自它，不是瓦形。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Roof Tile", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float RoofTileLengthJitter = 0.2f;

	/**
	 * 绕屋面法线的朝向抖动（弧度）。
	 *
	 * ⚠️ **默认 0（2026-08-31 用户裁决「每一个元素都有点歪，修复它」）。** 原默认 0.04 rad ≈ 2.3°，
	 * 在当时的密铺（RowPitch 16.9 / ColumnPitch 27.75）下每片瓦都看得出转了一点，整片屋面读成"歪的"
	 * 而不是"自然的"。TG 也不转（`rotation_quat` 只有坡度与排向）。想要一点随机感用 `RoofTileLengthJitter`。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Roof Tile", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float RoofTileYawJitter = 0.0f;

	/** 沿屋面法线的高度抖动 cm —— TG 的瓦是一片起伏的鳞，不是一张平面。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Roof Tile", meta = (ClampMin = "0.0"))
	float RoofTileLiftJitter = 0.6f;

	/** 逐实例随机的用户种子。随机**只由 (面号, 排号, 列号, 种子) 决定**，不取 GPU 槽位。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Roof Tile")
	int32 RoofTileSeed = 1;

	/**
	 * 自动判定把上坡向与沿排向弄反了就勾上（判定结果每次重建都打在日志里）。
	 * 判定把长的那轴给上坡向：09-29 起的 `SM_TinyGladeRoofTile` 长 64 > 宽 49.7，**不勾**才对；
	 * 旧烘焙宽 > 长，所以 `BP_TinyGladeHouse` 以前勾着它（09-29 已随新网格一起关掉）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Roof Tile")
	bool bRoofTileSwapAxes = false;

	/** 单边推拉的尺寸下限 cm（`PushEdge` 的硬下界）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House", meta = (ClampMin = "100.0"))
	float MinFootprint = 200.0f;

	/**
	 * 调高度的下限 cm（`PushHeight` 的硬下界）。
	 *
	 * 200 不是随手写的：洞顶被 `墙高 − LintelBand`（默认 40）夹着，再低下去门和窗会被谓词
	 * 全部拒掉 —— 画面上是"房子越压越矮，然后门窗突然一起消失"，而那看起来像是开洞坏了。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House", meta = (ClampMin = "50.0"))
	float MinWallHeight = 200.0f;

	/** 相对地面参考高度的抬升；落座公式 = max(footprint 地面高度) + HeightOffset。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House")
	float HeightOffset = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Material")
	TObjectPtr<UMaterialInterface> WallMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Material")
	TObjectPtr<UMaterialInterface> RoofMaterial;

	/** 承重柱的整体覆盖材质。**留空 = 砖柱逐段画 `PillarBrickMesh` 资产自带的材质**（方盒占位路取那张资产 0 号槽）；
	 *  设了 = 盖住砖的每一段。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Material")
	TObjectPtr<UMaterialInterface> PillarMaterial;

	// -------------------------------------------------------------------------
	// Door（**道路区间制**，D6；2026-09-04 从"等分槽 + 覆盖率二值投票"重做）
	//
	// 门宽 = 路在这面墙上截出的弦长，门心 = 那段的中心 —— 逐条依据与 TG 对位见
	// `CSHouseDoorRuns.h` 文件头。等分槽那一套（`DoorPitchTarget` / `SplitEdgeIntoSlots`）
	// **已随之删干净**，别再找它。
	// -------------------------------------------------------------------------

	/**
	 * 相邻拱之间保留的墩宽 cm（拱宽 = 段长 − 墩宽，再乘离地收窄）。
	 *
	 * ⚠️ 默认值**钉在 `FrameBrickDepth` 上**，不是随手取的：墩上那一列砖骑在墩心、横向占
	 * `FrameBrickDepth` ⇒ 墩宽超过它，墩两侧就会露出 `墩宽 − FrameBrickDepth` 的可见缝
	 * （灰泥已经被 `CSHouse_PierClipField` 裁掉了，缝里直接透到背景）。
	 * 见 `bPierStyleEnabled` 的字段注释。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "10.0"))
	float PierWidth = 20.0f;

	/**
	 * 拱间墩的柱头 / 柱础（2026-09-04，实拍 `img/tiny-glade-ref-twin-arch-pier.jpg`：
	 * 两道拱之间那根小石柱是柱础 + 柱身 + **更宽的柱头**，两道拱圈收在柱头上）。
	 * 横截面放大倍数；≤ 1 = 不出。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "1.0", ClampMax = "3.0"))
	float PierCapitalScale = 1.5f;

	/** 柱头与柱础各自的高度 cm。0 = 不出。代码里会再夹在墩高的 40% 内。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "0.0", ClampMax = "60.0"))
	float PierCapitalHeight = 14.0f;

	/**
	 * 连续拱之间的窄残料按**墩**处理：起拱线以下那片灰泥被裁掉，只剩门框砖站着。
	 * 关掉即退回"整跨都是灰泥墙"的旧观感。
	 *
	 * ⚠️ **裁掉 ≠ 不生成**（2026-08-30 裁决三，全项目架构不变量）：面板照常砌成实心盒，
	 * 那片灰泥由 `CSHouse_PierClipField` 在像素阶段 discard。几何永远实心，烘成 StaticMesh
	 * 之后洞仍由材质切出（裁决六）。
	 *
	 * ⚠️ **墩上是一列砖，不是两列**（2026-08-30 随解析推导一起改）：旧路让两侧的拱各出一条
	 * 门樘砖脚、各伸进跨度一半，于是两列砖在**墩正中共面对接**，从地面一直贯着一条竖缝
	 * （出图 `pier_after_pier.png` 可见），而 TG 实拍里墩就是一列。现在墩由
	 * `CSHouseFrame::MakePierPath` 单独出一条竖直砖路，两侧的拱都不出那一侧的门樘。
	 *
	 * ⚠️ **墩宽与砖进深仍是一对必须配平的参数**：那一列砖骑在墩心、横向占 `FrameBrickDepth`
	 * ⇒ 跨度 ≤ `FrameBrickDepth` 时正好盖满整条跨度（实拍里那种"一块砖宽的墩"），再宽下去
	 * 墩两侧会露出 `跨度 − FrameBrickDepth` 的**可见缝**（渲染层的缝，不是几何洞，但一样难看）。
	 * 条件与旧路"两条砖脚各伸进一半"给出的完全相同，所以 `PierWidth` 的默认值照旧钉在
	 * `FrameBrickDepth` 上，回归里那条断言继续有效，别把两者之一单独调走。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door")
	bool bPierStyleEnabled = true;

	/** 残料跨度 ≤ 此值 ⇒ 转墩（双阈迟回的低阈）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "0.0"))
	float PierStyleMaxWidth = 60.0f;

	/**
	 * 已是墩的跨度 ≥ 此值才转回灰泥墙（双阈迟回的高阈）。
	 *
	 * 与门拱点亮同一条纪律：跨度是拉尺寸/离地收窄的连续函数，单阈会让样式在阈值附近来回切换。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "0.0"))
	float PierStyleRestoreWidth = 75.0f;

	/** 每面墙两端的护角保留 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "0.0"))
	float CornerMargin = 60.0f;

	/** 拱顶目标高 cm（受 墙高 − 过梁带 约束）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "80.0"))
	float DoorHeight = 165.0f;

	/** 拱上方保留的过梁带 cm（保证墙顶连续）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "10.0"))
	float LintelBand = 40.0f;

	/** 沿子段的道路采样步长 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "5.0"))
	float DoorSampleStep = 25.0f;

	/** 墙线内外两条探测线的偏移 cm（取两者较大道路权重——路铺到墙根即算经过）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "0.0"))
	float DoorProbeOffset = 30.0f;

	/** 单个采样点计为"有路"的顶点色 R 权重阈值。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float DoorOnWeight = 0.5f;

	/**
	 * 单个洞的宽度上限 cm：一条比它更宽的路会被切成一排拱（相邻之间留 `PierWidth`），
	 * 而不是截断成一个巨拱。切分逻辑在 `CSHouse_SolveRoadRuns`，理由见那个文件的要点 ③。
	 *
	 * ⚠️ 上界另有一条**硬**约束：拱是半径 = 半宽的正半圆，`半宽 ≤ 门高 − 15 cm 起拱段`
	 * ⇒ 默认门高 165 时任何拱都不会超过 300 cm。这个参数是**风格**上限，不是安全阀。
	 *
	 * ⚠️ **默认值必须比一面墙的可用跨度小，否则这条路永远走不到。** 260 那一版
	 * （2026-09-04 上午）比演示房 400 深那面墙的可用长 232 还大，宽路只会得到一个顶满整墙的
	 * 巨拱，"连续石拱门"那档从没出现过。160 ⇒ 232 的跨度切成两拱各 106 + 一道 20 的墩，
	 * 与 `img/tiny-glade-ref-arcade-piers.jpg` 里"两拱夹一道窄墩"的比例同档。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "40.0"))
	float DoorMaxWidth = 160.0f;

	/**
	 * 滞回的保活宽度系数：**已经开着**的洞窄到 `DoorMinWidth × 此值` 以下才关。
	 *
	 * 旧口径的滞回是"覆盖率双阈 + key 里带段数 N"，拉尺寸跨过 `round()` 边界那一帧整条边的
	 * key 全部失配、滞回集体失效（合卷卷一记过）。区间没有编号，继承靠**与上一帧区间交叠**，
	 * 那个断点随之消失，所以这里只需要一个系数而不是两条覆盖率阈值。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "0.1", ClampMax = "1.0"))
	float DoorKeepWidthRatio = 0.8f;

	/**
	 * 洞心进哈希前的量化步长 cm。宽度那份是 `DoorWidthQuantum`，位置这份单列 ——
	 * 路是连续场，端点每帧都在亚厘米地抖，不量化的话哈希永远不等、房体每帧全量重建。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "0.5"))
	float DoorCenterQuantum = 2.0f;

	/**
	 * 离地收窄这一项还要不要参与门宽。
	 *
	 * ⚠️ 2026-09-04 之前它是**唯一**的宽度源（门宽 = 槽宽 × 离地收窄，与路无关），
	 * 现在门宽来自路在墙上截出的弦长，它降级成一个附加乘数。平地上 `GapMax ≈ 0` ⇒ 系数恒 1，
	 * 关掉与开着看不出区别；只有房子悬在坎上时才有效。保留是因为悬空的门确实该收窄。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door")
	bool bDoorGroundNarrowing = true;

	/** 离地收窄：落差 ≤ 此值门全宽 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "0.0"))
	float DoorGapFull = 30.0f;

	/** 离地收窄：落差 ≥ 此值门完全消失 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "10.0"))
	float DoorGapZero = 120.0f;

	/**
	 * 拱顶相对**起拱线**能升多高 cm。**0 = 半宽**（正半圆，2026-09-04 之前的唯一行为）。
	 *
	 * 半圆下"拱高 ≡ 半宽"，洞一宽拱就顶得很高（用户 2026-09-04：*门上升过高*）。
	 * 给它一个上限之后拱变**扁**（椭圆拱），与 `img/tiny-glade-ref-twin-arch-pier.jpg`
	 * 里"跨约 200 cm、起拱线以上只升约 80 cm"那一档对上。
	 *
	 * ⚠️ 这条只管**拱那一段**。整个洞的顶高另有一条 `DoorHeight`（并被
	 * `墙高 − LintelBand` 夹住）—— 想让门整体矮下去改那一条，想让拱扁下去改这一条。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "0.0"))
	float DoorMaxArchRise = 70.0f;

	/** 拱宽下限 cm，低于即不点亮。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "10.0"))
	float DoorMinWidth = 40.0f;

	/**
	 * 两个洞之间**剩下的采样环段**短于此值 cm 就并掉（两个洞合成一个）。0 = 不并。
	 *
	 * 路是在底部那条环线上切口子，切剩下的才是墙；碎到几厘米的环段既砌不成墙也挡不住路。
	 * 最刺眼的是转角：两边都有路时角上常留一点点，于是两道拱之间夹一片没意义的灰泥薄片。
	 * 详见 `FCSDoorRunParams::MinWallSegment`（它也解释了为什么这条不会把拱廊的墩并掉）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "0.0"))
	float DoorMinWallSegment = 30.0f;

	/** 拱宽进哈希前的量化步长 cm——地形微抖不触发全量重建。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Door", meta = (ClampMin = "0.5"))
	float DoorWidthQuantum = 2.0f;

	// -------------------------------------------------------------------------
	// Opening（洞 = 原型剖面 + 摆位，D4/D8）
	// -------------------------------------------------------------------------

	/**
	 * 洞缘曲线的弦高容差 cm：分段数由它反解（N = ceil(π / 2·acos(1 − Tol/R))），
	 * 所以房子放大时折角**不会**跟着线性放大 —— 写死段数才会。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Opening", meta = (ClampMin = "0.02", ClampMax = "5.0"))
	float OpeningChordTolerance = 0.2f;

	/** 两个洞的面板格之间必须留出的净距 cm（同边一维 S 区间按它膨胀后相交即判冲突）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Opening", meta = (ClampMin = "0.0"))
	float OpeningClearance = 10.0f;

	// -------------------------------------------------------------------------
	// Window（D8：诉求走显式列表，房子照旧只认谓词）
	//
	// ⚠️ **本节不含任何"自动填窗"的规则，也不许加。** TG 的窗全是玩家手放的，本项目的放置 UI
	// 还没做、「门洞触发规则」又是唯一没拍板的一条 —— 窗的来源就停在 `Windows` 这份列表上。
	//
	// 窗与门共用**同一条**通路：同一张 openings 表、同一个 clip 场、同一条谓词。
	// 与门不同的只有两件事：① 洞底离地（`Z0 > 0`）⇒ 房体在洞面板下面另砌一块无 clip 的实心
	// 窗台盒；② **不出门框砖** —— 洞缘由附属物自带的预制框盖住，窗周围没有任何砖头补全
	// （`CSHouseFrame::BuildEdgeElements`）。
	// -------------------------------------------------------------------------

	/** 关掉即整份列表不出洞（出图脚本靠它拍"同机位只切开关"的对照图）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Window")
	bool bWindowsEnabled = true;

	/** 窗户诉求列表。属性面板 / Blueprint / 测试都往这里填；被拒的条目留着，只是这一轮不出洞。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Window")
	TArray<FCSHouseWindow> Windows;

	/**
	 * 窗台下限 cm：洞底低于它的**窗**判 `SillTooLow`。
	 *
	 * 谓词原本只判 `Z0 < 0`（几何合法性），而窗台贴地的窗在几何上完全成立、观感上是个门洞。
	 * 门不受这条约束 —— 门恒 `Z0 = 0`，拿它卡门等于把所有门都拒了（判据里按 `Type` 分流）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Window", meta = (ClampMin = "0.0"))
	float WindowMinSillZ = 40.0f;

	// -------------------------------------------------------------------------
	// Frame（门框砖：洞缘那圈砌块）
	//
	// **这是 clip 路线的配套件，不是可选装饰**：discard 只丢像素、不生成表面，而判据沿墙厚
	// 方向恒定 —— 洞的范围内外脸、内脸、上下盖会被同时弃掉，中间不剩任何面，从洞口一眼看穿墙。
	// TG 用的就是这一手：拱/楣是与墙砖并列的真实实例（flags&32：按拱高压扁贴合曲线 + 免拱裁剪），
	// 用来遮住裁剪断口。
	//
	// 网格用 TG 提取出来的那块 brick —— 整个游戏只有这一块砖，是个 100³ 的**居中单位立方体**，
	// 尺寸全靠逐实例非均匀缩放（逆向报告 §1.4/§1.6）。所以这里给的是"想要多大"，不是"选哪块"。
	// -------------------------------------------------------------------------

	/** 是否砌门框。关掉会直接露出 clip 断口，只在调试判据时才该关。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Frame")
	bool bFrameEnabled = true;

	/** 门框砖的字典网格（TG 的 /PCGPlugins/HouseTest/TinyGladeAsset/Meshes/brick）。留空则不砌。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Frame")
	TObjectPtr<UStaticMesh> FrameBrickMesh;

	/**
	 * 门框砖（含接缝 / 角石 / 包边 / 砖层）的整体覆盖材质。**留空 = 逐段画 `FrameBrickMesh` 资产自带的材质**；
	 * 设了 = 盖住每一段。母材质必须勾 "Used with Instanced Static Meshes"（资产开了 Nanite 时是 "Used with Nanite"）。
	 *
	 * 转角墩下要剔的砖不靠材质藏（2026-09-15 起）：kernel 写负随机数，实例组件的剔除 / GPU-Scene 写入按契约把它藏掉，
	 * 所以留空换成资产材质也不会冒砖。`M_TinyGladeBrick` 的 OpacityMask 仍然认那个哨兵，但已经不是唯一的防线。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Frame")
	TObjectPtr<UMaterialInterface> FrameMaterial;

	/** 一块砖沿洞缘曲线的长度 cm（铺装缩放会在此基础上微调，让整条恰好铺满）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Frame", meta = (ClampMin = "4.0"))
	float FrameBrickLength = 26.0f;

	/** 砖沿面内径向的尺寸 cm —— 也就是砌块伸进墙体的那一维。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Frame", meta = (ClampMin = "2.0"))
	float FrameBrickDepth = 20.0f;

	/** 砖穿过墙厚的尺寸 cm。留 0 = 用墙厚（砖正好填满断口，两侧各露一个面）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Frame", meta = (ClampMin = "0.0"))
	float FrameBrickThickness = 0.0f;

	/**
	 * 砖相对墙面的**外凸** cm（两面各凸这么多；只在 `FrameBrickThickness = 0` 的自动档生效）。
	 *
	 * 2026-09-04 用户："很多结构离墙太近了，应该是它们的中心在墙 mesh 上，而不是它们的最远端。"
	 * 之前砖的穿墙厚度 = 墙厚、路走墙厚正中 ⇒ 砖的外表面与墙面**共面**，拱圈石 / 墩 / 勒脚
	 * 全贴在灰泥里一点都不凸；角石则独立按两面墙的外棱锚定，见 `CSHouseQuoinLayout.ush`。
	 * TG 的砖本来就是墙本身、灰泥是盖在外面的一层，拱圈石天然凸出灰泥面并在门上投影
	 * （实拍 `img/tiny-glade-ref-door-in-arch.png`）。
	 * 中心仍在墙厚正中（"中心在墙 mesh 上"），厚度加 2×此值 ⇒ 两面各凸一截，
	 * 洞口断口两侧照旧被砖封住。门框砖 / 勒脚 / 角石共用一份 `BlockSize`，三家一起凸。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Frame", meta = (ClampMin = "0.0", ClampMax = "30.0"))
	float FrameBrickProtrude = 6.0f;

	/**
	 * 相邻砖之间的**排布缝** cm（随铺装缩放一起缩）。默认 0：净缝整个交给 FrameBrickBloat 出。
	 *
	 * 净缝 = FrameBrickLength × (1 − FrameBrickBloat) + FrameBrickGap，**必须为负**，理由见下。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Frame", meta = (ClampMin = "0.0"))
	float FrameBrickGap = 0.0f;

	/**
	 * 砖沿曲线（长度轴）的**胀大系数**：渲染尺寸 = FrameBrickLength × 本值，而排布位置一步不动
	 * ⇒ 相邻两块必然互相穿插，**砖缝是负的**。小于 1 会被夹回 1 —— 正缝正是它要消掉的那个缺陷。
	 *
	 * 这是照抄 TG 的一步，也是它"逐帧重排砖却完全看不出跳变"的物理原因：
	 * `_wall_wall_brick_lod0...vs_main.glsl:160-175` 把逐实例缩放乘在单位立方体的局部坐标上，
	 * `flags & 4` 置位、`flags & 1` 未置位那一支就是**水平两轴 ×1.1、竖直 ×1.0**。
	 * 砖数一变只是穿插量微调；**正缝**会在块数跳变的那一帧沿整条拱缘露出一条缝
	 * （旧默认 FrameBrickGap = 1.5 就是正缝，`Saved/TinyGladeShots/lit_after_archframe.png`
	 * 里一格一格断开的拱缘就是它）。
	 *
	 * 轴的对位 —— 本项目没有 TG 那套 flags，简化成一条规则、只胀长度轴：
	 *   · 长度轴（+Y，沿洞缘曲线，见 CSHouseFrame.usf 的基约定）对应 TG 的"沿砌层水平轴"，
	 *     是**唯一有邻居、唯一会露缝的轴** —— 必须胀。
	 *   · 穿墙轴（+Z）钉死在墙厚上（砖正好封住 clip 断口）、径向轴（+X）是拱缘的可见带宽，
	 *     两者都不与任何邻居相接：胀它们只会漂移一个被刻意标定过的尺寸，正是 TG 让竖直轴
	 *     ×1.0 的那条理由（"砖一层层码上去，竖直也胀会让层高漂移"），所以都留 1.0。
	 * TG 那条 10% "整块不胀"的抖动**没抄**：它是给成千上万块墙砖打散规律用的，摊到一条
	 * 十几块砖的拱缘上就是随机两块露出可见缝，还会直接推翻 House.FrameBrickOverlap 那条
	 * "任何块数下都不露缝"的断言 —— 而那条断言才是本字段存在的全部意义。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Frame", meta = (ClampMin = "1.0", ClampMax = "1.5"))
	float FrameBrickBloat = 1.1f;

	/** 铺装随机种子；同参数同结果（洞的 Tag 与边号参与派生）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Frame")
	int32 FrameSeed = 1;

	/**
	 * Reserved instance capacity for the door-frame bricks.
	 *
	 * Paid once at registration so the interactive path never reaches the growth branch:
	 * the allocation has to happen on the render thread, so growing always blocks, and which
	 * brush dab it lands on depends entirely on where the user happens to paint — a stall that
	 * is both hard to reproduce and hard to attribute. One brick is 5 float4 = 80 bytes, so 512
	 * bricks cost 40 KB.
	 *
	 * ⚠️ **订正（2026-09-06）**：原注释末句"Growth still happens if a house genuinely lays more
	 * than this"**是错的** —— `FBrickParams::MaxBricks` 走的是 `AppendFlatRun` 里那句
	 * `Count = Min(Count, MaxBricks - Cursor)`，**只截断，从不扩容**。撞上限的画面是"洞缘没砌完"，
	 * 而砖数 / 三角数 / 零阻塞断言全绿，只有那条 Warning 说得出来。
	 *
	 * ⚠️ **这不是砖层实际用的那个数**：开了 `bBrickWallEnabled` 之后走
	 * `EffectiveFrameCapacity()` —— 砖层的量级由 footprint 决定（6 × 4 m、檐高 3 m 就要 1110 块），
	 * 不该让用户手算，所以它自己按上界加够，**本属性那份留给门框 / 接缝 / 角石 / 包边四家当余量**。
	 * 上限 65536（用户 2026-09-06 定，≈ 5.2 MB/房）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Frame", meta = (ClampMin = "64", ClampMax = "65536"))
	int32 FrameReserveCapacity = 512;

	/**
	 * 这栋房**真正**预留的砖容量：`FrameReserveCapacity`，开了砖层就再加上砖层自己的上界。
	 *
	 * 注册期一次付清、之后只截断不扩容（扩容要在渲染线程分配，一定阻塞，且落在用户恰好画到的
	 * 那一笔上）。⚠️ 因此**把房子拉大到超出注册时算出的量，砖层会被截断而不是扩容** ——
	 * 那条 Warning 是唯一的提示。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House|Frame")
	int32 EffectiveFrameCapacity() const;

	// -------------------------------------------------------------------------
	// Seam（D7 接缝，2026-09-16「接触记录」）—— 共享记录，谁动了谁发，松手才提交
	//
	// 两栋房之间的关系是一条 `FCSHouseContact`（见 CSHouseContact.h）：房子只知道自己跟哪些接触
	// 有关（`Contacts`），接触只知道自己跟哪两栋房有关。两栋房各持一个 TSharedPtr 指向同一个对象。
	// 竖缝产生两样东西：轮廓交点上的接缝砖柱（只由 `Owner()` 那一端砌**一次**），以及把插进邻居
	// 房间里的那截墙抹掉的裁剪场（两端各裁自己的）。协议见 `ReceiveContactFrom` / `UpdateContacts`。
	//
	// 接缝砖**共用门框砖那一个组件**（`FrameComponent`）：TG 全库也只有一块 `brick`，
	// 而且这样容量、交接、剔除球、`SaveToStaticMesh` 出口、以及"材质勾没勾
	// bUsedWithInstancedStaticMeshes"那条执行面判据全都白拿。砖数因此共享 `FrameReserveCapacity`。
	// -------------------------------------------------------------------------

	/**
	 * 关掉即**这栋房不参与接缝**：接缝存在 ⇔ 两端都开着（09-15 裁决，取代早先"我画不画我这一份"）。
	 * 任一端关掉，那条接触整个消失 —— 两端的砖与裁剪一起没。出图脚本靠它拍"同机位只切开关"的对照图。
	 * 08-31 那条"不读邻居开关"的教训已被协议吸收：开关变化经提交发给对端，顺序不再影响结果。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Seam")
	bool bSeamEnabled = true;

	/**
	 * 横缝容差 cm：上房底与下房檐口相差不超过它 ⇒ 判横缝（承托 / 悬挑），不出竖缝。
	 * 挡的是「上房嵌进下房几十厘米」这个常态；TG 用 1.5 m 的柱高门槛顶替这条（计划 D7）。
	 * 一对房子取两栋里的较大者 —— 种类不能取决于谁最后提交。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Seam", meta = (ClampMin = "0"))
	float BearingTolerance = 100.0f;

	/**
	 * 同一条接触里 XY 更近的交点并成一根柱 cm（`CSHouseSeam::MergePosts`）。
	 * **不同接触的柱永不合并**：一条接触一组柱，数量与接触一一对应（2026-09-16 晚用户裁决）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Seam", meta = (ClampMin = "0"))
	float SeamPostMergeDistance = 30.0f;

	/** 交点离任一房 footprint 顶点不足它就不立柱（角石已在那儿）。TG 0.2 m。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Seam", meta = (ClampMin = "0"))
	float SeamVertexClearance = 20.0f;

	/** 柱高不足 N 块砖（`FrameBrickLength`）不砌。TG 3 块（1.5 m）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Seam", meta = (ClampMin = "0"))
	int32 SeamMinPostBricks = 2;

	/** 两墙外法线点积 ≥ 它（近平行同向）不出交点。TG 0.9。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Seam", meta = (ClampMin = "-1", ClampMax = "1"))
	float SeamParallelDot = 0.9f;

	/**
	 * 拖动兜底 s：拖动事件停了这么久还没等到松手（`PostEditMove(true)` / `Push*(bFinished=true)`），就按松手处理 ——
	 * 清掉拖动标志、重求值一次，接缝照常提交。房子为此在拖动标志亮着时自己 tick（同特征标记的 `DragIdleSeconds`）。
	 * 防的是松手事件不来：抓手收到的第一个 `bFinished` 会被降级成拖动帧、Esc 取消、选中不拖。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "CS House|Seam", meta = (ClampMin = "0.1"))
	float GizmoDragIdleSeconds = 2.0f;

	/** 这栋房参不参与接缝（`bSeamEnabled`）。接触的 `IsAlive()` 读它。 */
	bool IsSeamParticipant() const { return bSeamEnabled; }

	/** 这栋房能不能砌接缝砖：`Owner()` 的判据。 */
	bool CanBuildSeamBricks() const { return bFrameEnabled && FrameBrickMesh != nullptr; }

	/** 这栋房喂给接缝纯函数的那份输入（身份 + 摆位 + 尺寸，无任何派生表）。`FCSHouseContact::Classify` 读两端的。 */
	CSHouseSeam::FHouse MakeSeamHouse() const;

	/** 出砖方出柱时用的四道门槛（贴顶点 / 近平行 / 同接触内合并距离 / 最低柱高），全是本房参数。 */
	CSHouseSeam::FPostParams MakeSeamPostParams() const;

	/**
	 * 唯一的跨房通道（09-16 裁决）。`Contact` = `From` 与我之间现在的接触；空 = 不再接触 / 任一端关着 / `From` 被删。
	 *
	 * 验 → 改 → 标脏：先丢掉我手里所有涉及 `From` 的旧接触（顺手丢掉不活的），非空则收下，然后
	 * `RequestReevaluate()`。**绝不同步重求值、不调 `From` 的 getter** —— `From` 正在遍历它自己的表。
	 */
	void ReceiveContactFrom(ACSHouseActor* From, TSharedPtr<FCSHouseContact> Contact);

	/** 我手里是否持有这条接触（审计用：两端必须持有同一个对象）。 */
	bool HoldsContact(const FCSHouseContact* Contact) const;

	// -------------------------------------------------------------------------
	// Quoin（D7 的**墙自身转角**那一半，合卷卷一 A7 / 卷五 A11）
	//
	// 相邻两面墙在转角上**斜接**（footprint 折线化 3a-2 起；此前是直角对接），**没有穿模要遮**；
	// 角石盖的是外角那条竖直棱上的 UV 岛断裂（两面墙各自从局部 (0,0) 起算 UV，砖纹到角就断）
	// 与硬棱。判据因此是"棱被遮住"，不是"不穿模"。只出在转角 ∈ (0°, 90°] 的凸角上；
	// 算法与"为什么不另起一套排布"见 CSHouseQuoin.h。
	//
	// 角石与接缝砖、门框砖**共用一个组件与一份容量**（`FrameReserveCapacity`）——
	// TG 全库也只有一块 `brick`，且它的每砖记录里根本没有 mesh 索引字段（合卷卷五 §1）。
	// -------------------------------------------------------------------------

	/** 关掉即这栋房不出角石。出图脚本靠它拍"同机位只切开关"的对照图。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Quoin")
	bool bQuoinEnabled = true;

	/** 包角外棱沿角平分线向内缩的距离 cm。0 = 按 TG 比例浅凸于两面墙。
	 * 正值把整列角石压入墙面；超过凸出量时会隐入墙内。Point 是外棱锚点，不是砖心。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Quoin", meta = (ClampMin = "-50.0", ClampMax = "100.0"))
	float QuoinInset = 0.0f;

	/** 角石长边的随机变化，TG 基准厘米（默认 16，上限 16）。
	 * 按 FrameBrickLength / 69 折算：26 cm 层高时长边 34.67 ± 6.03 cm。
	 * 0 = 固定长边；长短边仍按层号交错。两张外表面的位置不受随机数影响。
	 * 原函数 0x141215E72–0x141215E8E 算 0.92 + 0.16*(2r-1)，
	 * 0x141215F0E / 0x14121698A 用层号奇偶换向；旧版“所有 test 都是 bool”的注释有误。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Quoin", meta = (ClampMin = "0.0"))
	float QuoinJitter = 16.0f;

	/** 分层抖动，TG 基准厘米（默认 21）；按 FrameBrickLength / 69 折算。
	 * 对位 random_splits 的 0.21 / 柱高，分点抖幅上限为 0.495 × 间距。
	 * 两端固定，内部边界共享，0 = 等分；改变此值会触发重新散布。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Quoin", meta = (ClampMin = "0.0"))
	float QuoinSplitJitter = 21.0f;

	// -------------------------------------------------------------------------
	// Trim（包边石，D7 的第三样；合卷卷一 A8 / 卷五 A11）
	//
	// 墙顶压顶石与墙脚勒脚石：同一套机制、只差一个高度。与角石共用 `SolveRun`、负缝、容量、
	// 随机数基；与门框砖共用组件与那条"母材质勾没勾 bUsedWithInstancedStaticMeshes"的执行面判据。
	// ⚠️ 必须避开洞，否则勒脚会从门口横穿过去 —— 而且所有数值断言都不会红。算法见 CSHouseTrim.h。
	// -------------------------------------------------------------------------

	/** 关掉即两条包边带都不出。出图脚本靠它拍"同机位只切开关"的对照图。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Trim")
	bool bTrimEnabled = true;

	/**
	 * 墙顶压顶石。
	 *
	 * ⚠️ **默认 false（2026-08-31 用户裁决「墙的上沿我看过 TG 中是没有的，可以去掉」）。**
	 * 与合卷卷五 §4.1 的实测一致：TG 的 `wall-constructor` 53 个源文件里没有任何
	 * coping / capping / parapet 产出物，墙顶就是最后一层墙砖本身，上沿看着不同只是因为
	 * 那一圈砖露出了顶面、又吃到 `flags&4` 的水平胀大。机制留着，默认不出。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Trim")
	bool bTrimTop = false;

	/** 墙脚勒脚石。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Trim")
	bool bTrimBase = true;

	/**
	 * 压顶石课程中心相对**墙顶**的偏移 cm。
	 *
	 * 0 = 骑在墙顶（一半埋进墙、一半探出去）—— 这是水平压顶的口径，也是 TG 那种
	 * "砖互相穿插、看不出接缝"的做法。正值整课往上抬（会在墙顶露出一条缝），负值往下沉。
	 * 课程本身的高度是 `FrameBrickDepth`（三家共用一份 `BlockSize`，见 CSHouseTrim.h）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Trim", meta = (ClampMin = "-100.0", ClampMax = "100.0"))
	float TrimTopOffset = 0.0f;

	/** 勒脚石课程中心相对**房底**的偏移 cm。0 = 骑在房底（一半埋进地）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Trim", meta = (ClampMin = "-100.0", ClampMax = "100.0"))
	float TrimBaseOffset = 0.0f;

	/**
	 * 包边在洞两侧额外让开的距离 cm。给门樘砖留位置，别和包边挤在一起。
	 *
	 * ⚠️ 太大会让短墙上的包边整段消失（剩余段短于半块砖就不出）—— 那是有意的下限，
	 * 不是 bug；症状是"某面墙没有勒脚"，先查这个值再查别的。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Trim", meta = (ClampMin = "0.0", ClampMax = "200.0"))
	float TrimOpeningClearance = 6.0f;

	// -------------------------------------------------------------------------
	// 砖层（两层墙之 A，2026-09-06 用户裁决「挖洞策略改成 TG 的真两层」）
	// -------------------------------------------------------------------------
	//
	// 砖层 = **一摞包边带**：`CSHouseTrim::BuildBand` 本来就是"沿四条边铺一行、按洞切断"，
	// 从房底摞到檐口就是整面砖墙。洞缘四级里的①删实例、②水平贴合由它现成做掉，
	// ③逐顶点垂直贴合与④逐像素兜底在 GPU 侧、还没做。

	/**
	 * ⚠️ **默认关**。开着它并**不会**把现在的灰泥墙板换掉 —— 两层是"砖底 + 灰泥面"，
	 * 而灰泥那半还没有独立网格（仍是墙板 + 材质假面）。所以现在开 = 在墙板外面再糊一层砖，
	 * 只用来对观感与量预算，**不是**终局形态。
	 *
	 * 计划 D4 的验收门要求"洞缘与改动前**逐像素相同**"，那本来就得两条路并存才比得了 ——
	 * 所以这个开关不是临时脚手架，是验收门的一部分。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Brick Wall")
	bool bBrickWallEnabled = false;

	/**
	 * 请求的层高 cm。实际层高会向下微调，让最后一层的顶正好对齐檐口（`PlanCourses`）。
	 *
	 * 默认钉在 `FrameBrickDepth` 上：`AppendFlatRun` 里砖的**进深轴朝上**，所以一层的竖向
	 * 占位就是进深。调得比进深小 ⇒ 层间穿插（TG 就是靠 `FrameBrickBloat` 的负缝咬住的）；
	 * 调得比进深大 ⇒ 层间露缝。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Brick Wall", meta = (ClampMin = "1.0"))
	float BrickWallCourseHeight = 20.0f;

	/** 砖层在洞两侧额外让开的距离 cm。与包边同义，但砖层要贴得更紧，所以默认小一半。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Brick Wall", meta = (ClampMin = "0.0"))
	float BrickWallOpeningClearance = 3.0f;

	// -------------------------------------------------------------------------
	// Pillar（承重柱，D9——独立组件，不进房体网格）
	// -------------------------------------------------------------------------

	/** 底边支撑点间距 cm（四角必有）。 */
	// -------------------------------------------------------------------------
	// 藤蔓（D13）—— 墙矩形 → 点集 → 填 ISM
	//
	// ⚠️ **既有的 `VineScatter` 三个入口对房子一个都用不了**：`CollectSurfaceTriangles`
	// （`CSVineScatter.cpp:38`）只遍历 `UStaticMeshComponent`，而房体挂在
	// `UCSMeshRenderComponent` 上 ⇒ 它**静默返回空三角集、不报错**。这条是新写的通路。
	// -------------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine")
	bool bVineEnabled = true;

	/** 枝：`ivy_branch`。⚠️ 它**只有 `Vertex_Position`**，法线与 UV 由
	 *  `CSHouseVine::BuildBaseMesh` 现补（见那里的注释），换别的网格前先读那一段。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine")
	TObjectPtr<UStaticMesh> VineBranchMesh;

	/** 叶：`ivy_leaf`。自带法线与 UV，只做换轴。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine")
	TObjectPtr<UStaticMesh> VineLeafMesh;

	/** ⚠️ 必须勾了 `bUsedWithInstancedStaticMeshes` —— 没勾的话引擎**静默退回默认材质**，
	 *  画面上是一片灰而所有 readback 断言照绿（石阶那个坑的同一条）。
	 *  `MI_ivy_branch_color` 的母材质 `M_TG_Texture` 恰恰没勾，所以本项目自建了
	 *  `M_TinyGladeIvyBranch`（`Scripts/TinyGladeMakeIvyMaterials.py`）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine")
	TObjectPtr<UMaterialInterface> VineBranchMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine")
	TObjectPtr<UMaterialInterface> VineLeafMaterial;

	/** 花：`ivy_flower`。实测包围盒 (76.04, 77.99, 38.40)、底面在 Z = 0 ⇒ 长度轴已经是 **+Z**，
	 *  `BuildBaseMesh` 的换轴参数取 2（恒等）。留空即不长花。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine")
	TObjectPtr<UStaticMesh> VineFlowerMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine")
	TObjectPtr<UMaterialInterface> VineFlowerMaterial;

	/** 叶子的季节。只写母材质上的 `Season` 标量，不换材质资产（见 `ECSVineSeason` 的注释）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine")
	ECSVineSeason VineSeason = ECSVineSeason::Summer;

	/** 沿墙每隔多少 cm 起一根藤。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "20.0"))
	float VineStrandSpacing = 90.0f;

	/** 一段的世界长度 = 一个 `ivy_branch` 实例的长度。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "5.0"))
	float VineSegmentLength = 26.0f;

	/** 一根藤最多几段。与 `VineStrandSpacing` 一起决定容量上限，所以它**必须有上界**：
	 *  容量是按配置一次预留的，交互期不许扩容（零阻塞纪律）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "1", ClampMax = "128"))
	int32 VineMaxSegments = 22;

	/** 每段方向的随机扰动（弧度）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.0", ClampMax = "1.5"))
	float VineWander = 0.55f;

	/** 相对"正上"的最大偏角（弧度）。撞到上界就**夹住** —— 障碍处怎么改向是 `VineMaxTurn`
	 *  那一条的事，与这里无关。取值明显大于 `VineMaxTurn` 时藤一段之内换不了横移的方向，
	 *  窄道里会靠"收短一步"熬过去（见 `CSHouseVine.cpp` 的 `CSHouseVine_StepFracs`）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.1", ClampMax = "1.5"))
	float VineMaxLean = 1.15f;

	/**
	 * **一段之内倾角最多转多少**（弧度）。这是"相邻段夹角有上界"的唯一来源。
	 *
	 * 2026-09-12 加的：旧算法在障碍处**直接改写**倾角（墙角把倾角取反、洞里再归零），
	 * 而倾角是相对竖直的**绝对**偏角 ⇒ 取反一次相邻段就折过 `2·|倾角|`，
	 * `VineMaxLean` 默认 1.15 时上界是 131.8°，症状是**藤在门洞边缘和墙角上"折断"式急拐**。
	 * 反编译实证：TG 的 `ivy_grower` 里一次镜像/归零都没有，方向是位置的连续函数
	 * （见 `Docs/TinyGlade/Vine_ObstacleTurning.md`）。
	 *
	 * ⚠️ **不要调到比 `VineWander` 小**：游走本身就是每段 ±`VineWander` 的转向，
	 * 预算更小会连**无障碍处**的形状一起改掉。取 0 = 藤笔直不转、撞上就收尾。
	 *
	 * 默认 0.70 是标定出来的拐点：它是"带洞的墙与无洞的墙爬得一样高"的第一个值，
	 * 再小藤会在两个拱之间 20 cm 的窄道里拐不过弯就收尾。标定表见 `FParams::MaxTurn`。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.0", ClampMax = "1.5"))
	float VineMaxTurn = 0.70f;

	/** 长度轴胀大系数。**与 `FrameBrickBloat` 同一条 TG 实证**：正缝会在藤的每个折点露出
	 *  一条亮缝，而折点恰恰最显眼；胀大之后相邻两段必然互穿，段数一变只是穿插量微调。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "1.0", ClampMax = "1.6"))
	float VineBloat = 1.15f;

	/** 枝的截面直径（世界 cm）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "1.0"))
	float VineThickness = 9.0f;

	/** 沿墙面法线离墙多远，避免与墙面 z-fighting。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.0"))
	float VineStandOff = 3.0f;

	/** 墙洞外扩多少才算"撞上"。TG 的 `ivy_grower` 读集里有 `PrevWallHoles`，藤是避让洞的。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.0"))
	float VineHoleClearance = 12.0f;

	/** 每段长叶子的概率。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float VineLeafChance = 0.72f;

	/** 叶片的世界长度与它的对称抖动。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "2.0"))
	float VineLeafSize = 26.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.0", ClampMax = "0.9"))
	float VineLeafSizeJitter = 0.35f;

	/** 段的开花概率。**只在 `VineFlowerFromFrac` 以上的段上掷** —— 贴地那一圈的花会被地形与
	 *  杂物挡住，是纯白付的实例。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float VineFlowerChance = 0.10f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float VineFlowerFromFrac = 0.45f;

	/** 花簇的世界**宽度**。高度 = 宽度 × `CSHouseVine::FParams::FlowerAspect`（实测高宽比）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "2.0"))
	float VineFlowerSize = 22.0f;

	/** 走到墙角时拐上相邻那面墙的概率（TG 的 `check_for_wall_jump`）。0 = 只镜像折返。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float VineJumpChance = 0.5f;

	/** 逐实例随机的用户种子。⚠️ 随机**只由 (墙号, 藤号, 段号, 种子) 决定**，不取
	 *  `InterlockedAdd` 的槽位 —— 槽位每次重扫都重掷，症状是"重建一次全场变色"，
	 *  而且不会有任何断言报红（S1 已经栽过一次）。 */
	/** 藤脚允许的最大地面空隙（cm）。超过它那一根不长 —— 房子悬空则一根藤都没有。
	 *  与 `PillarMinGap` 共用同一个 Gap 量，取值应当略大于它（理由见 `CSHouseVine::FParams::MaxGroundGap`）。 */
	/**
	 * 枝走**扫掠管子**（2026-09-06 裁决 1/2）而不是一段一实例。
	 *
	 * 留这个开关只为一件事：**新旧两条路的同机位对照**。管子那条定型后它连同 `VineBranchMesh`
	 * 一起删 —— 别把它当成一个长期的表现选项，两条路的观感差异（接缝 / 粗细阶梯）正是换路的理由。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine")
	bool bVineUseTube = true;

	/** 管子截面环的周向段数。3 = 三棱柱（TG 原始形态），6–8 接近圆。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "3", ClampMax = "24"))
	int32 VineTubeSegments = 8;

	/** 折线的 Catmull-Rom 细分次数。0 = 不细分（管子跟着原始折线的折点走）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0", ClampMax = "6"))
	int32 VineTubeSubdivide = 2;

	/**
	 * 生长前沿的速度（cm/s）。
	 *
	 * ⚠️ **房子是唯一真源**：它同时被 CPU 与材质用 —— CPU 拿它判断"前沿有没有越过变化点"
	 * （见 `ResolveVineSpawnTimes`），材质拿它推前沿。所以它由 `EnsureVineComponents`
	 * 下推进三张藤材质的 MID（同 `Season` 那条），**别去材质实例上直接改**：
	 * 改了那边 CPU 不知道，症状是改门之后藤要么跳一段要么倒退一段。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "1.0"))
	float VineGrowSpeed = 60.0f;

	/**
	 * 叶子比**枝的生长前沿**迟多少**秒**才开始张开（用户裁决 2026-09-06）。
	 *
	 * ⚠️ 材质里的量是**弧长 cm**（前沿是按弧长推的），这里给秒、由 `EnsureVineComponents`
	 * 按 `延迟 × VineGrowSpeed` 换算后下推。所以**改速度时延迟的秒数不变、光杆那一截的长度会变** ——
	 * 反过来（材质里直接写 cm）则是长度不变、秒数随速度伸缩。给秒是因为"延迟"本来问的就是时间。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.0", ClampMax = "30.0"))
	float VineLeafGrowDelay = 0.75f;

	/** 花比枝的前沿迟多少**秒**才开。**比叶子更迟** —— 花是长成之后才开的。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.0", ClampMax = "30.0"))
	float VineFlowerGrowDelay = 1.8f;

	/**
	 * 一个点从"没长"到"长成"花多少**秒** —— 也就是"张开"这个动作本身的时长。
	 *
	 * 枝与叶花共用它：三者的前沿是同一条，各写各的会让叶子在枝还没长实的地方就张开。
	 * ⚠️ **它太小的话延迟看不出来**：原来固定 12 cm（默认速度下 0.2 s），快到读不出动作，
	 * 于是"叶子比枝迟"这件事也无从分辨。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.01", ClampMax = "10.0"))
	float VineGrowFadeSeconds = 0.5f;

	/**
	 * 加载 / PIE 开始时是否把全部藤当成"首次出现"（= 整栋房子从零长一遍）。
	 * 假则写一个很早的哨兵时刻，藤在第一帧就是长成的。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine")
	bool bVineGrowOnLoad = true;

	/** 梢部收紧的辐射长度（cm）。从梢往回这么长的一段里管径平滑压到 `VineTipTaperMin`。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.0"))
	float VineTipTaperLength = 60.0f;

	/** 梢尖处相对主锥度的残留比例。**不能取 0**（零面积三角形 + NaN 法线）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float VineTipTaperMin = 0.06f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "0.0"))
	float VineMaxGroundGap = 25.0f;

	/** 沿墙采样地面空隙的间距（cm）。只影响"悬空判据"的分辨率，不影响藤的形态。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine", meta = (ClampMin = "10.0"))
	float VineGroundSampleSpacing = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Vine")
	int32 VineSeed = 1;

	// -------------------------------------------------------------------------
	// 装饰摆件（D12 的**锚点那一半**）—— 围着已有构件长，不是在地面上找空地
	//
	// ⚠️ 计划 D12 写的是「复杂度场 `RT_DecorField` + tile-argmax」，而 TG 里**没有对位物**
	// （它是七家锚点生产者 + 候选点烘在资产里）。两套方案的取舍是**挂起的决策 C2**，
	// 这里**只做锚点这一半**、不实现场、也不替 C2 下结论。详见 `CSHouseDecor.h` 的文件头。
	//
	// 每一家有自己的一组网格（TG 的对位物是每个生产者各自的 `ClutterMeshes` 读集）：
	// 一张网格 = 一个 palette 条目 = 一个实例化组件。**留空即这一家不长任何东西**。
	// -------------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor")
	bool bDecorEnabled = true;

	/** 门/拱两侧与门前引道两侧（TG 的 `add_autoclutter_around_gates`）。桶、箱、摊子这一类。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor")
	TArray<TObjectPtr<UStaticMesh>> DecorGateMeshes;

	/** 墙脚。柴垛、篮子、农具这一类。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor")
	TArray<TObjectPtr<UStaticMesh>> DecorWallFootMeshes;

	/** 檐口与屋脊（TG 的 `add_birdnests`）。鸟窝、鸟屋这一类。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor")
	TArray<TObjectPtr<UStaticMesh>> DecorRoofMeshes;

	/**
	 * 摆件的整体覆盖材质（三家共用一张）。
	 *
	 * **留空 = 每张摆件网格逐段画它资产上挂的材质**（2026-09-15 用户："让它使用我的资产的正确材质"）：
	 * `barrel` / `crates_w_flowers` / `stall_veggies` 是贴图木头（`MI_nani_solid_albedo_layer03`）+ 顶点色两段，
	 * `firewood` / `birdnest` 是 `MI_TG_VertexColor` 一段。设了 = 盖住每一张网格的每一段（旧用法，比如
	 * `Scripts/TinyGladeMakeDecorMaterial.py` 建的那张只读顶点色的 `M_TinyGladeDecor`）。
	 *
	 * ⚠️ **母材质必须勾 `bUsedWithInstancedStaticMeshes`**，否则引擎在实例路径上会
	 * **静默换成默认材质**，症状与"没绑材质"逐像素相同（一片灰），而所有 readback 断言照绿。
	 * `IsDecorDrawable()` 逐段查每一张解析出来的材质（覆盖或资产）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor")
	TObjectPtr<UMaterialInterface> DecorMaterial;

	/** 墙脚每隔多少 cm 一个锚点。**摆件的多少完全由锚点个数决定** —— 这是锚点法与场法最本质的
	 *  差别（TG：有几扇窗就有几个花箱位）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor", meta = (ClampMin = "40.0"))
	float DecorWallFootSpacing = 150.0f;

	/** 檐口每隔多少 cm 一个锚点（屋脊按它的 1.13 倍，见 `CSHouseDecor::FParams`）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor", meta = (ClampMin = "40.0"))
	float DecorEaveSpacing = 220.0f;

	/** 摆件之间的最小间距 cm（球测）。计划 D12 的「同类间距球」在只有一层锚点时退化成这一条。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor", meta = (ClampMin = "0.0"))
	float DecorMinSpacing = 90.0f;

	/** 道路权重过阈的锚点直接丢掉（TG 的 `PathRaster` mask 订阅）。摆件因此自动分列路的两侧。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DecorRoadReject = 0.3f;

	/**
	 * 整体缩放。**0.5 是量出来的不是猜的**：`clutter/` 那 58 张网格的包围盒实测在
	 * 100–250 cm 之间（`barrel` 106×106×175、`stall_veggies` 206×122×148、`birdnest` 104×97×36），
	 * 而本工程的墙高默认 300 cm —— 原尺寸摆上去一个桶就有半堵墙高。
	 * 枚举值写在这里而不是 `CSHouseDecor::FParams`：模块侧不该知道资产多大（同藤蔓的
	 * `VineThickness`）。换别的网格前先量一遍包围盒。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor", meta = (ClampMin = "0.05"))
	float DecorScale = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor", meta = (ClampMin = "0.0", ClampMax = "0.8"))
	float DecorScaleJitter = 0.16f;

	/**
	 * 逐实例随机的用户种子。⚠️ 随机**只由 (家族, 锚点 id, 种子) 决定**，不取 `InterlockedAdd`
	 * 的槽位 —— 槽位每次重扫都重掷，症状是"重建一次全场变色"，而且不会有任何断言报红
	 * （S1 已经栽过一次）。这个身份同时是将来 `DeletedAutoClutter` 抑制集的键，见 `CSHouseDecor.h`。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Decor")
	int32 DecorSeed = 7;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Pillar", meta = (ClampMin = "50.0"))
	float PillarSpacing = 250.0f;

	/** 悬空判定阈值 cm：支撑点落差超过它才生柱。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Pillar", meta = (ClampMin = "1.0"))
	float PillarMinGap = 10.0f;

	/** 方柱截面边长 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Pillar", meta = (ClampMin = "5.0"))
	float PillarSize = 30.0f;

	/**
	 * 柱子用**砖砌**而不是摞方盒（2026-09-06 用户裁决，B 档：石柱 + 托架）。
	 *
	 * 留这个开关只为新旧同机位对照。砖那条定型后它连同 `PillarMesh` / `SubmitPillarMesh`
	 * 一起删 —— 方盒本来就是占位。
	 * ⚠️ 切换时**另一条必须显式清掉**（`PillarMeshComponent->SetGpuMesh(nullptr)` /
	 * 砖的空表走 counter 清零）：藤蔓换管子时正是漏了这一条，画面上两套同时存在。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Pillar")
	bool bPillarUseBricks = true;

	/** 柱砖的网格。留空则退回方盒。与墙砖 / 门框砖是**同一块** `brick`（TG 没有柱子网格）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Pillar")
	TObjectPtr<UStaticMesh> PillarBrickMesh;

	/** 一层砖的高度（cm）。层数按柱长取整后层高会被摊匀，所以这只是目标值。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Pillar", meta = (ClampMin = "4.0"))
	float PillarCourseHeight = 20.0f;

	/** 逐层绕竖轴的随机偏转（弧度）。TG 的石柱不是笔直码齐的。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Pillar", meta = (ClampMin = "0.0", ClampMax = "0.8"))
	float PillarYawJitter = 0.035f;

	/** 顶部出挑的层数（TG 的 brackets / small_brackets）。0 = 不做托架。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Pillar", meta = (ClampMin = "0", ClampMax = "8"))
	int32 PillarBracketCourses = 3;

	/** 顶层相对砖宽的出挑比例。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Pillar", meta = (ClampMin = "0.0", ClampMax = "1.5"))
	float PillarBracketOverhang = 0.55f;

	/** 柱脚扎入地面的深度 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Pillar", meta = (ClampMin = "0.0"))
	float PillarEmbed = 5.0f;

	// -------------------------------------------------------------------------
	// Ground wiring
	// -------------------------------------------------------------------------

	/** 交互的地面。为空时 PostRegisterAllComponents 自动找关卡里第一个 ACSGroundActor。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House")
	TObjectPtr<ACSGroundActor> Ground;

	// -------------------------------------------------------------------------
	// Public API
	// -------------------------------------------------------------------------

	/** 清空全部哈希与滞回状态后全量重求值（调参后的手动强刷入口）。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS House")
	void RebuildHouse();

	/** 声明式重求值：落座 → 门拱 → 柱，两份 desc 各自哈希守卫。幂等，可被高频调用。 */
	virtual void ReevaluateSite() override;

	//~ ACSWallBase —— 房子是一圈**围合的墙**（外皮 = footprint）+ 屋顶
	virtual ECSWallKind GetWallKind() const override { return ECSWallKind::Enclosure; }
	/** footprint 围成的环（局部空间，墙脚 0、墙顶 `WallHeight`），`OutWorld = GetBuildTransform()`。 */
	virtual bool GetWallPath(FCSWallPath& OutPath, FCSWallSkins& OutSkins, FTransform& OutWorld) const override;
	/**
	 * 藤的参数：从本类那 38 个 `Vine*` 平铺属性**现组**（2026-09-22 统一时刻意不迁数据 —— 蓝图 / 关卡里的覆盖值、
	 * 脚本里的 `vine_*` 属性名都不动）。字段一一对应，见 `FCSWallVineSettings`。
	 */
	virtual FCSWallVineSettings GetVineSettings() const override;

	/**
	 * **合批唤醒：只标脏，由本栋房子自己的 `Tick` 兑现。** 一帧里会被同一栋房子收到很多次的
	 * 那些通知（标记登记 / 注销、地面广播）走这条，不要直接调 `ReevaluateSite()`。
	 *
	 * 起因（2026-09-10）：gizmo 多选拖 N 个窗标记时，每个标记**每 tick** 都会
	 * `RegisterFeatureMarker` 一次，而那里原本直接同步重求值 ⇒ 一帧 N 次完整重建：算门
	 * （约 340 次镜像双线性）、接缝、砖、藤、瓦、尖顶、门扇、摆件全跑 N 遍，收尾的
	 * `NotifyMarkersRebuilt` 还要对**每个**标记吸附一次 ⇒ N² 次。而前 N−1 次的结果全部被
	 * 后一次覆盖 —— 第 k 次跑的时候，第 k+1..N 个标记这一帧的新诉求还没写进来。纯粹是白烧。
	 *
	 * 现在一帧里来多少个通知，都只在 `Tick` 里兑现**一次**，与 N 无关。落在本帧还是下一帧，看通知
	 * 在世界 tick 之前还是之后到：gizmo 拖动走 `PostEditMove`，在世界 tick 之前 ⇒ 本帧；EdMode
	 * 的画笔在世界 tick 之后 ⇒ 下一帧。**至多延迟一帧**（用户裁决：视觉上无差别，且不经
	 * 任何 subsystem —— 房子自己决定什么时候更新）。
	 *
	 * "改完立刻读得到"不靠时机，靠的是每个派生物 getter 读前的 `FlushPendingReevaluate`：
	 * 无头脚本与单测整段跑在同一帧里，`Tick` 根本插不进来。
	 *
	 * tick 平时关着：这里打开，`Tick` 兑现后自己关掉，闲置的房子零开销。
	 */
	void RequestReevaluate();

	/**
	 * 有待兑现的合批唤醒就地补上。幂等；没有待兑现的就只是一次布尔判断。
	 *
	 * **所有读派生状态的入口都先走它**：各族计数、洞表、`Is*Drawable` / `Get*UndrawableReason`、
	 * GPU 回读、烘焙。`RequestReevaluate` 只标脏、等房子自己的 `Tick` 兑现，而"改完立刻读"
	 * 那条路不等 tick —— 无头回归、单测、Python 脚本都是改完紧接着读，而且整份脚本跑在同一帧里，
	 * 根本没有"下一帧"可等。不补票的症状是**读到上一次的账**，且没有任何报错。
	 *
	 * ⚠️ 2026-09-10 实测：起初只挂了洞相关的四个，理由是"别的计数只在同步入口之后读"——
	 * 那句是错的。`TinyGladeDemoRegression.py` 画完路紧接着读 `get_open_door_count()` /
	 * `get_frame_brick_count()`，读到的是画路**之前**的门数与砖数（188 = 72 角石 + 116 包边，
	 * 一块拱砖都没有），两条断言当场转红。
	 *
	 * **不挂它的只有三类，且都有理由**：`QueryFeatureReject`（标记每 tick 都会调的预判，挂了会让
	 * N 个标记重新变回 N 次重建；终裁由 `NotifyMarkersRebuilt` 下）、读的不是派生物的
	 * （`GetFeatureMarkerCount` 是登记表、`GetBrickWallBrickBudget` 是纯配置）、以及
	 * `GetReevaluateCount`（合批的观测量，挂了就测不到合批）。
	 *
	 * 重建途中被调到（`ReevaluateSite` 内部有几处读计数写日志）是安全的：欠账在 `ReevaluateSite`
	 * 一开头就清了，补票只是一次布尔判断；即便重建途中又来了新通知，`bInReevaluate` 也会让它
	 * 立刻返回，留给下一次 `Tick`。
	 */
	void FlushPendingReevaluate() const;

	/**
	 * `ReevaluateSite` 真正跑完的次数。
	 *
	 * 合批是否生效的**唯一可观测判据**（单测 `House.MarkerDragBatchesRebuilds` 读它）：
	 * 一帧里拖 N 个窗，这个数的增量必须与 N 无关。没有它，"合批"只是一句没人守着的话。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House")
	int64 GetReevaluateCount() const { return ReevaluateCount; }

#if WITH_EDITOR
	/**
	 * 基类烘焙入口（一族一张，族表见 `GetInstancedFamilies`），这里只先补合批欠账 ——
	 * 烘的必须是这一刻的状态。房体 / 柱走网格路（`UCSMeshRenderComponent::SaveToStaticMesh`）。
	 */
	virtual int32 SaveInstancedToStaticMeshes(const FString& BakeFolder, bool bSaveAssets) override;
#endif

	/**
	 * 单边推拉（计划 D5 的**机制入口**）：把第 EdgeIndex 面墙沿它的世界外法线推 Offset cm，
	 * 对侧墙世界位置不变、中心随动，然后立刻重求值。返回**实际**生效的位移。
	 *
	 * ⚠️ 这一轮**不做**抓手 / gizmo / EdMode（与 D8 窗户同一条纪律）。这里是"尺寸连续变化时
	 * 派生物跟得住"的那套机制的唯一入口，将来的 handle actor 也只是往这里喂 Offset。
	 *
	 * 两件事都在这条路上一次做完，分开做就会各漏一样：
	 *  ① `MinFootprint` 下限（`CSHouse_ApplyEdgePush`，纯函数、可单测）——⚠️ 早先这里还写着
	 *     "禁带"，那一套已随四坡屋顶于 2026-08-31 删除，`ApplyEdgePush` 只剩硬下界这一条 clamp；
	 *  ② `ReevaluateSite()` —— 走的是与平移完全相同的那条零阻塞路径，不另开快路。
	 *     （曾经夹在中间的 `UCSHouseSubsystem::MarkHouseDirty` 随 subsystem 于 2026-09-16 删除：它只是让
	 *     subsystem 下一 tick 再重求值一遍，与这里的同步重求值重复。）
	 *
	 * `bFinished` 传 true 表示"松手"：等价于 gizmo 的 `PostEditMove(bFinished=true)`，
	 * 会置 `bForceFullRebuild` 把拖动期为了不阻塞而留下的容量/包围盒余量重新收紧。
	 *
	 * ⚠️ **历史：这里曾经挂着一条「拖动中的画面 ≠ 松手后的画面」的未结案缺陷**，
	 * 2026-08-31 已定位并修掉。当时的现象：同一份世界状态下两图差 6.3–9.0% 像素、
	 * 背光墙整片压到精确 (0,0,0)，而 CPU 侧每个量都相同、零像素位移。
	 * **真因是两条互相独立的缺陷，都不在拉尺寸这条路上**：
	 *  ① `CSMeshOps.usf` 的 `TransformMeshCS` / `NegateNormalsCS` 用 **uint 的逻辑右移**解包
	 *     snorm8 切空间，把每个分量的符号剥掉了 ⇒ 凡是被 `ApplyBodyPlacement` 增量搬过的
	 *     网格，法线全部折进 +X+Y+Z 卦限，朝 −X/−Y 的墙当场翻面。修法见
	 *     `UnpackSnorm8888`。（拖动归位、平移、快扫唤醒都走那条增量路，所以不只拉尺寸中招。）
	 *  ② `RebuildFrame` 的早退哈希漏了墙框架，拉垂直方向时门框砖整段不重排。
	 *     修法见 `CSHouse_HashElementFrames`，判据见 `GetFrameScatterCount`。
	 * 修完同机位实测：拖动态 vs 松手态 6.83% → **0.013%**，`SCS_BASE_COLOR` 与关掉 Lumen
	 * 两组都是 **0.000%**（`Scripts/TinyGladeShotResizeProbe.py`）。
	 *
	 * 所以「画面收敛以松手为准」**不再成立** —— 拖动中的画面就是对的。
	 * 但 `bFinished` 仍然该传：它是把拖动期为了不阻塞留下的容量/包围盒余量收紧的唯一入口，
	 * 只是它不再背着一条画面正确性的债。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS House")
	float PushEdge(int32 EdgeIndex, float Offset, bool bFinished = false);

	/**
	 * 改墙高（D5 的第二个自由度）：`WallHeight += Offset`，夹在 `MinWallHeight` 上，然后立刻
	 * 重求值。返回**实际**生效的高度变化。
	 *
	 * 与 `PushEdge` 的两点不同：
	 *  ① **不动 actor 变换** —— 只改一个标量，所以没有那边"父级移动 Applied/2 把抓手一起带走"
	 *     的 2× 回路。抓手侧的记账量法照旧保留，它还管着"顶在下限上不许攒残差"。
	 *  ② 波及面更广：檐口高、屋面、门洞的离地收窄、窗的 `AboveEave` 谓词、藤蔓与摆件的锚点
	 *     全都读 `WallHeight`，所以这条路必须走完整的 `ReevaluateSite()`，不能只重建墙板。
	 *
	 * ⚠️ 调用方必须用**返回值**记账，不是传入的 `Offset`（同 `CSHouseResize.h` 的返回值契约）：
	 * 顶在 `MinWallHeight` 上时两者不等，记请求值会让残差一路累积，松手瞬间房子跳一大截。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS House")
	float PushHeight(float Offset, bool bFinished = false);

	/** 只改屋脊相对檐口的起伏，不改变墙高或房底。返回实际变化 cm。 */
	UFUNCTION(BlueprintCallable, Category = "CS House|Roof")
	float PushRoofHeight(float Offset, bool bFinished = false);

	UFUNCTION(BlueprintPure, Category = "CS House|Roof")
	float GetRoofRise() const;

	UFUNCTION(BlueprintPure, Category = "CS House|Roof")
	bool IsFlatRoof() const;

	/** 楼梯只读露台诉求输入。避免查询时兑现房屋更新而重入彼此的重建。 */
	FCSStairsTerraceSite GetStairsTerraceSite() const;
	static FCSHouseTerraceChanged OnTerraceChanged;

	UFUNCTION(BlueprintPure, Category = "CS House|Flat Roof")
	int32 GetTerraceEntranceCount() const { FlushPendingReevaluate(); return CurrentTerraceEntranceCount; }

	UFUNCTION(BlueprintPure, Category = "CS House|Resize")
	ACSHouseHeightHandleActor* GetRoofHandle() const;

	/**
	 * 改房底（与 `PushHeight` 对称的第三个自由度，用户裁决 2026-09-14「底动顶不动」）：
	 * `HeightOffset += Offset`、`WallHeight -= Offset`，**同一次调用里两个量一起改完**再重求值。
	 * 返回**实际**生效的变化（契约同 `PushHeight` 与 `CSHouseResize.h`：记账用返回值，不用请求值）。
	 *
	 * 往上（Offset > 0）：房子离地变高、墙变矮，悬空部分照现有规则长承重柱；往下反过来。落座公式
	 * `房底 Z = max(footprint 地面) + HeightOffset` 不变，所以**檐口与屋顶的世界高度不动**
	 * （`House.BaseHandle` 钉着；落座有 0.5 cm 的迟滞，亚厘米级的单步会攒到越过它才一起落地）。
	 *
	 * 两条下限都夹，顶在任一条上时请求值与生效值不等：
	 *  ① `WallHeight ≥ MinWallHeight` —— 限制往上；已经矮于下限的旧存档不许再往上，但往下照常。
	 *  ② `HeightOffset ≥ min(0, 当前 HeightOffset)` —— 贴地为止；旧存档里 `HeightOffset` 已经是负数的
	 *     以当前值为界（**不许一抓就跳到 0**）。
	 *
	 * ⚠️ 与 `PushHeight` 的关键不同：这条路**会动 actor 变换**（重求值落座 `SetActorLocation`），而抓手
	 * attach 在房子下 —— 父级 Z 移动会把底框一起带走，正是拉尺寸锥子那边写过的 2× 回路。底框走同一套
	 * 记账量法 + 推完统一重摆（`ACSHouseHeightHandleActor::OnHandleDrag`），拖一下房底恰好走一下。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS House")
	float PushBase(float Offset, bool bFinished = false);

	// -------------------------------------------------------------------------
	// 拉尺寸模式（计划 D5 的交互层）
	// -------------------------------------------------------------------------

	/**
	 * 进入 / 退出拉尺寸模式的静态广播。编辑器模块据此维护"处于编辑态的房屋集"并接管
	 * 失选监听（`FCSHouseResizeSelectionWatcher`）。**静态**是因为监听方是模块而不是实例。
	 */
	static FCSHouseResizeModeChanged OnResizeModeChanged;

	/**
	 * 生成 N + 3 个抓手 actor：footprint 的每条边一个**推墙箭头**（水平推拉那面墙；N = `GetFootprint()`
	 * 的边数，也就是**取完凸包之后**的边数；观感是 TG 原版 `flat_arrow` + 描边，2026-09-22），外加墙顶、房底、
	 * 屋顶三个高度**矩形框**（`ACSHouseHeightHandleActor`，画的是包围盒）：檐口那个上下拖改墙高、房底那个上下拖改房底
	 * （`PushBase`，底动顶不动），屋顶小框改起伏（`PushRoofHeight`）。选中任一个用编辑器原生 gizmo 拖即可。
	 *
	 * 这就是"点一个蓝图函数，冒出几个能拖的把手"的那个函数：`CallInEditor` 让它直接出现在
	 * 房子详情面板上，`BlueprintCallable` 让蓝图 / Python 也能调。**幂等** —— 已经在模式里
	 * 再调一次只把锥子数对齐边数、把抓手摆回规范位置，不会生出第二组。
	 *
	 * 在模式里改了边数（改 `FootprintShape`、撤销等）时，锥子在下一次重求值里按新边数对齐
	 * （`SyncEdgeHandlesToFootprint`），三个高度框不重建。
	 *
	 * 抓手是 `RF_Transient` 的，不存盘、不进 outliner 的保存路径；房子被删或调
	 * `ExitResizeMode()` 即销毁。
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS House|Resize")
	void EnterResizeMode();

	/** 销毁全部抓手并广播退出。幂等：不在模式里调它什么都不做。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS House|Resize")
	void ExitResizeMode();

	UFUNCTION(BlueprintPure, Category = "CS House|Resize")
	bool IsInResizeMode() const { return ResizeHandles.Num() > 0; }

	/**
	 * 当前抓手（失效的已剔除）：每条边一个水平锥子 + 檐口框 + 房底框，**混在同一个数组里**，
	 * 顺序不承诺（边数变了补生的锥子排在末尾）—— 按类型取用走下面几个接口。
	 * 类型是共同基类 `ACSHouseHandleActor` —— 房子对它们只做三件无差别的事
	 * （摆位、注销、销毁），没有一处需要区分是哪一种。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House|Resize")
	TArray<ACSHouseHandleActor*> GetResizeHandles() const;

	/** 只要水平推拉锥子（每条边一个，无头测试按边号取用）。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Resize")
	TArray<ACSHouseResizeHandleActor*> GetEdgeHandles() const;

	/** 檐口那个调墙高的框；不在模式里返回 nullptr。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Resize")
	ACSHouseHeightHandleActor* GetHeightHandle() const;

	/** 房底那个框（`PushBase`，底动顶不动）；不在模式里返回 nullptr。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Resize")
	ACSHouseHeightHandleActor* GetBaseHandle() const;

	/**
	 * 把**全部**抓手摆回各自的规范位置，并按当前 footprint 重算"窗框"四条边的长度。
	 *
	 * 单边推拉会改 footprint 与房心，而 attach 只保相对位置不变 —— 不重摆的话四根条子会
	 * 各自漂出框外（被拖的那根漂得最快，画面上是框裂开）。
	 *
	 * ⚠️ **早先这里有个 `Except` 参数，用来"拖拽期不回写正在被拖的那一个"（怕与 gizmo 抢写）。
	 * 2026-09-06 连同那条纪律一起删掉** —— 那是设计期的预判，实际代价是被拖的条子会跑到
	 * 光标前面去。重摆之后抓手的记账量恰好等于规范位置，"拖 1 m 墙走 1 m"不受影响，
	 * 判据见 `House.ResizeHandle` 的第 ②③ 段。
	 */
	void SnapResizeHandles();

	/** 抓手自毁时回调，把它从表里摘掉；表空了即广播退出。 */
	void NotifyResizeHandleDestroyed(ACSHouseHandleActor* Handle);

	UFUNCTION(BlueprintPure, Category = "CS House")
	int32 GetOpenDoorCount() const;

	/** 当前洞的总数（门 + 窗 + 注入）。读之前补票，理由见 `FlushPendingReevaluate`。 */
	UFUNCTION(BlueprintPure, Category = "CS House")
	int32 GetOpeningCount() const { FlushPendingReevaluate(); return CurrentOpenings.Num(); }

	/**
	 * 稳定身份。subsystem 的注册表以它为 key，D7 的接缝 key 也用它（两房 GUID 的无序对）——
	 * 指针不行：接缝要能在 actor 重建/流送进出之后仍指向"同一栋房子"。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House")
	FGuid GetHouseId() const { return HouseId; }

	/**
	 * 这个洞放得下吗（D8 的可行性谓词，纯参数判定、零 GPU 回读）：与任一已有洞的面板格按
	 * OpeningClearance 膨胀相交即拒绝（**同边一维 S 区间**，Z 不参与 —— 用户裁决 2026-08-30，
	 * C1 选甲：永久放弃"门上开窗"），落在墙面之外或超出墙高也拒绝。
	 * 门拱优先于特征标记 —— 已点亮的子段先占位，窗再来就判不可行。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS House")
	bool QueryFeaturePlacement(const FCSWallOpening& Candidate) const;

	/**
	 * 同一条谓词，但**把拒绝原因给出来**（计划 D8 的 `FCSFeaturePlacement::Reason`）。
	 *
	 * 为什么必需而不是锦上添花：门拱优先于特征标记（D6），一面墙被道路点亮成连拱时窗
	 * **永远**放不进去 —— 那是预期行为，但用户看到的是"窗放上去就消失"。没有回执，
	 * 这两件事在画面上逐像素相同。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS House")
	ECSFeatureReject QueryFeatureReject(const FCSWallOpening& Candidate) const;

	/**
	 * 射线打在本房哪面外墙上（D8 宿主解析）。世界空间入、**本房局部**命中出。
	 *
	 * ⚠️ **解析求交，不是引擎 trace** —— 房子的 gpumesh 全线 `NoCollision`，`LineTraceSingle`
	 * 一栋都打不到（计划 D8 明写这条）。判据本体在纯函数 `CSHouse_RayHitWall` 里，本方法
	 * 只负责一件别处做不了的事：拿**烘常驻流用的那个变换**（`GetBuildTransform`，只取 yaw +
	 * 位置）去解世界坐标。拿 `GetActorTransform` 解会在有 pitch/roll 的房子上把命中点算到墙外。
	 */
	FCSWallHit RayHitWall(const FVector& WorldOrigin, const FVector& WorldDir, float MaxDistance) const;

	/** 就近找墙（射线落空时的退路）。同样是世界入、局部出，见 `CSHouse_NearestWall`。 */
	FCSWallHit NearestWall(const FVector& WorldPoint, float MaxDistance) const;

	/**
	 * 锚点 → **世界**变换（D8，2026-09-06）。附属物按锚点吸附时调它 ——
	 * 对位 TG 的 `CachedDecoratorTransforms`：变换是从锚点算出来的派生量，不是存下来的。
	 *
	 * 放在房子上而不是让标记自己拼，是因为"构建空间"（`GetBuildTransform`，只取 yaw）
	 * 是房子的私事：烘常驻流、`RayHitWall`、这里，三处必须用同一个变换，各拼一遍就会出现
	 * "房子一转，窗贴到旁边去了"。顺带它也能给 Blueprint / 无头脚本直接用。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House|Window")
	FTransform AnchorToWorld(const FCSWallAnchor& InAnchor, float HalfHeight, float Standoff) const;

	// -------------------------------------------------------------------------
	// 特征标记登记（D8）—— 标记提诉求，房子照旧只认谓词
	//
	// ⚠️ **这份表是 transient 的，故意不序列化**：权威在标记 actor 自己身上（它序列化了自己的
	// 摆位），加载后由标记在 `PostRegisterAllComponents` 重新登记。序列化它就会出现"标记删了
	// 但房子里还留着一扇窗"这类只有重开关卡才显形的幽灵 —— 与「派生物纯函数、不序列化」
	// 是同一条纪律。
	//
	// 与属性面板那份 `Windows` 并存、互不覆盖：两者都只是"诉求"，一起喂给同一条谓词。
	// -------------------------------------------------------------------------

	/** 幂等：同一个 `MarkerId` 再登记就是改诉求。会标脏（下一次重求值生效）。 */
	void RegisterFeatureMarker(const FGuid& MarkerId, const FCSHouseWindow& Demand,
		class ACSHouseFeatureMarker* Marker = nullptr);

	/** 注销。找不到就什么都不做（标记自毁 / 换宿主时两边都会调，允许空打）。 */
	void UnregisterFeatureMarker(const FGuid& MarkerId);

	/** 当前登记的标记数（无头断言用 —— 只看窗洞数分不清"没登记"与"登记了被拒"）。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Window")
	int32 GetFeatureMarkerCount() const { return MarkerWindows.Num(); }

	/** 见 `StartWindowBrush()`。编辑器模块在启动时订阅。 */
	static FCSHouseWindowBrushRequest OnWindowBrushRequest;

	/**
	 * 窗笔刷这一笔要放的窗（子蓝图那一档，如 `BP_Window_Cottage_1x1`）。
	 * 空 = 退回 `ACSWindowMarker` 本身（C++ 默认那块 cottage 框板）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS House|Window")
	TSubclassOf<ACSHouseFeatureMarker> WindowBrushClass;

	/**
	 * **点一下加一扇窗**（2026-09-06 用户裁决）：进笔刷模式 → 在墙上点一下 → 立刻退出。
	 *
	 * 为什么是笔刷而不是"把窗蓝图拖进视口"：拖放那条路要靠 actor 自己的 forward 去解析宿主，
	 * 而**朝向什么时候被应用**在两条 spawn 路径上不一样（`UEditorEngine::AddActor` 把 Rotation
	 * 一起传给 `SpawnActor`，`EditorActorSubsystem` 那条却是先放置、回调之后才设朝向）。点击给的
	 * 是相机射线 + 精确命中点，不依赖 actor 自身朝向 —— 从根上没有那个问题。
	 *
	 * 落地全在 `UCSHouseLibrary::PlaceMarkerAlongRay`，本函数只发一个请求。
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS House|Window", meta = (DevelopmentOnly))
	void StartWindowBrush();

	/** 这一轮真正砌出来的窗洞数（`Windows` 里过了谓词的那些）。读之前补票。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Window")
	int32 GetWindowCount() const { FlushPendingReevaluate(); return CurrentWindowCount; }

	/** 这一轮被谓词拒掉的窗诉求数。**必须与上一条一起看** —— 只看前者分不清"没填"与"被拒"。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Window")
	int32 GetWindowRejectCount() const { FlushPendingReevaluate(); return CurrentWindowRejectCount; }

	/**
	 * **诊断 / 验收专用**：窗这一帧到底会不会出现在画面上，不会的话原因是什么。
	 *
	 * ⚠️ 执行面照抄 `IsVineDrawable` / `IsDecorDrawable`（本项目在"readback 全绿而画面上
	 * 什么都没有"这件事上栽过两次），但窗多一环、且那一环是窗**独有**的致命项：
	 * **洞是墙材质用 OpacityMask 逐像素切出来的** —— 墙材质一旦不是 Masked，洞在画面上
	 * 根本不存在，而洞数、砖数、三角形数、零阻塞四条断言**全部照绿**。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House|Window", meta = (DevelopmentOnly))
	bool IsWindowDrawable(FString& OutReason) const;

	/** 同上，但把原因当返回值给出来（空串 = 画得出来）。理由逐字见 `GetVineUndrawableReason`。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Window", meta = (DevelopmentOnly))
	FString GetWindowUndrawableReason() const;

	/** 这一轮与几个邻居交汇（= 交点数，一个交点一根接缝砖柱）。0 = 没和谁碰上。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Seam")
	int32 GetSeamCornerCount() const { FlushPendingReevaluate(); return CurrentSeamCornerCount; }

	/** 接缝砖数（含在 `GetFrameBrickCount()` 里 —— 两者共用一个组件与一份容量）。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Seam")
	int32 GetSeamBrickCount() const { FlushPendingReevaluate(); return CurrentSeamBrickCount; }

	/** 这一轮被接缝抹掉的墙段数（clip，不是几何洞）。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Seam")
	int32 GetSeamCutCount() const { FlushPendingReevaluate(); return CurrentSeamCuts.Num(); }

	/** 我手里的接触数（竖缝 + 横缝）。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Seam")
	int32 GetContactCount() const { FlushPendingReevaluate(); return Contacts.Num(); }

	/** 我出砖的柱数（聚簇之后）。非出砖方恒为 0 —— 这是"一条缝只砌一次"的观测量。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Seam")
	int32 GetSeamOwnedPostCount() const { FlushPendingReevaluate(); return CurrentSeamOwnedPostCount; }

	/** 正在被 gizmo / 抓手拖动（接缝冻结期）。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Seam")
	bool IsInGizmoDrag() const { return bInGizmoDrag; }

	/** 地面正在被拖动 / 落笔途中（最近一次地面广播未提交，接缝冻结期）。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Seam")
	bool IsInGroundDrag() const { return bInGroundDrag; }

	/** 欠着一次重求值（`RequestReevaluate` 之后、`Tick` / getter 补票之前）。 */
	UFUNCTION(BlueprintPure, Category = "CS House")
	bool IsReevaluatePending() const { return bReevaluatePending; }

	/**
	 * 接触表审计（调试与回归用）：每条接触都活着、两端持有同一个对象、两端现在真的接触、同一对没有两条、
	 * 不在拖拽时没有欠着的提交。返回违反项，空即通过 —— 它把"静默陈旧"变成会红的断言。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS House|Seam")
	TArray<FString> AuditContacts() const;

	/** 测试用：当前裁剪段（读前补票）。 */
	const TArray<FCSWallCut>& DebugGetSeamCuts() const { FlushPendingReevaluate(); return CurrentSeamCuts; }
	/** 测试用：接触表（读前补票）。 */
	const TArray<TSharedPtr<FCSHouseContact>>& DebugGetContacts() const { FlushPendingReevaluate(); return Contacts; }

	/** 角石砖数（同样含在 `GetFrameBrickCount()` 里 —— 三者共用一个组件与一份容量）。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Quoin")
	int32 GetQuoinBrickCount() const { FlushPendingReevaluate(); return CurrentQuoinBrickCount; }

	/** 本轮实际出砖的角石柱数（= footprint 上转角不超过 90° 的凸角数，矩形恒 4；退化 footprint 或容量耗尽时会少）。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Quoin")
	int32 GetQuoinColumnCount() const { FlushPendingReevaluate(); return CurrentQuoinColumnCount; }

	/** 包边砖数（同样含在 `GetFrameBrickCount()` 里 —— 四者共用一个组件与一份容量）。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Trim")
	int32 GetTrimBrickCount() const { FlushPendingReevaluate(); return CurrentTrimBrickCount; }

	/** 砖层这一轮实际发出的砖数（**已被容量截断之后**的数）。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Brick Wall")
	int32 GetBrickWallBrickCount() const { FlushPendingReevaluate(); return CurrentBrickWallBrickCount; }

	/** 砖层的层数（`PlanCourses` 的结果）。0 = 整层没开或墙高为零。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Brick Wall")
	int32 GetBrickWallCourseCount() const { FlushPendingReevaluate(); return CurrentBrickWallCourseCount; }

	/**
	 * 砖层**不减洞**的砖数上界（`CSHouseBrickWall::EstimateBricks`）。
	 * 与 `FrameReserveCapacity` 一起看才有意义：上界超容量 = 这栋房的砖层一定会被截断。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House|Brick Wall")
	int32 GetBrickWallBrickBudget() const;

	/** 墙顶包边被洞切成了几段（无洞的矩形房恒 4）。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Trim")
	int32 GetTrimTopRunCount() const { FlushPendingReevaluate(); return CurrentTrimTopRunCount; }

	/** 墙脚包边被洞切成了几段。**开一扇门就会多一段**——这正是"包边避开了洞"的可断言证据。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Trim")
	int32 GetTrimBaseRunCount() const { FlushPendingReevaluate(); return CurrentTrimBaseRunCount; }

	/**
	 * 接缝画得出来吗（**执行面**判据，不是数值判据）。
	 *
	 * ⚠️ 形状照抄 `IsWindowDrawable`，理由也一样：接缝有**两半**，两半各自能静默失效 ——
	 * 洞那一半根本不是几何（墙材质不是 Masked ⇒ 一段墙都没抹掉），砖那一半走 GPU 实例路
	 * （母材质没勾 `bUsedWithInstancedStaticMeshes` ⇒ 引擎静默换默认材质）。两种情形下
	 * 交点数 / 砖数 / 裁剪段数三条数值断言**全部照绿**。本项目在这上面栽过两次。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House|Seam", meta = (DevelopmentOnly))
	bool IsSeamDrawable(FString& OutReason) const;

	/** 同上，但把原因当返回值给出来（空串 = 画得出来）。理由逐字见 `GetVineUndrawableReason`。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Seam", meta = (DevelopmentOnly))
	FString GetSeamUndrawableReason() const;

	UFUNCTION(BlueprintPure, Category = "CS House")
	int32 GetPillarCount() const { FlushPendingReevaluate(); return CurrentPillarCount; }

	/** 当前砌出的门框砖总数。CPU 侧本来就排好了记录，这个数不需要回读 GPU。 */
	UFUNCTION(BlueprintPure, Category = "CS House")
	int32 GetFrameBrickCount() const { FlushPendingReevaluate(); return CurrentFrameBrickCount; }

	/**
	 * `RebuildFrame` **真的重排过几次砖**（单调递增，不序列化）。
	 *
	 * ⚠️ 这是「砖跟没跟上」的唯一无头判据，砖数**答不了**这个问题：拉尺寸时砖数逐位不变
	 * （洞集合没变），而砖该去的地方已经变了。2026-08-31 实测的缺陷正是这个形状 ——
	 * 连拉 8 帧只重排 1 次，而 `GetFrameBrickCount()` / GPU 回读 / 三角形数 / 零阻塞
	 * **四条断言全绿**，只有像素看得见（拱圈上一块砖都没有）。
	 * 拿它写断言时要判**增量下界**（"拉 N 帧至少重排 N−k 次"），不要判等号：
	 * 哈希短路吸收无效唤醒是**设计**，同一帧被唤醒两次只该重排一次。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House|Diagnostics")
	int32 GetFrameScatterCount() const { FlushPendingReevaluate(); return FrameScatterCount; }

	// -------------------------------------------------------------------------
	// GPU 侧真值（**诊断 / 验收专用，阻塞**）
	//
	// ⚠️ **绝对不许进交互路径**：回读就是阻塞，且每一次都被 `UCSMesh::GetBlockingFlushCount()`
	// 数到 —— 十一条 `flushes=0` 断言会在误用的那一刻报红，那是有意的警戒线。
	//
	// 为什么非要有这一条：上面那些 `GetXxxCount()` 全是 **CPU 侧的记录**。本项目已经出现过
	// 「CPU 说 0、GPU 说 12」—— 砖数掉到 0 时实例源被撤走、交接缓存被清空，下一轮
	// `EnsureFrameComponent` 又把同一批 buffer 交回去，**带着陈旧的计数器**，画面上 12 层砖
	// 原样立着，而砖数 / 三角形数 / 零阻塞所有无头断言全绿。两个数必须能被同一条断言比出来。
	// -------------------------------------------------------------------------

	/** 门框砖 + 接缝砖在 GPU 上的实例计数器（两者共用一个组件）。−1 = 读不到，不是 0。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Diagnostics", meta = (DevelopmentOnly))
	int32 DebugReadFrameBrickCountGpuSync() const;

	/** 藤枝在 GPU 上的实例计数器。−1 = 读不到，不是 0。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Diagnostics", meta = (DevelopmentOnly))
	int32 DebugReadVineBranchCountGpuSync() const;

	/** 藤叶在 GPU 上的实例计数器。−1 = 读不到，不是 0。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Diagnostics", meta = (DevelopmentOnly))
	int32 DebugReadVineLeafCountGpuSync() const;

	/** 同上，花那一路。⚠️ 没配 `VineFlowerMesh` 时它恒为 0 —— 那是合法状态，不是缺陷。 */
	UFUNCTION(BlueprintCallable, Category = "CS House|Debug", meta = (DevelopmentOnly))
	int32 DebugReadVineFlowerCountGpuSync() const;

	/** 摆件在 GPU 上的实例计数器（所有 palette 组件合计）。−1 = 任一组件读不到。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Diagnostics", meta = (DevelopmentOnly))
	int32 DebugReadDecorInstanceCountGpuSync() const;

	/** 基类诊断（逐族回读对资产，族表见 `GetInstancedFamilies`），这里只先补合批欠账。 */
	virtual FString DebugGetGpuAssetMismatchSync() const override;

#if WITH_EDITOR
	/**
	 * **自动化测试专用**：把门框砖（含接缝砖，同一个组件）走真正的
	 * `UCSGpuInstancedMeshComponent::SaveToStaticMesh` 出口烘成资产，再从烘出来的
	 * `GetMeshDescription(0)` 里数回来 —— 裁决六 ①②的判据本身，抄的是岩壳那条
	 * `RockShell.CapSkirtSurvivesBake` 的形状（走**组件自己的出口**，不另拼等价路径）。
	 *
	 * `OutDistinctBakedRandoms` 是烘焙件顶点色 **A** 里出现过的不同取值个数，也就是
	 * `CSInstanceRandom` 这条通道（字典见 `UCSGpuInstancedMeshComponent` 类注释）。
	 * ⚠️ **只有一种取值 = 整片同色**，而那正是"烘完就没有 PerInstanceRandom 了"这条缺陷的
	 * 全部症状 —— 它不报错、不掉三角、不改包围盒，只有这个数看得见。
	 *
	 * `bOutRandomsMatchGpu` 把烘出来的那组取值与 **GPU 上 packed 行的 `Origin.w`** 逐个对，
	 * 两边各算一次哈希再比"看着都挺随机"是没有意义的断言。
	 */
	bool DebugBakeFrameBricksSync(const FString& AssetPath, int32& OutTriangles, int32& OutVertexInstances,
		int32& OutUVChannels, int32& OutDistinctBakedRandoms, int32& OutGpuInstanceCount, bool& bOutRandomsMatchGpu);

	/**
	 * **出图 / 验收专用**：把藤枝走真正的 `SaveToStaticMesh` 出口烘成资产并返回它。
	 *
	 * 为什么挑藤枝做出图对照而不是门框砖：`M_TinyGladeBrick` 是一张**纯常数**材质，
	 * 它读不到逐实例随机 ⇒ 拿它对照，"通道丢没丢"这件事在像素上根本不显影，门是假的。
	 * 藤枝材质有 0.80~1.15 的逐实例明暗，通道一断整墙藤就塌成同一个色 —— 那才是活门。
	 *
	 * 烘回**本 actor 的局部空间**，所以把资产挂在一个摆在同一变换上的 StaticMeshActor 上，
	 * 就该与实例版逐像素重合。⚠️ 阻塞 + 建资产，只给出图与测试。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS House|Diagnostics", meta = (DevelopmentOnly))
	UStaticMesh* DebugBakeVineBranchesSync(const FString& AssetPath);

	/**
	 * 把藤枝的实例组件藏起来 / 放出来。出图对照必须"同机位、只换渲染路径"，
	 * 两条路一起画的话差异率永远是 0，门看着绿其实什么都没测。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS House|Diagnostics", meta = (DevelopmentOnly))
	// 参数不叫 bHidden：AActor 的作用域里已经有一个同名的（UHT 不许遮蔽）。
	void DebugSetVineBranchInstancesHidden(bool bHideInstances);
#endif

	/**
	 * 这一轮判为**墩**的残料跨度个数（不含判为墙的那些）。
	 *
	 * 迟回是路径依赖的，只有把结论暴露出来才断言得了"跨度在 60/75 之间来回时样式不切换" ——
	 * 从外面只看得见三角形数，而三角形数还同时被拱宽、洞数、屋面拖着动。
	 *
	 * ⚠️ 定义就写在声明处（同 `GetPillarCount`）：UFUNCTION 会让 UHT 生成 exec 桩，
	 * 少一个函数体是**链接错误**而不是编译错误 —— 上一轮在这里栽过一次，只有全量构建照得出来。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House")
	int32 GetPierSpanCount() const { FlushPendingReevaluate(); return CurrentPierSpanCount; }

	UFUNCTION(BlueprintPure, Category = "CS House|Vine")
	int32 GetVineSegmentCount() const { FlushPendingReevaluate(); return CurrentVineSegmentCount; }

	UFUNCTION(BlueprintPure, Category = "CS House|Vine")
	int32 GetVineLeafCount() const { FlushPendingReevaluate(); return CurrentVineLeafCount; }

	UFUNCTION(BlueprintPure, Category = "CS House|Vine")
	int32 GetVineFlowerCount() const { FlushPendingReevaluate(); return CurrentVineFlowerCount; }

	/**
	 * **诊断 / 验收专用**：藤蔓这一帧到底会不会被画出来，不会的话原因是什么。
	 *
	 * ⚠️ **形状照抄 `ACSGroundActor::IsRockShellDrawable`，理由也一样**：GPU 石阶的
	 * `StairMesh` / `StairMaterial` 在两张演示关卡里一直是 NULL，画面上是一撮黑块，
	 * 而所有 readback 断言全绿 —— 因为 **readback 证明的是"buffer 里有数"，对"画的是哪张网格、
	 * 有没有材质、材质勾没勾实例化"一个字都没说**。藤蔓这条路比石阶更容易中招：
	 * 现成的 `MI_ivy_*` 全都挂在 `M_TG_Texture` 下，而它**没有勾
	 * `bUsedWithInstancedStaticMeshes`** ⇒ 引擎静默退回默认材质，一片灰。
	 * 所以这里连"材质支持实例化"都做成显式判据。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House|Vine", meta = (DevelopmentOnly))
	bool IsVineDrawable(FString& OutReason) const;

	/**
	 * 同上，但**把原因当返回值给出来**（空串 = 画得出来）。
	 *
	 * ⚠️ **`IsVineDrawable(FString&)` 在 UE Python 侧是残废的**（实测）：Python 把
	 * "bool 返回值 + 一个 out 参数"收成单一返回值 —— 可画时拿到空串、**不可画时拿到 `None`**，
	 * 原因串整个丢掉。恰好在唯一需要它的时候失效：红灯只能说"画不出来"，说不出为什么，
	 * 而这个函数存在的全部价值就在那句原因上。脚本一律调这一版；`IsVineDrawable` 保留给
	 * C++ 与蓝图，并在返回前把原因打进日志兜底。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House|Vine", meta = (DevelopmentOnly))
	FString GetVineUndrawableReason() const;

	/** 四面墙**处处**悬空（空隙全部超 `VineMaxGroundGap`）？是则"零藤"是合法状态而非缺陷。
	 *  ⚠️ 纯 C++，**不要**在它和上面那个 `UFUNCTION` 之间插东西 —— 宏只作用于紧随其后的
	 *  那一个声明，插进去就是把 `GetVineUndrawableReason` 的暴露悄悄抢走（脚本侧报
	 *  `AttributeError`，而 C++ 一切正常）。 */
	bool IsVineSuppressedByGroundGap() const;

	/** 这一轮铺出来的瓦片数。CPU 侧本来就排好了记录，不需要回读 GPU。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Roof Tile")
	int32 GetRoofTileCount() const { FlushPendingReevaluate(); return CurrentRoofTileCount; }

	/** 画不出来的原因（空串 = 画得出来）。理由逐字见 `GetVineUndrawableReason`。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Roof Tile", meta = (DevelopmentOnly))
	FString GetRoofTileUndrawableReason() const;

	/** 这一轮立起来的尖顶数（矩形 2 根、正方形退化成 1 根、没网格 0 根）。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Roof Finial")
	int32 GetRoofFinialCount() const { FlushPendingReevaluate(); return CurrentRoofFinialCount; }

	/**
	 * 当前生效的洞表（门 + 窗 + 第三方注入）。
	 *
	 * `CurrentOpenings` 本身是 protected，**Python / 出图脚本读不到**（`get_editor_property`
	 * 会报 "protected and cannot be read"）。门宽验收要逐洞量宽度与洞心，所以开这个只读口。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House")
	TArray<FCSWallOpening> GetCurrentOpenings() const { FlushPendingReevaluate(); return CurrentOpenings; }

	/** 当前立着的门扇数（= 有门扇的洞数）。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Door Leaf")
	int32 GetDoorLeafCount() const { FlushPendingReevaluate(); return CurrentDoorLeafCount; }

	/** 空串 = 画得出来。⚠️ 一律调它，不要调 is_*_drawable（见出图脚本坑 ⑩）。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Door Leaf", meta = (DevelopmentOnly))
	FString GetDoorLeafUndrawableReason() const;

	/** 画不出来的原因（空串 = 画得出来）。理由逐字见 `GetVineUndrawableReason`。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Roof Finial", meta = (DevelopmentOnly))
	FString GetRoofFinialUndrawableReason() const;

	/** 当前摆出来的装饰件总数（所有 palette 合计）。CPU 侧本来就排好了记录，不需要回读 GPU。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Decor")
	int32 GetDecorInstanceCount() const { FlushPendingReevaluate(); return CurrentDecorInstanceCount; }

	/** 这一轮生产出来的锚点个数。**摆件密度就是这个数**（过完填充概率与间距球之后才是上一条）。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Decor")
	int32 GetDecorAnchorCount() const { FlushPendingReevaluate(); return CurrentDecorAnchorCount; }

	/**
	 * 其中门/拱那一家的锚点个数（TG 的 `add_autoclutter_around_gates`）。
	 *
	 * 单独暴露它是因为**总数对这一家是盲的**：画一笔路开出拱时，门那一家长出来的锚点
	 * 与被门口净空、路面排掉的墙脚锚点**数量上可以刚好抵消**（实测就碰上过：
	 * 20 → 20）。拿总数做断言会在那一刻变成空判据。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House|Decor")
	int32 GetDecorGateAnchorCount() const { FlushPendingReevaluate(); return CurrentDecorGateAnchorCount; }

	/**
	 * **诊断 / 验收专用**：装饰摆件这一帧到底会不会被画出来，不会的话原因是什么。
	 *
	 * ⚠️ 执行面照抄 `IsVineDrawable`（目前最完整的一版），理由也一样：本项目在"readback 全绿
	 * 而画面上什么都没有"这件事上栽过两次 —— 石阶的 `StairMesh`/`StairMaterial` 一直是 NULL、
	 * 画面是黑块；母材质没勾 `bUsedWithInstancedStaticMeshes` 会被引擎**静默换成默认材质**，
	 * 症状与"没绑材质"逐像素相同。所以"材质支持实例化"必须是显式判据，不能只查材质非空。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House|Decor", meta = (DevelopmentOnly))
	bool IsDecorDrawable(FString& OutReason) const;

	/** 同上，但把原因当返回值给出来（空串 = 画得出来）。理由逐字见 `GetVineUndrawableReason`。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Decor", meta = (DevelopmentOnly))
	FString GetDecorUndrawableReason() const;

	UFUNCTION(BlueprintPure, Category = "CS House")
	UCSMesh* GetPillarMesh() const { return PillarMesh; }

	/**
	 * 当前屋面描述（四坡）。瓦 / 梁 / 尖顶 / 雪、摆件的檐口与屋脊锚点、以及 D8「落屋顶 →
	 * 不生成」谓词全部从这一份组装 —— 屋面方程只有 CSHouseRoof.h 一个真源。
	 */
	FCSRoofDesc GetRoofDesc() const;

	// -------------------------------------------------------------------------
	// 判定纯函数（无 GPU、无 world 依赖 —— 直接进 CSHouseLogicTests）
	//
	// 计划纪律：门洞区间、接触段、柱布点、openings 排布这类"能不能 / 在哪 / 多大"的判定
	// 全部做成纯函数并单测，几何生成只负责照着摆。
	// -------------------------------------------------------------------------

	/**
	 * 离地收窄系数（D6，用户裁决"离地越高门越窄，直至消失"）：落差 ≤ GapFull 全宽，
	 * ≥ GapZero 归零，中间线性。**连续量因此不需要滞回** —— 消失点发生在拱已经很窄时，
	 * 观感上不突兀；但算出的宽度必须量化后再进形状哈希，否则地形每抖一下都是一次全量重建。
	 */
	static float ComputeDoorWidthScale(float GapMax, float GapFull, float GapZero);

	//~ AActor interface（OnConstruction → ReevaluateSite 在基类）
	/** 旧存档的弧长口径迁移（见 `WallSConvention`）。 */
	virtual void PostLoad() override;
	/** 新生成的房子直接写斜接口径 —— 没有旧数据可迁移。 */
	virtual void PostActorCreated() override;
	virtual void PostRegisterAllComponents() override;
	/** 编辑器 world 里删房子只走这一条（那个 world 没有 begun play）—— 拉尺寸抓手在这里收。 */
	virtual void Destroyed() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void BeginDestroy() override;
	/** 只做一件事：兑现 `RequestReevaluate` 攒下的那一次重求值，然后把自己的 tick 关掉。 */
	virtual void Tick(float DeltaSeconds) override;
	/**
	 * 编辑器 world 按 `LEVELTICK_ViewportsOnly` tick，不 override 这个的 actor 在编辑器里一帧都不跑
	 * ——而本项目的创作全在编辑器里。它与构造里的 `bCanEverTick` 缺一不可，缺哪个都是静默失效：
	 * 读派生结果的路会补票，所以单测照绿，只有编辑器画面停在旧洞上。
	 */
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditMove(bool bFinished) override;
	virtual void PostEditUndo() override;
#endif

protected:
	//~ ACSTinyGlade interface
	/** 门框砖（含接缝 / 角石 / 包边 / 砖层）/ 藤枝 / 藤叶 / 藤花 / 每个摆件 palette / 屋瓦 / 柱砖。 */
	virtual void GetInstancedFamilies(TArray<FCSInstancedFamily>& OutFamilies) const override;
	virtual void ReleaseInstancedBuffers() override;

private:
	/**
	 * 当前的拉尺寸抓手（计划 D5）：每条边一个锥子 + 檐口框 + 房底框。
	 *
	 * `Transient` 且刻意**不进任何 desc 哈希**：抓手是纯编辑设施，房子的几何与它无关 ——
	 * 混进哈希的症状是"一进拉尺寸模式整栋房子重建一次"，而那正是零阻塞纪律要挡的东西。
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<ACSHouseHandleActor>> ResizeHandles;

	/** 生成一个锥子（attach → 认宿主与边号 → 摆位 → 进表）。`EnterResizeMode` 与边数对齐共用。 */
	ACSHouseResizeHandleActor* SpawnEdgeHandle(int32 EdgeIndex);

	/** 生成一个高度框（檐口 / 房底），同上。竖直抓手将来再加一种也只是再调一次它。 */
	ACSHouseHeightHandleActor* SpawnHeightHandle(ECSHouseHeightHandleSide Side);

	/**
	 * 在拉尺寸模式里时，把锥子数对齐到 `GetFootprint().NumEdges()`（用户裁决 2026-09-14：
	 * "折线有多少折手柄就有多少个"，按**凸包之后**的边数）。不在模式里什么都不做；已经一一对上时
	 * 零成本返回（每次重求值都会走到）。
	 *
	 * **复用而不是整组重建**：按边号排好，前 min(旧, 新) 个锥子改认 0..k-1 号边，只销毁多出来的、
	 * 只补生缺的 —— 被选中的锥子因此多半留得住，编辑器侧的失选监听（选中集里没有归属者就退模式）
	 * 不会因为改了一下形状就把模式退掉。三个高度框不重建。
	 */
	void SyncEdgeHandlesToFootprint();

	/** `SyncEdgeHandlesToFootprint` 的重入保护（补生锥子期间不许再对齐一次）。 */
	bool bSyncingResizeHandles = false;

	/** 上一次抓手对齐 / 重摆时的摆位几何哈希（footprint 顶点 + 墙厚 + 墙高）。边数没变、几何变了只重摆。transient。 */
	uint32 ResizeHandleGeometryHash = 0;

	/**
	 * `FootprintShape` 不是严格凸、按凸包 / 矩形使用时打一次 Warning（顶点数 N → M）。
	 * 同一个形状只打一次：记下已告警形状的哈希，形状没变就不重复（重求值每帧都会来）。
	 */
	void WarnIfFootprintShapeFixed();

	/** 上一次已告警的 (形状, 处置) 哈希；0 = 当前形状无需告警。transient。 */
	uint32 FootprintShapeWarnedHash = 0;

	/** 材质三槽重绑（房体墙/顶 + 柱），不碰几何 —— 计划 D14「纯外观量绝不进 desc 哈希」。 */
	void BindHouseMaterials();

	/** Ground 为空时自动解析；顺带完成 OnGroundChanged 的订阅/换订阅。
	 *  解析不到地面时改挂 `ACSGroundActor::OnAnyGroundRebuilt`，等地面出现再来一次。 */
	void ResolveGroundAndSubscribe();
	void UnsubscribeGround();   // 连同类级那条一起撤
	void UnsubscribeAnyGroundRebuilt();
	void HandleGroundChanged(ACSGroundActor* ChangedGround, const FBox& ChangedBounds);
	void HandleAnyGroundRebuilt(ACSGroundActor* RebuiltGround, const FBox& ChangedBounds);

	/**
	 * 把外皮凸多边形（世界 XY）推给地面 —— 建筑周边灌木的唯一输入。地面没变就当场返回，
	 * 所以每次重求值都推也不花钱。房子不知道灌木的存在，只是登记数据（同塑形物登记高度场）。
	 */
	void PublishFootprintToGround();

	/** 落座目标：max(footprint 全域地面高度) + HeightOffset。无地面时返回当前 Z（不动）。 */
	double ComputeSeatZ() const;

	/** 边缘线段分割 + 采样点亮（滞回），产出 CurrentDoors。返回房体**形状**哈希（不含世界变换）。 */
	uint32 ComputeDoors();

	/** `Windows` 列表 + 标记登记表 → 候选洞（尚未过谓词）。身份派生见实现里那两段。 */
	void BuildWindowOpenings(TArray<FCSWallOpening>& OutCandidates) const;

	/** 一条标记登记的窗诉求。**不是 UPROPERTY**：整张表 transient，理由见公开区那段。 */
	struct FCSMarkerWindow
	{
		FGuid MarkerId;
		FCSHouseWindow Window;
		/**
		 * 登记时那一刻的锚点（2026-09-06）。**洞的弧长从它现算，不用 `Window.CenterS`。**
		 *
		 * ⚠️ `CenterS` 是 footprint 的函数：`S = bFromEndCorner ? Len − Dist : Dist`。把它缓存下来，
		 * 房子一改尺寸缓存就过期，而**没有任何东西会去刷新它** —— `NotifyMarkersRebuilt` 只调
		 * `SnapToAnchor()`（挪 actor），不重新登记。症状是**框跑到新位置、洞留在原地，且永不自愈**
		 * （实测：墙 600→1000，锚 `fromEnd=True dist=100`，洞恒在 S=500 而框走到了 S=900）。
		 * 锚点本身与 footprint 无关，存它才是稳的。
		 */
		FCSWallAnchor Anchor;
		/**
		 * 反引标记本人（2026-09-06）。**弱引用**：标记的生命周期归它自己，房子只是借来
		 * 回推裁决与吸附（`NotifyMarkersRebuilt`）。强引用会让删掉的标记活到房子销毁。
		 */
		TWeakObjectPtr<class ACSHouseFeatureMarker> Marker;
	};

	/** 标记登记表。按 `MarkerId` 升序保存 —— 与花名册同一条理由：次序进哈希，不定序就会抖。 */
	TArray<FCSMarkerWindow> MarkerWindows;

	/**
	 * 上一次调和锚点时的墙几何 + 构建变换。**世界位置守恒就是拿它当参照系**
	 * （2026-09-06 用户裁决：拉尺寸时窗不许沿墙滑）。
	 *
	 * ⚠️ 为什么必须存参照系而不能只靠锚点：`PushEdge(e, d)` 动的是**哪一个角**取决于推的是哪条边，
	 * 而锚点自己不知道这件事。不存在一种静态编码能在所有推法下都保持世界位置不变 —— 只能拿
	 * 「上一次的墙」把同一个物理点重新表达一次。
	 */
	FCSHouseFootprint MarkerRefFootprint;
	float MarkerRefThickness = 0.0f;
	FTransform MarkerRefBuild = FTransform::Identity;
	bool bMarkerRefValid = false;

	/** 见 `MarkerRefFootprint`。排在算门之前 —— 洞的弧长就是从这些锚点现解出来的。 */
	void ReanchorMarkersToPreserveWorld();

	/**
	 * 这一轮每个洞的裁决（key = `FCSWallOpening::SourceId`）。**在真正的落位循环里记下来**，
	 * 不是事后再问一遍谓词 —— 事后问会拿"已经把自己放进去了"的那份 openings 去判自己。
	 * 只给 `NotifyMarkersRebuilt` 回推用。
	 */
	TMap<FGuid, ECSFeatureReject> CurrentFeatureVerdicts;

	/**
	 * 重建之后把结果推回每个标记：① 裁决回执（否则拉尺寸把窗挤掉了、标记还在说"我切出洞了"，
	 * 2026-09-05 核出的过期回执）；② **按锚点吸附**（TG `move_decorators_following_anchors`
	 * 的对位物 —— 锚点是权威，标记的世界变换是派生量）。
	 *
	 * ⚠️ **正在被 gizmo 拖的那一个跳过不写**：标记不 attach 在房子下，写它纯粹是和 gizmo
	 * 抢方向盘。（拉尺寸抓手是另一回事 —— 那边 attach 着，2026-09-06 改成每次都重摆。）
	 */
	void NotifyMarkersRebuilt();

	/** 谓词的输入打包成一份 `FCSOpeningSite`。**唯一**的一处 —— 别在调用点各填一遍。 */
	FCSOpeningSite MakeOpeningSite() const;

	/** 周界支撑点落差判定，产出柱盒列表。返回柱**形状**哈希（局部布点 + 柱长，不含世界变换）。 */
	uint32 ComputePillars(TArray<FVector>& OutPillarCenters, TArray<float>& OutPillarLengths) const;

	/**
	 * 量化世界变换的哈希，房体与柱共用（同一个 actor 变换）。
	 *
	 * 与形状哈希分开是**功能必需而非优化**：常驻流是世界空间，落座（D4 ①）本身就在改 Z，
	 * 而容差与量化都是 0.5 cm ⇒ 混在一个哈希里时「地形抬 1 cm → 落座 → 全量重建」是常态，
	 * 计划 D4 那条"仅 Z 变则走 TransformMesh"的便宜路径永远不可达。
	 */
	uint32 ComputePlacementHash() const;

	/** 烘进常驻流时用的世界变换：只取 yaw + 位置（常驻流是世界空间，不支持缩放/pitch/roll）。 */
	FTransform GetBuildTransform() const;

	void RebuildBodyMesh();
	void RebuildPillarMesh(const TArray<FVector>& Centers, const TArray<float>& Lengths);

	/**
	 * 保证门框的实例组件存在并绑好网格/材质，备容量、交接实例源（多退少补，同藤蔓/摆件的做法）。
	 * 返回这一趟的交接结果：`HandedOver` = 组件刚拿到一批 buffer（扩容后是全新、清零的一批），
	 * `RebuildFrame` 的早退门看到它就不许早退 —— 否则扩容那一轮画的是空 buffer（2026-09-07 审查 B1）。
	 */
	CSShaperSteps::EHandoverResult EnsureFrameComponent();

	/**
	 * 组装门框的**解析砖路**（2026-08-30「裁决一」选乙，**唯一**的一条）。返回门框 desc 哈希。
	 *
	 * CPU 只算**逐路**的标量（弧长、砖数、铺装缩放、墙框架），一块砖的位置与朝向都不在 CPU 上
	 * 出现 —— 那是 `CSHouseFrame.usf` 里"一线程一砖"的事。规模因此是 `O(洞数)` 而不是
	 * `O(砖数)`，容量恒定（注册期 `ReserveCapacity` 一次付清，交互期不扩容、不阻塞）。
	 *
	 * 砖路取自 `CSHouse_ComputeClipField` 那条**解析**洞缘 —— 与材质切洞判据同源，砖因此
	 * 正好骑在切出来的那条边上。每个洞出两条：上边界（含两侧门樘）与下边界（有窗台时才有）。
	 *
	 * 曾经并存的旧路（`BuildFramePlan`：B 样条 + 逐砖记录 + 可扩容，靠 `csh.FrameLegacy` 切换）
	 * 已随裁决一第二步整条删除。两条路"把砖摆在同一个地方"这条等价性判据仍然活着，靠的是
	 * 单测 `House.FrameAnalyticMatchesLegacy` 里那份**测试内部的 CPU 镜像**（不调任何产线代码）。
	 */
	uint32 BuildFrameArches(TArray<CSHouseFrame::FElement>& OutElements, int32& OutBrickCount) const;

	/**
	 * world 里外接圆够得着的邻居，**按 GUID 升序**。名单 = `UCSHouseLibrary::GetHouses`（`TActorRange`），
	 * 2026-09-16 起没有登记表：只在提交那一刻翻一次，开销可忽略。
	 */
	void GatherSeamCandidates(TArray<ACSHouseActor*>& Out) const;

	/** 我的接缝输入（量化摆位 / yaw / footprint / 底 Z / 墙高 / 墙厚 + 开关 + 能否出砖 + 判定参数）。变了才提交。 */
	uint32 ComputeSeamKey() const;

	/** 本次重求值能否提交接缝：不在 gizmo 拖动、不在地面拖动。拖动标志有 2 s 无位移超时兜底。 */
	bool CanCommitSeams();

	/**
	 * 接触协议（09-16）：验 → （输入变了且可提交）清空自己的表、对"旧对端 ∪ 当前邻居"逐个 `Classify` 并
	 * `ReceiveContactFrom` 发过去（不接触发空）。被发醒的一方只验只用不反发。
	 */
	void UpdateContacts(bool bCanCommit);

	/** 删除路径：对每个旧对端发空。幂等；世界卸载 / 引擎退出期间不碰对端。 */
	void DetachContacts();

	/** 根组件的 `TransformUpdated`：任何来源的 transform 变化（gizmo / 脚本 / 复制 / Undo）都从这里标脏。 */
	void HandleRootTransformUpdated(USceneComponent* UpdatedComponent, EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport);

	/** 接缝裁剪段（先跑接触协议，再从竖缝接触取我这一端的裁剪段写 `CurrentSeamCuts`），返回它对房体形状哈希的贡献。 */
	uint32 ComputeSeamCuts();

	/**
	 * 接缝砖：**追加**进门框砖那份元素表（同一个组件、同一次 dispatch），返回它的哈希贡献。
	 *
	 * 非 const（与 `BuildFrameArches` 不同）：它顺手写 `CurrentSeamBrickCount`。那个数只有它
	 * 算得出来 —— 门框砖那边的总数由 `RebuildFrame` 统一落账，而接缝砖是总数里的一个子段。
	 */
	uint32 BuildSeamBricks(TArray<CSHouseFrame::FElement>& InOutElements, int32& InOutBrickCount);

	/**
	 * 角石：同样**追加**进门框砖那份元素表，返回它的哈希贡献。
	 *
	 * 排在接缝砖之后：门框 → 接缝 → 角石。**次序是承重的** —— 前面任何一段的砖数一变就会
	 * 把后面所有砖的槽位推走。门框砖的逐实例随机数今天还是槽位派生的，所以它必须排第一；
	 * 接缝与角石的随机数都已从各自身份派生，彼此换序无害，但固定下来省得将来有人来回改。
	 */
	uint32 BuildQuoinBricks(TArray<CSHouseFrame::FElement>& InOutElements, int32& InOutBrickCount);

	/**
	 * 转角墩：转角配成墩的角上那根柱础 / 柱身 / 柱头（2026-09-06 用户裁决"转角就是一个墩"）。
	 * 立在两面墙墙厚中线的交点（`FCSHouseCornerFrame::PointAtDepth(T/2)`，直角上是沿角平分线内缩 T/√2），
	 * 门樘砖与拱廊的墩都在那条中线上。
	 * 高度读 `ResolvePierSpans` 写的 `CornerPierTopZ`，角石在同一高度以下让路。追加进同一份元素表。
	 */
	uint32 BuildCornerPierBricks(TArray<CSHouseFrame::FElement>& InOutElements, int32& InOutBrickCount);

	/** 包边石：两条带（墙顶 / 墙脚），同样追加进那份元素表，返回哈希贡献。排在砖序最后。 */
	uint32 BuildTrimBricks(TArray<CSHouseFrame::FElement>& InOutElements, int32& InOutBrickCount);

	/** 砖层：逐层调 `CSHouseTrim::BuildBand`。排在所有砖家族**最后**，理由同包边。 */
	uint32 BuildBrickWallBricks(TArray<CSHouseFrame::FElement>& InOutElements, int32& InOutBrickCount);

	void RebuildFrame();

	/** 藤蔓：组件/容量/交接一次付清（同 EnsureFrameComponent），交互期只剩录 pass。
	 *  返回交接结果，`HandedOver` 时 `RebuildVine` 不许早退（理由见 EnsureFrameComponent）。 */
	CSShaperSteps::EHandoverResult EnsureVineComponents();

	/** 墙矩形（世界空间）—— 与房体面板同一份 `CSHouse_GetEdge`，不另起一套口径。 */
	void BuildVineStrips(TArray<CSHouseVine::FWallStrip>& OutStrips) const;

	/** 规划 + 录一趟打包 pass；返回这次的形态哈希（喂幂等短路）。 */
	void RebuildVine();

	/** 屋面瓦：组件/容量/交接一次付清（同 EnsureVineComponents），交互期只剩录 pass。
	 *  返回交接结果，`HandedOver` 时 `RebuildRoofTiles` 不许早退（理由见 EnsureFrameComponent）。 */
	CSShaperSteps::EHandoverResult EnsureRoofTileComponent();

	/** 参数打包，顺带从网格包围盒判定三条轴。**只有这一处**组装，别在调用点各写一份。 */
	CSHouseTile::FParams MakeRoofTileParams() const;

	/** 排布 + 录一趟打包 pass（幂等哈希短路无效唤醒）。 */
	void RebuildRoofTiles();
	void RebuildFlatRoof();

	/** 尖顶：脊端点各立一根。走**普通** `UStaticMeshComponent`（一两根而已，不值得再复制一套
	 *  palette / 容量 / 交接机器）。 */
	void RebuildRoofFinials();
	void RebuildDoorLeaves();

	/** 摆件：组件/容量/交接一次付清（同 EnsureVineComponents），交互期只剩录 pass。
	 *  返回交接结果，`HandedOver` 时 `RebuildDecor` 不许早退（理由见 EnsureFrameComponent）。 */
	CSShaperSteps::EHandoverResult EnsureDecorComponents();

	/** 锚点生产者要读的世界（墙矩形 + 洞 + 屋面 + 地面采样器）。 */
	void BuildDecorSite(CSHouseDecor::FSite& OutSite) const;

	/** 参数打包：细节面板只暴露改观感的那几个，其余以 `CSHouseDecor::FParams` 的默认值为准。 */
	CSHouseDecor::FParams MakeDecorParams() const;

	/** 生产锚点 + 规划 + 录一趟打包 pass（幂等哈希短路无效唤醒）。 */
	void RebuildDecor();

	/** 拱间墩：按双阈迟回给 `CurrentOpenings` 打 `CSHouse_StylePier*` 位，并刷新迟回表。 */
	void ResolvePierSpans();

	/**
	 * 整栋房子**一次** EditMeshAsync：上传基体 → 排序分段组进同一个 EditFunc、同一张 RDG 图
	 * （基类 `SubmitMeshSlotAsync`；这里只给材质表与流布局）。
	 *
	 * 为什么不能"每个算子各发一次异步编辑"：EditMeshAsync 在途时会拒绝第二次（返回 false 且
	 * OnComplete 永不触发），两个算子各发一次必然互相拒绝。也不能"只把上传异步化" ——
	 * 那是 2 次 flush 变 1 次，不是变 0 次。在途被拒时的最新态合并见基类那条注释。
	 */
	void SubmitBodyMesh(TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe> Snapshot);
	void SubmitPillarMesh(TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe> Snapshot);

	// 藤的生长相位（`CSHouseVine::ResolveSpawnTimes`）、管子递交（`SubmitVineTube` / `OnVineTubeEditComplete`）与打包
	// （`PackVine`）2026-09-22 起在墙的基类 `ACSWallBase` 里，房子与样条墙共用一份。

	/** 异步编辑的游戏线程尾巴（分段表已由基类发布）：有 pending 就补发，否则补上被推迟的摆位增量。 */
	void OnBodyEditComplete();
	void OnPillarEditComplete();

	/**
	 * 形状未变、只是搬了地方：基类 `ApplyMeshSlotPlacement` 一个变换 pass 把已有几何搬到
	 * `GetBuildTransform()`。返回 false = 这一次没送出去（在途 / 被拒），调用方不许推进摆位哈希；
	 * 补送由 OnBodyEditComplete / OnPillarEditComplete 兜底。
	 */
	bool ApplyBodyPlacement();
	bool ApplyPillarPlacement();

	/** 承重柱网格宿主（独立于基类房体组件——纯地形变化只动它，不碰房体重建）。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS House", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCSMeshRenderComponent> PillarMeshComponent;

	/** 柱网格对象。Transient：派生物，加载后由 ReevaluateSite 重建。 */
	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> PillarMesh;

	/** 柱砖的实例宿主。几百块砖走实例而不是 CPU 三角汤 —— `brick` 是 600 顶点的倒角石块，
	 *  三角汤那条路 300 块砖就是 18 万顶点，且每次重建都要 CPU 变换一遍。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS House", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCSGpuInstancedMeshComponent> PillarBrickComponent;

	TArray<CSShaperSteps::FPaletteBuffers> PillarGpuBuffers;
	/** 上次交给组件的容量/包围盒：只有它们真变了才需要再走一次阻塞的 SetInstanceSourceGPU。 */
	CSShaperSteps::FHandoverCache PillarHandover;

	/** 砖石柱：备容量 / 交接实例源（与 `EnsureFrameComponent` 同型，阻塞的活都在这里一次付清）。 */
	void EnsurePillarBrickComponent();

	// 藤蔓管子（枝）的网格宿主 `VineTubeComponent` / `VineTubeMesh` 在基类 `ACSWallBase` 上（默认子对象仍由本类构造函数建，
	// 名字仍是 `VineTubeMesh`，存档不受影响）。**与 `PillarMeshComponent` 同型**：几何在世界空间产出，组件钉在恒等世界变换上。

	/**
	 * 当前生效的洞（房体形状 desc 的一部分）。门由道路推导、窗由特征标记注册，两者同表 ——
	 * 墙板生成只认剖面 + 摆位，不关心洞是谁提的。
	 */
	UPROPERTY(Transient)
	TArray<FCSWallOpening> CurrentOpenings;

	/**
	 * 门洞的滞回状态：**上一帧每条边上的洞区间**（`FCSDoorRunMemory`，沿边弧长）。
	 *
	 * 继承判据是"与上一帧区间交叠"，不是编号 —— 旧口径 key 里带段数 N，拉尺寸跨过 `round()`
	 * 边界那一帧整条边 key 全失配、滞回集体失效，那条坑随这次重做一起消失。
	 *
	 * **必须序列化，且必须 NonTransactional**（计划 D6，理由逐字照旧）：滞回让 ReevaluateSite()
	 * 成为路径依赖函数，表冷加载时为空 ⇒ 宽度落在保活区间里的已开拱重开关卡直接消失。
	 * NonTransactional 不能省 —— 普通 UPROPERTY 会被事务缓冲整份捕获，一次无关的 details 改参
	 * + Ctrl+Z 就把滞回表回滚到旧代。凡滞回状态都照此办理。
	 */
	UPROPERTY(NonTransactional)
	TArray<FCSDoorRunMemory> DoorRunMemory;

	/**
	 * 拱间墩的迟回状态：key = (Edge<<24) | (该边洞数<<16) | 跨度序号。
	 *
	 * 洞数进 key：多开/少开一个拱会把整条边的跨度
	 * 重新编号，不把编号基准放进 key 就会把旧跨度的样式误继承给完全不同的一段墙。
	 * 序列化与 NonTransactional 的理由同上一条，逐字适用（迟回让重求值成为路径依赖函数）。
	 */
	UPROPERTY(NonTransactional)
	TMap<uint32, bool> PierSpanIsPier;

	/**
	 * 每个角各自的转角墩顶（墙空间高度；0 = 这个角没配成墩），长度 = footprint 顶点数；
	 * 角 k 夹在 k 号边远端与 k+1 号边近端之间。`ResolvePierSpans` 每轮整份重写（跑之前为空，读方一律按 Num 截断），
	 * `BuildCornerPierBricks` 按它立柱、`BuildQuoinBricks` 按它让角石在墩顶以下让路 ——
	 * 两个消费者读同一份数，才不会出现"墩砌到 A、角石剔到 B"。序号与 `CSHouse_GetCorner` 同号。
	 */
	TArray<float> CornerPierTopZ;
	/** 这一轮立起来的转角墩数（0..角数）。 */
	int32 CurrentCornerPierCount = 0;

	/**
	 * 房体（基类主网格）与柱的网格槽簿记：两级哈希（形状变 → 全量重建；只有摆位变 → 一个变换
	 * pass）、几何烘在哪个世界变换下（同 ACSGroundActor::MeshBuiltAtLocation）、在途待发的最新快照。
	 */
	FCSMeshSlotState BodySlot;
	FCSMeshSlotState PillarSlot;

	int32 CurrentPillarCount = 0;
	int32 CurrentFrameBrickCount = 0;

	/** `RebuildFrame` 越过早退门、真的重排砖的次数（`GetFrameScatterCount` 读）。 */
	int32 FrameScatterCount = 0;

	/** 窗：过了谓词的 / 被拒的。两个数一起才说得清"没填"与"被门吃掉了"的区别。 */
	int32 CurrentWindowCount = 0;
	int32 CurrentWindowRejectCount = 0;

	/** 这一轮判为墩的跨度个数（`ResolvePierSpans` 写、`GetPierSpanCount` 读）。 */
	int32 CurrentPierSpanCount = 0;

	/** 门框砖的 GPU 实例宿主（单 palette：TG 只有一块 brick）。 */
	UPROPERTY(Transient)
	TObjectPtr<UCSGpuInstancedMeshComponent> FrameComponent;

	/** 实例行与计数的 pooled buffer（渲染线程分配、渲染线程释放）。 */
	TArray<CSShaperSteps::FPaletteBuffers> FrameGpuBuffers;

	/** 上次交给组件的容量/包围盒：只有它们真变了才需要再走一次阻塞的 SetInstanceSourceGPU。 */
	CSShaperSteps::FHandoverCache FrameHandover;

	uint32 FrameDescHash = 0;

	/** 这一轮的接缝裁剪段（D7）：从竖缝接触取我这一端的那份。派生物，不序列化。 */
	TArray<FCSWallCut> CurrentSeamCuts;
	int32 CurrentSeamCornerCount = 0;
	int32 CurrentSeamBrickCount = 0;
	int32 CurrentSeamOwnedPostCount = 0;

	/**
	 * 我参与的接触（09-16）。**故意不是 UPROPERTY**：不存盘、不进事务、复制 / PIE 得到空表，加载后第一次
	 * 提交重建。两端各持一个指针指向同一个对象；对端失效 / 关掉开关的记录在下次「验」时丢弃。
	 */
	TArray<TSharedPtr<FCSHouseContact>> Contacts;
	/** 上次提交时的 `ComputeSeamKey()`；`bSeamKeyValid` 为假 = 还没提交过（首次重求值即提交）。 */
	uint32 LastSeamKey = 0;
	bool bSeamKeyValid = false;
	/**
	 * gizmo / 抓手拖动中（`PostEditMove(false)` / `Push*(bFinished=false)` 置，`true` 清）。接缝冻结期。
	 * 松手事件没来时由 `Tick` 按 `GizmoDragIdleSeconds` 自己清 —— 只在下次重求值里查超时等于没人查
	 * （09-16 晚编辑器实测：标志卡在 true、没人再叫醒这栋房、拖完一直不出缝）。
	 */
	bool bInGizmoDrag = false;
	double GizmoDragTouchedAt = 0.0;
	/** 记一次拖动事件：置 / 清拖动标志、记时间，拖着时把 tick 打开等超时。`PostEditMove` 与三个 `Push*` 共用。 */
	void NoteGizmoDrag(bool bFinished);
	/** 同一 world 里别的房子已经用着 `Id`（此前存盘的旧副本、不走 T3D 的属性拷贝）。 */
	bool IsHouseIdTakenByOther(const FGuid& Id) const;
	/** 地面最近一次广播未提交（塑形物拖动 / 落笔途中）。接缝冻结期。 */
	bool bInGroundDrag = false;
	FDelegateHandle RootTransformUpdatedHandle;

	/** 这一轮的角石（D7 墙自身转角）。同样是派生物，每轮从 footprint 重算。 */
	int32 CurrentQuoinColumnCount = 0;
	int32 CurrentQuoinBrickCount = 0;

	/** 这一轮的包边（D7 第三样）。同样是派生物，每轮从 footprint + 洞表重算。 */
	int32 CurrentTrimTopRunCount = 0;
	int32 CurrentTrimBaseRunCount = 0;
	int32 CurrentTrimBrickCount = 0;
	int32 CurrentBrickWallBrickCount = 0;
	int32 CurrentBrickWallCourseCount = 0;

	// 藤的三个 GPU 实例宿主、三张 MID、调色板缓冲与交接缓存、基础网格快照的就绪位与"从哪张资产建的"
	// 2026-09-22 起都在墙的基类 `ACSWallBase` 上（房子与样条墙共用一份胶水，见 `ACSWallBase::EnsureVineRig`）。
	uint32 VineDescHash = 0;
	int32 CurrentVineSegmentCount = 0;
	int32 CurrentVineLeafCount = 0;
	int32 CurrentVineFlowerCount = 0;

	// ---- 屋面瓦 ----

	/** 屋面瓦的 GPU 实例宿主（一张网格 ⇒ 一个组件）。 */
	UPROPERTY(Transient)
	TObjectPtr<UCSGpuInstancedMeshComponent> RoofTileComponent;

	UPROPERTY(VisibleAnywhere, Category = "CS House|Flat Roof")
	TObjectPtr<UCSGpuInstancedMeshComponent> FlatRoofParapetComponent;
	uint32 FlatRoofParapetHash = 0;
	uint32 TerraceSiteHash = 0;
	int32 CurrentTerraceEntranceCount = 0;

	/** 实例行与计数的 pooled buffer。**恒 1 条**，用数组只是为了直接吃 `CSShaperSteps` 那几个
	 *  批量接口（ReserveCapacity / ZeroCounters / ReleaseOnRenderThread）。 */
	TArray<CSShaperSteps::FPaletteBuffers> RoofTileGpuBuffers;

	/** 基础网格快照建成过没有（同 `bVineBaseMeshReady`：没建成时组件画的是**上一次**的网格）。 */
	bool bRoofTileBaseMeshReady = false;

	/** 快照是从哪张网格建的（同 `VineBranchMeshBuiltFrom`：只靠一个 bool 会"换了资产什么都没发生"）。 */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> RoofTileMeshBuiltFrom;

	/** 从包围盒判出来的三条轴与网格自身尺寸。`EnsureRoofTileComponent` 建快照时一并算好。 */
	CSHouseTile::FMeshAxes RoofTileAxes;

	/** 上次交给组件的容量/包围盒：只有它们真变了才需要再走一次阻塞的 SetInstanceSourceGPU。 */
	CSShaperSteps::FHandoverCache RoofTileHandover;
	uint32 RoofTileDescHash = 0;
	int32 CurrentRoofTileCount = 0;

	// ---- 尖顶 ----

	/**
	 * 脊端点上的尖顶。**恒 ≤ 2 个**（正方形退化成 1 个）。
	 *
	 * ⚠️ 这是本 actor 身上**唯一**的 `UStaticMeshComponent`，而 `CSVineScatter` 的
	 * `CollectSurfaceTriangles`（`CSVineScatter.cpp:38`）恰好按这个类型遍历 —— 旧的
	 * `VineScatter` 三个入口对房子本来**恒返回空三角集**（房体挂在 `UCSMeshRenderComponent`
	 * 上），加了尖顶之后就变成"只有尖顶那点面"。房子自己的藤蔓走的是另一条通路（`RebuildVine`），
	 * 不受影响；但如果将来有人把房子喂给旧入口，长出来的藤蔓会全爬在尖顶上。
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> RoofFinialComponents;

	/** 门扇：一洞一个组件。与尖顶同一档设施（普通静态网格组件 + 形态哈希短路）。 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> DoorLeafComponents;

	uint32 DoorLeafDescHash = 0;
	int32 CurrentDoorLeafCount = 0;

	uint32 RoofFinialDescHash = 0;
	int32 CurrentRoofFinialCount = 0;

	/** 摆件的 GPU 实例宿主：**一张网格一个**（一个 palette 条目 = 一个组件）。
	 *  顺序恒为「门 → 墙脚 → 屋顶」，`DecorPaletteRanges` 记的就是这三段的起止。 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UCSGpuInstancedMeshComponent>> DecorComponents;

	/** 实例行与计数的 pooled buffer，逐 palette。容量按**配置上限**一次预留，
	 *  规划结果再多也只截断不扩容 —— 交互期一次设备同步都不许有。 */
	TArray<CSShaperSteps::FPaletteBuffers> DecorGpuBuffers;

	/** 每一家能用 palette 的哪一段，按 `CSHouseDecor::EFamily` 下标。
	 *  窗户那一家恒 `{0, 0}`（不长），见 `CSHouseDecor.h` 的文件头。 */
	TArray<CSHouseDecor::FPaletteRange> DecorPaletteRanges;

	/** 上次交给组件的容量/包围盒：只有它们真变了才需要再走一次阻塞的 SetInstanceSourceGPU。 */
	CSShaperSteps::FHandoverCache DecorHandover;

	uint32 DecorDescHash = 0;
	int32 CurrentDecorInstanceCount = 0;
	int32 CurrentDecorAnchorCount = 0;
	int32 CurrentDecorGateAnchorCount = 0;

	/** 基础网格快照建成过没有（同 `bVineBaseMeshReady`：没建成时组件画的是**上一次**的网格）。 */
	bool bDecorBaseMeshReady = false;

	/** 快照是从哪几张网格建的。⚠️ 与 `VineBranchMeshBuiltFrom` 同一条：在细节面板里换掉网格时
	 *  组件不一定被重建，只靠一个 bool 会画着旧网格而"什么都没发生"。 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMesh>> DecorMeshesBuiltFrom;

	/** 下一次重求值强制全量重建：手动强刷，以及拖动松手时清掉增量变换攒下的浮点误差。 */
	bool bForceFullRebuild = false;

	// 管子在途时挂起的最新折线（`PendingVineTubePath`）与逐藤的生长历史（`VineStrandHistory`，存墙面参数坐标、
	// 键 = 身份哈希不含位置 ⇒ 拖房子不重播生长）在基类 `ACSWallBase` 上。

	/**
	 * 稳定身份，随关卡序列化；首次注册时生成。
	 *
	 * `NonPIEDuplicateTransient`（2026-09-16 晚，结构审查 13 / 计划 D7「前置」）：编辑器里 Ctrl+C/V、Ctrl+D、
	 * Alt 拖复制全走 T3D 文本导出再导入（`PasteActors`），这个说明符让导出跳过它 ⇒ 副本导入后仍是无效 GUID，
	 * 注册时发新的。不加的话副本与原件同 GUID：接缝规范序打平、`Classify` 判成同一栋，两栋永远不出缝
	 * （09-16 编辑器实测 House_Pillar / House_Pillar2）。PIE 复制（`PPF_DuplicateForPIE`）照旧保留；
	 * `DuplicateTransient` 连 PIE 也清，别换成它。已经撞上的（此前存盘的旧副本）由 `PostRegisterAllComponents` 查重。
	 */
	UPROPERTY(NonPIEDuplicateTransient)
	FGuid HouseId;

	/**
	 * 本房子存盘数据的**弧长口径**。0 = 直角对接（2026-09-13 之前），1 = 斜接。
	 *
	 * 口径换了，存下来的绝对弧长就要换算：`Windows` 表里奇数边的 `CenterS` 从缩进 `T` 的那一点
	 * 量起，斜接之后要补一个 `T`；门段记忆 `DoorRunMemory` 存的是环参数，而环长随奇数边变长了
	 * `4T`，旧值落在错的位置上（迟回门槛 32 cm 可能在 32–40 cm 宽的路上放出一道持久的幻门），
	 * 连同 `PierSpanIsPier` 一起清掉，下一轮重判。标记的锚点不在这里迁，它自带口径字段
	 * （`FCSWallAnchor::SConvention`），在 `CSHouse_AnchorS` 里就地换算。
	 *
	 * ⚠️ **默认必须是 0**：这个字段是后加的，旧存档里没有它，读进来拿到的是默认值。新生成的
	 * actor 在 `PostActorCreated` 里写 1。**模板（蓝图 CDO）恒为 0**、`PostLoad` 不碰它 ——
	 * 实例按原型增量存盘，原型被置 1 会让旧实例继承到 1 而漏迁（理由见 `PostLoad`）。
	 */
	UPROPERTY()
	int32 WallSConvention = 0;

	FDelegateHandle GroundChangedHandle;
	FDelegateHandle AnyGroundRebuiltHandle;   // 仅在"还没找到地面"期间有效
	bool bInReevaluate = false; // SetActorZ 落座引发的重入保护

	/** `GetReevaluateCount` 读。 */
	int64 ReevaluateCount = 0;

	/**
	 * 欠着一次重求值。`RequestReevaluate` 置位，`ReevaluateSite` **一开头**就清 ——
	 * 放在结尾清的话，重建途中新到的通知会被这一行一起抹掉（它读的输入已经过了那一步）。
	 */
	bool bReevaluatePending = false;
};
