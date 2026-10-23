"""Translate exported CSSW numeric expression islands to typed Custom HLSL.

Run prepare.py inside Unreal to export dependencies and generate this plan.
The original material, its instances, and engine functions are never edited.
"""
import json, re, functools
from pathlib import Path

ROOT=Path(__file__).resolve().parents[4]
OUT=ROOT/'Saved/CSSWCustomMaterial'
G=json.loads((OUT/'graphs.json').read_text(encoding='utf-8'))
META=json.loads((OUT/'metadata.json').read_text(encoding='utf-8'))
BINARY={'Add':'+','Subtract':'-','Multiply':'*','Divide':'/','Min':'min','Max':'max','DotProduct':'dot','CrossProduct':'cross','Distance':'distance'}
UNARY={'Abs':'abs','Saturate':'saturate','Normalize':'normalize','SquareRoot':'sqrt','Frac':'frac','Floor':'floor','Ceil':'ceil','Sign':'sign','Length':'length','OneMinus':None}
MATH=set(BINARY)|set(UNARY)|{'AppendVector','ComponentMask','LinearInterpolate','Power','Constant','Constant2Vector','Constant3Vector','Constant4Vector','Sine','Cosine','Clamp','DeriveNormalZ'}
CONSTANTS={'Constant','Constant2Vector','Constant3Vector','Constant4Vector'}
ATTR3={'BaseColor','EmissiveColor','Normal','WorldPositionOffset','WorldDisplacement','SubsurfaceColor','Tangent'}

def conn(raw):
    m=re.search(r'Expression="[^\x27]*\x27[^\x27]*:(MaterialExpression\w+)\x27',raw)
    if not m:return None
    i=re.search(r'OutputIndex=(-?\d+)',raw)
    return (m[1],int(i[1]) if i else 0)

def fname(n):
    return n['props']['MaterialFunction'].split("'")[1].split('.')[-1]

def output_name(n,i):
    if i==0:return ''
    raw=n['props'].get(f'Outputs({i})','')
    m=re.search(r'OutputName="([^"]*)"',raw)
    if m:return m[1]
    if n['type'] in ('VectorParameter','VertexColor','TextureSample','TextureSampleParameter2D'):
        return ['RGB','R','G','B','A','RGBA'][i]
    return ''

def dflt(g,n,key,default):
    return META[g]['nodes'][n['name']].get('defaults',{}).get(key,default)

def input_width(g,n,key,default=1):
    c=conn(n['props'].get(key,''))
    return width(g,*c) if c else default

@functools.lru_cache(None)
def width(g,name,i=0):
    n=G[g]['nodes'][name];t=n['type'];p=n['props']
    w=lambda key,default=1:input_width(g,n,key,default)
    if t in ('ScalarParameter','Constant','Time','PixelDepth','SceneDepth','SceneDepthWithoutWater','StaticBool','StaticBoolParameter'):return 1
    if t=='Constant2Vector' or t=='TextureCoordinate':return 2
    if t=='Constant3Vector':return 3
    if t=='Constant4Vector':return 4
    if t in ('VectorParameter','VertexColor','TextureSample','TextureSampleParameter2D'):return 3 if i==0 else (4 if i==5 else 1)
    if t in ('WorldPosition','ActorPositionWS','CameraVectorWS','CameraPositionWS','VertexNormalWS','PixelNormalWS','SkyAtmosphereLightDirection'):return 3
    if t=='CollectionParameter':return 4
    if t=='ScreenPosition':return 2 if i==0 else 4
    if t=='FunctionInput':return {'FunctionInput_Scalar':1,'FunctionInput_Vector2':2,'FunctionInput_Vector3':3,'FunctionInput_Vector4':4,'FunctionInput_StaticBool':1}.get(p.get('InputType','FunctionInput_Vector3'),0)
    if t=='FunctionOutput':return w('A')
    if t=='GetMaterialAttributes':
        if i==0:return 0
        k=output_name(n,i).replace(' ','')
        return 3 if k in ATTR3 else (2 if k.startswith('CustomizedUV') else 1)
    if t=='MaterialFunctionCall':
        target=fname(n)
        out=p.get(f'FunctionOutputs({i})','')
        guid=re.search(r'ExpressionOutputId=(\w+)',out)
        candidates=[x for x in G[target]['nodes'].values() if x['type']=='FunctionOutput' and (x['props'].get('Id')==guid[1] if guid else False)]
        if not candidates:raise ValueError(f'Unknown function output {g}/{name}/{i}')
        return input_width(target,candidates[0],'A')
    if t=='Custom':
        kind=p.get('OutputType','CMOT_Float3') if i==0 else re.search(r'OutputType=(\w+)',p.get(f'AdditionalOutputs({i-1})',''))[1]
        return int(kind[-1]) if kind[-1].isdigit() else 0
    if t in ('Reroute','NamedRerouteDeclaration','VertexInterpolator'):return w('Input')
    if t=='NamedRerouteUsage':
        target=p['Declaration'].split(':')[-1].split("'")[0]
        return width(g,target)
    if t in ('StaticSwitch','StaticSwitchParameter'):return max(w('A'),w('B'))
    if t=='ShaderStageSwitch':return max(w('PixelShader'),w('VertexShader'))
    if t in ('FeatureLevelSwitch','ShadingPathSwitch'):return w('Default')
    if t=='If':return max(w('AGreaterThanB'),w('ALessThanB'),w('AEqualsB'))
    if t in ('MakeMaterialAttributes','SetMaterialAttributes'):return 0
    if t in ('DotProduct','Distance','Length'):return 1
    if t in ('CrossProduct','DeriveNormalZ'):return 3
    if t=='AppendVector':return w('A')+w('B')
    if t=='ComponentMask':return sum(dflt(g,n,k,False) for k in 'rgba')
    if t in BINARY:return max(w('A'),w('B'))
    if t=='Normalize':return w('VectorInput')
    if t in UNARY or t in ('Sine','Cosine','Clamp'):return w('Input')
    if t=='LinearInterpolate':return max(w('A'),w('B'),w('Alpha'))
    if t=='Power':return w('Base')
    raise ValueError(f'Unknown width {g}/{name}:{t}')

def lit(v):return format(float(v),'.9g')+('' if '.' in format(float(v),'.9g') or 'e' in format(float(v),'.9g') else '.0')
def htype(w):return 'float' if w==1 else f'float{w}'

def plan_graph(g):
    nodes=G[g]['nodes'];meta=META[g]['nodes'];convert=set();skipped={}
    for name,n in nodes.items():
        if n['type'] not in MATH:continue
        try:
            w=width(g,name)
            if not 1<=w<=4:raise ValueError('non numeric/invalid width '+str(w))
            # Require known numeric boundary widths; disconnected invalid math stays untouched.
            for raw in n['props'].values():
                c=conn(raw)
                if c and width(g,*c)<1:raise ValueError('non numeric input')
            convert.add(name)
        except (ValueError,KeyError,RecursionError) as e:skipped[name]=str(e)
    roots=set()
    for name,info in meta.items():
        if name not in convert:
            roots.update(x for x in info['input_nodes'] if x in convert)
    roots.update(x['node'] for x in META[g]['roots'].values() if x['node'] in convert)
    # Keep constants as parameter-like inputs unless consumed by a translated island.
    for name in list(roots):
        if nodes[name]['type'] in CONSTANTS:
            convert.remove(name);roots.remove(name)
    groups=[]
    for root in sorted(roots):
        boundary={};lines=[];cache={};included=[]
        def emit(name,i=0):
            n=nodes[name];t=n['type'];p=n['props']
            if name not in convert:
                key=(name,i)
                if key not in boundary:boundary[key]='In'+str(len(boundary))
                return boundary[key]
            if name in cache:return cache[name]
            w=width(g,name)
            def arg(k,d=0):
                c=conn(p.get(k,''))
                return emit(*c) if c else lit(d)
            a=lambda:arg('A',dflt(g,n,'const_a',0))
            b=lambda:arg('B',dflt(g,n,'const_b',1))
            if t=='Constant':expr=lit(dflt(g,n,'r',0))
            elif t=='Constant2Vector':expr=f"float2({lit(dflt(g,n,'r',0))}, {lit(dflt(g,n,'g',0))})"
            elif t in ('Constant3Vector','Constant4Vector'):
                vals=dflt(g,n,'constant',[0,0,0,0]);expr=f'{htype(w)}('+', '.join(lit(v) for v in vals[:w])+')'
            elif t in BINARY:
                aa,bb=a(),b();op=BINARY[t]
                expr=f'({aa} {op} {bb})' if op in '+-*/' else f'{op}({aa}, {bb})'
            elif t in UNARY:
                x=arg('VectorInput' if t=='Normalize' else 'Input');expr=f'(1.0 - {x})' if t=='OneMinus' else f'{UNARY[t]}({x})'
            elif t=='AppendVector':expr=f'{htype(w)}({arg("A")}, {arg("B")})'
            elif t=='ComponentMask':
                x=arg('Input');iw=input_width(g,n,'Input');mask=''.join(c for c in 'rgba' if dflt(g,n,c,False))
                expr=f'({x}).{mask}' if iw>1 else (x if w==1 else f'{htype(w)}({x})')
            elif t=='LinearInterpolate':expr=f'lerp({a()}, {b()}, {arg("Alpha",dflt(g,n,"const_alpha",0.5))})'
            elif t=='Power':expr=f'PositiveClampedPow({arg("Base")}, {arg("Exponent",dflt(g,n,"const_exponent",2))})'
            elif t in ('Sine','Cosine'):
                period=dflt(g,n,'period',1);expr=f'{"sin" if t=="Sine" else "cos"}({arg("Input")} * {lit(6.283185307179586/period if period>0 else 1)})'
            elif t=='Clamp':expr=f'clamp({arg("Input")}, {arg("Min",dflt(g,n,"min_default",0))}, {arg("Max",dflt(g,n,"max_default",1))})'
            elif t=='DeriveNormalZ':
                x=arg('InXY');expr=f'float3({x}, sqrt(saturate(1.0 - dot({x}, {x}))))'
            else:raise ValueError(t)
            var='v'+str(len(lines));cache[name]=var;included.append(name)
            lines.append(f'{htype(w)} {var} = {expr}; // {name}')
            return var
        value=emit(root)
        code='// CSSW: direct translation; preserve original operation order.\n'+'\n'.join(lines)+f'\nreturn {value};\n'
        groups.append({'root':root,'width':width(g,root),'code':code,'included':included,'inputs':[{'node':n,'index':i,'output':output_name(nodes[n],i),'name':v,'width':width(g,n,i)} for (n,i),v in boundary.items()]})
    return {'path':META[g]['path'],'before':len(nodes),'convert':sorted(convert),'groups':groups,'skipped':skipped}

def main():
    plans={g:plan_graph(g) for g in META if META[g]['path'].startswith('/PCGPlugins/')}
    (OUT/'plan.json').write_text(json.dumps(plans,indent=2),encoding='utf-8')
    print('graphs',len(plans),'replaced',sum(len(x['convert']) for x in plans.values()),'custom',sum(len(x['groups']) for x in plans.values()))
    for g,p in plans.items():print(g,p['before'],'->',p['before']-len(p['convert'])+len(p['groups']),'custom',len(p['groups']),'skip',p['skipped'])
if __name__=='__main__':main()
