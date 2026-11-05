# Instances of the multi-mesh NightCity_WP actors (BP_CityAtlasRegionChunk), each run of instance matrices matched to
# the component's StaticMesh: the name map and the import table are found by their shape, and the mesh is the
# StaticMesh property's import index nearest before the run.
import csv, math, os, re, struct, sys
from concurrent.futures import ProcessPoolExecutor

ROOT = r"D:\MyProject\UnrealProject\UETest574_2\Content\__ExternalActors__\BuildingGenerator\Cyberpunk2077\Maps\NightCity_WP"
ONE = struct.pack("<d", 1.0)

def name_map(d):
	# A chain of FNameEntrySerialized: int32 length (with the null), ASCII, null, uint32 hashes.
	for start in range(0, min(len(d), 4096), 4):
		names, p = [], start
		while p + 4 < len(d):
			n = struct.unpack_from("<i", d, p)[0]
			if n <= 0 or n > 1024 or p + 4 + n + 4 > len(d) or d[p + 4 + n - 1] != 0: break
			s = d[p + 4:p + 4 + n - 1]
			if not all(32 <= c < 127 for c in s): break
			names.append(s.decode()); p += 4 + n + 4
		if len(names) >= 8 and "None" in names: return names, p
	return None, 0

def imports(d, names, after):
	# FObjectImport, 40 bytes: ClassPackage, ClassName, OuterIndex, ObjectName, PackageName, bImportOptional. The table
	# follows the name map at no particular alignment: found from one StaticMesh import, then walked back to its start.
	cls = names.index("StaticMesh") if "StaticMesh" in names else -1
	sm = [i for i, s in enumerate(names) if s.startswith("SM_")]
	if cls < 0 or not sm: return {}
	def record(r):
		if r < after or r + 40 > len(d): return None
		cp, cpn, cn, cnn, outer, on, onn, pn, pnn, opt = struct.unpack_from("<10i", d, r)
		ok = 0 <= cp < len(names) and 0 <= cn < len(names) and 0 <= on < len(names) and 0 <= pn < len(names) and cpn == cnn == 0 and 0 <= onn < 1 << 20 and abs(outer) < 1 << 20 and opt in (0, 1)
		return (cn, on) if ok else None
	key = struct.pack("<ii", cls, 0)
	q = d.find(key, after)
	while q >= 0:
		r = q - 8
		rec = record(r)
		if rec and rec[0] == cls and names[rec[1]].startswith("SM_"):
			while record(r - 40): r -= 40
			table, k = {}, 0
			while True:
				rec = record(r + k * 40)
				if not rec: break
				if rec[0] == cls: table[-(k + 1)] = names[rec[1]]
				k += 1
			return table
		q = d.find(key, q + 1)
	return {}

def valid(d, o):
	if o < 4 or o + 128 > len(d): return None
	f = struct.unpack_from("<16d", d, o)
	if f[3] != 0.0 or f[7] != 0.0 or f[11] != 0.0 or f[15] != 1.0: return None
	if not all(abs(v) < 1e4 for v in f[:3] + f[4:7] + f[8:11]) or not all(abs(v) < 1e8 for v in f[12:15]): return None
	rows = [math.sqrt(f[r * 4] ** 2 + f[r * 4 + 1] ** 2 + f[r * 4 + 2] ** 2) for r in range(3)]
	return (f, rows) if all(1e-4 < x < 1e4 for x in rows) else None

def root_location(d, names):
	# RelativeLocation's tag, then its size (24: three doubles) and a flags byte, then the vector.
	if "RelativeLocation" not in names or "StructProperty" not in names: return (0.0, 0.0, 0.0), False
	tag = struct.pack("<iiii", names.index("RelativeLocation"), 0, names.index("StructProperty"), 0)
	q = d.find(tag)
	if q < 0: return (0.0, 0.0, 0.0), False
	for s in range(q + 16, q + 80):
		if struct.unpack_from("<i", d, s)[0] == 24:
			v = struct.unpack_from("<3d", d, s + 5)
			if all(abs(x) < 1e9 for x in v): return v, True
	return (0.0, 0.0, 0.0), False

def scan(path):
	d = open(path, "rb").read()
	names, after = name_map(d)
	if not names or "StaticMesh" not in names: return path, "nonames", []
	table = imports(d, names, after)
	if not table: return path, "noimports", []
	(ox, oy, oz), found = root_location(d, names)
	rotated = "RelativeRotation" in names or "RelativeScale3D" in names
	sm_name = struct.pack("<i", names.index("StaticMesh"))
	out, o, end = [], 0, len(d)
	name = os.path.relpath(path, ROOT).replace("\\", "/")
	for m in re.finditer(re.escape(ONE), d):
		o = m.start() - 120
		if o < 0 or (out and o < out[-1][0]): continue
		if valid(d, o) is None: continue
		n = struct.unpack_from("<i", d, o - 4)[0]
		if n <= 0 or o + n * 128 > end: continue
		run = [valid(d, o + i * 128) for i in range(n)]
		if any(v is None for v in run): continue
		# The component's StaticMesh: nearest "StaticMesh" property tag before the run, its value an import.
		mesh = None
		p = d.rfind(sm_name, 0, o)
		while p >= 0 and mesh is None:
			for q in range(p + 8, min(p + 64, o) - 3):
				v = struct.unpack_from("<i", d, q)[0]
				if v in table: mesh = table[v]; break
			if mesh is None: p = d.rfind(sm_name, 0, p)
		out.append((o + n * 128, mesh, run))
	rows = []
	for _, mesh, run in out:
		for f, r in run: rows.append((mesh or "?", name, round(ox + f[12], 1), round(oy + f[13], 1), round(oz + f[14], 1), round(math.degrees(math.atan2(f[1], f[0])), 2), round(r[0], 4), round(r[1], 4), round(r[2], 4)))
	return path, ("ok" if found else "ok-nolocation") + ("-rotated" if rotated else ""), rows

if __name__ == "__main__":
	chunks = sorted({r["actor"] for r in csv.DictReader(open("nc_instances.csv", encoding="utf-8")) if "|" in r["mesh"]})
	paths = [os.path.join(ROOT, a.replace("/", "\\")) for a in chunks]
	if len(sys.argv) > 2: paths = paths[:int(sys.argv[2])]
	status = {}
	with open(sys.argv[1], "w", newline="", encoding="utf-8") as fo, ProcessPoolExecutor(12) as pool:
		w = csv.writer(fo)
		w.writerow(["mesh", "actor", "x", "y", "z", "yaw", "sx", "sy", "sz"])
		total = unknown = 0
		for path, st, rows in pool.map(scan, paths, chunksize=32):
			status[st] = status.get(st, 0) + 1
			w.writerows(rows); total += len(rows); unknown += sum(1 for r in rows if r[0] == "?")
	print(len(paths), "chunk actors", status, total, "instances,", unknown, "with no mesh found")
