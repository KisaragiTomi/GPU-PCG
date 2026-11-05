# Usage: python nc_orient.py <folder with nc_catalog.csv, nc_instances.csv, nc_chunks.csv>
# Which local side of each facade part faces out, from how Night City places it: for every placed copy, the floor and
# ceiling tiles of the same building kits near it lie inside the building, so the sign of their offset along the
# copy's local +Y says whether +Y points in or out. Also votes with the centroid of the copy's actor's kit pieces.
import csv, math, os, sys, collections
os.chdir(sys.argv[1])
BS = chr(92)

TARGETS = {
    'SM_75C359765B5FB481': 'a_wall_w300_h400_aa',
    'SM_C462863AFD826788': 'a_wall_window_w300_h400_ac',
    'SM_66F6CF0FCDB1B0BA': 'a_wall_pillar_aa',
    'SM_A0B41F62BBADFA7C': 'a_roof_cornice_w300_aa',
    'SM_042B54B9283A6DB6': 'a_roof_w300_aa',
    'SM_EC5A1AF92817B42A': 'b_wall_w200_h400_door_ac',
    'SM_6E499BBB42F56C0C': 'b_wall_w300_h400_ac',
    'SM_AE47FC650A6826CD': 'b_wall_window_w600_h400_ab',
    'SM_225E25A0A90F8038': 'b_wall_cornice_w_300_aa',
    'SM_DA6B9DA9AD1E8070': 'b_wall_w600_h400_double_door_ac',
}

interior = set()
kit = set()
for r in csv.DictReader(open('nc_catalog.csv', encoding='utf-8')):
    d = r['depot'].split(BS)[-1]
    if not d.startswith('wbr_cha_building'):
        continue
    kit.add(r['asset'])
    if '_floor_w' in d or '_ceiling_w' in d:
        interior.add(r['asset'])
print('interior refs', len(interior), 'kit meshes', len(kit))

CELL = 1000.0
grid = collections.defaultdict(list)
targets = collections.defaultdict(list)
actors = collections.defaultdict(lambda: [0.0, 0.0, 0])
for path in ('nc_instances.csv', 'nc_chunks.csv'):
    with open(path, encoding='utf-8') as f:
        f.readline()
        for line in f:
            mesh = line[:line.index(',')]
            if mesh not in kit:
                continue
            p = line.rstrip('\n').split(',')
            x, y, z, yaw = float(p[2]), float(p[3]), float(p[4]), float(p[5])
            a = actors[p[1]]
            a[0] += x; a[1] += y; a[2] += 1
            if mesh in interior:
                grid[(int(math.floor(x / CELL)), int(math.floor(y / CELL)))].append((x, y, z))
            if mesh in TARGETS:
                targets[mesh].append((x, y, z, yaw, p[1]))
    print('read', path, file=sys.stderr)

for mesh, name in TARGETS.items():
    near = collections.Counter()
    cent = collections.Counter()
    for x, y, z, yaw, actor in targets[mesh]:
        ly = (-math.sin(math.radians(yaw)), math.cos(math.radians(yaw)))
        total = 0.0
        n = 0
        cx, cy = int(math.floor(x / CELL)), int(math.floor(y / CELL))
        for gx in (cx - 1, cx, cx + 1):
            for gy in (cy - 1, cy, cy + 1):
                for qx, qy, qz in grid.get((gx, gy), ()):
                    dx, dy = qx - x, qy - y
                    if dx * dx + dy * dy > 800.0 ** 2 or not (-100.0 < qz - z < 500.0):
                        continue
                    total += dx * ly[0] + dy * ly[1]
                    n += 1
        if n:
            near['+Y in' if total > 0 else '+Y out'] += 1
        a = actors[actor]
        if a[2] > 4:
            d = (a[0] / a[2] - x) * ly[0] + (a[1] / a[2] - y) * ly[1]
            cent['+Y in' if d > 0 else '+Y out'] += 1
    print('{:34s} {:22s} copies {:6d}  floors say {}  actor centre says {}'.format(name, mesh, len(targets[mesh]), dict(near), dict(cent)))
