# -*- coding: utf-8 -*-
"""实例路跑测（RunGpuInstancedBench.ps1 → summary.json）的报表。

    python GpuInstancedBenchReport.py <summary.json>                    # 一次跑测的总表
    python GpuInstancedBenchReport.py <summary.json> --ab               # 同一次跑测里交错 A/B 的配对差
    python GpuInstancedBenchReport.py <before.json> --compare <after.json>   # 两次跑测逐场景逐机位的差
    ... --md <out.md>                                                   # 同时落一份 Markdown

口径（与 CSGpuInstancedBench.cpp 文件头一致）：每格是那个机位采集帧的**中位数**，毫秒。
    Frame / GT / RT / GPU   CsvProfile 的 FrameTime / GameThreadTime / RenderThreadTime / GPUTime
    剔除                    GPU/CSGpuInstanced{PackPoints,ClusterCull,InstanceCull,BuildArgs} 之和（经典路）
    写入                    GPU/CSGpuInstancedNaniteWrite（Nanite 路的 GPU-Scene 写入）
    预通道 / 基通道 / 阴影   引擎的 GPU/Prepass、GPU/BasePass + GPU/NaniteBasePass、GPU/ShadowDepths
    Nanite                  GPU/NaniteVisBuffer
A/B 的判据是**各对同号**，不是单次读数：n 对里有几对的差与平均差同号，写在"同号"一列。
"""
import argparse
import io
import json
import statistics
import sys

CULL_KEYS = ("GPU/CSGpuInstancedPackPoints", "GPU/CSGpuInstancedClusterCull", "GPU/CSGpuInstancedInstanceCull", "GPU/CSGpuInstancedBuildArgs")


def stat(stats, key, field="med"):
    entry = stats.get(key)
    return entry[field] if entry else 0.0


def derived(stats, field="med"):
    """把一堆列折成报表用的那几个数。"""
    return {
        "Frame": stat(stats, "FrameTime", field),
        "GT": stat(stats, "GameThreadTime", field),
        "RT": stat(stats, "RenderThreadTime", field),
        "GPU": stat(stats, "GPUTime", field),
        "Cull": sum(stat(stats, key, field) for key in CULL_KEYS),
        "Write": stat(stats, "GPU/CSGpuInstancedNaniteWrite", field),
        "Prepass": stat(stats, "GPU/Prepass", field),
        "Base": stat(stats, "GPU/BasePass", field) + stat(stats, "GPU/NaniteBasePass", field),
        "Shadow": stat(stats, "GPU/ShadowDepths", field),
        "Nanite": stat(stats, "GPU/NaniteVisBuffer", field),
        "SceneUpd": stat(stats, "GPU/GPUSceneUpdate", field),
        "CullSetupCPU": stat(stats, "CSGpuInstanced/CullSetupMs", field),
    }


COLUMNS = ("Frame", "GT", "RT", "GPU", "Cull", "Write", "Prepass", "Base", "Shadow", "Nanite", "SceneUpd", "CullSetupCPU")
HEADERS = {
    "Frame": "Frame", "GT": "GT", "RT": "RT", "GPU": "GPU", "Cull": "剔除", "Write": "写入", "Prepass": "预通道",
    "Base": "基通道", "Shadow": "阴影", "Nanite": "Nanite", "SceneUpd": "场景更新", "CullSetupCPU": "建图(CPU)",
}


def mib(value):
    return "%.1f" % (value / (1024.0 * 1024.0))


def visible_text(measurement, live):
    per_lod = measurement.get("visible_per_lod") or []
    if not per_lod:
        return "—"
    total = sum(per_lod)
    share = (" %d%%" % round(100.0 * total / live)) if live else ""
    if len(per_lod) == 1:
        return "%d%s" % (total, share)
    return "%d%s (%s)" % (total, share, "/".join(str(v) for v in per_lod))


def table(headers, rows):
    lines = ["| " + " | ".join(headers) + " |", "| " + " | ".join("---" for _ in headers) + " |"]
    lines.extend("| " + " | ".join(row) + " |" for row in rows)
    return "\n".join(lines)


def run_header(run):
    res = run.get("resolution", [0, 0])
    return "%s · %s %s · %s · %dx%d · 引擎 %s %s · %s · 每机位 %d 帧" % (
        run.get("suite"), run.get("gpu"), run.get("driver"), run.get("rhi"), res[0], res[1],
        run.get("engine"), run.get("build_config"), run.get("time"), run.get("measure_frames", 0))


def report_single(data):
    out = ["## 跑测：" + run_header(data["run"]), ""]
    cvars = data["run"].get("cvars", {})
    out.append("关键 CVar：" + "，".join("`%s=%s`" % (k, v) for k, v in cvars.items()))
    out.append("")

    # --- 场景信息 ---
    rows = []
    for s in data["scenarios"]:
        note = s["note"] if not s["spawned"] else (s["mismatch"] or "")
        rows.append([
            s["name"], s["route"], s["source"], str(s["live_instances"]), str(s["capacity"]), str(s["components"]),
            "%d / %d" % (s["base_vertices"], s["base_triangles"]), "%d / %d" % (s["lods"], s["sections"]),
            "是" if s["nanite_path"] else "否", mib(s["resident_bytes"]), mib(s["source_bytes"]), note])
    out.append("### 场景")
    out.append("")
    out.append(table(["场景", "路", "源", "活跃", "容量", "组件", "顶点 / 三角", "LOD / 段", "Nanite", "常驻流 MiB", "源缓冲 MiB", "备注"], rows))
    out.append("")

    # --- 逐机位 ---
    out.append("### 逐机位（中位数，ms）")
    out.append("")
    rows = []
    for s in data["scenarios"]:
        for m in s["measurements"]:
            d = derived(m["stats"])
            rows.append([s["name"], m["pose"], visible_text(m, s["live_instances"])] + ["%.3f" % d[c] for c in COLUMNS])
    out.append(table(["场景", "机位", "可见 (逐 LOD)"] + [HEADERS[c] for c in COLUMNS], rows))
    out.append("")

    # --- CPU 增删 ---
    if data.get("cpu_ops"):
        out.append("### CPU 实例数组的增删（游戏线程上一次调用，ms）")
        out.append("")
        rows = [[o["route"], str(o["instances"]), o["op"], str(o["calls"]), "%.2f" % o["mean_ms"], "%.2f" % o["max_ms"], "%.2f" % o["flushes_per_call"]] for o in data["cpu_ops"]]
        out.append(table(["路", "实例数", "操作", "次数", "平均", "最大", "阻塞刷新 / 次"], rows))
        out.append("")
    return "\n".join(out)


def report_ab(data):
    """同一次跑测里的交错 A/B：第一个变体当基线，其余逐个对它。"""
    variants = [v["name"] for v in data["run"].get("variants", [])]
    if len(variants) < 2:
        return "这次跑测只有一个变体，没有 A/B 可比。"
    base = variants[0]
    out = ["## 交错 A/B：" + run_header(data["run"]), ""]
    for v in data["run"]["variants"]:
        out.append("- `%s`：%s" % (v["name"], v["commands"] or "（不改任何 CVar）"))
    out.append("")
    out.append("每格 = 变体 − `%s` 的配对差的平均（ms）；括号里是各对里与平均差同号的对数。" % base)
    out.append("")

    # 常驻显存：变体换的是布局时，这才是它的主要差别。取每个变体第一次测量时记下的字节数。
    rows = []
    for s in data["scenarios"]:
        resident = {}
        for m in s["measurements"]:
            if m.get("resident_bytes") and m["variant"] not in resident:
                resident[m["variant"]] = m["resident_bytes"]
        if len(resident) < 2 or len(set(resident.values())) < 2:
            continue
        base_bytes = resident.get(base, 0)
        row = [s["name"], str(s["capacity"]), str(s["lods"])]
        for v in variants:
            b = resident.get(v, 0)
            pct = (" (%+.0f%%)" % (100.0 * (b - base_bytes) / base_bytes)) if v != base and base_bytes else ""
            row.append(mib(b) + pct)
        rows.append(row)
    if rows:
        out.append("### 常驻流（MiB）")
        out.append("")
        out.append(table(["场景", "容量", "LOD"] + ["`%s`" % v for v in variants], rows))
        out.append("")

    keys = ("Frame", "RT", "GPU", "Cull", "Write", "Prepass", "Base", "Shadow")
    for other in variants[1:]:
        rows = []
        for s in data["scenarios"]:
            poses = []
            for m in s["measurements"]:
                if m["pose"] not in poses:
                    poses.append(m["pose"])
            for pose in poses:
                by_pair = {}
                for m in s["measurements"]:
                    if m["pose"] == pose and m["variant"] in (base, other):
                        by_pair.setdefault(m["pair"], {})[m["variant"]] = derived(m["stats"])
                pairs = [p for p in by_pair.values() if base in p and other in p]
                if not pairs:
                    continue
                row = [s["name"], pose, str(len(pairs))]
                for key in keys:
                    diffs = [p[other][key] - p[base][key] for p in pairs]
                    mean = statistics.mean(diffs)
                    base_mean = statistics.mean(p[base][key] for p in pairs)
                    same = sum(1 for d in diffs if (d > 0) == (mean > 0) and d != 0)
                    pct = (" %+.0f%%" % (100.0 * mean / base_mean)) if base_mean > 1e-6 else ""
                    row.append("%+.3f%s (%d/%d)" % (mean, pct, same, len(pairs)))
                rows.append(row)
        out.append("### `%s` − `%s`" % (other, base))
        out.append("")
        out.append(table(["场景", "机位", "对数"] + [HEADERS[k] for k in keys], rows))
        out.append("")
    return "\n".join(out)


def report_compare(before, after):
    out = ["## 两次跑测的差（后 − 前）", "", "- 前：" + run_header(before["run"]), "- 后：" + run_header(after["run"]), ""]
    out.append("⚠️ 两次是**先后**跑的，不是交错的：机器状态的漂移也在差里。几个百分点以内的差别当真，要当真就用 `--ab` 的交错跑法。")
    out.append("")
    after_by_name = {s["name"]: s for s in after["scenarios"]}
    keys = ("Frame", "RT", "GPU", "Cull", "Write", "Prepass", "Base", "Shadow")
    rows = []
    mem_rows = []
    for s in before["scenarios"]:
        t = after_by_name.get(s["name"])
        if not t:
            continue
        if s["resident_bytes"] != t["resident_bytes"]:
            mem_rows.append([s["name"], mib(s["resident_bytes"]), mib(t["resident_bytes"]), "%+.1f" % ((t["resident_bytes"] - s["resident_bytes"]) / 1048576.0)])
        after_by_pose = {m["pose"]: m for m in t["measurements"]}
        for m in s["measurements"]:
            n = after_by_pose.get(m["pose"])
            if not n:
                continue
            a, b = derived(m["stats"]), derived(n["stats"])
            row = [s["name"], m["pose"]]
            for key in keys:
                pct = (" %+.0f%%" % (100.0 * (b[key] - a[key]) / a[key])) if a[key] > 1e-6 else ""
                row.append("%.3f → %.3f%s" % (a[key], b[key], pct))
            rows.append(row)
    out.append(table(["场景", "机位"] + [HEADERS[k] for k in keys], rows))
    out.append("")
    if mem_rows:
        out.append("### 常驻流字节数变了的场景（MiB）")
        out.append("")
        out.append(table(["场景", "前", "后", "差"], mem_rows))
        out.append("")
    return "\n".join(out)


def load(path):
    with io.open(path, encoding="utf-8") as handle:
        return json.load(handle)


def main():
    # --help 是 parse_args 里打印的：先把输出切成 UTF-8，否则中文说明在 GBK 控制台下是乱码。
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("summary")
    parser.add_argument("--ab", action="store_true")
    parser.add_argument("--compare")
    parser.add_argument("--md")
    args = parser.parse_args()

    data = load(args.summary)
    if args.compare:
        text = report_compare(data, load(args.compare))
    elif args.ab:
        text = report_ab(data)
    else:
        text = report_single(data)

    if args.md:
        with io.open(args.md, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
