# -*- coding: utf-8 -*-
"""给 NaniteCutHLODVideo.py 渲出的帧序列加中文字幕，用 ffmpeg 编成 mp4（系统 Python 跑，不进编辑器）。

    python NaniteCutHLODVideoEncode.py

读 Saved/NaniteCutHLODVideo/timeline.json（分镜区间 + 实测数字），逐帧画字幕后用管道喂给 ffmpeg，
不再落一份带字幕的 PNG。产物 Saved/NaniteCutHLODVideo/NaniteCutHLOD.mp4。
"""
import io
import json
import os
import shutil
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
OUT_DIR = os.path.join(ROOT, "Saved", "NaniteCutHLODVideo")
FRAME_DIR = os.path.join(OUT_DIR, "frames")
MP4 = os.path.join(OUT_DIR, "NaniteCutHLOD.mp4")

FONTS = [r"C:\Windows\Fonts\msyhbd.ttc", r"C:\Windows\Fonts\msyh.ttc", r"C:\Windows\Fonts\simhei.ttf"]
FADE = 8          # 字幕淡入淡出帧数
AB_SWITCH = 30    # 与渲染脚本里的交替周期一致


def font(size, bold=True):
    for path in (FONTS if bold else FONTS[1:]):
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def shot_at(shots, frame):
    for s in shots:
        if s["start"] <= frame < s["end"]:
            return s
    return shots[-1]


def alpha_for(shot, frame):
    into, left = frame - shot["start"], shot["end"] - frame
    return min(1.0, into / FADE if into < FADE else 1.0, left / FADE if left < FADE else 1.0)


def ab_label(shot, frame):
    """compare / closeup 两段里逐秒交替，规则与渲染脚本一致：偶数段是源。"""
    if shot["name"] not in ("compare", "closeup"):
        return None
    return "源物体" if ((frame - shot["start"]) // AB_SWITCH) % 2 == 0 else "烘焙 HLOD"


def draw(frame_img, shots, index, total, f_title, f_sub, f_chip, f_ab):
    shot = shot_at(shots, index)
    a = alpha_for(shot, index)
    img = frame_img.convert("RGB")
    w, h = img.size
    overlay = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(overlay)

    # 底部字幕条
    bar_h = 116
    d.rectangle([0, h - bar_h, w, h], fill=(8, 10, 14, int(170 * a)))
    d.line([(0, h - bar_h), (w, h - bar_h)], fill=(120, 190, 255, int(150 * a)), width=2)
    d.text((34, h - bar_h + 20), shot["caption"], font=f_title, fill=(255, 255, 255, int(255 * a)))
    if shot.get("sub"):
        d.text((34, h - bar_h + 64), shot["sub"], font=f_sub, fill=(196, 214, 236, int(235 * a)))

    # 左上角分镜序号
    order = [s["name"] for s in shots].index(shot["name"]) + 1
    chip = "%d / %d" % (order, len(shots))
    d.rectangle([28, 26, 28 + 78, 26 + 34], fill=(8, 10, 14, 150))
    d.text((40, 31), chip, font=f_chip, fill=(150, 205, 255, 235))

    # A/B 段落：当前画的是哪一边
    label = ab_label(shot, index)
    if label:
        tw = d.textlength(label, font=f_ab)
        x = (w - tw) * 0.5
        d.rectangle([x - 22, 24, x + tw + 22, 78], fill=(8, 10, 14, 165))
        color = (255, 226, 150, 245) if label == "源物体" else (160, 235, 190, 245)
        d.text((x, 32), label, font=f_ab, fill=color)

    # 底边进度条
    d.rectangle([0, h - 4, int(w * (index + 1) / total), h], fill=(120, 190, 255, 220))

    img.paste(Image.alpha_composite(img.convert("RGBA"), overlay).convert("RGB"), (0, 0))
    return img


def main():
    with open(os.path.join(OUT_DIR, "timeline.json"), encoding="utf-8") as f:
        timeline = json.load(f)
    shots, fps = timeline["shots"], timeline["fps"]
    frames = sorted(n for n in os.listdir(FRAME_DIR) if n.endswith(".png"))
    if not frames:
        sys.exit("没有帧：先跑 NaniteCutHLODVideo.py")
    total = len(frames)
    print("%d 帧 @ %d fps = %.1f s，分镜 %d 个" % (total, fps, total / fps, len(shots)))

    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        sys.exit("找不到 ffmpeg")
    w, h = timeline["width"], timeline["height"]
    proc = subprocess.Popen(
        [ffmpeg, "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (w, h), "-r", str(fps), "-i", "-",
         "-c:v", "libx264", "-preset", "medium", "-crf", "20", "-pix_fmt", "yuv420p", "-movflags", "+faststart", MP4],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    f_title, f_sub, f_chip, f_ab = font(31), font(21, False), font(19), font(30)
    for i, name in enumerate(frames):
        with Image.open(os.path.join(FRAME_DIR, name)) as src:
            img = draw(src, shots, i, total, f_title, f_sub, f_chip, f_ab)
        proc.stdin.write(img.tobytes())
        if i % 120 == 0:
            print("  %d/%d" % (i, total))
    proc.stdin.close()
    err = proc.stderr.read().decode("utf-8", "replace")
    if proc.wait() != 0:
        sys.exit("ffmpeg 失败：\n" + err[-2000:])
    print("-> %s (%.1f MB)" % (MP4, os.path.getsize(MP4) / 1048576.0))


main()
