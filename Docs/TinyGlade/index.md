# Tiny Glade 复刻：文档索引

本目录集中存放 Tiny Glade 式交互房屋系统（`ACSGroundActor` / `ACSHouseActor` / `ACSGroundShaperActor` / `ACSStairsActor` / `ACSWallActor` 一族）的全部设计、对照与逆向文档。

## 命名规则

文件名一律 `前缀_主题.md`（英文 PascalCase，不带 `TinyGlade` 前缀、不带日期；日期写在正文里）。配图与所属文档同前缀。

| 前缀 | 含义 |
| --- | --- |
| `Plan_` | 设计裁决与计划 |
| `Progress_` | 进度与模块对照（自监督循环的状态文件） |
| `Review_` | 审查与对比评审报告；`Review_Structure_{A,B,C}_*` 是结构审查的子代理报告 |
| `RE_` | TG 逆向子代理报告（附录 D、E、F） |
| `House_` / `Ground_` / `RockShell_` / `Roof_` / `Stairs_` / `Tree_` / `Vine_` / `Asset_` | 按模块的专题文档 |

## 从哪读起

- **要改代码** → 先读 [`Plan_House.md`](Plan_House.md)：唯一的裁决记录。
- **要接着推进** → 读 [`Progress_ModuleComparison.md` 卷零](Progress_ModuleComparison.md#vol-0)：模块状态表、证据来源、已定裁决、验收门与「踩过的坑」；卷一～卷五是历史对照，不能直接作为当前待办。
- **墙化重构 / 样条墙** → [`Plan_Wall.md`](Plan_Wall.md)；世界级登记处与 dirty 通道那一半在 [`../cs-scene-dirty-3d-design.md`](../cs-scene-dirty-3d-design.md)。
- **要动任何"砖"** → [卷五](Progress_ModuleComparison.md#vol-5)：门框 / 拱圈石 / 转角 / 墙裙 / 承重柱 / 接缝 / 垛口 / 上沿在 TG 里是不是一套。
- **要动窗（D8）** → 只读 [`House_Window.md`](House_Window.md)，原文各处只剩存根。
- **要动接缝 / 摞房（D7）** → 计划 D7「接触记录 `FCSHouseContact` 与派生」；TG 侧证据在 `evidence/inter-shape-stitches*.asm`。⚠️ 横缝只判定、不出产物 —— 2026-09-17 照反汇编加的承托梁已被用户判「TG 中没有这个结构」退回，别再加。
- **要动玩家楼梯 / 屋顶** → [`Stairs_PlayerStairs.md`](Stairs_PlayerStairs.md)、[`Roof_TerraceAndRidge.md`](Roof_TerraceAndRidge.md)；逆向依据 [附录 D](RE_PlayerStairs.md)（楼梯）、[附录 F](RE_RoofRidgeGable.md)（脊长、双坡与山墙）。
- **要动草 / 花** → [`Ground_Cover.md`](Ground_Cover.md)：Nanite 路、WPO 距离淡出、花的聚散、白花的颜色与三个配置脚本；默认值的调参记录在 [`Ground_Tuning.md`](Ground_Tuning.md)。
- **要重构编排层** → 先读 [`Review_Structure.md`](Review_Structure.md) 顶部的后续变化，再按当前代码复核历史条目；房子与样条墙已统一的部分见 [`Plan_Wall.md`](Plan_Wall.md)。
- **想翻某条已定结论** → 先查 [`Review_ReverseComparison.md`](Review_ReverseComparison.md) 的「被否条目」与「看似该抄其实不该抄」，别重复推翻。

## 文档清单

| 文件 | 内容 |
| --- | --- |
| [`Plan_House.md`](Plan_House.md) | 设计裁决主文档：D1–D14、阶段计划 P0–P9、风险、开放问题 |
| [`Plan_Wall.md`](Plan_Wall.md) | 墙化重构计划（2026-09-22）：TG 墙模型证据（`InnerWalls` → `PublicWalls`、屋顶另挂）、`CSWall` 核心与 `ACSWallActor`、房子各模块的去向、六步计划与各步验证 |
| [`Progress_ModuleComparison.md`](Progress_ModuleComparison.md) | 六卷合卷：完成进度（卷零）+ 模块对照（卷一～卷五） |
| [`Review_ReverseComparison.md`](Review_ReverseComparison.md) | 两卷合卷：两轮多 agent 对比逆向评审的原始报告，含被否条目 |
| [`Review_Structure.md`](Review_Structure.md) | 2026-09-06 整体结构审查：编排层复制、`ReevaluateSite` 隐式数据流与哈希缺口、矩形假设、两套墙体；模块交互 / 冗余代码（8–20）、GPU 基座与打包契约（21–25）、生命周期与持久化（26–30）；09-11 全文复核（新发现 31–38），09-18 补记子系统删除与接触记录之后的现状 |
| [`Review_Structure_A_WallProfile.md`](Review_Structure_A_WallProfile.md) | 子代理报告 A（深挖 C）：洞曲线消费者对照、`CLIP_HLSL` 逐语句对照、矩形假设清单；结论已并入结构审查大问题 3、4 |
| [`Review_Structure_B_GpuBase.md`](Review_Structure_B_GpuBase.md) | 子代理报告 B：GPU 基座与九条打包路、31 处阻塞点、容量 / 确定性契约；结论已并入第 21–25 条 |
| [`Review_Structure_C_Lifecycle.md`](Review_Structure_C_Lifecycle.md) | 子代理报告 C（深挖 G）：状态分类、生命周期路径矩阵、钩子对称性矩阵（行号是 09-07 的）；结论已并入第 26–30 条并订正第 12 条 |
| [`RE_PlayerStairs.md`](RE_PlayerStairs.md) | 附录 D：玩家绘制楼梯（§4.1）的数据模型、踏步算法与常量、支撑、穿墙洞、栏杆，门前踏步（§4.0），资产清单；`ACSStairsActor` 照它实现 |
| [`RE_WindowToDoor.md`](RE_WindowToDoor.md) | 附录 E：`snap_balcony_door` 的判据与吸附、`DecoratorSubtype` 派生、门与窗的档位 / 洞 / 件、门铃 / 花环；`ACSWindowMarker` 门形态照它实现 |
| [`RE_RoofRidgeGable.md`](RE_RoofRidgeGable.md) | 附录 F：`ridge_length_01` 与脊长抓手、`Roof::is_gable` 判据、山墙时屋面的改写（两种 `GableRoofStyle`）、山墙墙顶 / 砖层收口 / 角石 / 抹灰、新建默认值、常量表；本项目的落法与有意偏离 |
| [`House_Window.md`](House_Window.md) | 窗户（D8）专卷：现状速查表 + 计划 D8 整章 + TG 侧对照 + 时间线 + 已作废结论 + 未完成事项 |
| [`House_MaterialAudit.md`](House_MaterialAudit.md) | 房屋材质与模型修正（2026-09-12）：转角覆盖、支撑柱截面、墙面法线与贴图配对、屋顶尖饰、草材质 |
| [`Roof_TerraceAndRidge.md`](Roof_TerraceAndRidge.md) | 屋顶：2026-10-02 脊长可调、脊长 = 房长时四坡变双坡 + 三角形山墙（阶梯 / 悬挑两种）；2026-09-29 屋面瓦资产按 TG 真值重做（用户确认的 TG 实拍 + `assemble_rectangular_roof` 逆向：瓦 45 × 64 × 10 cm、排距 / 列距 45、瓦长随机、水平屋脊平躺厚板）；2026-09-20 瓦片错排、高度框、低屋顶露台与垛口、屋脊收口、瓦片 VSM |
| [`Stairs_PlayerStairs.md`](Stairs_PlayerStairs.md) | 玩家样条楼梯（2026-09-20）：栏杆与木梯、侧面拱墙、砖石材质、露台接合、宽度把手、样条实时重建 |
| [`Ground_Shaper.md`](Ground_Shaper.md) | 地形塑形物（D9）：Houdini 原型 → UE 的逐节点对照与算法替换 |
| [`Ground_Tuning.md`](Ground_Tuning.md) | 地面派生链（地被 / 石阶 / 石阶材质 / 岩壳）的默认值与调参记录，含 2026-09-22 草的近 / 远两档 |
| [`Ground_Cover.md`](Ground_Cover.md) | 地被（2026-09-29）：草与花改走 Nanite（距离带在 GPU-Scene 写入时抖、花的影子留在 CSM 的经典副本）、WPO 在近 / 远交界前淡出只留法线风、花的聚散、白花改淡黄 |
| [`Ground_Material.md`](Ground_Material.md) | 地面三层材质（草地 / 深色土 / 浅色土路，2026-09-12）：反编译混合公式、高度噪声露土斑块；`M_TinyGladeGround` |
| [`RockShell_Pattern.md`](RockShell_Pattern.md) | TG 原件的历史通道契约与五条实测订正；当前运行时图案改见 `RockShell_Sparse.md` |
| [`RockShell_EdgeBevel.md`](RockShell_EdgeBevel.md) | 岩壳假倒角：运行时壳 = v3（邻接进 UV，`M_TG_Texture` 静态开关 `RockShellBevel`）；直摆资产 = v2（顶点色，legacy） |
| [`RockShell_Sparse.md`](RockShell_Sparse.md) | 稀疏岩壳（2026-09-30 用户定程序化、M = 16，P1 已落地）：程序化胞腔、按裙边环带激活、定长槽位池 + 作业表；TG 位移公式修正与盖裙陡沟的未解决项见「八」；P2 增量 / P3 分页待做 |
| [`Tree_CanopyShading.md`](Tree_CanopyShading.md) | 树冠着色：叶卡片 VS 变形、紧凑 G-buffer、专用延迟光照、深度浮雕；2026-09-09 落地为 `M_TinyGladeCanopy`。几何侧在 `build_tree.py` / `Tree.hip` |
| [`Vine_ObstacleTurning.md`](Vine_ObstacleTurning.md) | 藤蔓遇墙洞 / 墙角的转向（2026-09-12）：绝对倾角改为预算内转向；证据 `evidence/ivy-*-20260912.asm` |
| [`Asset_MeshMaterialRepair.md`](Asset_MeshMaterialRepair.md) | 提取网格的材质槽修正（2026-09-12）：483 个静态网格逐一检查，重绑 21 个网格的 23 个槽 |

合卷各卷的入口锚点：

| 锚点 | 卷 |
| --- | --- |
| [`#vol-0`](Progress_ModuleComparison.md#vol-0) | 卷零 · 完成进度与循环协议 |
| [`#vol-1`](Progress_ModuleComparison.md#vol-1) | 卷一 · 拉尺寸（D5）与接缝角柱（D7） |
| [`#vol-2`](Progress_ModuleComparison.md#vol-2) | 卷二 · 装饰／藤蔓（D12/D13）；窗那半已迁到 [`House_Window.md`](House_Window.md) |
| [`#vol-3`](Progress_ModuleComparison.md#vol-3) | 卷三 · 楼梯模块 |
| [`#vol-4`](Progress_ModuleComparison.md#vol-4) | 卷四 · 渲染与光照（D14） |
| [`#vol-5`](Progress_ModuleComparison.md#vol-5) | 卷五 · 砖构件：门框 / 转角 / 垛口 / 接缝 / 上沿 |
| [`#round-1`](Review_ReverseComparison.md#round-1) | 逆向评审第一轮：门洞机制与改进清单 |
| [`#round-2`](Review_ReverseComparison.md#round-2) | 逆向评审第二轮：未覆盖维度 |

模块完成进度与证据来源（PDB 符号、反汇编用法、提取资产等）只在 [卷零](Progress_ModuleComparison.md#vol-0) 维护，这里不再复述。

## 图索引

| 图 | 说明 | 被谁引用 |
| --- | --- | --- |
| [`House_ChangeFlows.svg`](House_ChangeFlows.svg) | 地面变化 vs 房子移动各自改什么：一律只标脏、房子自己的 Tick 合批兑现；Colors / Heights 两支分别改门洞与落座 + 柱；接触记录拖动期冻结、松手提交、对端只标脏；楼梯按变更盒过滤。2026-09-18 按代码重画（旧版的 subsystem 分发、中立链接墙 actor、布尔重切都已不存在） | 计划 D3 |
| [`House_DecorPlacementFlow.svg`](House_DecorPlacementFlow.svg) | 摆件／植被放置流程：场在 GPU、异步回读、diff 应用。⚠️ **是 D12「复杂度场」那一半的设计稿、尚未实施**（C2 裁决五：保留自有设计、暂不做）；已落地的锚点层（房子四家 + 地面裙边一家）不在图里，图顶横幅有说明 | 计划 D12 |
| [`Review_Structure_ReevaluateDataflow.svg`](Review_Structure_ReevaluateDataflow.svg) | `ReevaluateSite` 的隐式数据流：12 步靠成员变量传中间结果，写 / 读 / 读写逐一标出，顺序约束只在注释里。09-18 按代码更新：新增「② 锚点重表达」列（09-11 版的 ②–⑪ 顺延为 ③–⑫）、接缝一步改为读写 `Contacts` 并由对端从外部写入、加冻结标志总线 | 结构审查 大问题 2 |
| [`Review_Structure_ModuleInteraction.svg`](Review_Structure_ModuleInteraction.svg) | 模块交互图：房子 / 地面 / `UCSHouseLibrary` / 接触记录 / 标记 / 抓手 / 塑形物 / 玩家楼梯 / 编辑器模块 / 遗留藤蔓之间的同步调用、委托广播、世界扫描三种边，圈码对应审查小节，✔ 标出已修的条目；09-18 按代码更新（子系统已删，右上角列出 09-11 以来的结构变化） | 结构审查 深挖 D（8–19） |
| [`Review_Structure_UndoChain.svg`](Review_Structure_UndoChain.svg) | 撤销链路：一次细节面板改 `FootprintSize` 的记录期、`FTransaction::Apply` 四步（`PreEditUndo` → 全部 `Restore` → 组件优先排序 → 逐个 `PostEditUndo`）、插件钩子的扇出（同一次 Ctrl+Z 三次进 `ReevaluateSite`；09-07 时是四次，09-10 起标记那次只置脏），底栏是事务看不见的状态（09-18 补接触记录与 `HouseId` 查重）；09-18 按代码更新行号 | 结构审查 深挖 G（27） |
| [`Ground_Stairs_Logic.svg`](Ground_Stairs_Logic.svg) | 石阶的 GPU 逻辑：marching squares、100% GPU 决策、零回读；单格早退是四道（09-06 加了矮包与台顶两道门控），S3 = 删旧路。09-18 按代码核对 | 合卷卷零「石阶 S1」 |
| [`House_Door_Logic.svg`](House_Door_Logic.svg) | 门洞的当前逻辑：闭环求解、弦长即门宽、转角配成墩；09-18 按斜接折线重画 ③（采样环 = 外皮折线，600×400 时周长 2000、80 个采样点、步长 25），补窗变门走同一张洞表；末栏「已修 / 仍开着」逐条对过代码，示例数字是离线复现 | 计划 D6 |
| [`RockShell_EdgeBevel_Logic.svg`](RockShell_EdgeBevel_Logic.svg) | 缺口法线的生成，两条链路并排：左 **v3**（运行时壳，现役：每趟披挂重写 UV1..UV6 → `M_TG_Texture` 静态开关 `RockShellBevel`）、右 **v2**（直摆资产，legacy：顶点色 → `M_TinyGladeRockShell`，09-11 已删、重跑脚本即重建）→ 折痕截面按 v3 默认档画；09-18 按代码核对 | `RockShell_EdgeBevel.md` |
| [`RockShell_EdgeBevel_TGNormals.svg`](RockShell_EdgeBevel_TGNormals.svg) | TG 原版法线流转：逐三角 SSBO 字段，CS 每帧重算写回、PS 按 id 查自己与邻面；末栏「本项目 UE 版」09-18 改为 v3 的自身 + 三邻面法线（UV3..UV6） | `RockShell_EdgeBevel.md` |
| [`Tree_CanopyShading_Pipeline.svg`](Tree_CanopyShading_Pipeline.svg) | 树冠着色链路：资产层 → VS 八段 → PS 四段 → G-buffer/深度 → 专用延迟光照；橙块与橙线是贯穿三处的深度浮雕 | `Tree_CanopyShading.md` |
| [`Tree_CanopyShading_DepthRelief.svg`](Tree_CanopyShading_DepthRelief.svg) | 深度浮雕的入门解释：平卡片相交是直线 → 按厚度图谎报深度 → 交线沿叶簇轮廓弯曲；末栏是「只有深度是假的」这条代价 | `Tree_CanopyShading.md` |
| [`RockShell_Pattern.preview.png`](RockShell_Pattern.preview.png) | 岩壳原件碎裂图案（TG `rocky_terrain_shell.glb`，609 胞腔） | `RockShell_Pattern.md` |
| [`img/RockShell_Sparse_PatternCompare.png`](img/RockShell_Sparse_PatternCompare.png) | 同一 24 m 窗口、同一比例尺：左 TG 原件 ×0.35（旧做法），右 程序化胞腔默认档（抖动 1.0、扭曲 A=0.5 L=3、M=16） | `RockShell_Sparse.md` |
| [`img/RockShell_Sparse_ExposureBA.png`](img/RockShell_Sparse_ExposureBA.png) | 露出前后对照（同机位近景）：左 竖向 × PatternScale、右 竖向世界 cm；上 关卡胞腔 361 cm、下 默认 193.6 cm | `RockShell_Sparse.md` |
| [`img/RockShell_Sparse_GapEngine.png`](img/RockShell_Sparse_GapEngine.png) | 引擎出图（2026-09-30 缝隙修正后）：`L_TerrainOpsDemo` 土台北面，左中景、右近景；`Scripts/TinyGladeShotRockShellGaps.py` 产出 | `RockShell_Sparse.md` |
| [`img/RockShell_SkirtFormula_Before.png`](img/RockShell_SkirtFormula_Before.png) / [`After`](img/RockShell_SkirtFormula_After.png) | `L_HouseGroundDemo` 同机位公式调整前后：胀缩与浮高共用随机数，保留 TG 位移向量长度衰减；深沟仍可见 | `RockShell_Sparse.md`「八」 |
| [`img/TG_continuous_arches.png`](img/TG_continuous_arches.png) | 连续拱之间没有「墙」这个表面的实拍裁决 | 计划 D6、`CSHouseProfile.h` |
| `img/tiny-glade-ref-*.jpg` | 五张 TG 实拍参考：拱间墩／角柱／垛口／双拱墩／深度融合无缝 | 计划 D6/D7 |
| [`img/tiny-glade-ref-overhang-beam-post.png`](img/tiny-glade-ref-overhang-beam-post.png) | 上房悬挑实拍：悬挑底下是斜撑与落地木柱，竖缝处出砖柱 ⇒ **接缝是「接触」的派生之一**。⚠️ 09-17 照反汇编（`construct_rect_bottom_layer`）给上房加的一圈承托梁被用户判「TG 中没有这个结构」已退回；横缝至今只判定、不出产物 | 计划 D7「接触基类与派生」 |
| [`img/house-bottom-face-20260917-on.png`](img/house-bottom-face-20260917-on.png) | 房体底面启用后的最新专项结果：悬挑下方为连续灰泥底面 | 计划 D4「房体底面」 |
| `img/tiny-glade-ref-{ground-three-layers,corner-quoin,roof-tiles}.png` | 09-12 那轮的三张 TG 实拍：地面三层 / 转角角石 / 屋面瓦 | `Ground_Material.md`、合卷 |
| [`img/tiny-glade-ref-roof-top-20260929.png`](img/tiny-glade-ref-roof-top-20260929.png) | 用户 09-29 确认的「正确的 TG 屋顶」俯视：四坡、大块木瓦错缝叠铺、瓦厚与楔形缝、斜脊两坡瓦端相接、短正脊上厚扁脊瓦 ⇒ 当时工程用的瓦资产是按旧密度压扁的薄片 | `Roof_TerraceAndRidge.md`「屋面瓦资产」 |
| [`屋顶俯视`](img/roof-reference-20260930-top.png)、[`原关卡近景`](img/roof-reference-20260930-demo.png) | 09-30 第二轮修改并重载后的窄斜脊、逐瓦裁剪与 Grey 配色 | `Roof_TerraceAndRidge.md` |
| `img/house-audit-20260912-support-after.png`、`img/quoin-reference-20260912-after.png`、`img/mesh-materials-{after,extended}.jpg` | 支撑柱、包角石及材质样本的最终专项结果 | `House_MaterialAudit.md`、合卷、`Asset_MeshMaterialRepair.md` |
| [`脊长六档`](img/roof-ridge-20261002-sheet.png)、[`端墙正视`](img/roof-ridge-20261002-gable-front.png)、[`俯视`](img/roof-ridge-20261002-top.png)、[`脊端箭头`](img/roof-ridge-20261002-handles.png) | 10-02 脊长可调：同机位从等坡度四坡到双坡 + 山墙、两种山墙样式、换脊向；`Scripts/TinyGladeRoofRidgeQA.py` 的出图拼成 | `Roof_TerraceAndRidge.md`「脊长可调与双坡山墙」 |
| [`img/tiny-glade-ref-door-in-arch.png`](img/tiny-glade-ref-door-in-arch.png) | 门在拱里的实拍：轮廓严丝合缝贴着拱圈石内缘 ⇒ **门是比洞更大的矩形，被墙切出剪影**，不是拱形网格 | 卷零「D6 续」③ |
| [`img/tiny-glade-ref-corner-arch-passage.png`](img/tiny-glade-ref-corner-arch-passage.png) | 转角实拍：**两道拱共用一根角柱，没有木门** | 卷零「D6 续」④ |
| [`img/tiny-glade-ref-freehand-wall-20260922.jpg`](img/tiny-glade-ref-freehand-wall-20260922.jpg) | TG 样条墙参考；最新结果见 [墙文档](Plan_Wall.md)，L 形与 S 形统一引用 `../Result/` 中的图片 | 墙化计划 §二、§八 |
| [`img/tiny-glade-ref-edit-mode-handles.png`](img/tiny-glade-ref-edit-mode-handles.png) | 编辑态标识实拍（09-22 用户贴）：虚线包围盒、每面墙一只平箭头、右侧竖线串两个六边形钮（高度）、顶上屋顶箭头、底部平移四向箭头与转角旋转弧箭头 | 计划 D5「标识 = TG 原版」 |
| [`img/tiny-glade-ref-flower-clusters-20260929.png`](img/tiny-glade-ref-flower-clusters-20260929.png) | TG 的花（09-29 用户贴）：低矮的花一片一片地密、片间零星几朵，风信子三五株一丛 | `Ground_Cover.md`「花的聚散」 |
| `img/cover-nanite-20260929-{usercam,top,wpo-depth-moved}.png` | 地被改走 Nanite 之后的离屏验收：同机位花色、俯视看聚散、两张深度对比看 WPO 在交界外停下 | `Ground_Cover.md`「验证」 |

TG 原件预览 `RockShell_Pattern.preview.png` 是 `Scripts/VerifyRockShellGlb.py` 的输出；当前图案对比 `img/RockShell_Sparse_PatternCompare.png` 是 `Scripts/PrototypeRockShellSparse.py` 的输出。后备脚本生成的旧方案预览不再收录到图索引。

## 相关但不在本目录

| 位置 | 内容 |
| --- | --- |
| `Plugins/PCGPlugins/Scripts/TinyGlade*.py` | 演示搭建 / 出图 / 回归脚本（回归入口 `TinyGladeDemoRegression.py`） |
| `Plugins/PCGPlugins/Source/ComputeShaderGenerator/` | `CSHouse*` / `CSGround*` / `CSStairs*` / `CSWall*` 的类注释；逐字段口径以源码注释为准，MD 不复述 |
| `/PCGPlugins/HouseTest/` | 演示关卡：`L_HouseGroundDemo`、`L_TerrainOpsDemo`，以及屋顶 / 楼梯各轮的专用验证关卡 |
| `doc/index.md` | 项目级文档索引 |

## 目录里的非文档文件

```text
Docs/TinyGlade/
├─ *.svg                                # 流程／算法图，逐张见「图索引」
├─ RockShell_Pattern.preview.png        # TG 原件参考图案
├─ TinyGlade.hip / Tree.hip             # Houdini 原型：地形塑形物 / 树；backup/ 是 TinyGlade.hip 的自动备份
├─ build_tree.py / measure_cards.py     # 在 Houdini 里生成 TG 式树、按资产口径量叶卡朝向
├─ lextab.py / yacctab.py               # pycparser（PLY）自动生成的解析表缓存
├─ geo/                                 # 壳与树的源数据：rocky_terrain.json / *.glb / *.FBX；bevel/ 下为倒角烘焙件
├─ out/                                 # build_tree.py 的输出：树与树冠卡片的 .obj / .bgeo.sc、渲染图
├─ evidence/                            # 反汇编 / 着色器摘录等证据（文件名带日期）
└─ img/                                 # 实拍参考 + 出图对照
```
