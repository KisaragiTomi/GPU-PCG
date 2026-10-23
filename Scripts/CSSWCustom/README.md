# CSSW Custom 材质工具

已经生成的材质位于 `/PCGPlugins/ShallowWater/Material30/Custom`，当前副本实例继承完整展开的 `M_CSSW_Surface_Custom_Flat`。结果说明及截图位于 `Docs/CSSWCustomMaterial/README.md`。

以下脚本通过 Unreal Editor 的 Python 执行，工作数据写入项目 `Saved/CSSWCustomMaterial`。无需修改 C++ 或进行项目构建。

1. `prepare.py`：只读导出原母材质及依赖，调用 `parse_exports.py`、`gather.py`、`plan.py`，生成图数据和 Custom HLSL 转换计划。
2. `build.py`：复制母材质、19 个函数、5 个实例并保存。**目标已存在时直接拒绝覆盖**；当前项目已生成这些资产，不需要重跑。要另建一套，先修改脚本中的 `DEST`。
3. `flatten.py`：将初始 Custom 母材质中的全部函数递归展开，重建原生节点与 Custom，保留输入类型和默认值。仅修改内存中的母材质，校验完成后再保存；当前工程已经另存 `_Flat`。它拒绝再次处理已经展开的图。报告写入 `Saved/CSSWCustomMaterial/flatten/result.json`。
4. `validate.py`：读取初次构建及展开报告，检查表达式数量、材质设置、实例父链、有效参数和着色器统计；展开版还检查函数调用、函数 IO 及函数所有者引用全部为零。`audit_links.py` 仅适用于第一阶段保留函数结构的 Custom 版本，展开版不再使用原节点 ID。
5. `compare.py`：在当前编辑器中打开一个未保存的空白测试地图，创建同帧 A/B/A 场景捕获，等待日志出现 `CSSW_COMPARISON_DONE`。运行前应保存自己的关卡工作；推荐使用单独编辑器进程。脚本不保存测试关卡，也不修改源材质。
6. `variants.py`：在 `compare.py` 建立的同一进程中创建五组临时静态开关实例，等待 `CSSW_VARIANTS_READY`，然后执行下面的捕获代码。

```python
import builtins
for label, pair in builtins.cssw_variants.items():
    builtins.cssw_capture('variant-' + label, pair)
```

`measure.py` 使用普通 Python（需 Pillow 和 NumPy）读取 PNG，更新 `Docs/CSSWCustomMaterial` 中的数值指标与对比图。它依赖 Saved 下本次生成的检查 JSON；`source-integrity.json` 是此次任务开始修改副本后记录的原资产哈希校验结果，不由渲染脚本生成。

测试覆盖当前 UE 5.7.4 SM6 设置及指定参数组合；原版和副本使用相同时间、光照、相机、贴图和参数。`valid-*` 与开关测试的向量覆盖仅存在于临时实例，没有写回保存的实例。若重新编译母材质，应重新执行 `compare.py` 创建实例，避免沿用旧动态实例的渲染资源。

转换代码保留节点运算顺序，使用显式 float 维度；Power 使用引擎 `PositiveClampedPow`，Sine/Cosine 保留原 Period。设置材质函数引用后会重建该调用的输入，因此 `build.py` 显式恢复全部输入，而不仅是被替换的数值分支。
