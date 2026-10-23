# -*- coding: utf-8 -*-
"""`TinyGladeShotSoftening.py` 出的图的**可量化判据**。编辑器外跑（PIL + numpy）。

为什么不在编辑器里算：`RenderingLibrary` 只能逐点读回渲染目标，144 万个点太慢；
而 PNG 已经落盘，离线算既快又能复算 —— 判据必须是**任何人拿着那两张 png 就能复现**的数，
不是"这次跑出来的一个值"。

判据（对位 `Docs/TinyGlade/Progress_ModuleComparison.md（卷四）` §5 归因表）：

  · `zero%`      **精确 `(0,0,0)`** 的像素占比，本文件里**唯一会报红**的一条（见下面的门限）。
                 它与 `dark%` 不是同一件事：`dark%` 量"暗"，`zero%` 量"这个像素一点光都没收到"。
                 精确零不是观感问题而是**故障指纹** —— 任何曝光/AO/天光旋钮都抬不动它
                 （零乘什么都是零），所以它一旦不为零，先查渲染路径，别去调参。
                 已知会把它顶起来的那条（2026-08-30 实测定位，修法在
                 `TinyGladeShotSoftening.py` 的坑 ⑨）：离屏 capture 有了 ViewState + GI 被翻回
                 Lumen 之后，**单帧抓拍时 Lumen 的最终聚集没有帧间历史 ⇒ 间接光恒为零**，
                 于是整张图里凡是没被太阳直射的 lit 表面全部落在精确 (0,0,0)。
                 那一轮的现场：岩壳板缝里透出来的地面是纯黑楔形，backlit 机位 14.60%；
                 而岩壳自己不黑只是因为它的材质是 `MSM_Unlit`、根本不参与光照。
  · `dark%`      luma < 0.06 的像素占比。TG 的截图里**从来没有近黑区域**（§5 第 2 条），
                 这是"AO 压不黑"最直接的像素表述。
  · `p01..p50`   亮度分位数。抬的是**下半段**才算对位 —— 整体加亮（提曝光）会把 p50 也拉走，
                 只看均值分不出这两件事。
  · `blown%`     luma > 0.98 的像素占比。曝光是往上调的，必须同时盯着高光有没有糊死。
  · `dark_chroma` 最暗 20% 像素的平均饱和度 (max−min)/max。对位 §3.4：TG 的阴影区**有颜色**
                 （每个 RGB 通道各带一条 L1 方向 ⇒ 背光面吃到地面草色），不是一片中性灰/蓝。
  · `was_dark→`  用**改前**那张的近黑掩膜（luma < 0.10）去量**改后**同一批像素的亮度。
                 这一条最硬：它问的不是"整张图亮了吗"，而是"原来黑掉的那些像素现在多亮"。
                 ⚠️ 只对几何未变的机位成立（`backlit`/`shadow`/`wide`）；石阶那两张埋深改了、
                 几何跟着变，那里的 `was_dark→` 只作参考。

用法::

    python TinyGladeShotSofteningStats.py <before-tag> <after-tag>
    python TinyGladeShotSofteningStats.py before after
"""
import os
import sys

import numpy as np
from PIL import Image

SHOTS = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__))))), "Saved", "TinyGladeShots")
VIEWS = ["backlit", "shadow", "tread", "toe", "wide"]
# 几何在本轮会变的机位（石阶埋深）—— 它们的跨图掩膜判据只作参考，标出来免得被当成硬证据。
GEOMETRY_CHANGED = {"tread", "toe"}

DARK = 0.06      # "近黑"：8 bit 下约 15/255，肉眼已经读不出细节
MASK = 0.10      # 跨图掩膜的阈；比 DARK 松一档，样本才够多
BLOWN = 0.95
# `zero%` 的报红门限。**0.5% 不是"够暗就行"的宽容度，是给抗锯齿留的余量** ——
# 天空与地平线交界处偶尔会有几个像素被 TAA 压到 0。真故障时这个数是两位数
# （实测 backlit 14.60%、关掉岩壳 41.56%），而修好之后是 0.000%，中间差两个数量级，
# 门限落在哪里都不影响判定。
ZERO_FAIL = 0.5
# ⚠️ **必须裁掉天空**：`backlit` / `toe` / `wide` 里天空占到四成，而天空的明暗由 SkyAtmosphere
#    与曝光决定，不是本轮要判的"阴影区压不压得黑"。不裁的话 `dark%` 量的一大半是天空，
#    改一下曝光这个数就整片走，判据当场失去意义。这三个机位的天空全在上方 45% 以内。
SKY_CROP = 0.45


def load(tag, view):
    path = os.path.join(SHOTS, "soft_%s_%s.png" % (tag, view))
    if not os.path.exists(path):
        return None
    img = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0
    return img[int(SKY_CROP * img.shape[0]):, :, :]


def luma(img):
    """Rec.709 亮度，直接在 sRGB 编码值上算。

    ⚠️ 故意**不**先解 sRGB：判据要跟"看到的暗"对齐，而人眼对 png 里的编码值近似线性；
    解到线性域再取分位数会把所有暗部挤到 0 附近，两张图的差别反而看不出来。
    """
    return 0.2126 * img[..., 0] + 0.7152 * img[..., 1] + 0.0722 * img[..., 2]


def chroma(img):
    m = img.max(axis=-1)
    return np.where(m > 1e-4, (m - img.min(axis=-1)) / np.maximum(m, 1e-4), 0.0)


def row(tag, view, mask=None):
    img = load(tag, view)
    if img is None:
        return None, None
    y = luma(img)
    c = chroma(img)
    dark_sel = y <= np.quantile(y, 0.20)
    out = {
        # 精确零要在**原始 8 bit 整数**上判，不能用 luma < eps 代替：Rec.709 的加权和会让
        # (1,0,0) 这种"其实收到了光"的像素也落进阈内，判据就从"故障指纹"退化成"够暗"。
        "zero%": 100.0 * float((np.rint(img * 255.0).astype(np.int32).sum(axis=-1) == 0).mean()),
        "dark%": 100.0 * float((y < DARK).mean()),
        "blown%": 100.0 * float((y > BLOWN).mean()),
        "p01": float(np.quantile(y, 0.01)),
        "p05": float(np.quantile(y, 0.05)),
        "p10": float(np.quantile(y, 0.10)),
        "p25": float(np.quantile(y, 0.25)),
        "p50": float(np.quantile(y, 0.50)),
        "dark_chroma": float(c[dark_sel].mean()),
    }
    if mask is not None and mask.shape == y.shape:
        out["was_dark_mean"] = float(y[mask].mean())
        out["was_dark_p50"] = float(np.quantile(y[mask], 0.50)) if mask.any() else 0.0
    return out, (y < MASK)


def main():
    before_tag = sys.argv[1] if len(sys.argv) > 1 else "before"
    after_tag = sys.argv[2] if len(sys.argv) > 2 else "after"
    print("shots dir: %s" % SHOTS)
    print("%-9s %-7s %7s %7s %7s %7s %7s %7s %7s %7s %8s %9s"
          % ("view", "tag", "zero%", "dark%", "blown%", "p01", "p05", "p10", "p25", "p50",
             "dkChroma", "wasDark->"))
    verdict = []          # (view, tag, zero%)：只收改后那一份，报红看它
    for view in VIEWS:
        b, mask = row(before_tag, view)
        if b is None:
            print("%-9s  (missing soft_%s_%s.png)" % (view, before_tag, view))
            continue
        a, _ = row(after_tag, view, mask)
        for tag, s in ((before_tag, b), (after_tag, a)):
            if s is None:
                print("%-9s %-7s  (missing)" % (view, tag))
                continue
            print("%-9s %-7s %7.3f %7.2f %7.2f %7.3f %7.3f %7.3f %7.3f %7.3f %8.3f %9s"
                  % (view, tag, s["zero%"], s["dark%"], s["blown%"], s["p01"], s["p05"], s["p10"],
                     s["p25"], s["p50"], s["dark_chroma"],
                     ("%.3f" % s["was_dark_mean"]) if "was_dark_mean" in s else "-"))
        if a:
            note = "  (几何已变，仅作参考)" if view in GEOMETRY_CHANGED else ""
            print("%-9s %-7s %+7.3f %+7.2f %+7.2f %+7.3f %+7.3f %+7.3f %+7.3f %+7.3f %+8.3f%s"
                  % (view, "delta", a["zero%"] - b["zero%"], a["dark%"] - b["dark%"],
                     a["blown%"] - b["blown%"],
                     a["p01"] - b["p01"], a["p05"] - b["p05"], a["p10"] - b["p10"],
                     a["p25"] - b["p25"], a["p50"] - b["p50"],
                     a["dark_chroma"] - b["dark_chroma"], note))
            verdict.append((view, after_tag, a["zero%"]))
        print("")

    # 唯一会报红的一条。判的是**改后**那一组 —— 改前那组本来就该是红的，它是被修的对象。
    bad = [(v, z) for v, _, z in verdict if z >= ZERO_FAIL]
    for view, _, z in verdict:
        print("[%s] exact-zero %-9s %s = %.3f%% (门限 < %.1f%%)"
              % ("FAIL" if z >= ZERO_FAIL else "PASS", view, after_tag, z, ZERO_FAIL))
    if bad:
        print("SOFTSTATS FAILED: %d 个机位有精确 (0,0,0) 像素 —— 先查渲染路径，别调参"
              % len(bad))
        return 1
    print("SOFTSTATS OK: %d 个机位精确零都在门限内" % len(verdict))
    return 0


if __name__ == "__main__":
    sys.exit(main())
