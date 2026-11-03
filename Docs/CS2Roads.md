# Palmsland 道路关卡

关卡：`/PCGPlugins/CS2Roads/Maps/L_Palmsland`。在 UE 内直接由路网快照生成道路的 GPU 版本见 [CS2 道路逻辑的 GPU 移植](CS2RoadsGpu.md)。
在内容浏览器启用「显示插件内容」，进入 `PCGPlugins Content / CS2Roads / Maps` 打开。

源数据来自本机 `D:\MyProject\CitiesSkylines-Road2-C#` 已还原的 Palmsland 存档路网：5,667 段道路、4,576 个节点。道路、高架桥面、已有桥体、路缘、人行道、车道标线、车痕及地形按当前 Godot 场景导出。没有额外生成原项目尚未还原的桥梁设施、建筑和水体。

## 资产

- `Meshes/Roads`：89 个区域静态网格，12,129,386 个有效三角形。
- `Meshes/Markings`：89 个区域静态网格，10,312,997 个有效三角形。
- `Meshes/Terrain`：196 个区域静态网格，12,071,936 个有效三角形。地形是 StaticMesh，未转换为 Landscape。
- `Materials`：13 个 Unreal 材质；`Textures`：29 张 Texture2D。
- 世界大纲中的 `Palmsland/Viewpoints` 保留 9 个互通／桥梁定位点；选中后按 F 可定位。

原始表面共 35,679,965 个三角形，清理 1,165,646 个在 Unreal 厘米浮点坐标下没有面积的退化三角形。有效表面没有减面。最终合计 34,514,319 个三角形，网格按 1,024 米区域组织。空间分组不切开三角形，不改变道路曲线。

Godot 米坐标 `(x,y,z)` 对应 Unreal 厘米坐标 `(x,z,y)*100`。glTF 导入器自行完成单位转换，因此导入缩放必须为 1。道路保持原 UV；标线额外使用 UV1–UV3 储存路径距离和端部渐变数据。材质重建了世界空间细节、打包法线、粗糙度、车痕与标线透明度；这是 Unreal 材质适配，不是原引擎逐像素等价渲染。

本关卡用于静态路网查看。没有迁移 Godot 的 C# 车流、道路编辑器、行人模拟或游戏碰撞系统。静态网格未启用 Nanite 和自动碰撞。

## 再次生成

源项目中执行 `Tools/Export-UnrealScene.ps1`，生成 `Artifacts/ue-palmsland/unreal-scene.json`、GLB 和 PNG。依赖 NumPy；首次全城表面生成需要数分钟。

本插件的 `Scripts/ImportCS2Roads.py` 用 Unreal 完整编辑器的 `-ExecutePythonScript` 执行。环境变量 `CS2ROAD_SOURCE` 指向上述导出目录。脚本逐网格检查三角形数、包围盒、材质映射及标线 UV 通道，创建原生 `.uasset` 与 `.umap`。为保护已经编辑的关卡，脚本遇到现有 `L_Palmsland` 会拒绝覆盖。

`Scripts/ValidateCS2Roads.py` 用 `-ExecCmds="py <脚本绝对路径>"` 执行，重新打开保存的关卡并核对引用，预热后拍摄全城、互通、高架近景、街道路口四张图。不能用 `-nullrhi` 运行关卡创建或截图。

此机器默认 Zen DDC 无法连接，本次运行使用命令行参数 `-ddc=NoZenLocalFallback` 和 `-LocalDataCachePath="D:\MyProject\UnrealProject\UETest574_2\DerivedDataCache"`，未修改项目配置。首次启用这个缓存会编译着色器。

原生资产在打开关卡时不依赖 Godot 工程。手动重新导入单个网格需要保留原项目 `Artifacts/ue-palmsland` 中的 GLB；复制到其他项目时需保留 `/PCGPlugins/CS2Roads` 包路径，或在 Unreal 中使用资产迁移并处理引用。

本次导入与截图诊断位于项目 `Saved/CS2Roads`。

## 本次验证

UE 5.7.4 已保存完整关卡并重新加载，374 个网格的资产引用、位置和三角形数量一致。所有网格的最大包围盒偏差为 0.00390625 cm；材质与贴图数量分别为 13 和 29。灯光使用 75,000 lux 的可移动太阳与固定 EV100 13 曝光。

`ImportProbe` 是本次创建的小型导入测试场，未被正式关卡引用。自动审批拒绝了临时目录删除，因此保留；应打开 `Maps/L_Palmsland`，而不是测试场景。
