# Ranks Night City's building facade modules by how often they are placed (world HISM + region chunks), and folds
# them into archetypes (role + size tokens, kit and variant stripped). Output: nc_rank.json, nc_bounds_list.json (the
# meshes placed 30 times or more, for nc_bounds_dump.py) and a text summary. Order of the analysis
# (NightCity_BuildingKit.md section 5): nc_rank.py, nc_bounds_dump.py (editor), nc_masses_prep.py, nc_shapes.py, nc_rhythm.py.
import collections
import csv
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# The data folder (nc_catalog.csv, nc_sizes.csv, nc_instances.csv, nc_chunks.csv, nc_mesh_bounds.json ...): the first
# argument, else this script's folder.
os.chdir(sys.argv[1] if len(sys.argv) > 1 else HERE)
BS = chr(92)

counts = collections.Counter()
for name in ('nc_instances.csv', 'nc_chunks.csv'):
    with open(name, encoding='utf-8') as f:
        next(f)
        for line in f:
            counts[line[:line.index(',')]] += 1

sizes = {}
for r in csv.DictReader(open('nc_sizes.csv', encoding='utf-8')):
    try:
        sizes[r['asset']] = tuple(int(round(float(r[k]))) for k in ('sx', 'sy', 'sz'))
    except ValueError:
        pass

catalog = {}
for r in csv.DictReader(open('nc_catalog.csv', encoding='utf-8')):
    catalog[r['asset']] = r['depot'].replace(BS, '/').lower()

ROLES = [
    ('window', ('window',)),
    ('door', ('door', 'entrance', 'gate', 'shutter')),
    ('corner_concave', ('concave',)),
    ('corner_convex', ('convex', '_corner')),
    ('parapet', ('cornice', 'parapet', 'ledge', 'coping', '_top')),
    ('roof', ('roof',)),
    ('floor', ('floor', 'ceiling', 'slab')),
    ('balcony', ('balcony', 'handrail', 'rail', 'railing')),
    ('canopy', ('canopy', 'awning', 'shield', 'shade', 'overhang')),
    ('pillar', ('pillar', 'column', 'beam', 'support', 'post')),
    ('stairs', ('stairs', 'stair')),
    ('wall', ('wall', 'panel', 'facade', 'backwall', '_f_')),
]


def role_of(name):
    for role, keys in ROLES:
        if any(k in name for k in keys):
            return role
    return 'other'


KIT = re.compile(r'^(.*?_building_[a-z0-9]+)_(.*)$')
VARIANT = re.compile(r'(_(a[a-z]|[a-h]|v\d+|dest_[a-z]|dst|dmg|damaged|broken|lod\d|\d{6,}.*))+$')


def archetype(name):
    m = KIT.match(name)
    rest = m.group(2) if m else name
    rest = VARIANT.sub('', rest)
    return rest


rows = []
for asset, n in counts.items():
    depot = catalog.get(asset)
    if not depot or '/architecture/' not in depot:
        continue
    file = depot.split('/')[-1].replace('.mesh', '')
    if file.startswith('int_') or '/interior' in depot:
        continue
    if 'building' not in file and 'backwall' not in depot:
        continue
    m = KIT.match(file)
    kit = m.group(1) if m else '/'.join(depot.split('/')[2:5])
    rows.append({'asset': asset, 'name': file, 'kit': kit, 'role': role_of(file), 'arch': archetype(file),
                 'size': sizes.get(asset), 'count': n})

rows.sort(key=lambda r: -r['count'])
total = sum(r['count'] for r in rows)
by_role = collections.Counter()
for r in rows:
    by_role[r['role']] += r['count']
arch = collections.defaultdict(lambda: {'count': 0, 'meshes': 0, 'kits': collections.Counter(), 'sizes': collections.Counter(), 'role': ''})
for r in rows:
    a = arch[r['role'] + ':' + r['arch']]
    a['count'] += r['count']
    a['meshes'] += 1
    a['kits'][r['kit']] += r['count']
    if r['size']:
        a['sizes'][r['size']] += r['count']
    a['role'] = r['role']
arch_rows = sorted(arch.items(), key=lambda kv: -kv[1]['count'])
kits = collections.Counter()
for r in rows:
    kits[r['kit']] += r['count']

with open('nc_rank.json', 'w', encoding='utf-8') as f:
    json.dump({"total": total, "by_role": by_role, "top": rows[:400], "all": [r for r in rows if r["count"] >= 30], 'kits': kits.most_common(60),
               'archetypes': [{'key': k, 'count': v['count'], 'meshes': v['meshes'], 'role': v['role'],
                               'kits': v['kits'].most_common(4), 'sizes': [list(s) + [c] for s, c in v['sizes'].most_common(3)]}
                              for k, v in arch_rows[:300]]}, f, indent=1, ensure_ascii=False)
with open('nc_bounds_list.json', 'w') as f:
    json.dump([r['asset'] for r in rows if r['count'] >= 30], f)

out = sys.stdout
out.write('facade modules placed: {} in {} meshes\n'.format(total, len(rows)))
out.write('by role: ' + ', '.join('{} {}'.format(k, v) for k, v in by_role.most_common()) + '\n\n')
out.write('kits:\n')
for k, v in kits.most_common(25):
    out.write('  {:45s} {}\n'.format(k, v))
out.write('\narchetypes:\n')
for k, v in arch_rows[:90]:
    s = ' '.join('{}x{}x{}({})'.format(*sz, c) for sz, c in v['sizes'].most_common(2))
    out.write('  {:7d} {:3d}m  {:55s} {:30s} {}\n'.format(v['count'], v['meshes'], k[:55], v['kits'].most_common(1)[0][0][:30], s))
out.write('\ntop meshes:\n')
for r in rows[:70]:
    out.write('  {:7d} {:14s} {:70s} {}\n'.format(r['count'], r['role'], r['name'][:70], 'x'.join(map(str, r['size'])) if r['size'] else '?'))
