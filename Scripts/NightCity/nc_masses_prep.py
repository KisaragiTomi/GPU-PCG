# Step 1 of the massing analysis (NightCity_BuildingKit.md section 5): Night City's roof tiles and walls on a 1 m grid.
# Every connected patch of roof tiles at one height (no wall rising more than 2 m over it) is the top of a mass;
# walls give each 1 m cell its lowest bottom and highest top. Writes nc_masses_cache.pkl for nc_shapes.py.
# Needs nc_rank.json (nc_rank.py) and nc_mesh_bounds.json (nc_bounds_dump.py).
import collections
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# The data folder (nc_catalog.csv, nc_sizes.csv, nc_instances.csv, nc_chunks.csv, nc_mesh_bounds.json ...): the first
# argument, else this script's folder.
os.chdir(sys.argv[1] if len(sys.argv) > 1 else HERE)

rank = json.load(open('nc_rank.json', encoding='utf-8'))
bounds = json.load(open('nc_mesh_bounds.json'))
info = {r['asset']: r for r in rank['all'] if r['asset'] in bounds and 'megabuilding' not in r['kit'] and 'bridge' not in r['name']}


def is_tile(asset):
    b = bounds[asset]
    return (b[3] - b[0]) >= 190 and (b[4] - b[1]) >= 190 and (b[5] - b[2]) <= 60


ROOF = {a for a, r in info.items() if r['role'] in ('roof', 'floor') and is_tile(a) and 'ceiling' not in r['name'] and 'interior' not in r['name']}
WALLS = {a for a, r in info.items() if r['role'] in ('wall', 'window', 'door', 'corner_convex', 'corner_concave', 'pillar', 'balcony', 'canopy')}

roofs = []   # (asset, x, y, z, yaw, sx, sy, sz)
walls = []
for fname in ('nc_instances.csv', 'nc_chunks.csv'):
    with open(fname, encoding='utf-8') as f:
        next(f)
        for line in f:
            asset = line[:19]
            if asset in ROOF:
                p = line.rstrip().split(',')
                roofs.append((asset,) + tuple(float(v) for v in p[2:9]))
            elif asset in WALLS:
                p = line.rstrip().split(',')
                walls.append((asset,) + tuple(float(v) for v in p[2:9]))
print('roof/floor tiles', len(roofs), 'walls', len(walls))


def samples(m, step=50.0, shrink=10.0):
    asset, x, y, z, yaw, sx, sy, sz = m
    b = bounds[asset]
    c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    x0, x1 = b[0] * sx + shrink, b[3] * sx - shrink
    y0, y1 = b[1] * sy + shrink, b[4] * sy - shrink
    if x1 < x0:
        x0 = x1 = (b[0] + b[3]) * 0.5 * sx
    if y1 < y0:
        y0 = y1 = (b[1] + b[4]) * 0.5 * sy
    nx = max(1, int((x1 - x0) / step) + 1)
    ny = max(1, int((y1 - y0) / step) + 1)
    for a in range(nx + 1):
        lx = x0 + (x1 - x0) * a / nx
        for bb in range(ny + 1):
            ly = y0 + (y1 - y0) * bb / ny
            yield x + c * lx - s * ly, y + s * lx + c * ly


# Wall columns: lowest wall bottom and highest wall top per 1 m cell (for the ground under a roof and what rises above it).
wall_lo = {}
wall_hi = {}
for m in walls:
    b = bounds[m[0]]
    z0 = m[3] + b[2] * m[7]
    z1 = m[3] + b[5] * m[7]
    for wx, wy in samples(m, 70.0, 5.0):
        k = (int(math.floor(wx / 100.0)), int(math.floor(wy / 100.0)))
        if z0 < wall_lo.get(k, 1e12):
            wall_lo[k] = z0
        if z1 > wall_hi.get(k, -1e12):
            wall_hi[k] = z1
print('wall cells', len(wall_lo))

# Roof cells keyed by height bin (top of the tile, 1 m); only the top-most tile per cell counts as roof (floors under a
# roof are slabs, not tops).
top = {}
top_tile = {}
for i, m in enumerate(roofs):
    b = bounds[m[0]]
    zt = m[3] + b[5] * m[7]
    for wx, wy in samples(m):
        k = (int(math.floor(wx / 100.0)), int(math.floor(wy / 100.0)))
        if zt > top.get(k, -1e12):
            top[k] = zt
            top_tile[k] = i
# A tile top is a roof only if no wall in that cell rises more than 2 m above it.
roof_cells = {k: z for k, z in top.items() if wall_hi.get(k, -1e12) <= z + 200.0}
print('roof cells', len(roof_cells))

# Connected regions: 4-neighbours whose heights differ less than 60 cm.
region = {}
regions = []
for start in roof_cells:
    if start in region:
        continue
    zs = roof_cells[start]
    region[start] = len(regions)
    stack = [start]
    cells = []
    while stack:
        k = stack.pop()
        cells.append(k)
        for d in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            n = (k[0] + d[0], k[1] + d[1])
            if n in roof_cells and n not in region and abs(roof_cells[n] - roof_cells[k]) < 60.0:
                region[n] = len(regions)
                stack.append(n)
    regions.append(cells)
print('roof regions', len(regions))

import pickle
pickle.dump({'roofs': roofs, 'wall_lo': wall_lo, 'wall_hi': wall_hi, 'roof_cells': roof_cells, 'top_tile': top_tile, 'regions': regions, 'region': region}, open('nc_masses_cache.pkl', 'wb'), protocol=4)
print('cached')
