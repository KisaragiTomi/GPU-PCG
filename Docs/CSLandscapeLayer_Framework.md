# Compute Shader 地形图层编辑框架

2026-09-30 按源码核对。当前编辑器图层以 `ACSLandscape` 为主，河流和道路继承它；
旧的临时图层已合并，`ACSLandscapeLayer`、运行时图层及 Manager 已删除。

## 模块与类层级

- `GeometryScriptExtraEditor`：`ULandscapeExtra::CreateLandscapeTextureData` 读取 Landscape 高度与范围。
- `ComputeShaderGenerator`：`ComputeShaderBasicFunction.h` 提供共享的 `FCSReadLandscapeData` 和 `ECSLandscapeBlendMode`。
- `PCGEditorProcess`：持有 Edit Layer actor、注册逻辑、CS 调度与 Merge 接入；`FCSLandscape` 在 `ComputeShaderLandscape.cpp` 中注册，入口为 `CSLandscape.usf::CSLandscapeFunction`。

```text
AComputeShaderMeshGenerator
└─ ACSLandscapeEditLayerBase（抽象，ILandscapeEditLayerRenderer）
   └─ ACSLandscape
      ├─ ACSLandscapeRiver
      └─ ACSLandscapeRoad
```

`ACSLandscapeTempLayer` 的旧资产由 `Config/DefaultPCGPlugins.ini` 重定向到 `ACSLandscape`。
`LandscapeLayer.usf` 已删除，不再存在独立的运行时图层栈。

## 注册与 Merge

1. 基类 `EnsureEditLayer()` 确保 actor 在 Landscape 上拥有一个 `UCSLandscapeEditLayer` 槽位，用序列化的 `OwnedEditLayerGuid` 关联。
2. `UCSLandscapeEditLayer::GetEditLayerRendererStates` 遍历 `ACSLandscapeEditLayerBase`，只返回 GUID 与当前槽位相符的 actor。
3. 基类提供 renderer 状态与生命周期清理，`ACSLandscape::RenderLayer` 负责具体的合成。
4. `RequestLandscapeUpdate()` 请求重新 Merge；`EditLayerAlpha` 控制引擎图层透明度，`LayerAlpha` 控制 actor 的实时混合。

## 高度数据流

`CopyLandscapeData()` 捕获 Box 范围的高度并发布到该 actor 的 Edit Layer。
实时输入由 `SourceMode` 选择 `FlatOffset`、`ProceduralNoise` 或 `ExternalRT`，经
`EnqueueLayerMerge` 按依赖顺序合成缓存结果与实时输入。道路等子类可以通过
`ApplyHeightmapRTToLandscape()` 接入自己的高度 RT。

- `RT_Result` 保存复制结果，`PersistentResult` 将其序列化，`PostLoad` 恢复到 RT。
- `RT_RealtimeResult` 保存实时合成结果。
- `bAffectHeightmap` 控制是否参与高度合成；启用条件为 `bRealtimeUpdate || bHasResult`。
- `ECSLandscapeBlendMode` 为共享枚举：Alpha、Override、Additive、Subtract、Multiply、MaterialDrive。

## 烘焙与清理

公开入口是 `ACSLandscape::BakeLandscape()`。实时路径使用基类的 `BakePackedHeightToLandscape`，
复制路径使用 `BakeResultToLandscape`；写入 Box 区域的基础高度图，保留周围地形，随后移除当前 Edit Layer。
旧文档的 `CommitToLandscape` / `Paste` 不是现行入口。

基类在销毁路径中移除拥有的图层；新一次 Copy / Refresh 可重新建立图层。
这套机制属于编辑器模块，不是游戏运行时的 Landscape 修改接口。

## 源码与历史图

- [公共基类](../Source/PCGEditorProcess/Public/CSLandscapeEditLayerBase.h)
- [图层注册](../Source/PCGEditorProcess/Private/CSLandscapeEditLayer.cpp)
- [Landscape actor 与河流](../Source/PCGEditorProcess/Public/ComputeShaderLandscape.h)
- [生成、Merge 与烘焙](../Source/PCGEditorProcess/Private/ComputeShaderLandscape.cpp)
- [道路](../Source/PCGEditorProcess/Public/ComputeShaderLandscapeRoad.h)
