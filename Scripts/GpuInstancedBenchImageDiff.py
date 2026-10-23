# -*- coding: utf-8 -*-
"""交错 A/B 跑测（RunGpuInstancedBench.ps1 -Variants ... -Pairs 2 以上）的画面一致性。

    python GpuInstancedBenchImageDiff.py <跑测目录> [--base <变体名>] [--threshold 8] [--margin 0.002] [--factor 2.0]

多个变体时，跑测在固定机位的头两对里每一步都留一张 `<场景>__<机位>__<变体>__p<对>.png`。这里对每个场景每个机位算两样：

    噪声底   同一个变体两次拍的差（p0 对 p1）—— 开关没动，画面本该一样，差出来的全是时域噪声
    信号     同一对里两个变体的差

每个差用"坏像素占比"表示：三个通道里最大的差超过 threshold（0–255）的像素占多少。
判据：信号 > factor × 噪声底 + margin 才算不一致（退出码 1）。

为什么要噪声底：TSR 的时域累积、Lumen 的噪声、自动曝光都不会在两次量测之间停在同一个状态上。实测不受开关影响的
ISM / Nanite 场景，平视机位地平线那一带（亚像素的远处实例）也有 1–2% 的像素差出 8 个灰阶以上。只比一对分不出
"换了布局画错了"和"本来就在抖"。

所以这是**粗筛**：抓得住"少画了一批 / 整片错位 / 阴影没了"，抓不住"千分之几的实例错了位"。后者归自动化测试管
（PCGPlugins.ComputeShaderGenerator.GpuInstancedMesh.VisibleSourceIndex*：无时域效果的场景捕获，逐像素比）。
"""
import argparse
import glob
import os
import sys

from PIL import Image, ImageChops


def bad_fraction(path_a, path_b, threshold):
    """(坏像素占比, 最大差, 均值差)；尺寸不同返回 None。"""
    a = Image.open(path_a).convert("RGB")
    b = Image.open(path_b).convert("RGB")
    if a.size != b.size:
        return None
    channels = ImageChops.difference(a, b).split()
    diff = ImageChops.lighter(ImageChops.lighter(channels[0], channels[1]), channels[2])
    histogram = diff.histogram()
    total = float(a.size[0] * a.size[1])
    maximum = max(i for i, count in enumerate(histogram) if count)
    mean = sum(i * count for i, count in enumerate(histogram)) / total
    return sum(histogram[threshold + 1:]) / total, maximum, mean


def main():
    # --help 是 parse_args 里打印的：先把输出切成 UTF-8，否则中文说明在 GBK 控制台下是乱码。
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("directory")
    parser.add_argument("--base", default="")
    parser.add_argument("--threshold", type=int, default=8)
    parser.add_argument("--margin", type=float, default=0.002)
    parser.add_argument("--factor", type=float, default=2.0)
    args = parser.parse_args()

    # shots[(场景, 机位)][变体][对] = 路径
    shots = {}
    for path in glob.glob(os.path.join(args.directory, "*__*__*__p*.png")):
        scenario, pose, variant, pair = os.path.basename(path)[:-4].split("__")
        shots.setdefault((scenario, pose), {}).setdefault(variant, {})[int(pair[1:])] = path

    failed = 0
    compared = 0
    unjudged = 0
    print("%-34s %-7s %-10s %10s %10s %8s %8s" % ("场景", "机位", "变体", "信号", "噪声底", "最大差", "均值差"))
    for (scenario, pose), by_variant in sorted(shots.items()):
        if len(by_variant) < 2:
            continue
        base = args.base if args.base in by_variant else sorted(by_variant)[0]

        # 噪声底：各变体自己 p0 对 p1，取最大的。
        noise = None
        for pairs in by_variant.values():
            if 0 in pairs and 1 in pairs:
                result = bad_fraction(pairs[0], pairs[1], args.threshold)
                if result:
                    noise = max(noise or 0.0, result[0])

        for variant, pairs in sorted(by_variant.items()):
            if variant == base:
                continue
            # 信号：同一对里 变体 对 基准，取各对里**最小**的 —— 真画错了每一对都错，噪声才时有时无。
            signal = None
            for pair, path in pairs.items():
                if pair not in by_variant[base]:
                    continue
                result = bad_fraction(by_variant[base][pair], path, args.threshold)
                if result is None:
                    print("%-34s %-7s %-10s 尺寸不同" % (scenario, pose, variant))
                    failed += 1
                    continue
                if signal is None or result[0] < signal[0]:
                    signal = result
            if signal is None:
                continue
            compared += 1
            if noise is None:
                unjudged += 1
                verdict = "  （没有第二对，判不了）"
            elif signal[0] > args.factor * noise + args.margin:
                failed += 1
                verdict = "  ← 不一致"
            else:
                verdict = ""
            print("%-34s %-7s %-10s %9.3f%% %10s %8d %8.3f%s" % (
                scenario, pose, variant, signal[0] * 100.0,
                ("%.3f%%" % (noise * 100.0)) if noise is not None else "—", signal[1], signal[2], verdict))

    print("比了 %d 组，%d 组不一致，%d 组没有噪声底（灰阶阈值 %d / 255，判据：信号 > %.1f × 噪声底 + %.2f%%）" % (
        compared, failed, unjudged, args.threshold, args.factor, args.margin * 100.0))
    if failed or not compared or unjudged:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
