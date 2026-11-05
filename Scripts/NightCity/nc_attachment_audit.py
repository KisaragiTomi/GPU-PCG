"""Read-only attachment-host audit of cached NightCity MCP / package exports.

python nc_attachment_audit.py --snapshot <scene_snapshot.json> --detail <detail_snapshot.json>
    --catalog-dir <folder containing nc_catalog/sizes/instances/chunks.csv> --output <report.json>

The two populations stay separate. Name categories and thin AABBs are proxies,
not verified installation surfaces. CSV scales are axis lengths, not signed scales.
"""
import argparse
import collections
import csv
import hashlib
import json
from pathlib import Path
import re

KIT = re.compile(r"(.*?_(?:building|bld|shack|house|tower|block|apartment|hotel|motel|store|shop)_[a-z0-9]{1,2})_(.*)")
INFRA = re.compile(r"overpass|highway|catwalk|pedestrian_bridge|railtrack|rail_track|crane|electric_tower|retaining_wall|riverbank|pier|tunnel|sewer|pipe|construction_site|station_metro|metro|monorail|bridge|dam|terrain|cliff|rock|fence")
ROLES = [
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


def classify(depot):
    parts = depot.lower().split("\\")
    if "architecture" not in parts[:3] or "int" in parts or any("interior" in p for p in parts):
        return None
    if INFRA.search("\\".join(parts[3:])):
        return None
    name = parts[-1].removesuffix(".mesh")
    kit = KIT.match(name)
    return {
        "name": name,
        "kit": kit.group(1) if kit else None,
        "role": next((role for role, pattern in ROLES if re.search(pattern, name)), "other"),
    }


def coverage(counts):
    ordered = sorted(counts.values(), reverse=True)
    total = sum(ordered)
    cumulative = 0
    thresholds = {}
    for i, n in enumerate(ordered, 1):
        cumulative += n
        for percent in (50, 80, 90, 95):
            if cumulative >= total * percent / 100 and str(percent) not in thresholds:
                thresholds[str(percent)] = i
    return {
        "meshes": len(ordered), "instances": total,
        "top_k_percent": {str(k): round(sum(ordered[:k]) * 100 / total, 3) if total else 0 for k in (10, 50, 100, 200)},
        "meshes_needed_for_percent": thresholds,
    }


def summarize(counts, meta, sizes):
    ext = {m: n for m, n in counts.items() if m in meta and meta[m]}
    roles = collections.defaultdict(collections.Counter)
    kits = collections.Counter()
    for m, n in ext.items():
        roles[meta[m]["role"]][m] = n
        if meta[m]["kit"]:
            kits[meta[m]["kit"]] += n
    # Names deliberately exclude corner/roof/canopy/etc.; opaque names remain 'other'.
    host = {m: n for role in ("facade", "window", "door") for m, n in roles[role].items()}
    dimensions = collections.Counter()
    thin = collections.Counter()
    for m, n in host.items():
        if m not in sizes:
            continue
        x, y, z = sizes[m]
        w, depth = max(x, y), min(x, y)
        dimensions[(round(w / 5) * 5, round(depth / 5) * 5, round(z / 5) * 5)] += n
        if depth <= 100 and w >= 200 and z >= 150:
            thin[m] = n
    top = []
    for m, n in sorted(ext.items(), key=lambda kv: (-kv[1], kv[0]))[:30]:
        top.append({"mesh": m, "instances": n, **meta[m], "size_cm": sizes.get(m)})
    return {
        "all_meshes": len(counts), "all_instances": sum(counts.values()),
        "exterior_name_proxy": coverage(ext),
        "roles": {role: coverage(c) for role, c in roles.items()},
        "named_kits": len(kits), "named_kit_instances": sum(kits.values()),
        "top_kits": kits.most_common(12),
        "facade_window_door_name_subset": coverage(host),
        "thin_bounds_subset": coverage(thin),
        "top_host_dimensions_5cm_bins": [{"width_depth_height_cm": key, "instances": n} for key, n in dimensions.most_common(12)],
        "top_exterior_meshes": top,
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--snapshot", type=Path, required=True)
    p.add_argument("--detail", type=Path, required=True)
    p.add_argument("--catalog-dir", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    with (args.catalog_dir / "nc_catalog.csv").open(encoding="utf-8", newline="") as f:
        catalog = {r["asset"]: r for r in csv.DictReader(f)}
    with (args.catalog_dir / "nc_sizes.csv").open(encoding="utf-8", newline="") as f:
        sizes = {r["asset"]: [float(r[k]) for k in ("sx", "sy", "sz")] for r in csv.DictReader(f)}
    meta = {m: classify(r["depot"]) for m, r in catalog.items()}
    snap = json.loads(args.snapshot.read_text(encoding="utf-8"))
    live_counts = collections.Counter()
    for c in snap["components"]:
        live_counts[c["mesh"].split(".")[-1]] += c["count"]
    live_sizes = {path.split(".")[-1]: [round(hi - lo, 3) for lo, hi in zip(b["min"], b["max"])] for path, b in snap["meshes"].items()}
    mesh_paths = collections.defaultdict(list)
    for path in snap["meshes"]:
        mesh_paths[path.split(".")[-1]].append(path)
    report = {
        "date": "2026-11-05", "script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "scope": "2026-11-04 MCP loaded snapshot and separately cached on-disk package exports; no new editor query",
        "limitations": ["Name-based roles are heuristics, not surface labels or building counts.",
                        "AABB similarity is not geometric, semantic or anchor equivalence.",
                        "Package CSVs are heuristic binary parses, not a fresh engine enumeration; ambiguous multi-mesh rows are excluded.",
                        "CSV axis lengths lose reflection signs; no claim about negative-scale incidence.",
                        "Mesh counters use short mesh IDs; identical IDs at different package paths are grouped. Loaded-path aliases are listed separately.",
                        "No triangle planarity, GPU runtime, placement success or network benchmark was measured."],
        "inputs": {},
        "loaded_mcp": summarize(live_counts, meta, live_sizes),
    }
    report["loaded_mcp"]["distinct_asset_paths"] = len(snap["meshes"])
    report["loaded_mcp"]["short_id_aliases"] = {m: paths for m, paths in mesh_paths.items() if len(paths) > 1}
    full_counts = collections.Counter()
    scales = collections.Counter()
    scale_examples = collections.Counter()
    for filename in ("nc_instances.csv", "nc_chunks.csv"):
        path = args.catalog_dir / filename
        read = skipped = accepted = 0
        with path.open(encoding="utf-8", newline="") as f:
            for r in csv.DictReader(f):
                read += 1
                m = r["mesh"]
                if "|" in m or m not in catalog:
                    skipped += 1
                    continue
                full_counts[m] += 1
                accepted += 1
                if not meta[m]:
                    continue
                s = tuple(round(float(r[k]), 3) for k in ("sx", "sy", "sz"))
                scales["exterior_total"] += 1
                scales["unit" if all(abs(x - 1) <= 0.001 for x in s) else "nonunit"] += 1
                if max(s) - min(s) > 0.001:
                    scales["nonuniform"] += 1
                scale_examples[s] += 1
        report["inputs"][filename] = {"path": str(path), "bytes": path.stat().st_size, "rows": read, "excluded_ambiguous_or_unknown": skipped, "accepted": accepted}
        print(filename, report["inputs"][filename], flush=True)
    report["package_exports"] = summarize(full_counts, meta, sizes)
    report["package_scale_axis_lengths"] = {**scales, "top_scales": scale_examples.most_common(12)}
    detail = json.loads(args.detail.read_text(encoding="utf-8"))
    focus = [t for a in detail["selected"] for c in a["components"] for t in c["transforms"]]
    focus_scales = collections.Counter(tuple(round(x, 3) for x in t["scale"]) for t in focus)
    report["loaded_focus_scales"] = {"count": len(focus), "top_scales": focus_scales.most_common(12)}
    for path in (args.snapshot, args.detail, args.catalog_dir / "nc_catalog.csv", args.catalog_dir / "nc_sizes.csv"):
        report["inputs"][path.name] = {"path": str(path), "bytes": path.stat().st_size, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("Written", args.output, flush=True)


if __name__ == "__main__":
    main()
