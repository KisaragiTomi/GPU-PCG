# Footprint shapes and stacking of Night City masses on a module grid (3 m, in each mass's own frame).
# Input: nc_masses_cache.pkl (nc_masses_prep.py), nc_mesh_bounds.json. Output: nc_shapes.json + summary.
import collections
import json
import math
import os
import sys
import pickle

HERE = os.path.dirname(os.path.abspath(__file__))
# The data folder (nc_catalog.csv, nc_sizes.csv, nc_instances.csv, nc_chunks.csv, nc_mesh_bounds.json ...): the first
# argument, else this script's folder.
os.chdir(sys.argv[1] if len(sys.argv) > 1 else HERE)
bounds = json.load(open('nc_mesh_bounds.json'))
rank = json.load(open('nc_rank.json', encoding='utf-8'))
kit_of = {r['asset']: r['kit'] for r in rank['all']}
C = pickle.load(open('nc_masses_cache.pkl', 'rb'))
roofs, wall_lo, wall_hi, roof_cells, top_tile, regions, region = (C[k] for k in ('roofs', 'wall_lo', 'wall_hi', 'roof_cells', 'top_tile', 'regions', 'region'))
G = 300.0  # module grid, cm
N4 = ((1, 0), (-1, 0), (0, 1), (0, -1))


def local_rect(m, c, s):
    asset, x, y, z, yaw, sx, sy, sz = m
    b = bounds[asset]
    cy, sn = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    xs, ys = [], []
    for lx in (b[0] * sx, b[3] * sx):
        for ly in (b[1] * sy, b[4] * sy):
            wx, wy = x + cy * lx - sn * ly, y + sn * lx + cy * ly
            xs.append(c * wx - s * wy)
            ys.append(s * wx + c * wy)
    return min(xs), min(ys), max(xs), max(ys)


def to_local(px, py, c, s):
    return c * px - s * py, s * px + c * py


def to_world(lx, ly, c, s):
    # inverse of to_local (rotation by +yaw)
    return c * lx + s * ly, -s * lx + c * ly


out = []
for ri, cells in enumerate(regions):
    if len(cells) < 40:
        continue
    tiles = {top_tile[k] for k in cells if k in top_tile}
    yaws = collections.Counter()
    for t in tiles:
        b = bounds[roofs[t][0]]
        yaws[round(roofs[t][4] % 90.0) % 90] += (b[3] - b[0]) * (b[4] - b[1])
    yaw = yaws.most_common(1)[0][0]
    c, s = math.cos(math.radians(-yaw)), math.sin(math.radians(-yaw))
    rects = [local_rect(roofs[t], c, s) for t in tiles if abs(((roofs[t][4] - yaw + 45.0) % 90.0) - 45.0) <= 3.0]
    if not rects:
        continue
    ox = min(r[0] for r in rects)
    oy = min(r[1] for r in rects)
    # 1 m sub-cells covered by tiles, then 3 m cells at least half covered.
    sub = collections.Counter()
    for r in rects:
        for ix in range(int((r[0] - ox) // 100 + 0.5), int((r[2] - ox) // 100 + 0.5)):
            for iy in range(int((r[1] - oy) // 100 + 0.5), int((r[3] - oy) // 100 + 0.5)):
                sub[(ix // 3, iy // 3)] += 1
    grid = {k for k, n in sub.items() if n >= 5}
    if len(grid) < 4:
        continue
    zt = sorted(roof_cells[k] for k in cells)[len(cells) // 2]
    xs = [k[0] for k in grid]
    ys = [k[1] for k in grid]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    w, d = x1 - x0 + 1, y1 - y0 + 1
    if max(w, d) * 3 > 150:
        out.append({'big': True, 'w': w * 3, 'd': d * 3, 'kit': '', 'area': len(cells)})
        continue

    def wall_top_at(gx, gy):
        # Highest wall top in this 3 m cell (world 1 m cells sampled at its 9 sub-centres).
        hi = -1e12
        for a in range(3):
            for b in range(3):
                wx, wy = to_world(ox + (gx * 3 + a + 0.5) * 100.0, oy + (gy * 3 + b + 0.5) * 100.0, c, s)
                hi = max(hi, wall_hi.get((int(math.floor(wx / 100.0)), int(math.floor(wy / 100.0))), -1e12))
        return hi

    def wall_low_at(gx, gy):
        lo = 1e12
        for a in range(3):
            for b in range(3):
                wx, wy = to_world(ox + (gx * 3 + a + 0.5) * 100.0, oy + (gy * 3 + b + 0.5) * 100.0, c, s)
                lo = min(lo, wall_lo.get((int(math.floor(wx / 100.0)), int(math.floor(wy / 100.0))), 1e12))
        return lo

    # Holes inside the bbox: a tower standing on this roof (walls rise > 3.5 m above it) or a courtyard (open).
    outside = set()
    stack = [(x0 - 1, y0 - 1)]
    while stack:
        q = stack.pop()
        if q in outside or q in grid or not (x0 - 1 <= q[0] <= x1 + 1 and y0 - 1 <= q[1] <= y1 + 1):
            continue
        outside.add(q)
        stack.extend((q[0] + dx, q[1] + dy) for dx, dy in N4)
    holes = [(gx, gy) for gx in range(x0, x1 + 1) for gy in range(y0, y1 + 1) if (gx, gy) not in grid and (gx, gy) not in outside]
    tower_holes = [h for h in holes if wall_top_at(*h) > zt + 350.0]
    court = len(holes) - len(tower_holes)
    # Notches open to the outside where a taller mass stands (setback against a taller neighbour).
    edge_out = {(k[0] + dx, k[1] + dy) for k in grid for dx, dy in N4} - grid
    edge_out = {q for q in edge_out if q in outside}
    taller_edge = sum(1 for q in edge_out if wall_top_at(*q) > zt + 350.0)
    shape_grid = grid | set(tower_holes)
    concave = 0
    for vx in range(x0, x1 + 2):
        for vy in range(y0, y1 + 2):
            n = sum(1 for q in ((vx - 1, vy - 1), (vx, vy - 1), (vx - 1, vy), (vx, vy)) if q in shape_grid)
            if n == 3:
                concave += 1
    fill = len(shape_grid) / float(w * d)
    # Ground: lowest wall bottom on the outline.
    lows = [wall_low_at(*q) for q in list(edge_out)[:60]] + [wall_low_at(*k) for k in list(grid)[:20]]
    lows = [v for v in lows if v < 1e11]
    if not lows:
        continue
    ground = sorted(lows)[len(lows) // 10]
    height = (zt - ground) / 100.0
    if height < 2.5:
        continue
    kit = collections.Counter(kit_of.get(roofs[t][0], '?') for t in tiles).most_common(1)[0][0]
    cx = sum(k[0] for k in cells) / len(cells)
    cy = sum(k[1] for k in cells) / len(cells)
    out.append({'w': w * 3, 'd': d * 3, 'cells': len(shape_grid), 'fill': round(fill, 3), 'concave': concave,
                'court': court, 'tower': len(tower_holes), 'taller_edge': taller_edge, 'edge': len(edge_out),
                'height': round(height, 1), 'storeys': max(1, int(round(height / 4.0))), 'kit': kit,
                'at': [round(cx), round(cy), round(ground / 100.0)]})

json.dump(out, open('nc_shapes.json', 'w'))
big = [m for m in out if m.get('big')]
ms = [m for m in out if not m.get('big')]
print('masses {} (+ {} longer than 150 m: street walls / merged blocks)'.format(len(ms), len(big)))


def shape(m):
    if m['court'] >= 2:
        return 'courtyard'
    if m['concave'] == 0:
        return 'rect'
    if m['concave'] == 1:
        return 'L'
    if m['concave'] == 2:
        return 'U/T/Z'
    return 'complex'


def stack(m):
    if m['tower'] >= 2:
        return 'podium'
    if m['taller_edge'] >= max(2, m['edge'] // 6):
        return 'step'
    return 'top'


n = len(ms)
print('\nfootprint (outline incl. towers standing on it), share of masses / of roof area:')
area = sum(m['cells'] for m in ms)
for k, v in collections.Counter(shape(m) for m in ms).most_common():
    sel = [m for m in ms if shape(m) == k]
    st = sorted(m['storeys'] for m in sel)
    sz = sorted(m['w'] * m['d'] for m in sel)
    print('  {:10s} {:5d} {:5.1f}% / {:5.1f}%   storeys median {:2d} p90 {:2d}, bbox median {} m2'.format(k, v, 100.0 * v / n, 100.0 * sum(m['cells'] for m in sel) / area, st[len(st) // 2], st[9 * len(st) // 10], sz[len(sz) // 2]))
print('\nstacking:')
for k, v in collections.Counter(stack(m) for m in ms).most_common():
    sel = [m for m in ms if stack(m) == k]
    st = sorted(m['storeys'] for m in sel)
    print('  {:7s} {:5d} {:5.1f}%   storeys median {} p90 {}'.format(k, v, 100.0 * v / n, st[len(st) // 2], st[9 * len(st) // 10]))
pod = [m for m in ms if stack(m) == 'podium']
if pod:
    print('  podium: storeys', sorted(collections.Counter(min(m['storeys'], 10) for m in pod).items()), ' tower share of outline median {:.2f}'.format(sorted(m['tower'] / max(1, m['cells']) for m in pod)[len(pod) // 2]))
stp = [m for m in ms if stack(m) == 'step']
if stp:
    print('  step: storeys', sorted(collections.Counter(min(m['storeys'], 10) for m in stp).items()))
print('\ncross: shape x stacking')
cross = collections.Counter((shape(m), stack(m)) for m in ms)
for k, v in cross.most_common(14):
    print('  {:10s} {:7s} {:5d} {:5.1f}%'.format(k[0], k[1], v, 100.0 * v / n))
print('\nrect outline sizes (short x long, m), top masses:')
for k, v in collections.Counter((min(m['w'], m['d']), max(m['w'], m['d'])) for m in ms if shape(m) == 'rect').most_common(14):
    print('   {:>3} x {:<3} {}'.format(k[0], k[1], v))
print('\nby kit:')
for kit, v in collections.Counter(m['kit'] for m in ms).most_common(10):
    sel = [m for m in ms if m['kit'] == kit]
    print('  {:32s} {:5d}  '.format(kit[:32], v) + ', '.join('{} {:.0f}%'.format(a, 100.0 * b / v) for a, b in collections.Counter(shape(m) for m in sel).most_common()) + ' | ' + ', '.join('{} {:.0f}%'.format(a, 100.0 * b / v) for a, b in collections.Counter(stack(m) for m in sel).most_common()))
print('\nexamples (UE world m):')
for k in ('rect', 'L', 'U/T/Z', 'courtyard', 'complex'):
    for st in ('top', 'step', 'podium'):
        sel = sorted([m for m in ms if shape(m) == k and stack(m) == st and m['storeys'] >= 3], key=lambda m: -m['cells'])[:2]
        for m in sel:
            print('  {:9s} {:6s} {} {}x{} m fill {} concave {} court {} tower {} {} storeys at ({}, {}, {})'.format(k, st, m['kit'][:28], m['w'], m['d'], m['fill'], m['concave'], m['court'], m['tower'], m['storeys'], *m['at']))
