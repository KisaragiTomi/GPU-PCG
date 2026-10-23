# -*- coding: utf-8 -*-
"""NaniteCutHLODPromo 的配乐：纯代码合成的原创氛围音乐（D 小调），节点对齐画面。

    python NaniteCutHLODPromoMusic.py   # → Saved/NaniteCutHLODVideo/promo_music.wav（94 s，48 kHz 立体声）

层次：长音垫（加法合成、左右微失谐）+ 低音 + 拨弦琶音（八分音符，带回声）+ 钟声点缀 + 风声底噪，
统一过一个卷积混响。与画面对齐的节点：
  20.5 s 像素网格出现（钟）· 24–28.5 s 簇一块块亮起（上行闪光）· 35.2 s 组边界锁住（两声钟）·
  38–42 s 内部顶点坍缩（下行轻拨）· 44.2 s 接缝特写（暖钟）· 49.4–53 s 距离波扫过（渐强 + 反向气声）·
  53 s 横切一刀（重音）· 64.4 s 屋内物体消散（下行闪光）· 74.5 s 三张贴图（三声钟）· 85.8 s 换成截面（钟）·
  89.4 s 片名（和声解决到 F）
"""
import math
import os
import sys

import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, fftconvolve, sosfilt

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
SR = 48000
# --combined：宣传片正文 → 章节卡 → 录屏 → 片名。这几个时间点与 NaniteCutHLODPromo.py 里的同名常量必须一致。
COMBINED = "--combined" in sys.argv
DEMO_START, DEMO_CARD, REC_DUR = 89.2, 3.0, 73.86
COMBINED_TITLE = DEMO_START + DEMO_CARD + REC_DUR
DUR = COMBINED_TITLE + 4.8 if COMBINED else 94.0
OUT = os.path.join(ROOT, "Saved", "NaniteCutHLODVideo", "promo_demo_music.wav" if COMBINED else "promo_music.wav")
N = int(SR * DUR)
T = np.arange(N) / SR
RNG = np.random.default_rng(2026)

BPM = 72.0
EIGHTH = 60.0 / BPM / 2.0


def midi(n):
    return 440.0 * 2.0 ** ((n - 69) / 12.0)


# 和弦：(起, 止, 垫音音符, 低音根音)
DM, BB, FM, CM, GM = "Dm", "Bb", "F", "C", "Gm"
VOICING = {
    DM: ([50, 57, 64, 65], 38),   # D3 A3 E4 F4（add9）
    BB: ([46, 53, 57, 62], 34),   # Bb2 F3 A3 D4（maj7）
    FM: ([53, 57, 60, 67], 41),   # F3 A3 C4 G4（add9）
    CM: ([48, 55, 62, 64], 36),   # C3 G3 D4 E4（add9）
    GM: ([43, 50, 53, 58], 43),   # G2 D3 F3 Bb3
}
CHORDS = [(0.0, 13.3, DM), (13.3, 20.0, BB), (20.0, 26.7, FM), (26.7, 33.3, CM), (33.3, 40.0, DM),
          (40.0, 46.7, BB), (46.7, 53.0, GM), (53.0, 59.7, DM), (59.7, 66.4, BB), (66.4, 73.1, FM),
          (73.1, 79.8, CM), (79.8, 86.5, DM)]
CUT = 53.0
if not COMBINED:
    TITLE = 89.4
    CHORDS += [(86.5, 89.4, BB), (89.4, 94.0, FM)]
else:
    TITLE = COMBINED_TITLE + 0.3
    CHORDS += [(86.5, DEMO_START, BB)]
    # 录屏那一段：Dm–Bb–F–C 慢循环当背景，最后停在 C 上导向片名的 F
    _t, _k = DEMO_START, 0
    while _t + 9.6 < TITLE - 2.0:
        CHORDS.append((_t, _t + 9.6, [DM, BB, FM, CM][_k % 4]))
        _t += 9.6
        _k += 1
    CHORDS += [(_t, TITLE, CM), (TITLE, DUR, FM)]
# 琶音用的和弦音（第 4/5 八度）
ARP = {DM: [62, 65, 69, 72, 74], BB: [62, 65, 69, 70, 74], FM: [60, 65, 69, 72, 77], CM: [60, 64, 67, 72, 74],
       GM: [62, 65, 67, 70, 74]}


def env_ar(t, a, b, attack, release):
    """a 起音、b 止音；起音 attack 秒线性升、b 之后 release 秒指数衰减。"""
    up = np.clip((t - a) / attack, 0, 1)
    up = up * up * (3 - 2 * up)
    down = np.where(t > b, np.exp(-(t - b) / (release / 4.0)), 1.0)
    return up * down * (t >= a)


def chord_at(t):
    for a, b, c in CHORDS:
        if a <= t < b:
            return c
    return CHORDS[-1][2]


def pad():
    out = np.zeros((N, 2))
    for a, b, c in CHORDS:
        notes, _ = VOICING[c]
        lo, hi = int(max(0, a - 0.1) * SR), int(min(DUR, b + 3.5) * SR)
        t = T[lo:hi]
        e = env_ar(t, a, b, 1.6 if a > 0 else 4.0, 3.0)
        lfo = 0.85 + 0.15 * np.sin(2 * math.pi * 0.09 * t + a)
        for n in notes:
            f = midi(n)
            for ch, cents in ((0, -4.0), (1, 4.0)):
                ff = f * 2 ** (cents / 1200.0)
                ph = RNG.uniform(0, 6.28)
                w = (np.sin(2 * math.pi * ff * t + ph) + 0.45 * np.sin(2 * math.pi * 2 * ff * t + ph * 1.3)
                     + 0.18 * np.sin(2 * math.pi * 3 * ff * t + ph * 0.7) + 0.08 * np.sin(2 * math.pi * 4 * ff * t))
                out[lo:hi, ch] += w * e * lfo * 0.06
    # 片头更空：前 13 s 只有垫音，音量压低
    out *= (0.55 + 0.45 * np.clip((T - 8.0) / 6.0, 0, 1))[:, None]
    # 距离波扫过时渐强
    out *= (1.0 + 0.5 * np.clip((T - 46.7) / (CUT - 46.7), 0, 1) * (T < CUT))[:, None]
    if COMBINED:
        # 录屏段让画面说话：垫音压低两成
        demo = np.clip((T - DEMO_START) / 3.0, 0, 1) * (1 - np.clip((T - (TITLE - 4.0)) / 3.0, 0, 1))
        out *= (1.0 - 0.2 * demo)[:, None]
    return out


def bass():
    out = np.zeros((N, 2))
    for a, b, c in CHORDS:
        if a < 13.0:
            continue
        _, root = VOICING[c]
        lo, hi = int(a * SR), int(min(DUR, b + 2.0) * SR)
        t = T[lo:hi]
        e = env_ar(t, a, b, 0.8, 1.6)
        f = midi(root)
        w = np.sin(2 * math.pi * f * t) + 0.25 * np.sin(2 * math.pi * 2 * f * t)
        out[lo:hi] += (w * e * 0.12)[:, None]
    return out


def pluck_note(buf, start, note, vel, pan, decay=0.45):
    f = midi(note)
    n = int(2.2 * SR)
    lo = int(start * SR)
    if lo >= N:
        return
    hi = min(N, lo + n)
    t = np.arange(hi - lo) / SR
    e = np.exp(-t / decay) * np.clip(t / 0.004, 0, 1)
    w = (np.sin(2 * math.pi * f * t) + 0.35 * np.sin(2 * math.pi * 2 * f * t) * np.exp(-t / (decay * 0.5))
         + 0.12 * np.sin(2 * math.pi * 3 * f * t) * np.exp(-t / (decay * 0.3)))
    s = w * e * vel
    buf[lo:hi, 0] += s * (1 - pan)
    buf[lo:hi, 1] += s * pan


def plucks():
    out = np.zeros((N, 2))
    pattern = [0, 2, 1, 3, 2, 4, 3, 1]
    step, t = 0, 8.6
    while t < (TITLE if COMBINED else 90.0):
        # 密度：开头稀疏；讲锁边那段放轻让画面说话；距离波扫过时停下蓄力；切完之后回来更亮
        if 48.6 <= t < CUT + 0.4:
            dens = 0.0
        elif t < 14.0:
            dens = 0.35
        elif t < 23.4:
            dens = 0.55
        elif t < 33.3:
            dens = 0.75
        elif t < 48.6:
            dens = 0.5
        elif t < 80.0:
            dens = 0.9
        elif COMBINED and t < DEMO_START:
            dens = 0.6
        elif COMBINED:
            dens = 0.35 * (1 - np.clip((t - (TITLE - 5.0)) / 4.0, 0, 1))   # 录屏段稀疏，片名前收住
        else:
            dens = 0.6 * (1 - np.clip((t - 86.0) / 4.0, 0, 1))
        if RNG.random() < dens:
            tones = ARP[chord_at(t)]
            note = tones[pattern[step % len(pattern)]]
            if t > CUT and step % 8 == 7:
                note += 12
            vel = 0.075 * (0.7 + 0.3 * RNG.random())
            pan = 0.5 + 0.35 * math.sin(step * 1.7)
            pluck_note(out, t + RNG.uniform(-0.006, 0.006), note, vel, pan)
        step += 1
        t += EIGHTH
    # 附点八分回声，左右交替
    d = int(EIGHTH * 1.5 * SR)
    echo = np.zeros_like(out)
    echo[d:, 0] += out[:-d, 1] * 0.35
    echo[d:, 1] += out[:-d, 0] * 0.35
    echo[2 * d:] += out[:-2 * d] * 0.15
    return out + echo


def bell(buf, start, note, amp, pan=0.5, decay=2.8):
    f = midi(note)
    lo = int(start * SR)
    hi = min(N, lo + int(decay * 3 * SR))
    t = np.arange(hi - lo) / SR
    partials = [(1.0, 1.0), (2.76, 0.5), (5.40, 0.25), (8.93, 0.12)]
    s = sum(a * np.sin(2 * math.pi * f * r * t) * np.exp(-t / (decay / (1 + r * 0.25))) for r, a in partials)
    s *= np.clip(t / 0.002, 0, 1) * amp
    buf[lo:hi, 0] += s * (1 - pan)
    buf[lo:hi, 1] += s * pan


def accents():
    out = np.zeros((N, 2))
    bell(out, 20.6, 81, 0.05, 0.4)                     # 像素网格
    bell(out, 20.9, 76, 0.035, 0.6)
    for k, n in enumerate([69, 72, 74, 77, 81, 84]):   # 簇一块块亮起：上行闪光
        bell(out, 24.2 + k * 0.72, n, 0.026, 0.25 + 0.1 * k, 2.2)
    bell(out, 35.25, 74, 0.05, 0.45, 3.2)              # 组边界锁住：两声钟
    bell(out, 35.55, 81, 0.04, 0.55, 3.2)
    for k, n in enumerate([84, 81, 77, 74, 72, 69, 67, 65]):   # 内部顶点坍缩：下行轻拨
        pluck_note(out, 38.1 + k * 0.48, n, 0.05, 0.3 + 0.06 * k, 0.6)
    bell(out, 44.25, 77, 0.045, 0.5, 3.6)              # 接缝特写：暖钟
    bell(out, 44.25, 69, 0.03, 0.5, 3.6)
    for k, n in enumerate([88, 84, 81, 79, 76, 74, 72]):   # 屋内物体消散：下行闪光
        bell(out, 64.4 + k * 0.28, n, 0.028, 0.3 + 0.07 * k, 1.8)
    for k, n in enumerate([77, 81, 84]):                   # 三张贴图
        bell(out, 74.5 + k * 0.38, n, 0.04, 0.35 + 0.15 * k)
    bell(out, 85.8, 84, 0.045, 0.5)                        # 换成截面
    if COMBINED:                                           # 章节卡「实机演示」：D 小调的一组钟
        for n, p in [(62, 0.4), (69, 0.5), (74, 0.6)]:
            bell(out, DEMO_START + 0.25, n, 0.035, p, 3.6)
    for n, p in [(65, 0.35), (69, 0.5), (72, 0.65), (77, 0.5)]:   # 片名：F 大调和弦的钟
        bell(out, TITLE + 0.05, n, 0.04, p, 4.0)
    return out


def noise(n):
    return RNG.standard_normal((n, 2))


def lowpass(x, fc, order=2):
    sos = butter(order, fc, "low", fs=SR, output="sos")
    return sosfilt(sos, x, axis=0)


def bandpass(x, lo, hi, order=2):
    sos = butter(order, [lo, hi], "band", fs=SR, output="sos")
    return sosfilt(sos, x, axis=0)


def impacts():
    out = np.zeros((N, 2))
    hits = [(CUT, 1.0), (TITLE, 0.55)] + ([(DEMO_START + 0.2, 0.3)] if COMBINED else [])
    for at, amp in hits:
        lo = int(at * SR)
        hi = min(N, lo + int(4.0 * SR))
        t = np.arange(hi - lo) / SR
        f = 34 + 44 * np.exp(-t / 0.18)                    # 70 → 34 Hz 下滑
        ph = 2 * math.pi * np.cumsum(f) / SR
        boom = np.sin(ph) * np.exp(-t / 1.1) * np.clip(t / 0.003, 0, 1) * 0.45 * amp
        out[lo:hi] += boom[:, None]
        burst = lowpass(noise(hi - lo), 900) * np.exp(-t / 0.35)[:, None] * 0.25 * amp
        out[lo:hi] += burst
    # 横切之前的反向气声：噪声渐强、截止频率上扬（分段近似）
    lo, hi = int((CUT - 3.6) * SR), int(CUT * SR)
    swell = np.zeros((hi - lo, 2))
    nz = noise(hi - lo)
    pieces = 12
    for k in range(pieces):
        a, b = k * (hi - lo) // pieces, (k + 1) * (hi - lo) // pieces
        fc = 300 + 3500 * (k / (pieces - 1)) ** 2
        swell[a:b] = lowpass(nz[a:b], fc)
    ramp = np.linspace(0, 1, hi - lo) ** 2.2
    out[lo:hi] += swell * ramp[:, None] * 0.22
    return out


def air():
    """风声底噪：带通噪声 + 慢速起伏，给画面背景那层"细节"配一层声音的质感。"""
    nz = bandpass(noise(N), 400, 3200)
    mod = 0.5 + 0.5 * np.sin(2 * math.pi * 0.05 * T)[:, None] * np.sin(2 * math.pi * 0.13 * T + 1.0)[:, None]
    return nz * (0.012 + 0.010 * mod)


def reverb_ir(seconds=3.6):
    n = int(seconds * SR)
    t = np.arange(n) / SR
    ir = RNG.standard_normal((n, 2)) * np.exp(-t / (seconds / 6.9))[:, None]
    ir = lowpass(ir, 6000)
    for k, (dt, g) in enumerate([(0.011, 0.6), (0.019, 0.5), (0.027, 0.4), (0.041, 0.3)]):
        ir[int(dt * SR), k % 2] += g
    return ir / np.abs(ir).sum(0).max() * 6.0


def mix():
    p, b, pl, ac, im, ai = pad(), bass(), plucks(), accents(), impacts(), air()
    dry = p * 1.0 + b * 1.0 + pl * 1.0 + ac * 1.0 + im * 1.0 + ai * 1.0
    send = p * 0.5 + pl * 0.9 + ac * 1.2 + im * 0.3
    ir = reverb_ir()
    wet = np.stack([fftconvolve(send[:, c], ir[:, c])[:N] for c in range(2)], 1)
    out = dry + wet * 0.35
    # 低切（去掉次声）+ 柔和限幅
    sos = butter(2, 28, "high", fs=SR, output="sos")
    out = sosfilt(sos, out, axis=0)
    out /= np.abs(out).max() + 1e-9
    out = np.tanh(out * 1.6) / np.tanh(1.6)
    fade = np.clip(T / 2.0, 0, 1) * np.clip((DUR - T) / 1.6, 0, 1)
    out *= fade[:, None]
    out *= 0.89 / (np.abs(out).max() + 1e-9)     # -1 dBFS
    return out


if __name__ == "__main__":
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    audio = mix()
    wavfile.write(OUT, SR, (audio * 32767).astype(np.int16))
    print("-> %s (%.1f s, peak %.2f, rms %.3f)" % (OUT, DUR, np.abs(audio).max(), np.sqrt((audio ** 2).mean())))
