# Night City buildings and building clusters from the placed exterior architecture.
#  1. Every exterior architecture instance (no interiors) with its mesh's district, kit and part role.
#  2. Structures: pieces whose footprints (pivot-centred discs of 0.75 x their scaled xy diagonal) touch, and whose
#     height ranges overlap, joined (union-find over a 16 m grid).
#  3. Buildings: structures at least 4 m tall with at least 30 m^2 of footprint box.
#  4. Clusters: buildings whose boxes come within GAP of each other (a street is wider), joined.
# Writes nc_buildings.csv, nc_clusters.csv and prints the summaries.
import collections, csv, math, re, sys
import numpy as np
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components

BS = chr(92)
GAP = 600.0  # cm between building boxes for the same cluster

cat = {r["asset"]: r for r in csv.DictReader(open("nc_catalog.csv", encoding="utf-8"))}
size = {r["asset"]: (int(r["sx"]), int(r["sy"]), int(r["sz"])) for r in csv.DictReader(open("nc_sizes.csv", encoding="utf-8"))}

DISTRICTS = {"watson", "westbrook", "city_center", "heywood", "pacifica", "santo_domingo", "badlands", "dogtown", "megabuilding", "spaceport"}
KIT = re.compile(r"(.*?_(?:building|bld|shack|house|tower|block|apartment|hotel|motel|store|shop)_[a-z0-9]{1,2})_(.*)")
ROLES = [  # first match wins
	("corner", r"corner|_a45|_a60|_a90|_a135|_c90|convex|concave"),
	("window", r"window|glass|shutter|blinds"),
	("door", r"door|entrance|gate|garage_door"),
	("balcony", r"balcony|gallery|handrail|railing|rail\b"),
	("roof", r"roof|rooftop|parapet|cornice|ledge|top\b|_top|cap\b|gutter"),
	("canopy", r"canopy|overhang|awning|porch|shield|slats"),
	("floor", r"floor|ceiling|celling|slab|basement"),
	("pillar", r"pillar|pilar|column|support|beam|frame|structur|steelframe"),
	("stairs", r"stair|steps|ramp"),
	("facade", r"facade|panel|grid|wall|backwall|shell|_gf|_uf|_mf|\bgf\b|\buf\b"),
]

INFRA = re.compile(r"overpass|highway|catwalk|pedestrian_bridge|railtrack|rail_track|crane|electric_tower|retaining_wall|riverbank|pier|tunnel|sewer|pipe|construction_site|station_metro|metro|monorail|bridge|dam|terrain|cliff|rock|fence")

def classify(asset):
	r = cat.get(asset)
	if not r or not r["depot"]: return None
	p = r["depot"].lower().split(BS)
	if "architecture" not in p[:3] or "int" in p or any("interior" in s for s in p): return None
	if INFRA.search(BS.join(p[3:])): return None
	area = p[3] if len(p) > 4 else "?"
	district = area if area in DISTRICTS else ("common" if area == "common" else area)
	name = p[-1][:-5]
	m = KIT.match(name)
	kit = m.group(1) if m else BS.join(p[3:5]) if len(p) > 5 else area
	role = next((k for k, rx in ROLES if re.search(rx, name)), "other")
	return district, kit, role, name

meta = {}
rows = []
def load(path, skip_multi):
	for r in csv.DictReader(open(path, encoding="utf-8")):
		m = r["mesh"]
		if skip_multi and "|" in m: continue
		if m not in meta: meta[m] = classify(m)
		if meta[m] is None or m not in size: continue
		rows.append((m, float(r["x"]), float(r["y"]), float(r["z"]), float(r["sx"]), float(r["sy"]), float(r["sz"])))
load("nc_instances.csv", True)
load("nc_chunks.csv", False)
print("exterior architecture instances:", len(rows), "meshes:", sum(1 for m, v in meta.items() if v))

mesh_ids = {m: i for i, m in enumerate(sorted({r[0] for r in rows}))}
mid = np.array([mesh_ids[r[0]] for r in rows], dtype=np.int32)
P = np.array([[r[1], r[2], r[3]] for r in rows])
S = np.array([[size[r[0]][0] * r[4], size[r[0]][1] * r[5], size[r[0]][2] * r[6]] for r in rows])
rad = 0.5 * np.hypot(S[:, 0], S[:, 1]) + 30.0
rad[np.maximum(S[:, 0], S[:, 1]) > 6000.0] = 0.0  # megastructure pieces join nothing
z0 = P[:, 2] - 0.25 * S[:, 2]; z1 = P[:, 2] + S[:, 2]

# 2. structures: candidate pairs over a 16 m grid (big pieces go into every cell their disc covers).
CELL = 1600.0
cells = collections.defaultdict(list)
for i in range(len(P)):
	r = rad[i]
	for cx in range(int(math.floor((P[i, 0] - r) / CELL)), int(math.floor((P[i, 0] + r) / CELL)) + 1):
		for cy in range(int(math.floor((P[i, 1] - r) / CELL)), int(math.floor((P[i, 1] + r) / CELL)) + 1):
			cells[(cx, cy)].append(i)
ia, ib = [], []
for members in cells.values():
	if len(members) < 2: continue
	m = np.array(members)
	if len(m) > 4000: m = m[np.argsort(-rad[m])]  # keep it bounded: biggest pieces first
	for k in range(0, len(m), 2000):
		a = m[k:k + 2000]
		d = np.hypot(P[a, None, 0] - P[None, m, 0], P[a, None, 1] - P[None, m, 1])
		touch = (d < rad[a, None] + rad[None, m]) & (z0[a, None] < z1[None, m]) & (z0[None, m] < z1[a, None])
		x, y = np.nonzero(touch)
		ia.append(a[x]); ib.append(m[y])
ia = np.concatenate(ia); ib = np.concatenate(ib)
graph = coo_matrix((np.ones(len(ia), dtype=np.int8), (ia, ib)), shape=(len(P), len(P)))
nstruct, label = connected_components(graph, directed=False)
print("structures:", nstruct)

# 3. buildings
inv = {i: m for m, i in mesh_ids.items()}
lo = np.full((nstruct, 3), 1e18); hi = np.full((nstruct, 3), -1e18)
np.minimum.at(lo, label, np.c_[P[:, 0] - rad * 0.6, P[:, 1] - rad * 0.6, z0])
np.maximum.at(hi, label, np.c_[P[:, 0] + rad * 0.6, P[:, 1] + rad * 0.6, z1])
count = np.bincount(label, minlength=nstruct)
height = (hi[:, 2] - lo[:, 2]) / 100.0
area = (hi[:, 0] - lo[:, 0]) * (hi[:, 1] - lo[:, 1]) / 1e4
is_bld = (height >= 4.0) & (area >= 30.0) & (count >= 3)
order = np.argsort(label, kind="stable")
bounds = np.searchsorted(label[order], np.arange(nstruct + 1))
buildings = []
for s in np.nonzero(is_bld)[0]:
	members = order[bounds[s]:bounds[s + 1]]
	dist = collections.Counter(); kits = collections.Counter(); roles = collections.Counter()
	for i in members:
		district, kit, role, _ = meta[inv[mid[i]]]
		dist[district] += 1; kits[kit] += 1; roles[role] += 1
	named = [d for d, _ in dist.most_common() if d != "common"]
	buildings.append(dict(id=int(s), x=(lo[s, 0] + hi[s, 0]) / 200, y=(lo[s, 1] + hi[s, 1]) / 200, z=lo[s, 2] / 100,
		w=(hi[s, 0] - lo[s, 0]) / 100, d=(hi[s, 1] - lo[s, 1]) / 100, h=height[s], pieces=int(count[s]), meshes=len({mid[i] for i in members}),
		district=named[0] if named else "common", kits=";".join(f"{k}:{n}" for k, n in kits.most_common(4)),
		roles=";".join(f"{k}:{n}" for k, n in roles.most_common(8))))
print("buildings:", len(buildings))

# 4. clusters
B = np.array([[b["x"] - b["w"] / 2, b["y"] - b["d"] / 2, b["x"] + b["w"] / 2, b["y"] + b["d"] / 2] for b in buildings]) * 100
gx = collections.defaultdict(list)
for i, b in enumerate(B):
	for cx in range(int(b[0] // 5000), int(b[2] // 5000) + 1):
		for cy in range(int(b[1] // 5000), int(b[3] // 5000) + 1): gx[(cx, cy)].append(i)
ca, cb = [], []
for members in gx.values():
	m = np.array(members)
	gapx = np.maximum(B[m, None, 0] - B[None, m, 2], B[None, m, 0] - B[m, None, 2])
	gapy = np.maximum(B[m, None, 1] - B[None, m, 3], B[None, m, 1] - B[m, None, 3])
	x, y = np.nonzero((gapx < GAP) & (gapy < GAP))
	ca.append(m[x]); cb.append(m[y])
ca = np.concatenate(ca); cb = np.concatenate(cb)
ncl, clabel = connected_components(coo_matrix((np.ones(len(ca), dtype=np.int8), (ca, cb)), shape=(len(B), len(B))), directed=False)
for b, c in zip(buildings, clabel): b["cluster"] = int(c)

with open("nc_buildings.csv", "w", newline="", encoding="utf-8") as f:
	w = csv.DictWriter(f, fieldnames=list(buildings[0].keys())); w.writeheader(); w.writerows(buildings)
clusters = collections.defaultdict(list)
for b in buildings: clusters[b["cluster"]].append(b)
with open("nc_clusters.csv", "w", newline="", encoding="utf-8") as f:
	w = csv.writer(f); w.writerow(["cluster", "buildings", "district", "x", "y", "w", "d", "h_min", "h_med", "h_max", "kits"])
	for c, bs in sorted(clusters.items(), key=lambda kv: -len(kv[1])):
		xs = [b["x"] - b["w"] / 2 for b in bs] + [b["x"] + b["w"] / 2 for b in bs]; ys = [b["y"] - b["d"] / 2 for b in bs] + [b["y"] + b["d"] / 2 for b in bs]
		hs = sorted(b["h"] for b in bs)
		dist = collections.Counter(b["district"] for b in bs).most_common(1)[0][0]
		kits = collections.Counter()
		for b in bs:
			for kv in b["kits"].split(";"):
				k, n = kv.rsplit(":", 1); kits[k] += int(n)
		w.writerow([c, len(bs), dist, round((min(xs) + max(xs)) / 2), round((min(ys) + max(ys)) / 2), round(max(xs) - min(xs)), round(max(ys) - min(ys)), round(hs[0]), round(hs[len(hs) // 2]), round(hs[-1]), ";".join(f"{k}:{n}" for k, n in kits.most_common(5))])
print("clusters:", ncl, "with 2+ buildings:", sum(1 for bs in clusters.values() if len(bs) > 1))
