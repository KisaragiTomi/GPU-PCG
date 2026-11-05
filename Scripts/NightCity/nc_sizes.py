# Bounds size of every catalogued mesh from its package's asset registry tag ApproxSize ("XxYxZ", cm, rounded).
import csv, os, re, sys
from concurrent.futures import ThreadPoolExecutor

ROOT = r"D:\MyProject\UnrealProject\UETest574_2\Content\BuildingGenerator\Cyberpunk2077"
SIZE = re.compile(rb"ApproxSize\x00.{0,8}?(\d+)x(\d+)x(\d+)", re.S)

def size(rel):
	m = SIZE.search(open(os.path.join(ROOT, rel.replace("/", os.sep)), "rb").read())
	return tuple(int(g) for g in m.groups()) if m else None

cat = list(csv.DictReader(open("nc_catalog.csv", encoding="utf-8")))
with ThreadPoolExecutor(16) as pool:
	sizes = list(pool.map(size, [r["file"] for r in cat]))
with open(sys.argv[1], "w", newline="", encoding="utf-8") as fo:
	w = csv.writer(fo); w.writerow(["asset", "sx", "sy", "sz"])
	for r, s in zip(cat, sizes):
		if s: w.writerow([r["asset"], *s])
print(len(cat), "meshes,", sum(1 for s in sizes if s), "with ApproxSize")
