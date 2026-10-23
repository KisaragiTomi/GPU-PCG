# -*- coding: utf-8 -*-
"""生成 Shaders/Private/CSGpuInstancedIndexedVertexFactory.ush。

    python GenerateGpuInstancedVertexFactoryShader.py [--engine <引擎根目录>] [--check]

那个文件是引擎 `/Engine/Private/LocalVertexFactory.ush` 的一份**打过补丁的副本**，给
`FCSGpuInstancedIndexedVertexFactory` 用（InstanceCullingOptimization.md §5）：剔除 pass 只往可见槽里写 4 字节的
源行号，顶点着色器经这个行号直接读源实例行（`CSInstanceRows`），不再读一份逐帧抄出来的可见属性缓冲。

为什么非得抄一份、不能 `#include` 引擎那份再改几个宏：引擎的取数写死成
`InstanceVF.VertexFetch_InstanceTransformBuffer[3 * i + k]` 与 `...OriginBuffer[i]`，两条下标的步长是 3 和 1；
本插件的源行是 5 个 float4 一行的 AoS（九条打包路 + 剔除 + GPU-Scene 写入共用的契约，改不得）。3i+k 与 5s+k 没有
公共的线性映射，换视图、换偏移、换宏都对不上，只能把取数那一个函数换掉。

所以补丁只有六处，每一处都**断言恰好命中一次**：引擎升级后那几段文字变了，这里当场报错，不会悄悄生成一份
半新半旧的着色器。生成物的文件头记着源文件的 SHA-1（换行归一化后），C++ 侧启动时拿它对引擎里实际的那份
（FCSGpuInstancedIndexedVertexFactory::VerifyShaderSourceUpToDate），对不上就报错提醒重新生成。

`--check`：不写文件，只报告现有生成物是不是最新（退出码 1 = 需要重新生成）。
"""
import argparse
import hashlib
import io
import json
import os
import sys

PLUGIN_ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUTPUT = os.path.join(PLUGIN_ROOT, "Shaders", "Private", "CSGpuInstancedIndexedVertexFactory.ush")
SOURCE_RELATIVE = os.path.join("Engine", "Shaders", "Private", "LocalVertexFactory.ush")

BANNER = """// ============================================================================================
// ⚠️ 生成的文件，不要手改：Scripts/GenerateGpuInstancedVertexFactoryShader.py
//
// 引擎 /Engine/Private/LocalVertexFactory.ush 的打补丁副本，FCSGpuInstancedIndexedVertexFactory 用。
// 改动只有标着 [PCGPlugins] 的几处：实例属性经剔除 pass 压出来的**可见源行号**直接从源实例行里取
// （InstanceCullingOptimization.md §5），其余逐字与引擎相同。
//
// SourceEngineVersion: {version}
// SourceSHA1: {sha1}
// ============================================================================================

"""

DECLARATIONS = """#if USE_INSTANCING
float4 InstancingOffset;
uint InstanceOffset;

// [PCGPlugins] 可见槽 → 源行号 → 源实例行。InstanceOffset 是这一级 LOD 在可见源行号缓冲里的区段起点。
#include "/Plugin/PCGPlugins/Shaders/Private/CSGpuSharedLayout.ush"
Buffer<float4> CSInstanceRows;          // 每个源行 CS_GPU_INSTANCED_ROW_FLOAT4S 个 float4（FCSGpuInstanceSourceGPU 的布局）
Buffer<uint>   CSVisibleSourceIndices;  // 每个可见槽一个源行号（InstanceCullCS 写）
// 生产者没给 custom data 时为 0：custom data 的下标恒取第 0 行，读到的是一行零（与旧路径"可见槽写零"同一口径）。
uint           CSCustomDataIndexMask;

uint CSResolveSourceIndex(uint DrawInstanceId)
{
	return CSVisibleSourceIndices[DrawInstanceId + InstanceOffset];
}
#endif
"""

FETCH = """FVertexFactoryInstanceInput ManualFetchInstanceInput(uint InstanceId)
{
	FVertexFactoryInstanceInput Result = (FVertexFactoryInstanceInput)0;
	// [PCGPlugins] 行向量约定与引擎相同：[0..2] 是实例→组件 3x3 的行，[3] 是原点 + 逐实例随机（.w）。
	const uint CSRow = CSResolveSourceIndex(InstanceId) * CS_GPU_INSTANCED_ROW_FLOAT4S;
	Result.Transform1 = CSInstanceRows[CSRow + 0];
	Result.Transform2 = CSInstanceRows[CSRow + 1];
	Result.Transform3 = CSInstanceRows[CSRow + 2];
	Result.Origin = CSInstanceRows[CSRow + 3];

	return Result;
}
"""

# (说明, 原文, 替换)。原文必须在引擎文件里恰好出现一次。
PATCHES = [
    ("相对 include 改成引擎虚拟路径（文件搬到插件目录之后相对路径不成立）",
     """#include "VertexFactoryCommon.ush"
#include "LocalVertexFactoryCommon.ush"
#include "LightmapData.ush"
#include "GpuSkinCommon.ush"
#include "SplineMeshCommon.ush"
""",
     """#include "/Engine/Private/VertexFactoryCommon.ush"
#include "/Engine/Private/LocalVertexFactoryCommon.ush"
#include "/Engine/Private/LightmapData.ush"
#include "/Engine/Private/GpuSkinCommon.ush"
#include "/Engine/Private/SplineMeshCommon.ush"
"""),
    ("末尾的默认接口 include",
     """#include "VertexFactoryDefaultInterface.ush"
""",
     """#include "/Engine/Private/VertexFactoryDefaultInterface.ush"
"""),
    ("声明源行与可见源行号两条缓冲",
     """#if USE_INSTANCING
float4 InstancingOffset;
uint InstanceOffset;
#endif
""",
     DECLARATIONS),
    ("实例变换与原点：经源行号读源实例行",
     """FVertexFactoryInstanceInput ManualFetchInstanceInput(uint InstanceId)
{
	FVertexFactoryInstanceInput Result = (FVertexFactoryInstanceInput)0;
	Result.Transform1 = InstanceVF.VertexFetch_InstanceTransformBuffer[3 * (InstanceId + InstanceOffset) + 0];
	Result.Transform2 = InstanceVF.VertexFetch_InstanceTransformBuffer[3 * (InstanceId + InstanceOffset) + 1];
	Result.Transform3 = InstanceVF.VertexFetch_InstanceTransformBuffer[3 * (InstanceId + InstanceOffset) + 2];
	Result.Origin = InstanceVF.VertexFetch_InstanceOriginBuffer[(InstanceId + InstanceOffset)];

	return Result;
}
""",
     FETCH),
    ("顶点着色器里 custom data 的下标（MaterialTemplate 按 InstanceId + InstanceOffset 取）：换成源行号",
     """	Result.InstanceId = GetInstanceId(GetInstanceIdFromVF(Input));
	Result.InstanceOffset = InstanceOffset;
""",
     """	// [PCGPlugins] custom data 按**源行号**取：MaterialTemplate 用的是 InstanceId + InstanceOffset。
	Result.InstanceId = CSResolveSourceIndex(GetInstanceId(GetInstanceIdFromVF(Input))) & CSCustomDataIndexMask;
	Result.InstanceOffset = 0;
"""),
    ("光照图偏移：这条路没有烘焙光照，恒零（旧路径是抄一整条零缓冲）",
     """	Intermediates.InstanceLightMapAndShadowMapUVBias = InstanceVF.VertexFetch_InstanceLightmapBuffer[(InstanceId + InstanceOffset)];
""",
     """	// [PCGPlugins] 这条路没有烘焙光照：恒零，不为它占一条缓冲。
	Intermediates.InstanceLightMapAndShadowMapUVBias = float4(0.0f, 0.0f, 0.0f, 0.0f);
"""),
    ("像素着色器里 custom data 的下标（经 PerInstanceParams.w 插值过去）：换成源行号",
     """			Intermediates.PerInstanceParams.w = asfloat(GetInstanceId(GetInstanceIdFromVF(Input)) + InstanceOffset);
""",
     """			// [PCGPlugins] 同上，按源行号取。
			Intermediates.PerInstanceParams.w = asfloat(CSResolveSourceIndex(GetInstanceId(GetInstanceIdFromVF(Input))) & CSCustomDataIndexMask);
"""),
]


def normalized_sha1(text):
    return hashlib.sha1(text.replace("\r\n", "\n").encode("utf-8")).hexdigest()


def engine_version(engine_root):
    path = os.path.join(engine_root, "Engine", "Build", "Build.version")
    try:
        with io.open(path, encoding="utf-8") as handle:
            data = json.load(handle)
        return "%s.%s.%s" % (data.get("MajorVersion"), data.get("MinorVersion"), data.get("PatchVersion"))
    except (IOError, ValueError):
        return "unknown"


def generate(engine_root):
    source_path = os.path.join(engine_root, SOURCE_RELATIVE)
    # utf-8-sig：有 BOM 就去掉 —— C++ 那边的 LoadFileToString 也会去，两边算的才是同一串字节。
    with io.open(source_path, encoding="utf-8-sig") as handle:
        source = handle.read().replace("\r\n", "\n")

    text = source
    for description, old, new in PATCHES:
        count = text.count(old)
        if count != 1:
            raise SystemExit("补丁没有恰好命中一次（命中 %d 次）：%s\n引擎的 LocalVertexFactory.ush 变了，对着新文本改这个脚本里的补丁。" % (count, description))
        text = text.replace(old, new)

    banner = BANNER.format(version=engine_version(engine_root), sha1=normalized_sha1(source))
    return banner + text


def main():
    # --help 是 parse_args 里打印的：先把输出切成 UTF-8，否则中文说明在 GBK 控制台下是乱码。
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--engine", default=os.environ.get("UE_ROOT", ""))
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    if not args.engine or not os.path.isfile(os.path.join(args.engine, SOURCE_RELATIVE)):
        raise SystemExit("找不到引擎的 LocalVertexFactory.ush：设 UE_ROOT 或传 --engine <引擎根目录>。")

    generated = generate(args.engine)
    existing = None
    if os.path.isfile(OUTPUT):
        with io.open(OUTPUT, encoding="utf-8") as handle:
            existing = handle.read().replace("\r\n", "\n")

    if args.check:
        if existing == generated:
            print("最新：%s" % OUTPUT)
            return
        print("需要重新生成：%s" % OUTPUT)
        raise SystemExit(1)

    if existing == generated:
        print("没有变化：%s" % OUTPUT)
        return
    with io.open(OUTPUT, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(generated)
    print("已生成：%s（%d 行）" % (OUTPUT, generated.count("\n")))


if __name__ == "__main__":
    main()
