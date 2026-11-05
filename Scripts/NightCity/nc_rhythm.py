# Facade rhythm in Night City: columns of modules on one facade plane (same yaw, plane offset and position along the
# facade), what changes up a column (window/wall, ground floor, top), how bay widths mix along a facade, and where the
# common archetypes sit vertically (ground / middle / top of their column).
import collections
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# The data folder (nc_catalog.csv, nc_sizes.csv, nc_instances.csv, nc_chunks.csv, nc_mesh_bounds.json ...): the first
# argument, else this script's folder.
os.chdir(sys.argv[1] if len(sys.argv) > 1 else HERE)
bounds = json.load(open('nc_mesh_bounds.json'))
rank = json.load(open('nc_rank.json', encoding='utf-8'))
info = {r['asset']: r for r in rank['all'] if r['asset'] in bounds and 'megabuilding' not in r['kit'] and 'bridge' not in r['name']}
FACADE = {a for a, r in info.items() if r['role'] in ('wall', 'window', 'door', 'balcony', 'canopy', 'parapet')}

mods = []
for fname in ('nc_instances.csv', 'nc_chunks.csv'):
    with open(fname, encoding='utf-8') as f:
        next(f)
        for line in f:
            asset = line[:19]
            if asset in FACADE:
                p = line.rstrip().split(',')
                mods.append((asset,) + tuple(float(v) for v in p[2:9]))
print('facade modules', len(mods))

# Each module as a facade cell: direction (yaw mod 360, 5 deg), plane offset along the normal (50 cm), extent along the
# facade (from its bounds) and its z range.
cols = collections.defaultdict(list)
planes = collections.defaultdict(list)
for m in mods:
    asset, x, y, z, yaw, sx, sy, sz = m
    b = bounds[asset]
    if (b[3] - b[0]) * sx < 90 or (b[5] - b[2]) * sz < 150:
        continue  # strips, sills, cornices: not a facade cell
    yq = int(round(yaw / 5.0)) % 72
    a = math.radians(yq * 5.0)
    ux, uy = math.cos(a), math.sin(a)       # along the facade (local +X)
    nx, ny = -math.sin(a), math.cos(a)      # local +Y
    along0 = x * ux + y * uy + b[0] * sx
    along1 = x * ux + y * uy + b[3] * sx
    plane = round((x * nx + y * ny) / 50.0)
    z0 = z + b[2] * sz
    z1 = z + b[5] * sz
    r = info[asset]
    kind = r['role']
    if kind == 'wall' and ('_gf' in r['name'] or 'shop' in r['name']):
        kind = 'wall_gf'
    if kind == 'window' and '_gf' in r['name']:
        kind = 'window_gf'
    cols[(yq, plane, round(along0 / 50.0))].append((z0, z1, kind, r['arch'], round((along1 - along0) / 100.0), r['kit']))
    planes[(yq, plane)].append((along0, along1, z0, z1, kind, r['arch']))

# Columns: modules stacked at the same position. Sequence of kinds from bottom to top.
seqs = []
for key, lst in cols.items():
    lst.sort()
    # Drop duplicates (two meshes on one cell, e.g. glass + frame).
    dedup = []
    for e in lst:
        if dedup and abs(e[0] - dedup[-1][0]) < 50:
            continue
        dedup.append(e)
    if len(dedup) >= 3:
        seqs.append(dedup)
print('columns with >= 3 storeys', len(seqs))

uniform = 0
uniform_mid = 0
kinds_per = collections.Counter()
ground_diff = 0
top_diff = 0
for s in seqs:
    ks = [e[2] for e in s]
    mid = ks[1:-1] if len(ks) > 3 else ks[1:]
    if len(set(ks)) == 1:
        uniform += 1
    if len(set(mid)) == 1:
        uniform_mid += 1
        if ks[0] != mid[0]:
            ground_diff += 1
        if len(ks) > 3 and ks[-1] != mid[0]:
            top_diff += 1
    kinds_per[len(set(ks))] += 1
n = len(seqs)
print('columns all one kind: {:.1f}%; middle storeys one kind: {:.1f}% (of those, ground differs {:.1f}%, top differs {:.1f}%)'.format(
    100.0 * uniform / n, 100.0 * uniform_mid / n, 100.0 * ground_diff / max(1, uniform_mid), 100.0 * top_diff / max(1, uniform_mid)))
print('distinct kinds per column:', sorted(kinds_per.items())[:6])
# Middle-storey kind of uniform columns.
mk = collections.Counter()
for s in seqs:
    ks = [e[2] for e in s]
    mid = ks[1:-1] if len(ks) > 3 else ks[1:]
    if len(set(mid)) == 1:
        mk[mid[0]] += 1
print('kind of uniform middle storeys:', mk.most_common(8))
# Ground-floor kind when the column is uniform above.
gk = collections.Counter()
for s in seqs:
    ks = [e[2] for e in s]
    mid = ks[1:-1] if len(ks) > 3 else ks[1:]
    if len(set(mid)) == 1 and ks[0] != mid[0]:
        gk[(ks[0], mid[0])] += 1
print('ground floor vs above (when different):', gk.most_common(8))
# Ground storey height.
gh = collections.Counter(round((s[0][1] - s[0][0]) / 50.0) * 50 for s in seqs)
print('ground cell height (cm):', gh.most_common(6))
uh = collections.Counter(round((e[1] - e[0]) / 50.0) * 50 for s in seqs for e in s[1:])
print('upper cell height (cm):', uh.most_common(6))

# Along a facade: neighbouring columns on one storey band, run lengths of window / wall columns and bay width mix.
runs = collections.Counter()
widths = collections.Counter()
patterns = collections.Counter()
for key, lst in planes.items():
    if len(lst) < 6:
        continue
    # One storey band: the most common z0 band in this plane (rounded 1 m) above the lowest.
    bands = collections.Counter(round(e[2] / 100.0) for e in lst)
    zlow = min(bands)
    cand = [b for b, c in bands.most_common() if b > zlow + 2]
    if not cand:
        continue
    band = cand[0]
    row = sorted((e for e in lst if round(e[2] / 100.0) == band), key=lambda e: e[0])
    if len(row) < 4:
        continue
    seq = []
    for e in row:
        w = round((e[1] - e[0]) / 100.0)
        widths[w] += 1
        seq.append('W' if e[4].startswith('window') else 'B' if e[4] == 'balcony' else 'C' if e[4] == 'canopy' else 'D' if e[4] == 'door' else '-')
    s = ''.join(seq)
    run = 1
    for i in range(1, len(s)):
        if s[i] == s[i - 1]:
            run += 1
        else:
            runs[(s[i - 1], min(run, 8))] += 1
            run = 1
    runs[(s[-1], min(run, 8))] += 1
    if 4 <= len(s) <= 10:
        patterns[s] += 1
print('\nbay widths along facades (m):', widths.most_common(8))
print('run lengths on a storey (kind, run):', sorted(runs.items(), key=lambda kv: (kv[0][0], kv[0][1]))[:40])
print('common rows (4-10 bays):')
for p, c in patterns.most_common(25):
    print('   ', p, c)

# Vertical placement of the top archetypes: ground (lowest cell of its column), top (highest), middle.
where = collections.defaultdict(collections.Counter)
for s in seqs:
    for i, e in enumerate(s):
        pos = 'ground' if i == 0 else 'top' if i == len(s) - 1 else 'middle'
        where[e[3]][pos] += 1
print('\nwhere the archetypes sit in their column:')
for arch, c in sorted(where.items(), key=lambda kv: -sum(kv[1].values()))[:30]:
    t = sum(c.values())
    print('  {:50s} {:6d}  ground {:3.0f}%  middle {:3.0f}%  top {:3.0f}%'.format(arch[:50], t, 100.0 * c['ground'] / t, 100.0 * c['middle'] / t, 100.0 * c['top'] / t))
