"""Diff the GPU geometry dumps (Saved/CSRoadGpu/<golden dir name>/<id>.bin, written by the
PCGPlugins.CSRoadNet.Geometry tests) against the C# golden data (Saved/CSRoadGolden).

Usage: python diff_gpu_geometry.py [ids...] [--golden <dir>] [--tol 1e-5] [--top 30]
Prints, per field of EdgeGeometry / EdgeNodeGeometry / NodeGeometry, how many values exceed
the tolerance (relative to max(1, |golden|)) and the worst cases.
"""
import json
import struct
import sys
from collections import defaultdict
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[4]


def main():
    args = sys.argv[1:]
    global GOLDEN, GPU
    GOLDEN = PROJECT / "Saved" / "CSRoadGolden"
    if "--golden" in args:
        i = args.index("--golden")
        GOLDEN = Path(args[i + 1])
        del args[i:i + 2]
    GPU = PROJECT / "Saved" / "CSRoadGpu" / GOLDEN.name
    tol = 1e-5
    top = 30
    if "--tol" in args:
        i = args.index("--tol")
        tol = float(args[i + 1])
        del args[i:i + 2]
    if "--top" in args:
        i = args.index("--top")
        top = int(args[i + 1])
        del args[i:i + 2]
    layouts = json.loads((GOLDEN / "layouts.json").read_text(encoding="utf-8"))
    ids = args or sorted(p.stem for p in GPU.glob("*.bin"))
    counts = defaultdict(int)
    worst = []
    total = 0
    for id_ in ids:
        golden = json.loads((GOLDEN / f"{id_}.json").read_text(encoding="utf-8"))
        data = (GPU / f"{id_}.bin").read_bytes()
        offset = 0

        def take(n):
            nonlocal offset
            values = struct.unpack_from(f"<{n}f", data, offset)
            offset += 4 * n
            return values

        for r, edge in enumerate(golden["edges"]):
            for kind, key, n in (("EdgeGeometry", "edgeGeometry", 58), ("EdgeNodeGeometry", "startGeometry", 79), ("EdgeNodeGeometry", "endGeometry", 79)):
                mine = take(n)
                gold = edge[key]
                for i, (m, g) in enumerate(zip(mine, gold)):
                    total += 1
                    err = abs(m - g)
                    if err > tol * max(1.0, abs(g)):
                        name = f"{key}.{layouts[kind][i]}"
                        counts[name] += 1
                        worst.append((err, id_, r, name, m, g))
        for n_, node in enumerate(golden["nodes"]):
            mine = take(9)
            for i, (m, g) in enumerate(zip(mine, node["geometry"])):
                total += 1
                err = abs(m - g)
                if err > tol * max(1.0, abs(g)):
                    name = f"node.{layouts['NodeGeometry'][i]}"
                    counts[name] += 1
                    worst.append((err, id_, n_, name, m, g))
    print(f"{len(ids)} ids, {total} values, {len(worst)} over {tol}")
    for name, c in sorted(counts.items(), key=lambda kv: -kv[1])[:60]:
        print(f"  {c:7d}  {name}")
    worst.sort(reverse=True)
    for err, id_, r, name, m, g in worst[:top]:
        print(f"  {err:10.4g}  {id_} #{r} {name}: gpu {m:.7g} golden {g:.7g}")


if __name__ == "__main__":
    main()
