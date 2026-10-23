"""Offline numerical PNG comparison. Requires Pillow and numpy; no UE imports."""
import json, shutil
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[4]
OUT = ROOT / 'Saved/CSSWCustomMaterial'
DOC = ROOT / 'Plugins/PCGPlugins/Docs/CSSWCustomMaterial'
DOC.mkdir(parents=True, exist_ok=True)
cases = ['saved', 'valid-500', 'valid-800', 'variant-no-foam', 'variant-no-detail',
         'variant-texture-simulation', 'variant-texture-no-wpo', 'variant-distortion-off']
results = []

def pixels(path):
    return np.array(Image.open(path).convert('RGB')).astype(np.int16)

for case in cases:
    for view in ('overview', 'surface', 'grazing'):
        prefix = case + '-' + view
        a, b, c = [pixels(OUT / (prefix + '-' + tag + '.png')) for tag in ('original', 'custom', 'control')]
        d = np.abs(a - b)
        noise = np.abs(a - c)
        results.append({'case': case, 'view': view, 'size': [a.shape[1], a.shape[0]],
                        'rgb_mean_absolute_error_8bit': float(d.mean()),
                        'rgb_max_absolute_error_8bit': int(d.max()),
                        'pixels_any_channel_over_2': int((d.max(axis=2) > 2).sum()),
                        'pixels_any_channel_different': int((d.max(axis=2) > 0).sum()),
                        'pixel_percent_over_2': float((d.max(axis=2) > 2).mean() * 100),
                        'aa_control_max_error_8bit': int(noise.max())})
(DOC / 'pixel-metrics.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
for file in ('build-result.json', 'validation.json', 'link-audit.json', 'source-integrity.json', 'variant-stats.json', 'capture-cases.json'):
    shutil.copy2(OUT / file, DOC / file)
if (OUT / 'flatten/result.json').exists():
    shutil.copy2(OUT / 'flatten/result.json', DOC / 'flatten-result.json')

fig = Image.new('RGB', (2700, 1980), '#f5f5f5')
draw = ImageDraw.Draw(fig)
font_path = Path('C:/Windows/Fonts/segoeui.ttf')
font = ImageFont.truetype(str(font_path), 26) if font_path.exists() else ImageFont.load_default()
title_font = ImageFont.truetype(str(font_path), 40) if font_path.exists() else font
draw.text((30, 15), 'CSSW original vs Custom Flat | UE 5.7.4 / SM6', fill='#222222', font=title_font)
for row, (case, view, label) in enumerate([
        ('valid-800', 'overview', 'Baked vertex data / overview'),
        ('valid-800', 'surface', 'Baked vertex data / surface'),
        ('variant-texture-simulation', 'surface', 'Simulation textures / surface')]):
    prefix = case + '-' + view
    a, b = [pixels(OUT / (prefix + '-' + tag + '.png')) for tag in ('original', 'custom')]
    d = np.abs(a - b)
    labels = [label + ' | Original', 'Custom Flat', 'Difference x20 | max ' + str(d.max()) + ' / 255']
    for col, data in enumerate((a, b, np.clip(d * 20, 0, 255))):
        x, y = 20 + col * 900, 100 + row * 600
        draw.text((x, y), labels[col], fill='#222222', font=font)
        panel = Image.fromarray(data.astype(np.uint8)).resize((880, 550), Image.Resampling.LANCZOS)
        fig.paste(panel, (x, y + 38))
draw.text((25, 1920), 'Same-frame A/B/A, 1280 x 800. Black difference pixels are equal. Identical camera and exposure.', fill='#222222', font=font)
fig.save(DOC / 'comparison.png')
print(json.dumps({'comparisons': len(results),
                  'max_mae_8bit': max(r['rgb_mean_absolute_error_8bit'] for r in results),
                  'max_channel_difference': max(r['rgb_max_absolute_error_8bit'] for r in results),
                  'max_pixels_over_2': max(r['pixels_any_channel_over_2'] for r in results),
                  'max_pixel_percent_over_2': max(r['pixel_percent_over_2'] for r in results),
                  'max_control_difference': max(r['aa_control_max_error_8bit'] for r in results)}, indent=2))
