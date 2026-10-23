import re,json,collections
from pathlib import Path
OUT=Path(__file__).resolve().parents[4]/'Saved/CSSWCustomMaterial'
graphs={}
for file in OUT.glob('*.t3d'):
    stack=[]; objects={};root=None
    raw=file.read_bytes()
    for line in raw.decode('utf-16' if raw.startswith(b'\xff\xfe') else 'utf-8-sig').splitlines():
        line=line.strip()
        if line.startswith('Begin Object'):
            name=re.search(r'Name="([^"]+)"',line)[1]
            cls=re.search(r'Class=/Script/\w+\.(\w+)',line)
            export=re.search(r'ExportPath="[^\x27]*\x27([^\x27]+)',line)
            obj=objects.setdefault(name,{'name':name,'props':{}})
            if cls:obj['type']=cls[1].replace('MaterialExpression','')
            if export:obj['path']=export[1]
            stack.append(obj)
            if root is None: root=obj
        elif line=='End Object':stack.pop()
        elif stack and '=' in line:
            key,val=line.split('=',1);stack[-1]['props'][key]=val
    graphs[file.stem]={'root':root,'nodes':{k:v for k,v in objects.items() if k.startswith('MaterialExpression') and v.get('type')!='Comment'},'objects':objects}
(OUT/'graphs.json').write_text(json.dumps(graphs,indent=2),encoding='utf-8')
for name,g in graphs.items():
    if name.startswith('M_'):
        print(name,len(g['nodes']),dict(collections.Counter(n['type'] for n in g['nodes'].values())))
print('All node types',dict(collections.Counter(n['type'] for g in graphs.values() for n in g['nodes'].values())))
