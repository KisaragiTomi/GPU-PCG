"""Build independent local-space surface meshes from exported source triangles.

Run with workstation Python. Dependencies: numpy, shapely, matplotlib.
Output is deterministic JSON + OBJ + geometry review plots. Does not edit Unreal assets.
"""
import collections
import hashlib
import json
from pathlib import Path
import sys
import uuid

ROOT = Path(__file__).resolve().parents[4]
WORK = ROOT/'Saved/CodexNightCityPlacement'
sys.path.insert(0,str(WORK/'pydeps'))
import numpy as np
from shapely.geometry import Polygon, GeometryCollection
from shapely.ops import unary_union, triangulate
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.collections import PolyCollection

OUT=ROOT/'Plugins/PCGPlugins/Docs/NightCity/PlacementBatch01'
OUT.mkdir(parents=True,exist_ok=True)
NS=uuid.UUID('e41722bc-5b68-4bc3-a991-16ac63226d75')
EDGE_MARGIN=2.0
MIN_AREA=2000.0  # cm^2: surface opportunity, not a guarantee that any particular device fits.
MIN_SPAN=20.0
PLANE_EPS=.05


def pieces(poly):
    if poly.is_empty: return []
    if poly.geom_type=='Polygon': return [poly]
    return [p for g in poly.geoms for p in pieces(g)]


def clip_positive(pts,n,d,epsilon=.15):
    result=[]
    for a,b in zip(pts,np.roll(pts,-1,axis=0)):
        da=float(a@n-d-epsilon); db=float(b@n-d-epsilon)
        if da>=0: result.append(a)
        if (da>=0)!=(db>=0): result.append(a+(b-a)*da/(da-db))
    return np.array(result)


def build(source):
    vertices=np.array(source['vertices'],dtype=float)
    triangles=[]
    groups=[]
    rejected=collections.Counter()
    for i,t in enumerate(source['triangles']):
        p=vertices[t[:3]]
        n=np.cross(p[2]-p[0],p[1]-p[0])  # Unreal's source winding, confirmed against imported normals.
        length=np.linalg.norm(n)
        if length<1e-7: continue
        n/=length
        shader=source['materials'][t[3]]['metadata'].get('SourceShader','').lower()
        triangles.append((p,n,shader,i))
        if not shader.endswith('multilayered.mt'):
            rejected['non_structural_material_triangles']+=1
            continue
        if abs(n[2])>.02:
            rejected['non_vertical_triangles']+=1
            continue
        imported=np.array(source['triangle_normals'][i])
        if imported.size==0 or np.mean(imported@n)<.95:
            rejected['unreliable_or_smoothed_normal_triangles']+=1
            continue
        group=next((g for g in groups if g['n']@n>.999999 and np.max(np.abs(p@g['n']-g['d']))<=PLANE_EPS),None)
        if group is None:
            group={'n':n,'d':float(p[0]@n),'ids':[],'points':[]}
            groups.append(group)
        group['ids'].append(i); group['points'].append(p)
    regions=[]
    for gi,g in enumerate(groups):
        n=g['n']; d=g['d']
        up=np.array([0.,0.,1.]); up-=n*(up@n); up/=np.linalg.norm(up)
        u=np.cross(up,n)
        basis=np.stack([u,up],axis=1)
        support=unary_union([Polygon(p@basis).buffer(0) for p in g['points']])
        if support.area<MIN_AREA: continue
        blockers=[]
        for p,_,shader,tid in triangles:
            if tid in g['ids'] or 'decal' in shader: continue
            clipped=clip_positive(p,n,d)
            if len(clipped)<3: continue
            polygon=Polygon(clipped@basis).buffer(0)
            if polygon.area>1e-4: blockers.append(polygon)
        blocked=unary_union(blockers) if blockers else GeometryCollection()
        # Remove any projected geometry in front of this plane, including window surfaces.
        visible=support.difference(blocked)
        allowed=visible.buffer(-EDGE_MARGIN,join_style='mitre')
        for pi,poly in enumerate(sorted(pieces(allowed),key=lambda p:(-p.area,p.centroid.x,p.centroid.y))):
            if poly.area<MIN_AREA: continue
            corners=np.array(poly.minimum_rotated_rectangle.exterior.coords)[:4]
            if min(np.linalg.norm(corners-np.roll(corners,1,axis=0),axis=1))<MIN_SPAN: continue
            # Keep only fully covered Delaunay triangles; reject gaps, never fill holes or concavities.
            ts=[]
            for tri in triangulate(poly):
                if poly.covers(tri): ts.append(tri)
            if not ts: continue
            covered=unary_union(ts)
            # Reject numerical boundary loss instead of silently claiming coverage.
            if poly.symmetric_difference(covered).area>max(.01,poly.area*1e-7):
                rejected['triangulation_coverage_failure']+=1
                continue
            points=[]; indices=[]; lookup={}
            for tri in ts:
                pts=np.array(tri.exterior.coords)[:3]
                a,b=pts[1]-pts[0],pts[2]-pts[0]
                if a[0]*b[1]-a[1]*b[0]<0: pts=pts[[0,2,1]]
                idx=[]
                for xy in pts:
                    key=tuple(np.round(xy,7))
                    if key not in lookup:
                        lookup[key]=len(points); points.append((basis@xy+n*d).tolist())
                    idx.append(lookup[key])
                indices.append(idx)
            # Geometric IDs are deterministic for this reviewed batch; later re-authoring must migrate IDs explicitly.
            identity=f'{source["mesh"]}|'+','.join(f'{x:.3f}' for x in n)+f'|{d:.2f}|{pi}'
            regions.append({'region_id':str(uuid.uuid5(NS,identity)), 'label':f'Surface_{gi:02d}_{pi:02d}',
                'positions':points,'triangles':indices,'outward_normal':n.tolist(),'preferred_up':up.tolist(),
                'allowed_categories':['WallMountedCandidate'], 'area_cm2':poly.area,
                'source_triangle_ids':g['ids'],'plane_d':d,'basis':[u.tolist(),up.tolist()],
                'boundary':list(map(list,poly.exterior.coords)), 'holes':[list(map(list,h.coords)) for h in poly.interiors],
                'support_area_cm2':support.area,'occluded_area_cm2':support.intersection(blocked).area,
                'checks':{'outside_source_support_cm2':covered.difference(support).area,'projected_obstacle_overlap_cm2':covered.intersection(blocked).area,'triangulation_gap_cm2':poly.symmetric_difference(covered).area}})
    # These are geometric candidates. A ready asset still requires building-use and device checks at placement time.
    profile={'schema_version':1,'data_revision':1,'profile_id':str(uuid.uuid5(NS,source['mesh'])),
        'source_mesh':source['path'],'mesh':source['mesh'],'depot':source['depot'],'loaded_instances':source['loaded_instances'],
        'source_geometry_hash':source['source_geometry_hash'], 'state':'Ready' if regions else 'PendingReview',
        'review_notes':'Built from source LOD0. Multilayer opaque surfaces only; window/parallax/decal support excluded; forward projected blockers removed; 2 cm inset. Geometric candidate regions only. Building exterior/use, device footprint, substrate strength and neighbouring objects must be checked separately. No load-bearing certification.',
        'regions':regions,'rejections':dict(rejected),'edge_margin_cm':EDGE_MARGIN,'min_area_cm2':MIN_AREA,'min_span_cm':MIN_SPAN,
        'winding':'Profile triangles use cross(B-A,C-A) == outward normal; reverse index order for UE render mesh authoring.'}
    return profile


def plot(source,profile,ax):
    v=np.array(source['vertices']); polys=[]; colors=[]
    for t in source['triangles']:
        p=v[t[:3]]
        # Front/back flattened review: profile projections plus source material classification.
        polys.append(p[:,[0,2]])
        sh=source['materials'][t[3]]['metadata'].get('SourceShader','')
        colors.append('#e3a276' if 'window' in sh else '#a2a5ab' if 'decal' not in sh else '#c0c0c0')
    ax.add_collection(PolyCollection(polys,facecolors=colors,edgecolors='#555555',linewidths=.18,alpha=.45))
    for r in profile['regions']:
        verts=np.array(r['positions'])
        ax.add_collection(PolyCollection([verts[t][:,[0,2]] for t in r['triangles']],facecolors='#20a777',edgecolors='#006b48',linewidths=.45,alpha=.65))
    ax.autoscale(); ax.set_aspect('equal'); ax.grid(alpha=.15)
    label=source['depot'].split('\\')[-1].removesuffix('.mesh')
    label=label.replace('wbr_cha_building_','CH_').replace('cct_cpz_building_','CP_')
    ax.set_title(label+'\n'+f'{len(profile["regions"])} regions | {sum(r["area_cm2"] for r in profile["regions"])/10000:.2f} m2',fontsize=8)
    ax.tick_params(labelsize=6)


def main():
    sources=json.loads((WORK/'sources.json').read_text(encoding='utf-8'))
    assert len(sources)==24 and not any('error' in s for s in sources)
    profiles=[]
    fig,axes=plt.subplots(4,6,figsize=(19,15))
    for s,ax in zip(sources,axes.flat):
        source=json.loads((WORK/(s['mesh']+'.json')).read_text(encoding='utf-8'))
        profile=build(source)
        for r in profile['regions']:
            assert r['checks']['outside_source_support_cm2']<.01
            assert r['checks']['projected_obstacle_overlap_cm2']<.01
        (OUT/(s['mesh']+'.json')).write_text(json.dumps(profile,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
        lines=['# Source local space (cm); mathematical CCW, normal points outward.']; offset=1
        for r in profile['regions']:
            lines.append('g '+r['label'])
            lines.extend('v '+' '.join(f'{x:.6f}' for x in p) for p in r['positions'])
            lines.extend('f '+' '.join(str(offset+i) for i in t) for t in r['triangles'])
            offset+=len(r['positions'])
        (OUT/(s['mesh']+'.obj')).write_text('\n'.join(lines)+'\n',encoding='utf-8')
        plot(source,profile,ax)
        profiles.append({k:v for k,v in profile.items() if k!='regions'} | {'region_count':len(profile['regions']),'triangle_count':sum(len(r['triangles']) for r in profile['regions']),'area_m2':sum(r['area_cm2'] for r in profile['regions'])/10000})
    fig.suptitle('Night City placement surfaces — source local X/Z projection\nGreen: candidate support, orange: window material, grey: source mesh. Both depth sides overlap in this overview.',fontsize=13)
    fig.tight_layout(rect=[0,0,1,.95]); fig.savefig(OUT/'overview.png',dpi=160); plt.close(fig)
    manifest={'batch':'PlacementBatch01','date':'2026-11-05','builder_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'models':profiles}
    (OUT/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps([(p['mesh'],p['state'],p['region_count'],round(p['area_m2'],2)) for p in profiles]))


if __name__=='__main__': main()
