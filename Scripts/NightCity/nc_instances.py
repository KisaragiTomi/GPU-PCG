# Every instance in the NightCity_WP external actors: HISM actors hold one mesh and its instances as FMatrix (16
# doubles, 128-byte stride) preceded by an int32 count. Writes mesh, actor file, x, y, z (UE cm, as stored), yaw (deg)
# and the three axis scales per instance; actors with no such run (region chunks) are counted and skipped.
import csv, math, os, re, struct, sys
from concurrent.futures import ProcessPoolExecutor

ROOT = r"D:\MyProject\UnrealProject\UETest574_2\Content\__ExternalActors__\BuildingGenerator\Cyberpunk2077\Maps\NightCity_WP"
ONE = struct.pack("<d", 1.0)
MESH = re.compile(rb"Meshes/[0-9A-F]{2}/(SM_[0-9A-F]{16})")

def valid(d, o):
	if o < 0 or o + 128 > len(d): return None
	f = struct.unpack_from("<16d", d, o)
	if f[3] != 0.0 or f[7] != 0.0 or f[11] != 0.0 or f[15] != 1.0: return None
	if not all(abs(v) < 1e4 for v in f[:3] + f[4:7] + f[8:11]): return None
	rows =[math.sqrt(f[r * 4] ** 2 + f[r * 4 + 1] ** 2 + f[r * 4 + 2] ** 2) for r in range(3)]
	if not all(1e-4 < x < 1e4 for x in rows): return None
	if not all(abs(f[12 + i]) < 1e8 for i in range(3)): return None
	return f, rows

def scan(path):
	d = open(path, "rb").read()
	meshes = sorted({m.decode() for m in MESH.findall(d)})
	best = []
	for m in re.finditer(re.escape(ONE), d):
		o = m.start() - 120
		if valid(d, o) is None: continue
		n = struct.unpack_from("<i", d, o - 4)[0] if o >= 4 else 0
		if n <= 0 or o + n * 128 > len(d): continue
		run = []
		for i in range(n):
			v = valid(d, o + i * 128)
			if v is None: run = []; break
			run.append(v)
		if len(run) > len(best): best = run
		if best: break
	name = os.path.relpath(path, ROOT).replace("\\", "/")
	out = []
	for f, rows in best:
		yaw = math.degrees(math.atan2(f[1], f[0]))
		out.append((meshes[0] if len(meshes) == 1 else "|".join(meshes), name, round(f[12], 1), round(f[13], 1), round(f[14], 1), round(yaw, 2), round(rows[0], 4), round(rows[1], 4), round(rows[2], 4)))
	return name, len(meshes), out

if __name__ == "__main__":
	paths = [os.path.join(dp, f) for dp, _, fs in os.walk(ROOT) for f in fs if f.endswith(".uasset")]
	empty = multi = 0
	with open(sys.argv[1], "w", newline="", encoding="utf-8") as fo, ProcessPoolExecutor(12) as pool:
		w = csv.writer(fo)
		w.writerow(["mesh", "actor", "x", "y", "z", "yaw", "sx", "sy", "sz"])
		total = 0
		for name, nm, rows in pool.map(scan, paths, chunksize=64):
			if not rows: empty += 1; continue
			if nm != 1: multi += 1
			w.writerows(rows); total += len(rows)
	print(len(paths), "actors;", total, "instances;", empty, "without instance data;", multi, "with several meshes")
