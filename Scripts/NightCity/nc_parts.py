# Night City building kits and part vocabulary from the mesh catalog's depot paths.
import collections, csv, re, sys

BS = chr(92)
rows = [r for r in csv.DictReader(open(sys.argv[1], encoding="utf-8")) if r["depot"]]
for r in rows: r["p"] = r["depot"].lower().split(BS)

arch = [r for r in rows if "architecture" in r["p"][:3] and "int" not in r["p"] and not any("interior" in s for s in r["p"])]
print("architecture, no interiors:", len(arch))

tok = collections.Counter()
for r in arch:
	for t in set(r["p"][-1][:-5].split("_")):
		if t and not re.fullmatch(r"[a-z]{1,2}|\d+|[lwhdr]\d+[a-z]?\d*", t): tok[t] += 1
print("== part tokens (meshes containing them)")
print(", ".join(f"{k}:{n}" for k, n in tok.most_common(120)))

KIT = re.compile(r"(.*?_(?:building|bld|shack|house|tower|block|apartment|hotel|motel|store|shop)_[a-z0-9]{1,2})_(.*)")
kits = collections.Counter()
kitparts = collections.defaultdict(collections.Counter)
kitdir = {}
for r in arch:
	m = KIT.match(r["p"][-1][:-5])
	if not m: continue
	kits[m.group(1)] += 1
	part = re.sub(r"_?(?:l|w|h|d|r)\d+[a-z]?\d*", "", m.group(2))
	part = re.sub(r"(?:_[a-z]{1,2}|_\d+)+$", "", part)
	kitparts[m.group(1)][part] += 1
	kitdir[m.group(1)] = BS.join(r["p"][3:-1])
print("== building kits:", len(kits), "meshes in them:", sum(kits.values()))
for k, n in kits.most_common(40):
	print(f"{n:4d} {k}  [{kitdir[k]}]  " + ", ".join(f"{p}:{c}" for p, c in kitparts[k].most_common(12)))
