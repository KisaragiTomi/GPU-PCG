# Night City mesh catalog: every SM_<hash>.uasset under Content/BuildingGenerator/Cyberpunk2077 -> its
# SourceDepotPath (the REDengine .mesh it was imported from), material references and file size.
import csv, os, re, sys
from concurrent.futures import ThreadPoolExecutor

ROOT = r"D:\MyProject\UnrealProject\UETest574_2\Content\BuildingGenerator\Cyberpunk2077"
OUT = sys.argv[1]
DEPOT = re.compile(rb"((?:base|ep1)\\[\x20-\x7e]{3,400}?\.mesh)")
DEPOT16 = re.compile(rb"((?:b\x00a\x00s\x00e\x00|e\x00p\x001\x00)\\\x00(?:[\x20-\x7e]\x00){3,400}?\.\x00m\x00e\x00s\x00h\x00)")
MAT = re.compile(rb"/Game/BuildingGenerator/Cyberpunk2077/[\x20-\x7e]*?/(MI_[0-9A-F]{16})")

def scan(path):
	data = open(path, "rb").read()
	m = DEPOT.search(data)
	depot = m.group(1).decode("ascii") if m else ""
	if not depot:
		m = DEPOT16.search(data)
		depot = m.group(1).decode("utf-16-le") if m else ""
	mats = sorted({x.decode() for x in MAT.findall(data)})
	rel = os.path.relpath(path, ROOT)
	return (os.path.splitext(os.path.basename(path))[0], rel.replace("\\", "/"), depot, len(data), ";".join(mats))

paths = []
for top in ("Meshes", "Recovery"):
	for dirpath, _, files in os.walk(os.path.join(ROOT, top)):
		paths += [os.path.join(dirpath, f) for f in files if f.startswith("SM_") and f.endswith(".uasset")]

with ThreadPoolExecutor(16) as pool:
	rows = list(pool.map(scan, paths))

with open(OUT, "w", newline="", encoding="utf-8") as f:
	w = csv.writer(f)
	w.writerow(["asset", "file", "depot", "bytes", "materials"])
	w.writerows(sorted(rows))
print(len(rows), "meshes,", sum(1 for r in rows if r[2]), "with a depot path")
