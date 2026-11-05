# Night City urban fabric types and part statistics from the placed exterior architecture.
#  - Roof height map (2 m grid): every piece's footprint (pivot-centred square of half its larger xy size) raised to
#    its top; the ground is the 5th percentile of piece bottoms per 32 m block. Height = roof - ground.
#  - Each 128 m cell with built area: coverage, roof height percentiles, height spread, kit / district / part-role mix.
#  - k-means over the cells' massing features gives the recurring fabric types, with example cells.
#  - Part statistics: per role, the most common sizes (ApproxSize, cm) and how many placements use them.
import collections, csv, json, math, re, sys
import numpy as np
from scipy.cluster.vq import kmeans2
from scipy import ndimage

exec(open("nc_city.py", encoding="utf-8").read().split("meta = {}")[0])  # catalog, sizes, classify, INFRA, ROLES

meta, rows = {}, []
def load(path, skip_multi):
	for r in csv.DictReader(open(path, encoding="utf-8")):
		m = r["mesh"]
		if skip_multi and "|" in m: continue
		if m not in meta: meta[m] = classify(m)
		if meta[m] is None or m not in size: continue
		rows.append((m, float(r["x"]), float(r["y"]), float(r["z"]), float(r["sx"]), float(r["sy"]), float(r["sz"]), float(r["yaw"])))
load("nc_instances.csv", True)
load("nc_chunks.csv", False)
print("pieces:", len(rows))

names = sorted({r[0] for r in rows}); mid = {m: i for i, m in enumerate(names)}
M = np.array([mid[r[0]] for r in rows], dtype=np.int32)
P = np.array([r[1:4] for r in rows]); SC = np.array([r[4:7] for r in rows]); YAW = np.array([r[7] for r in rows])
SZ = np.array([size[m] for m in names], dtype=float)[M] * SC
role_of = np.array([meta[m][2] for m in names]); kit_of = np.array([meta[m][1] for m in names]); dist_of = np.array([meta[m][0] for m in names])

# Part statistics (by mesh, weighted by placements).
uses = np.bincount(M, minlength=len(names))
stats = {}
for role in sorted(set(role_of)):
	idx = [i for i in range(len(names)) if role_of[i] == role and uses[i] > 0]
	c = collections.Counter()
	for i in idx: c[tuple(size[names[i]])] += int(uses[i])
	stats[role] = dict(meshes=len(idx), placements=int(sum(uses[i] for i in idx)), sizes=[(f"{a}x{b}x{h}", n) for (a, b, h), n in c.most_common(12)])
json.dump(stats, open("nc_partstats.json", "w"), indent=1)

# Roof height map.
G = 200.0
x0, y0 = P[:, 0].min() - 2000, P[:, 1].min() - 2000
nx, ny = int((P[:, 0].max() + 2000 - x0) // G) + 1, int((P[:, 1].max() + 2000 - y0) // G) + 1
half = 0.5 * np.maximum(SZ[:, 0], SZ[:, 1])
keep = (half < 3000) & (SZ[:, 2] > 50)
top = np.full(nx * ny, -1e9)
for i in np.nonzero(keep)[0]:
	ix0, ix1 = int((P[i, 0] - half[i] - x0) // G), int((P[i, 0] + half[i] - x0) // G)
	iy0, iy1 = int((P[i, 1] - half[i] - y0) // G), int((P[i, 1] + half[i] - y0) // G)
	t = P[i, 2] + SZ[i, 2]
	blk = top.reshape(nx, ny)[ix0:ix1 + 1, iy0:iy1 + 1]
	np.maximum(blk, t, out=blk)
top = top.reshape(nx, ny)
B = 16  # 32 m blocks for the ground
gx, gy = (P[:, 0] - x0) // (G * B), (P[:, 1] - y0) // (G * B)
ground = collections.defaultdict(list)
for a, b, z in zip(gx.astype(int), gy.astype(int), P[:, 2]): ground[(a, b)].append(z)
gnd = np.full((nx // B + 1, ny // B + 1), np.nan)
for (a, b), zs in ground.items(): gnd[a, b] = np.percentile(zs, 5)
gnd_full = np.repeat(np.repeat(np.nan_to_num(gnd, nan=0.0), B, 0), B, 1)[:nx, :ny]
height = np.where(top > -1e8, (top - gnd_full) / 100.0, 0.0)
built = height > 3.0
np.save("nc_height.npy", height.astype(np.float32))
json.dump(dict(x0=x0, y0=y0, G=G, nx=nx, ny=ny), open("nc_height.json", "w"))

def plateaus(h):
	# Roof plateaus: connected built cells in the same 3 m height band, at least 5 cells (20 m^2).
	out = []
	band = np.where(h > 3.0, np.floor(h / 3.0), -1).astype(int)
	for v in np.unique(band[band >= 0]):
		lab, n = ndimage.label(band == v)
		if n == 0: continue
		sizes = ndimage.sum(np.ones_like(lab), lab, index=np.arange(1, n + 1))
		for s in sizes[sizes >= 5]: out.append((float(s) * (G / 100) ** 2, (v + 0.5) * 3.0))
	return out

# 128 m cells.
C = 64
cells = []
for a in range(0, nx, C):
	for b in range(0, ny, C):
		h = height[a:a + C, b:b + C]; bt = built[a:a + C, b:b + C]
		cov = bt.mean()
		if cov < 0.08: continue
		hv = h[bt]
		lab, n = ndimage.label(bt)
		pl = plateaus(h)
		cells.append(dict(ix=a // C, iy=b // C, a=a, b=b, x=x0 + (a + C / 2) * G, y=y0 + (b + C / 2) * G, coverage=float(cov), h50=float(np.percentile(hv, 50)), h90=float(np.percentile(hv, 90)), hstd=float(hv.std()), blobs=int(n),
			plateau_area=float(np.median([p[0] for p in pl])) if pl else 0.0, plateaus=len(pl), plateau_h=float(np.median([p[1] for p in pl])) if pl else 0.0))
cx = np.array([c["x"] for c in cells]); cy = np.array([c["y"] for c in cells])
cell_of_piece = {}
pa, pb = ((P[:, 0] - x0) // (G * C)).astype(int), ((P[:, 1] - y0) // (G * C)).astype(int)
index = {(c["ix"], c["iy"]): k for k, c in enumerate(cells)}
mix_kit = [collections.Counter() for _ in cells]; mix_dist = [collections.Counter() for _ in cells]; mix_role = [collections.Counter() for _ in cells]
for i in range(len(P)):
	k = index.get((pa[i], pb[i]))
	if k is None: continue
	mix_kit[k][kit_of[M[i]]] += 1; mix_dist[k][dist_of[M[i]]] += 1; mix_role[k][role_of[M[i]]] += 1
X = np.array([[c["coverage"], math.log1p(c["h50"]), math.log1p(c["h90"]), c["hstd"] / (c["h50"] + 5)] for c in cells])
Xs = (X - X.mean(0)) / X.std(0)
np.random.seed(7)
K = 8
cent, lab = kmeans2(Xs, K, minit="++", seed=7)
# A cell's district: the one most over-represented against the whole city (kits are reused across districts).
glob = collections.Counter()
for m in mix_dist: glob.update({d: n for d, n in m.items() if d != "common"})
gtot = sum(glob.values())
def district(k):
	m = {d: n for d, n in mix_dist[k].items() if d != "common"}
	tot = sum(m.values())
	if tot < 50: return "?"
	cand = [(n / tot) / (glob[d] / gtot) for d, n in m.items() if n / tot >= 0.1]
	best = [(lift, d) for (d, n), lift in zip([(d, n) for d, n in m.items() if n / tot >= 0.1], cand)]
	return max(best)[1] if best else "?"
for k, c in enumerate(cells): c["district"] = district(k)
json.dump([{k: v for k, v in c.items()} for c in cells], open("nc_cells.json", "w"))

types = []
for t in range(K):
	ks = np.nonzero(lab == t)[0]
	if len(ks) == 0: continue
	kit = collections.Counter(); dist = collections.Counter(); role = collections.Counter()
	for k in ks: kit.update(mix_kit[k]); dist.update({d: n for d, n in mix_dist[k].items() if d != "common"}); role.update(mix_role[k])
	tot = sum(role.values()) or 1
	# examples: the cells nearest the type's centre
	ex = sorted(ks, key=lambda k: float(((Xs[k] - cent[t]) ** 2).sum()))[:4]
	types.append(dict(type=t, cells=len(ks), coverage=round(float(X[ks, 0].mean()), 2), h50=round(float(np.median([cells[k]["h50"] for k in ks])), 1), h90=round(float(np.median([cells[k]["h90"] for k in ks])), 1),
		hstd=round(float(np.median([cells[k]["hstd"] for k in ks])), 1), blobs=int(np.median([cells[k]["blobs"] for k in ks])),
		plateau_area=round(float(np.median([cells[k]["plateau_area"] for k in ks])), 0), plateaus=int(np.median([cells[k]["plateaus"] for k in ks])), plateau_h=round(float(np.median([cells[k]["plateau_h"] for k in ks])), 1),
		cell_districts=collections.Counter(cells[k]["district"] for k in ks).most_common(5),
		districts=[(str(d), int(n)) for d, n in dist.most_common(5)], kits=[(str(k), int(n)) for k, n in kit.most_common(8)], roles={str(r): round(n / tot, 3) for r, n in role.most_common()},
		examples=[dict(cell=int(k), x=round(cells[k]["x"] / 100), y=round(cells[k]["y"] / 100), coverage=round(cells[k]["coverage"], 2), h50=round(cells[k]["h50"]), h90=round(cells[k]["h90"]), district=cells[k]["district"]) for k in ex]))
types.sort(key=lambda t: -t["cells"])
json.dump(types, open("nc_fabric.json", "w"), indent=1, default=str)
for t in types:
	print(f"type {t['type']}: {t['cells']} cells  coverage {t['coverage']}  roof h50 {t['h50']} m h90 {t['h90']} m  spread {t['hstd']}  plateaus/cell {t['plateaus']} median {t['plateau_area']} m2 at {t['plateau_h']} m")
	print("   cell districts", t["cell_districts"]); print("   kits", t["kits"][:6]); print("   roles", dict(list(t["roles"].items())[:7])); print("   examples (UE m)", [(e["x"], e["y"], e["district"]) for e in t["examples"]])
