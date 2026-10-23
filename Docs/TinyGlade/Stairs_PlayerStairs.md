# 玩家样条楼梯：栏杆、拱墙、露台接合与宽度把手

`ACSStairsActor`（蓝图 `BP_TinyGladeStairs`）在 2026-09-20 几轮迭代里的实现记录：栏杆与木梯、侧面拱墙、砖石材质、露台自动接合、宽度把手、样条拖动实时重建。
逆向依据见 [附录 D](RE_PlayerStairs.md)；同期的屋顶改动（瓦片错排、高度框、露台、屋脊、VSM）见 [`Roof_TerraceAndRidge.md`](Roof_TerraceAndRidge.md)。

![用户参考：瓦屋顶、石铺露台及带栏杆的楼梯](img/tiny-glade-ref-flat-roof-stairs-20260920.png)

![用户参考：楼梯侧墙、大小不同的贯穿拱洞与木栏杆](img/stairs-arches-reference-20260920.png)

## 未实现

挂墙托架、穿房屋墙洞、分叉图编辑仍未实现；露台接合开的是围边入口，不是房屋墙身的门洞。原版 `ArchWalker` 的全部高度阈值尚未逆向完。栏杆尺寸、木梯装配、拱的留边 / 墩宽 / 拱高都是 UE 适配规则，不声称是原版常量。

## 蓝图与资产

- `BP_TinyGladeStairs` 继承 `ACSStairsActor`，与房屋模块并列放在 `/PCGPlugins/HouseTest`；含默认样条、砖 / 栏杆 / 木梯组件，默认木栏杆、接地拱墙、露台自动接合。建蓝图与绑材质的脚本是 `TinyGladeSetupStairsBlueprint.py`。
- 踏步与拱墙用 `TinyGladeAsset/Meshes/brick`，木栏杆与木梯用 `wooden_plank`；`stairs_step` 是地形石阶的资产，不是玩家楼梯的。
- 编辑 Spline 点改路径与高度，点的 Y 缩放改局部宽度；`Wood Mesh / Wood Material` 替换木栏杆与梯子资产。

## 栏杆与木梯

- `Railing` 提供 `None / Low / High / Wooden`，默认木栏杆。木栏杆含立柱与上下两条扶手；石栏杆使用现有砖资产。栏杆随路径转向、高差、节点宽度变化。
- 超过原有 2.5 坡度界限的分段生成木梯（边梁与约 30 cm 间距横档），不再留空；梯子不生成石墙。木构共用 `wooden_plank`，可通过 `WoodMesh` / `WoodMaterial` 替换。
- 栏杆和木梯是独立实例族，支持重建、删除和基类烘焙；关闭栏杆会清空旧实例。
- 护栏中心落在踏面范围内；平走道采用与踏步一致的端点地面抬升，避免斜地面上的护栏悬空或陷入踏面。

## 侧面拱墙

- 支撑按水平样条弧长排墙，砖层交错；沿路径规划拱洞，按踏步底部和地形缩小洞高、洞宽，保留拱间墩。低处空间不足时不挖洞。
- 取消旧的「每一级底下各自砌到地面」实现。踏步和支撑仍共用一个砖实例组件；支撑砖标记 `StepIndex = INDEX_NONE`，不再假装属于某一级踏步。
- 逆向依据：[附录 D §2.2、§3、§8](RE_PlayerStairs.md) 与 [汇编摘录](evidence/stairs-playermade-20260916.asm)。支撑复用墙构造器；`preprocess_curve` 在已确认分支中按约 400 cm 分拱，汇编 `1407810DE–1407810EF` 为扣除端部长度后乘 0.25，再取整。

### 与房屋共用的拱形逻辑

`FCSWallOpening` 是房屋和楼梯共用的洞描述，`Rise()` 与 `SpringZ()` 统一拱高和起拱线。`CSHouseTrim::SplitEdge` 负责按洞剖面切砖层，`CSHouseFrame::MakeOpeningPath / EvalPath / SolveRun` 负责拱圈与排砖。楼梯只增加沿样条规划洞和定位砖的适配层 `CSStairsSupport`，没有「楼梯专用拱曲线」。

原来的墙洞为椭圆，框砖仍用正圆，同边拱间墩又用半宽算起拱线；现统一为洞的实际竖向半径。椭圆弧长与反求角度放在 `CSArchCurve.ush`，由 C++ 和 HLSL 共用；正圆保留原解析解。

窗户的预制拱带属于另一类资产，不合并成砖拱；这里整合的是程序生成的砖拱、洞剖面及拱间墩。

## 砖石材质

楼梯蓝图和示例实例的 `BrickMaterial` 原先为空，回落到 `brick` 网格的展示材质 `MI_TG_MeshProjected_brick_colors_layer00`。它按单块网格局部空间投影、没有逐砖相位，相同纹理随每块砖缩放重复，形成褐色竖向条纹。

现在显式使用 `/PCGPlugins/HouseTest/MI_TinyGladeStairsBrick`，继承 `M_TinyGladeBrick`，共用世界尺度投影和逐砖随机偏移；只把 `BrickColor` 设为提取纹理 `brick_colors_layer03`，房屋的默认砖色不变。原版 `wall_brick_lod0` 像素着色器确有 `brick_colors` 第 3 层分支，但没有证据把参考图对应到某个原版主题 ID，层选择依据的是参考图外观。`TinyGladeMakeBrickMaterial.py` 保留 Nanite 用途；预览等材质编译完成再出图，避免未就绪着色器的黑色误判。

![最新砖石材质：世界尺度投影、逐砖色差与灰绿石色](img/stairs-material-20260920-after.png)

## 露台接合

- 默认启用 `Connect Terraces`。首点或末点放在平屋顶边缘附近、路径从房屋外侧接近时，该端对齐露台石板高度，并略伸入围边内侧。原始样条点不写回；移开后恢复原路径。
- 水平吸附默认 **80 cm**、竖向差默认 **100 cm**（`Terrace Snap Distance / Height`）。这是本项目交互参数，不是逆向常量。
- 每个端点只接最近的一栋房屋；距离相同时按房屋 GUID 稳定排序。斜屋顶、竖向相差过大、擦着墙平行经过、边长不足及闭环样条均不接合。
- 围边入口使用同一份接合结果，按楼梯宽度和入射角留空并加余量；跨入口的围边砖切成剩余段，多段入口重叠也保持畅通。其它围边与转角保留。
- 接口逻辑高度与露台一致，末级砖顶微降 1 cm，伸入露台的部分由石板覆盖，消除共面闪烁。
- 房屋移动、墙高变化和屋顶形态切换会让楼梯在下一次更新重算。楼梯移走、关闭接合或删除后，房屋补回围边；撤销会重算接合。
- 接合是当前位置的几何关系，不是父子绑定，也不存盘：样条及房屋参数才是存盘数据，接合和入口在加载后重算。
- 地形通知与根变换通知统一标脏，在楼梯自身下一次 Tick 合批处理；空闲时关闭 Tick。观测接口读取前兑现待处理更新，材质 / 几何哈希继续吸收重复上传。

## 宽度把手

用法与房屋一致：进入调整模式后在楼梯中段两侧生成临时锥形把手，选中后用编辑器移动工具横向拖动。把手样式、高亮材质、不挡射线、不投影复用房屋的公共辅助函数。

在 `BP_TinyGladeStairs` 实例上调用 `CS Stairs / Resize` 分类中的函数：

```text
EnterResizeMode      // 生成左右两个把手；重复调用不叠加，手动删掉的会补齐
ExitResizeMode       // 销毁把手；删除楼梯或结束关卡也会自动清理
IsInResizeMode       // 查询当前模式
GetResizeHandles     // 取把手 actor
SetStairWidth(W)     // 直接设整体宽度（cm，45–400），返回生效值并立即重建
```

`EnterResizeMode` / `ExitResizeMode` 也有 `CallInEditor` 按钮。把手是临时编辑器道具，不存进地图、游戏中不显示。蓝图自己实现拖拽时，移动把手 actor 后调用把手的 `HandleDrag(bFinished)`，或用 `ConsumeDragToHost` 取该侧实际生效位移；原生 gizmo 会自动调用。

调整规则：

- 样条中心线不动：向外拖一侧 10 cm，两侧各扩 10 cm，总宽加 20 cm；反向收窄。
- 作用于踏步、拱墙、栏杆，以及露台围边入口。
- 保留各样条点的 Y 缩放系数；中段有宽度缩放时按实际显示宽度换算（系数 2 时侧边拖 10 cm，基础 `Width` 加 10 cm）。
- 沿楼梯方向或竖直方向的拖动不改宽度；把手每次拖动后归位。到上下限不积攒残余位移，反向拖动立即生效。
- 支持旋转、移动及缩放宿主，参与撤销 / 重做。路径没有有效长度时不生成把手。

实现：`CSStairsResize.cpp` 从 `BuildRuns` 的已解析路径取中段位置、横向和宽度系数，复用地形贴合及露台吸附。把手类是 `CSStairsWidthHandleActor`，拖拽记账点存于宿主局部空间，防止宿主整体变换被误认为调宽。`SetStairWidth` 统一改属性、登记撤销并触发重建。

## 样条拖动实时重建

原先楼梯只订阅宿主 actor 的变换，没有订阅样条曲线更新，也没处理 `PostEditMove(false)`；移动控制点时宿主不动，通知链不触发（关闭拖动时运行构造脚本后尤其明显）。

现在订阅样条的数据更新和组件变换，并补上编辑器拖动入口。控制点位置、切线、逐点 Y 缩放、增删点都会唤醒下一帧重建队列；同帧通知合并，松手补齐待处理变化。样条组件单独撤销 / 重做也会通知楼梯。更换样条组件时重新绑定，销毁时解绑，空闲时不保持 Tick。

非实时视口及编辑器拖动节流可能不跑 Actor Tick，所以编辑器中有变化时额外登记一次性 Core Ticker 回调，消费完即移除，弱引用宿主、销毁 / 退出时取消；游戏世界仍用 Actor Tick。

蓝图改样条时用默认的 `Update Spline = true` 即可；批量设多个点时先设 false，结束后调用 `UpdateSpline`。不需要再调 `RebuildStairs`。

## 验证

各轮构建都按 Rider 所选 `UETest574_2` 配置的第一项推定 `Development_Editor / x64`（未见显式活动子配置），引擎 `D:/UnrealEngine-5.7.4-release`；每轮都检查过 action plan，只涉及项目插件，没有引擎重建。

| 轮次 | 自动化 | 新增用例 | 日志 / 报告 |
| --- | --- | --- | --- |
| 栏杆与木梯 | 22 / 22（与屋顶同轮，见 [屋顶文档](Roof_TerraceAndRidge.md#验证)） | `Stairs.RailsAndLadder` | `Saved/Logs/RoofStairs_20260920_*` |
| 拱墙 | 31 / 31（与屋脊 / VSM 同轮） | 楼梯接合、拱洞贯穿、反向绘制、栏杆缩宽、共享椭圆剖面 | `Saved/Logs/RoofStairsArch_20260920_BuildFinal.log`、`Saved/RoofStairsArch_20260920_AutomationFinal/index.json` |
| 露台接合 | 25 / 25 | `Stairs.TerraceGeometry`、`Stairs.TerraceOpenings`、`Stairs.TerraceLifecycle` | `Saved/Logs/StairsConnections_20260920_{BuildFinal,AutomationFinal,Reload}.log`、`Saved/StairsConnections_20260920_Automation/index.json` |
| 宽度把手 | 18 项相关用例全过 | `Stairs.WidthHandle`、`Stairs.WidthHandleTransform`、`Stairs.WidthHandleTerrace` | `Saved/Logs/StairsWidthHandle_20260920_Build.log`、`Saved/StairsWidthHandle_20260920_Automation/index.json` |
| 样条实时重建 | 14 项楼梯用例全过 | `Stairs.SplineLiveRebuild`、`Stairs.SplineComponentUndo` | `Saved/Logs/StairsSplineLive_20260920_BuildFinal.log`、`Saved/StairsSplineLive_20260920_AutomationFinal/index.json` |

各轮的警告都是既有场景警告：测试地图缺 RecastNavMesh、凹轮廓按设计取凸包、测试场景缺岩壳资源。

实景验证：

- **拱墙**：`/PCGPlugins/HouseTest/L_RoofStairsArchDemo`，脚本 `Scripts/TinyGladeRoofArchQA.py`（重载验证加 `-RoofStairsReload`）。保存重载后实例来自实际子蓝图、原版砖与木板资产可用、空闲帧不重复上传。
- **材质**：`Scripts/TinyGladeStairsMaterialQA.py`，`-StairsMaterialApply` 保存蓝图及三个示例关卡的缺省材质修正（保留显式自定义材质），`-StairsMaterialReload` 独立重载。重载后蓝图默认、关卡实例、渲染组件都解析到 `MI_TinyGladeStairsBrick`（第 3 层、Nanite 用途开），日志 `Saved/Logs/StairsMaterial_20260920_Reload.log` 记 `STAIR MATERIAL QA PASSED applied=False reload=True`；几何保持 33 级、377 块砖。只改了材质资产、绑定与设置脚本。审计结果在 `Saved/StairsMaterial_20260920/`。
- **露台接合**：`/PCGPlugins/HouseTest/L_StairsTerraceConnectionDemo`，正向木栏杆与反向石栏杆两条楼梯接入同一露台。`TinyGladeStairsConnectionQA.py` 首次创建该专用地图，再次执行加载并验证存盘恢复。坡顶 / 平顶往返、楼梯移开 / 移回时接合数与入口数同步变化；另启编辑器重载后两个连接与两个入口均恢复，末级与露台无共面交错。改动前源码在 `Saved/StairsConnections_20260920_Backup`。
- **宽度把手**：`BP_TinyGladeStairs` 重新编译保存，经反射的蓝图接口生成两把手、向外拖一侧 60 cm，宽度 180 → 300 cm，样条点不动，退出后把手清理干净。脚本 `Scripts/TinyGladeStairsWidthHandleQA.py`，记录 `Saved/StairsWidthHandle_20260920/result.json`；示例地图不保存测试中的宽度。
- **样条实时重建**：关闭拖动构造脚本的离屏编辑器里连续三次改中间样条点，每次都自动重建，上传次数 1 → 2 → 3 → 4，砖数 377 → 390 → 412 → 427；全程没有手动重建或 Actor Tick，把手同步跟随。脚本 `Scripts/TinyGladeStairsSplineQA.py`，结果 `Saved/StairsSplineLive_20260920/result.json`。

## 截图

以下为验证关卡原始截图（补光便于看几何），不是参考图光照的复刻。


![重新加载演示地图后的陡坡木梯](img/roof-stairs-20260920-ladder.png)

![随踏步高度变化的贯穿拱洞、交错砖层与木栏杆](img/roof-arch-20260920-arches.png)


![正反两条楼梯连接同一座露台](img/stairs-connections-20260920-overview.png)

![木栏杆楼梯入口，末级与石板交接](img/stairs-connections-20260920-entrance.png)

![反向绘制的石栏杆楼梯入口](img/stairs-connections-20260920-reverse.png)

![宽度把手拖动后的 300 cm 楼梯](img/stairs-width-20260920-after.png)

![样条变化后自动重建的楼梯](img/stairs-spline-20260920-after.png)
