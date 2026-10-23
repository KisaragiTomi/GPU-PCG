# Instance 剔除与实例 ID 稳定性优化方案

日期：2026-09-30。依据：当前插件工作区实现与本机 UE 5.7.4 源码。

状态（2026-10-03）：第一至第三阶段已落地，第四阶段做了实测是热点的那一项（每组件固定成本），其余按实测判为不做或待定；逐步进度、开关与结果见 §11。
§2 的字节数来自缓冲布局计算，实测的 GPU 耗时、帧率与显存见 [Docs/InstanceCulling/](Docs/InstanceCulling/) 下的三份结果文档
（[基线](Docs/InstanceCulling/Baseline_20261002.md)、[可见源行号](Docs/InstanceCulling/VisibleSourceIndex_20261002.md)、[第三 / 四阶段](Docs/InstanceCulling/Step3_20261002.md)）。

## 1. 目标与范围

优化 `UCSGpuInstancedMeshComponent` 的经典 GPU 剔除路线，以及 `UCSGpuInstancedNaniteComponent` 的 GPUScene 数据更新路线。同时明确实例身份与存储、绘制下标的区别，避免增删、排序、剔除后状态绑定到错误的实例。

优先顺序：建立性能与正确性基线 → 明确身份和索引契约 → 经典路线输出可见源索引 → 减少无效派发与重复更新 → 根据实测决定分簇、批处理和遮挡优化。

两条路线分别处理：

- **经典路线**：插件自己执行距离、视锥剔除、LOD 选择、可见数据压缩和间接绘制参数生成。
- **Nanite 路线**：插件把实例写入 GPUScene，由 UE Nanite 管线完成后续剔除、LOD、光栅与阴影。优化重点是数据供给、更新范围和缓存失效。若创建的是引擎 ISM 回退代理，应单独记录，不能当作 Nanite 测试结果。

## 2. 已核实的当前行为

本节记的是 2026-09-30 写方案时的行为，也就是各阶段要改的起点。之后改掉的几条在行尾标了「→ §11.x」，现状以 §11 为准。

### 2.1 经典路线的输出与显存开销

每个可见实例输出 3 个 Transform float4、1 个 Origin/Random float4、1 个 Lightmap float4，以及 2 个 CustomData float，总计 **88 字节**。其中 Lightmap 当前总是写零，未提供 CustomData 时仍写两个零。（→ §11.2：默认已改成每槽 4 字节的源行号，这一种留作对照。）

每个 LOD 预留完整实例容量。记容量为 C、LOD 数为 L、单次剔除后可见实例数为 V：

- 可见输出缓冲预留约为 `88 × C × L` 字节。
- 每次剔除写出实例属性约为 `88 × V` 字节；不是每帧都写满所有 LOD 预留区域。
- 100 万容量、4 个 LOD，对应约 336 MiB 输出缓冲，尚未包括源实例、基础网格及其他资源。

UE 桌面 GPUScene 的普通 instance culling 输出分支主要写出 4 字节的打包实例 ID，绘制时访问已有实例数据。88/4 = 22 仅表示这部分单条输出的数据量比例，不表示总带宽、显存或帧率会改善 22 倍；UE 还有输入读取、不同 draw/view/pass 的输出，以及其他压缩分支。

### 2.2 两级剔除的覆盖范围

- CPU 实例源会根据空间位置进行 Morton 排序，并生成固定大小的簇。
- Packed GPU 源和点云 GPU 源当前没有簇表，经典路线跳过簇级剔除，直接逐实例检查。
- InstanceCull 按 `InstanceCapacity` 派发，不按 GPU 活跃计数派发。超出活跃数量、或所在簇被拒绝的线程提前返回，但相应线程组仍被派发。（→ §11.3：CPU 源已改成按行数派发；GPU 源不变。）
- 点云源在剔除前每帧执行一次 PackPointInstances，即使生产者的数据没有发生变化。

### 2.3 调度与功能差异

- 每个 Proxy 独立提交计数器清零、可选 ClusterCull、InstanceCull 和 BuildArgs 等操作。大量小组件可能受 CPU 建图、GPU 小任务和资源依赖开销影响。（→ §11.4：每个代理的固定开销压过一轮，仍是各提交各的。）
- 同一个 LOD 的材质 sections 共享可见结果，不必按 section 重复复制实例；优化时应保留此特性。
- 当前每个 ViewFamily 只使用第一个视图执行剔除，共享每个 Proxy 的输出缓冲。不能直接把这种工作量与完整的多视图或阴影视图处理比较。
- 当前经典路线没有逐实例 HZB 遮挡测试。这不表示整个组件完全没有引擎层面的可见性处理。
- 本机 UE 5.7.4 中普通 GPU instance culling 的 `r.InstanceCulling.OcclusionCull` 源码默认值为 0；测试须记录实际运行时值，不能假定原生 ISM 默认开启逐实例 HZB。此开关不能用来概括 Nanite 的遮挡管线。
- 插件使用逐实例原子计数；UE 普通输出分支也有逐实例 `InterlockedAdd`，不能仅凭这一点判定插件落后。

### 2.4 Nanite 更新行为

- GPU 源当前每帧触发 GPUScene 写入；CPU 源主要在数据变化时写入，启用距离带后，相机位移超过当前阈值也触发更新。（→ §11.3：承诺了"改了就通知"的 GPU 源与 CPU 源同一口径。）
- GPU 源给 GPUScene 申请的槽数来自容量，CPU 源来自打包行数。
- 槽数变化时重建代理，GPUScene 分配区间可能改变。
- GPU-only 实例在当前引擎 SceneCulling 的上层空间组织中按图元进入 SingleCell。组件跨越很大空间时，上层裁剪粒度可能不足；后续仍有逐实例剔除，不能理解成只做一次组件包围盒测试。
- 每帧写入可能增加 VSM 缓存失效成本，必须与实际阴影缓存行为一起测量，不能只看写入 pass 的耗时。

## 3. InstanceID：必须分开的四种含义

### 3.1 StableInstanceID：逻辑身份（CPU 实例源已落地）

2026-10-02 落地为 `FCSGpuInstanceHandle`（稳定槽 + 代数，作用域是单个组件，随组件存盘），接口与语义见
`CSGpuInstancedMeshComponent.h` 的结构体注释与「稳定句柄」那一组函数。GPU 实例源的身份仍归生产者（见 §4.2）。

用于识别同一块砖、同一棵草或同一个交互对象。在实例存活期间保持不变，不由当前数组位置、Morton 顺序、可见列表位置或 GPUScene 地址计算。

需要随实例长期保存的状态，例如随机种子、动画时间、选中状态、交互记录，应归属于此身份。跨场景保存需要明确作用域和序列化规则；单个组件里的整数不能直接作为全局唯一 ID。

### 3.2 SourceIndex：当前源缓冲位置

用于本次绘制定位 Transform、Random、CustomData 等数据，可以发生变化。

当前 CPU API 的增删行为：

- `AddInstance` 在 CPU 数组末尾追加，已有 CPU 数组下标不变。
- `RemoveInstance` 使用 `RemoveAtSwap`，最后一个实例填补删除位置。例如 `[A,B,C,D]` 删除 B 后成为 `[A,D,C]`，D 从下标 3 移到 1。
- 增删后重建 Morton 排列，因此 CPU 数组下标与 PackedInstances 源下标不是同一个索引空间。
- 整体包围盒变化会改变 Morton 编码的归一化范围，可能引起更广泛的 GPU 源顺序变化。
- CPU 源随机数按**稳定槽**取（`InstanceRandom(PerInstanceSlots[Src])`，2026-10-02 起）：被交换到新 CPU 下标的实例随机值不变。此前按下标取（`InstanceRandom(Src)`），被换进空位的实例随机值会变；从没删过实例的内容槽号等于下标，数值与改动前逐位相同。

GPU 直接供给源是否重排，取决于生产者。只修改计数器、保持已有源行不动时，不会因此自动重排已有源行；如果生产者重新散点、排序或压缩，源下标可能改变。

### 3.3 VisibleIndex：当前 view/pass/LOD 的绘制槽

当前 InstanceCull 使用原子计数分配 Slot。线程到达顺序没有稳定性保证；实例数量不变时也不能依赖其跨帧保持一致。视点、可见性、LOD 改变都会影响输出。

现有 Transform、Random、CustomData 被复制到同一个 Dst，因此本帧属性仍然配套。真正危险的是外部状态仍按上帧 VisibleIndex 查找。

后续索引路线的关系应为：

```text
本次绘制的 VisibleIndex
    → VisibleSourceIndices[View/Pass/LOD 对应区段 + VisibleIndex]
    → 当前 SourceIndex
    → SourceInstances / SourceCustomData / SourceStableIds
    → StableInstanceID 对应的持久状态
```

可见列表存的是当前源索引，不是自动获得稳定性的“永久 ID”。如果持久 ID 是稀疏句柄，不能不经过映射就拿它直接索引紧凑源数组。

### 3.4 GPUSceneInstanceID：引擎内部地址

当前 Nanite writer 使用 `InstanceSceneDataOffset + Slot`。GPU 源容量及源排列不变、代理未重建时，仅活跃数量变化不会让 writer 主动搬移已有槽；减少数量时尾部槽被写成 HIDDEN。

代理重建或区间重新分配后，起始偏移可能变化。CPU 源的 Morton 重排也会改变对象与 Slot 的对应关系。因此 GPUScene 全局 ID 不应作为业务层持久身份。

UE 原生普通 InstanceIndex 同样可能因删除而移动。ISM 提供 `FPrimitiveInstanceId` 和 ById 接口，但不能将其等同于 GPUScene 地址，也不能无条件用于 HISM 或会重置 ID 跟踪的编辑器操作。

## 4. 第一阶段：基线与身份契约

状态：已完成（2026-10-02），产物与结论见 §11.1。

### 4.1 建立基线

记录实例容量、活跃数、可见数、LOD 分布、组件数、材质 section 数、更新比例与相机路线，同时记录 GPU/驱动、分辨率、构建配置、实际渲染路线及关键 CVar。

分别统计 CPU 源重建/排序/上传、渲染线程建图、PackPointInstances、ClusterCull、InstanceCull、BuildArgs、Nanite writer、主通道与阴影开销。最终以整帧 CPU/GPU 耗时、显存与画面一致性判断收益，不能只比较剔除 pass。

### 4.2 明确身份和映射

拟为需要持久状态的实例增加独立身份流或记录，并维护 `StableInstanceID ↔ 当前 CPU/SourceIndex` 映射。排序和 RemoveAtSwap 时同步更新映射，随机种子在创建时确定，或由稳定身份确定。

句柄若会复用，使用 slot + generation 或等效机制，使删除后保存的旧引用不会错误指向新实例。必须声明“删除后重新生成”是新实例还是同一逻辑对象；不能用位置相同来隐式判定身份。

GPU 生产者若每帧重建全体实例，需要提供稳定的生成键或持久身份映射。仅对本帧数组下标做 hash 不足以保证增删后的身份稳定。

可以暂时保留现有按数组下标操作的 API，但应显式注明其不稳定性。StableInstanceID 是新增能力，不应静默改变旧接口的语义。

**验收**：追加、中间删除、尾部删除、跨容量增长、Morton 重排后，同一存活实例保留身份和随机种子；已删除句柄不能访问后来占用同一槽的新实例。

## 5. 第二阶段：经典路线只输出可见源索引

状态：§5.1、§5.2 已完成并设为默认（2026-10-02），§5.3 实测判为不做；见 §11.2。

### 5.1 第一版保持现有 LOD 分区

保留距离/视锥/LOD 选择，剔除通过后写出 `uint SourceIndex`，不再复制完整 Transform、Origin、Lightmap 和 CustomData。初版继续为每个 LOD 保留 C 个索引，降低实现耦合。

预期可见输出存储从 `88 × C × L` 降为约 `4 × C × L` 字节。100 万容量、4 个 LOD 时，这部分从约 336 MiB 降为约 15.3 MiB。源数据及新增稳定身份流仍需单独计算。

需要修改插件的 VertexFactory/实例取数路径，让所有实例属性通过同一个 SourceIndex 获取。当前经典路线依赖 LocalVertexFactory 所需的可见实例属性布局，因此不能只把 compute shader 的输出换成 ID 而保持取数路径原样。

Lightmap 当前恒零，可以考虑由取数路径返回常量；前提是验证材质和 VertexFactory 的全部相关访问。CustomData 缺失时也应使用明确的默认值规则，不能读未绑定资源。

重点验证实例变换、Random、CustomData、LOD 偏移、多材质 section，以及实际支持的深度、阴影、速度等 pass。未来加入 PreviousTransform 时也必须使用同一身份/源索引规则。

### 5.2 资源与同步约束

- 可见索引与源数据必须属于同一数据版本。剔除结束后、绘制消费前，不能把源缓冲原地重排成下一批数据。
- 在 RDG 中正确声明源数据、索引、间接参数的读写依赖；外部资源继续遵守现有常驻流访问状态规则。
- 多视图、不同 pass 使用独立结果或经过证明的保守共享结果，避免后一次剔除覆盖前一次尚未消费的数据。
- 不以 GPU 回读可见计数驱动 CPU 绘制；仍通过 GPU 计数和间接参数完成。

### 5.3 紧凑 LOD 区段作为后续选项

若索引缓冲的 `C × L` 预留仍显著，可增加 LOD 计数前缀和与 scatter，生成动态区段起点，把单视图、每实例只选一个 LOD 的总索引容量压到约 C。

该方案需要 VertexFactory/绘制参数能读取 GPU 决定的区段起点，并增加若干 pass。LOD 淡入淡出、多个视图或多份输出会改变容量上限，应单独推导；小组件可能不值得支付这些额外开销。

**验收**：相同源实例的可见集合、LOD 和材质属性与基线一致；显存下降；输出带宽收益没有被顶点阶段的间接读取抵消。高顶点数、多 pass 场景必须测量，不能只测简单草片。

## 6. 第三阶段：减少更新、扫描和原子竞争

状态：§6.1 已完成；§6.2 只做了 CPU 源按行数派发，GPU 源的间接派发实测判为不做；§6.3 与 §6.4 的聚合原子实测判为不做；
§6.4 的跨组件合批只做了"压每个代理的固定成本"这第一刀。见 §11.3、§11.4。

### 6.1 CPU 增删合批与重建范围

当前单个 AddInstance/RemoveInstance 都会重建实例数据并标记渲染状态。大量操作优先使用现有 AddInstances/SetInstances 合批入口；后续考虑批量删除或 Begin/EndUpdate，在事务结束时统一排序、打包和上传。

已有容量滞回减少缓冲扩缩容，但不会自动消除排序、上传和代理更新成本。是否采用局部簇更新、空间桶或异步重建，应由编辑频率和实例规模决定；重建期间保持映射与渲染数据版本一致。

### 6.2 GPU 实际计数驱动派发

对于容量远大于有效范围的情况，在 GPU 上生成间接 dispatch 参数，避免扫描大量尾部空槽。CPU 源已知数量时，可以直接按有效数量派发，无需 GPU 回读。

先处理“扫描范围内的行数”，再区分隐藏实例与真实可见数。当前 Counter 表示连续前缀的有效范围；如果引入删除留洞和空闲槽复用，需要新增有效标记与 high-water-mark，或维护 ActiveSourceIndices。不能将存活总数直接用作稀疏槽数组的扫描上限。

间接参数生成本身有开销，保留直接派发的小规模路径，并以测试确定切换条件。

### 6.3 GPU 源分簇与可见簇工作列表

允许生产者提供簇范围及保守包围体，或在有收益时构建 GPU 空间分组。对频繁变化的源，构建/维护成本也计入整帧。

粗剔除输出可见簇列表，后续按列表间接派发实例细剔除，才可能避免为已拒绝簇派发全部实例线程。只增加 ClusterVisible 标记、仍全容量派发，不等于消除了这些线程组。

优先使用生产者已有的局部性，例如地被 patch、墙段或空间块；若为排序搬移源行，StableInstanceID 和 CustomData 必须同步保持对应。

**正确性前置检查**：当前簇和实例共享 PassesViewTests，最大距离判断只检查中心。簇中心超距不代表全部成员都超距；簇级应使用覆盖成员的保守距离下界，例如在同一距离度量下仅当 `中心距离 > 最大距离 + 簇半径` 才整体拒绝。实例最终的中心距离判据可保持原语义。非均匀缩放与 WPO 的边界覆盖也需明确，不能把误剔除当作性能收益。

### 6.4 聚合原子计数与跨组件合批

可尝试按 wave/group 内相同 LOD 分组，先计算局部前缀，再集中申请输出区间，降低热点计数器竞争。要处理同一 wave 内不同 LOD、不同 wave 大小和目标平台支持，并保留正确的回退路径。

组件数量成为瓶颈时，再合并兼容工作的清零、剔除和 BuildArgs。不同源缓冲、视图、材质及资源生命周期增加批处理复杂度，先合并可共享的阶段，不假定一条 dispatch 可以直接覆盖所有组件。

## 7. Nanite 路线：减少无变化数据的重复写入

状态："整批无变化则跳过"已完成（逐源自愿的改动通知，地被与灌木已接入），局部范围写入与点云打包复用没做；见 §11.3。

拟为生产者建立显式的数据版本/脏范围协议，区分 Transform、Random/CustomData、活跃范围、容量及分配代数等变化。生产者确实没有改数据时才允许跳过写入；当前生产者可以原地改 buffer，所以不能简单删除每帧写入而不补通知协议。

优先实现“整批无变化则跳过”，再评估局部范围写入。局部写入必须处理新增槽、删除槽的 HIDDEN 标记、数据布局变化，以及代理重建后的完整初始化。

相机变化影响距离带时，即使源数据未变也可能需要更新隐藏标记；组件变换、包围盒、代理/分配代数变化也不能被数据版本判断遗漏。距离带判定与 Transform 写入是否拆分，需结合引擎更新接口和收益验证。

点云打包也可复用同一版本机制；源点、法线、实例尺度或 WorldToComponent 变化时必须重新打包。

对于横跨大范围的 GPU-only 组件，可评估按空间 patch 拆分以改善上层裁剪和更新粒度，同时记录额外组件、代理与调度成本。

阴影方面同时测量 VSM 缓存失效。只有实例和阴影形态确实符合静态承诺时才采用相应缓存策略；不能为提高命中率而把真实动态数据统一声明为 Static。

**验收**：静止数据降低写入次数；增删和源更新当帧可见；缩减计数后无幽灵实例；代理重建无漏写；距离带与阴影仍随相机和对象正确变化。

## 8. 遮挡与多视图：按实际用途补齐

HZB 作为后续可选优化。遮挡密集场景可能减少大量顶点和像素工作，开阔地被场景则可能只增加测试成本。不能以“测试更复杂”直接判断整帧更慢或更快。

实现前明确使用当前帧还是历史 HZB，处理相机切换、快速移动、近裁面、遮挡物变化、动态实例与 WPO 的保守边界，并建立必要的重新测试或历史失效机制。不要只加一次历史深度比较就视为完成遮挡剔除。

主相机可见集不能直接代表阴影视图可见集：主视锥外的实例可能向画面内投影。分屏、立体和独立捕获视图也须明确输出缓冲的所有权。性能对比必须按相同功能与相同画面结果分组。

## 9. 验证方案与完成标准

### 9.1 性能场景

- 数量：1 万、10 万、100 万；容量等于活跃范围，以及容量显著大于活跃范围。
- 可见比例：接近全部、约一半、很少；包含开阔场景和密集遮挡场景。
- 数据变化：静态、少量局部增删、持续追加、整批 GPU 重建、点云源。
- 组织方式：单个大组件与多个小组件；1/2/4 个 LOD；单材质与多 section。
- 几何与材质：低顶点数、高顶点数、Masked/WPO；阴影开关及相同阴影配置。
- 路线：插件经典路线、匹配条件的原生 ISM/HISM、插件 Nanite 路线；单独记录回退代理。

先用代表性组合定位瓶颈，再扩展到最相关的边界组合，不要求机械地穷举所有排列。

固定相机路线和种子，完成资源与 shader 预热后采样，排除 shader 编译和后台编辑器活动干扰。记录整帧与关键阶段的中位数、P95、显存峰值及有效实例/三角形数；冷启动、代理重建和容量增长尖峰另列，不能被稳态平均值掩盖。

### 9.2 正确性检查

- 用 StableInstanceID 对可见集合和 LOD 做比较，而不是比较原子压缩产生的列表顺序。
- 追加、删除中间/尾部实例、清空后重建、跨容量阈值后，身份、随机数和 CustomData 保持契约。
- 高容量低活跃数、隐藏槽、空源、计数上限保护和复用旧槽均不发生越界或幽灵绘制。
- 视锥边界、簇跨距离边界、非均匀缩放、WPO、快速相机移动不漏画。
- 多视图与阴影不消费被其他视图覆盖的结果；源数据与可见索引版本一致。
- Nanite 更新跳过、距离带变化、代理重建、GPUScene 区间变化及阴影缓存均正确。

开发阶段可使用异步诊断回读验证身份和集合；常规渲染路径不应依赖同步回读。GPU 测试要验证真实映射和资源生命周期，仅检查 CPU 计算公式不够。

### 9.3 实施顺序与验收门槛

1. 保存可重复的基线和实际功能配置，明确身份语义与映射。（完成，§11.1）
2. 实现固定 LOD 分区的可见源索引路线，验证属性读取和整帧收益。（完成，§11.2）
3. 对 CPU 增删合批、GPU 低占用容量派发、Nanite/点云无变化更新分别优化，独立测量。（完成；GPU 源的间接派发与点云打包复用量过之后不做，§11.3）
4. 对实测热点增加可见簇工作列表、聚合原子和跨组件批处理。（实测只有"组件多"是热点：压了每个代理的固定成本，真正的合批没做，§11.4）
5. 有明确收益后再实施紧凑 LOD 分区、HZB 或更复杂的空间层级。（没有量到明确收益，没做）

每一步保留可比较的基线开关。只有画面及身份契约正确、目标场景整帧或内存指标改善、其他关键场景没有无法接受的回退，才进入下一阶段。具体性能阈值应在取得基线后确定，不预先承诺帧率提升比例。

后续 C++ 编译验证遵循项目约定：先读取 Rider 当前配置并匹配其引擎、目标、平台和架构；优先验证改动翻译单元，不使用模块级构建开关。需要广泛的完整目标构建时按项目规则取得明确批准。本次仅新增文档，不触发构建。

## 10. 代码依据与实施入口

插件路径均相对本文件所在目录；函数和符号比易变化的行号更适合作为长期定位入口。

- [CSGpuInstancedMesh.usf](Shaders/Private/CSGpuInstancedMesh.usf)：PassesViewTests、PackPointInstancesCS、ClusterCullCS、InstanceCullCS、BuildArgsCS；原子压缩和完整属性输出。
- [CSGpuSharedLayout.ush](Shaders/Private/CSGpuSharedLayout.ush)：行布局、2 个 CustomData float、4 个 LOD 上限等共享常量。
- [CSGpuInstancedMeshComponent.cpp](Source/ComputeShaderGenerator/Private/CSGpuInstancedMeshComponent.cpp)：AddInstance、RemoveInstance、RebuildInstanceData、ResolveInstanceCapacity；Morton 排序与 CPU/GPU 源布局分支。
- [CSGpuInstancedMeshComponent.h](Source/ComputeShaderGenerator/Public/CSGpuInstancedMeshComponent.h)：FCSGpuInstanceSourceGPU、FCSGpuInstancePointSourceGPU、源计数与容量契约、实例操作接口。
- [CSGpuInstancedMeshSceneProxy.cpp](Source/ComputeShaderGenerator/Private/CSGpuInstancedMeshSceneProxy.cpp)：CSGpuInstancedBuildAuxStreamDescs、RunCulling、视图扩展与间接绘制偏移。
- [CSGpuInstancedMeshVertexFactory.cpp](Source/ComputeShaderGenerator/Private/CSGpuInstancedMeshVertexFactory.cpp)：实例属性读取及实例区段偏移绑定；索引路线需共同调整。
- [CSGpuInstancedNaniteComponent.cpp](Source/ComputeShaderGenerator/Private/CSGpuInstancedNaniteComponent.cpp)：SetFeed、TickComponent、IssueGpuSceneWrite、CreateSceneProxy；更新频率与代理生命周期。
- [CSGpuInstancedNaniteComponent.h](Source/ComputeShaderGenerator/Public/CSGpuInstancedNaniteComponent.h)：FCSGpuNaniteInstanceFeed::GetSlotCount；CPU 数量与 GPU 容量区别。
- [CSGpuInstancedNaniteWriter.usf](Shaders/Private/CSGpuInstancedNaniteWriter.usf)：WriteInstancesCS；GPUScene 地址、尾部隐藏及距离带写入。
- [CSGpuInstancedIndexedVertexFactory.ush](Shaders/Private/CSGpuInstancedIndexedVertexFactory.ush)：可见源行号布局的顶点工厂着色器，引擎 `LocalVertexFactory.ush` 的打补丁副本，由 [GenerateGpuInstancedVertexFactoryShader.py](Scripts/GenerateGpuInstancedVertexFactoryShader.py) 生成，**不要手改**。
- [CSGroundShaperSteps.cpp](Source/ComputeShaderGenerator/Private/CSGroundShaperSteps.cpp)：HandOverInstanceSources；生产者交接实例源、兑现"改了就通知"的地方。
- [CSMesh.cpp](Source/ComputeShaderGenerator/Private/CSMesh.cpp)：FCSMeshRenderThreadEdit；剔除借渲染器的图编辑常驻流的入口（流位图参数）。
- [Tests/](Source/ComputeShaderGenerator/Private/Tests/)：`CSGpuInstancedMeshTests.cpp`（StableIdentity、BatchUpdate、Streams）、`CSGpuInstancedVisibleIndexTests.cpp`（Picture、Attributes）、`CSGpuInstancedNaniteTests.cpp`（NaniteMode、NaniteNotify）。
- [Bench/CSGpuInstancedBench.cpp](Source/ComputeShaderGenerator/Private/Bench/CSGpuInstancedBench.cpp)：`CSGpuInstanced.Bench`，场景表、机位与口径；包装脚本在 [Scripts/](Scripts/)。

引擎根目录：写方案的那台机器是 `D:/UnrealEngine-5.7.4-release/Engine`，跑基线的那台是 `D:/UE-SourceCode-5.7.4/Engine`（脚本读 `UE_ROOT`）。以下路径相对引擎根目录：

- `Shaders/Private/InstanceCulling/BuildInstanceDrawCommands.usf`：WriteInstance、IsInstanceVisible；打包 ID 输出、距离/视锥/HZB、原子计数分支。
- `Shaders/Private/InstanceCulling/CompactVisibleInstances.usf`：保序压缩及前缀和实现；不能泛化为所有普通剔除分支都采用 wave 聚合输出。
- `Source/Runtime/Renderer/Private/InstanceCulling/InstanceCullingManager.cpp`：批处理与 deferred culling。
- `Source/Runtime/Renderer/Private/InstanceCulling/InstanceCullingContext.cpp`：普通 GPU instance culling 的相关 CVar 默认值。
- `Source/Runtime/Renderer/Private/SceneCulling/SceneCulling.cpp`：GPU-only 图元的 SingleCell 路径。
- `Source/Runtime/Engine/Private/HierarchicalInstancedStaticMesh.cpp`：HISM 的 Traverse 与可见实例区间。
- `Source/Runtime/Engine/Private/InstancedStaticMesh.cpp`：原生增删、RemoveAt/RemoveAtSwap 与 ById 接口。
- `Source/Runtime/Engine/Public/InstanceDataTypes.h`：FPrimitiveInstanceId 的定义。

官方补充资料：[ISM/HISM 与 Nanite 说明](https://dev.epicgames.com/documentation/unreal-engine/instanced-static-mesh-component-in-unreal-engine)、[FPrimitiveInstanceId](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FPrimitiveInstanceId)、[AddInstancesById 使用限制](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/UInstancedStaticMeshComponent/AddInstancesById)。在线默认文档可能随引擎版本更新，具体实现以项目使用的 5.7.4 源码为准。

## 11. 实施记录

### 11.1 第一阶段：基线与身份契约（2026-10-02，已完成）

**基线**（§4.1）

- 分段计时：经典路四个 pass 与 Nanite 路的 GPU-Scene 写入各有一条 `DECLARE_GPU_STAT`（`GPU/CSGpuInstanced*`），
  渲染线程建图耗时、派发行数、写入次数进 CsvProfile 的 `CSGpuInstanced/` 分类。只加计时，不改任何剔除 / 绘制逻辑。
- 跑测工具：`-game` 进程里的 `CSGpuInstanced.Bench`（`Source/ComputeShaderGenerator/Private/Bench/CSGpuInstancedBench.cpp`），
  包装脚本 [Scripts/RunGpuInstancedBench.ps1](Scripts/RunGpuInstancedBench.ps1)，报表 [Scripts/GpuInstancedBenchReport.py](Scripts/GpuInstancedBenchReport.py)。
  30 个场景 × 5 个机位，支持交错 A/B（`-Variants` + `-Pairs`）—— 后面每一步的"可比较的基线开关"就走它。
- 基线数据与发现：[Docs/InstanceCulling/Baseline_20261002.md](Docs/InstanceCulling/Baseline_20261002.md)。
- 正确性基线：自动化 `PCGPlugins` 全套 240 条，`GpuInstancedMesh.*` 12 条全过；既存红 4 条、间歇红 1 条，均与实例路无关（同上文档末节）。
  无头跑测脚本 [Scripts/RunPluginAutomationTests.ps1](Scripts/RunPluginAutomationTests.ps1)。

**身份契约**（§4.2）

- CPU 实例源：`FCSGpuInstanceHandle`（稳定槽 + 代数）与按句柄的查 / 改 / 删接口；逐实例随机数改按稳定槽取；
  `GetPackedInstanceSlots()` 给出"源行号 → 稳定槽"（Morton 重排后拿它认实例）。按下标的旧接口语义不变，注释里标明了下标不稳定。
- "删了再加回来"是新实例（新的代数）；`SetInstances` / `ClearInstances` 是整套换掉，旧句柄全部失效、新实例占槽 0..N-1。
- 2026-10-02 以前存的资产没有身份表：读盘时按下标补发槽号，随机数与改动前逐位相同。
- GPU 实例源：身份归生产者，契约写在组件类注释「实例的四种编号」一节 —— 随机数必须按稳定的生成键取（地被、石阶、门框砖、瓦、藤
  已经是这样）；点云源按行号取，只在生产者只追加、不搬行时稳定。没有给 GPU 源加身份流：目前没有消费者。
- 验收用例：`GpuInstancedMesh.StableIdentity`（追加、中间 / 尾部删除、槽复用、Morton 重排、成批增长、复制、老资产、整套重置、清空）。

**基线对实施顺序的提示**（实测，详见基线文档「发现」）

| 方案里的项 | 实测上限 | 提示 |
| --- | --- | --- |
| §5 只输出可见源索引 | 显存 −50% ～ −95%（可见缓冲是常驻显存的大头）；GPU 时间约 −0.5 ms / 百万个可见实例 | 收益主要是显存；顶点阶段取数成本要盯（小网格现在就比 ISM 贵三成） |
| §6.1 CPU 增删合批 | 10 万个实例每次调用约 12 ms，几乎全是整份重排 | 值得做；现有的"攒着传"形态没有攒 CPU 那一半 |
| §6.2 GPU 计数驱动派发 | 约 0.06–0.08 ms / 70 万行空槽 | 收益很小，可以排到最后 |
| §6.4 跨组件合批 | 256 个组件时 GPU −2 ～ −3 ms、渲染线程 −2.5 ms 以上 | 多组件场景里这是最大的一笔，建议提前 |
| §7 Nanite 路跳过无变化写入 | 10 万槽约 −0.35 ms、100 万槽约 −1.8 ms（主要是 VSM 不再每帧作废）；槽数按容量申请另有光追场景的成本 | 值得做 |
| （方案里没有）经典路的阴影 | 100 万个实例 `edge` 机位阴影 20.5 ms，占 GPU 帧时间六成 | 开投影的经典路族最大的一笔，需要单独立项 |

### 11.2 第二阶段：可见源行号（2026-10-02，已完成，默认开）

详见 [Docs/InstanceCulling/VisibleSourceIndex_20261002.md](Docs/InstanceCulling/VisibleSourceIndex_20261002.md)。

- 剔除只往可见槽写 4 字节的源行号，绘制经它直接读源实例行；LOD 分区、剔除判据、材质段共享都没动。
  新顶点工厂 `FCSGpuInstancedIndexedVertexFactory` 画的是引擎 `LocalVertexFactory.ush` 的打补丁副本（脚本生成，引擎升级后要重新生成）。
- 开关 `r.CSGpuInstanced.VisibleSourceIndex`：1（默认）新布局，0 旧布局留作对照；运行中可切。
- 结果（16 个场景 × 5 个机位 × 3 对交错 A/B）：常驻显存 −35% ～ −95%（100 万个实例 4 级 LOD 619.8 → 139.2 MiB，GPU 源 83.9 → 3.8 MiB）；
  剔除 pass −15% ～ −26%；整帧持平或略好（100 万全可见 −0.13 ms），没有超出噪声的回退；高顶点数网格没有量到间接读取的代价。
- 验收：`VisibleSourceIndex.Picture` / `.Attributes`（深度图与 BaseColor 逐像素、逐实例对；三种实例源；custom data 的顶点与像素两条路径），
  故意改错着色器五处全部被抓住；全套自动化两种布局各一遍。
- 新约束：一个视图族的渲染期间生产者不许改写 packed 行（写在 `FCSGpuInstanceSourceGPU` 的注释里，现有生产者都满足）。
- §5.3 紧凑 LOD 区段不做：索引缓冲只剩常驻流的 16%，再压只省十几 MiB。

### 11.3 第三阶段：合批、派发范围、Nanite 路的改动通知（2026-10-02 ～ 10-03，已完成）

详见 [Docs/InstanceCulling/Step3_20261002.md](Docs/InstanceCulling/Step3_20261002.md) §1–§3。

- **§6.1**：`BeginInstanceUpdate` / `EndInstanceUpdate`（`FCSGpuInstancedUpdateScope`）与 `RemoveInstances`；
  `bMarkRenderStateDirty=false` 不再当场重排；Morton 重排换成基数排序。10 万个实例：连改 8 个 82.5 → 7.3 ms，
  区间里 24 个混合改动 7.4 ms（逐个调约 310 ms），单次增删 12.6–13.1 → 7.2–7.9 ms。结果与逐个调用逐位相同。验收 `GpuInstancedMesh.BatchUpdate`。
- **§6.2**：CPU 源的 InstanceCull 按行数派发（以前按 1.5 倍棘轮的容量）。GPU 源的间接派发不做（收益上限 0.06–0.08 ms，要多一个 pass）。
- **§7**：GPU 实例源可以逐源承诺"改了就通知"（`FCSGpuInstanceSourceGPU::bNotifiesChanges` + `NotifyInstanceSourceChanged()`），
  承诺了的源 Nanite 路只在被通知、图元动了、代理重建或距离带视点挪动时重写 GPU-Scene。地被与灌木已接入（经 `CSShaperSteps::HandOverInstanceSources`），
  石阶、摆件与房子那几家没接。开关 `r.CSGpuInstanced.Nanite.SkipUnchangedWrites`（默认 1；0 = 不信承诺、每帧重写）。
  100 万槽静止：整帧 7.82 → 6.02 ms（VSM 不再每帧作废，与 CPU 源打平）；真实地图 `L_HouseGroundDemo` 整帧 −0.3 ms。验收 `GpuInstancedMesh.NaniteNotify`。
- 没做：局部范围写入（整批跳过之后剩下的写入都是真改动，没有量到值得拆的场景）、点云打包复用同一机制（每帧重打包 0.012 ms）。

### 11.4 第四阶段：每组件固定成本（2026-10-03，做了第一刀）

详见 [Docs/InstanceCulling/Step3_20261002.md](Docs/InstanceCulling/Step3_20261002.md) §4、§5。

- 剔除借图编辑只登记用到的常驻流（`FCSMeshRenderThreadEdit` 的流位图）、CPU 源的实例数走 uniform（每代理每帧 5 个 pass → 4 个）、
  只剔本视图族所在场景的代理。开关 `r.CSGpuInstanced.LeanCullSetup`（位标志，默认 3，0 = 旧做法）。
- 256 个组件：整帧 −0.56 ～ −0.70 ms（−5% ～ −7%），GPU −0.15 ～ −0.22 ms；单组件场景持平或略好。
- 试过把计数器清零并进 BuildArgs：与"实例数走 uniform"同开时 GPU 反而变慢，已撤掉（消融数据在结果文档）。
- 没做：真正的跨组件合批（常驻流所有权的重构）、可见簇工作列表、聚合原子计数、HZB —— 剔除 pass 自己 100 万个实例才零点几毫秒，
  没有量到值得的场景。经典路的阴影成本（基线发现 1）不在方案里，需要单独立项。

**开关一览**

```text
r.CSGpuInstanced.VisibleSourceIndex          1   # 0 = 可见缓冲抄完整属性（88 B / 槽）
r.CSGpuInstanced.LeanCullSetup               3   # 位 0 只登记用到的流，位 1 CPU 源实例数走 uniform；0 = 旧做法
r.CSGpuInstanced.Nanite.SkipUnchangedWrites  1   # 0 = 承诺了通知的 GPU 源也每帧重写
```

### 11.5 累计结果与遗留（2026-10-03）

三个开关全关对全开的交错 A/B（3 对；详见 [Step3_20261002.md](Docs/InstanceCulling/Step3_20261002.md) §6）：

| 场景 | 整帧（ms） | 常驻流（MiB） |
| --- | --- | --- |
| 10 万个实例拆成 256 个组件（经典路） | 11.48 → 10.39（−9%） | 24.8 → 12.8 |
| 100 万个实例（经典路，CPU 源 / GPU 源） | 17.88 → 17.79 / 17.85 → 17.74 | 240.8 → 120.6 / 83.9 → 3.8 |
| 100 万个实例 4 级 LOD（经典路） | 19.75 → 19.72；阴影最重的机位 39.49 → 39.59（+0.3%） | 619.8 → 139.2 |
| Nanite 路，GPU 源 100 万槽，数据静止 | 7.83 → 6.08（−22%） | — |
| 真实地图 `L_HouseGroundDemo`（32 个实例组件，150 万个实例） | 11.70 → 11.39（−3%） | 40.0 → 2.5 |
| CPU 实例数组连改 8 个（10 万个实例，游戏线程） | 82.5 → 7.3 | — |

自动化 `PCGPlugins` 全套 244 条（实例路 16 条，本轮新增 `StableIdentity`、`BatchUpdate`、`VisibleSourceIndex.Picture` / `.Attributes`、`NaniteNotify`）：
默认配置与旧开关配置各跑一遍，红的只有四条既存红（浅水求解器两条、藤蔓两条），与实例路无关。

遗留，按影响排：

1. **经典路的阴影**（方案里没有）：开投影时的大头，100 万个实例 `edge` 机位 20.6 ms / 34.8 ms，这一轮没有碰。
   方向有三个 —— 让经典路进 VSM、给回退级联做自己的剔除、或把投影交给 Nanite 路的替身（进 VSM，有缓存）—— 需要先定方向。
2. **房子那几家生产者还没接"改了就通知"**（门框砖、柱、瓦、藤、摆件，以及石阶 / 石子）：实例数小，收益也小；接之前要逐条核它们写 buffer 的路径。
3. **真正的跨组件合批**：256 个组件仍是引擎 ISM 的两倍（10.4 对 5.4 ms）。组件数到这个量级的内容出现之前不值得做。
4. **两个顶点工厂并存**：实例化材质多编一套着色器。旧布局不再需要对照时退役旧工厂。
