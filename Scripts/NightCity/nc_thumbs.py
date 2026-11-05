# Roof-height thumbnails of each fabric type's example cells (128 m squares, 2 m pixels), one row per type.
import json, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

h = np.load("nc_height.npy"); meta = json.load(open("nc_height.json"))
cells = json.load(open("nc_cells.json")); types = [t for t in json.load(open("nc_fabric.json")) if t["cells"] >= 20]
C = 64
fig, axes = plt.subplots(len(types), 4, figsize=(11, 2.9 * len(types)))
for row, t in enumerate(types):
	for col in range(4):
		ax = axes[row][col]; ax.set_xticks([]); ax.set_yticks([])
		if col >= len(t["examples"]): ax.axis("off"); continue
		e = t["examples"][col]; c = cells[e["cell"]]
		img = h[c["a"]:c["a"] + C, c["b"]:c["b"] + C].T[::-1]
		ax.imshow(np.clip(img, 0, 120), cmap="magma", vmin=0, vmax=120, interpolation="nearest")
		ax.set_title(f"({e['x']}, {e['y']}) m  {e['district']}", fontsize=8)
		if col == 0: ax.set_ylabel(f"type {t['type']}\ncover {t['coverage']}\nroof {t['h50']:.0f}/{t['h90']:.0f} m", fontsize=9)
fig.colorbar(plt.cm.ScalarMappable(cmap="magma", norm=plt.Normalize(0, 120)), ax=axes, shrink=0.25, label="roof height above ground (m)")
fig.savefig(sys.argv[1], dpi=110, bbox_inches="tight")
print("saved", sys.argv[1])
