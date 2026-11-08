"""Render a frozen assembled-scene example. This does not generate placement permission meshes."""
import json,itertools
from pathlib import Path
import numpy as np
from scipy.spatial.transform import Rotation
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from mpl_toolkits.mplot3d.art3d import Poly3DCollection
from matplotlib.patches import Patch
from matplotlib.colors import to_rgb

ROOT=Path(__file__).resolve().parents[4]
WORK=ROOT/'Saved/CodexNightCityAssembled'
OUT=ROOT/'Plugins/PCGPlugins/Docs/NightCity/AssembledExample01'
OUT.mkdir(parents=True,exist_ok=True)
c=json.loads((OUT/'context.json').read_text(encoding='utf-8'))
g=json.loads((WORK/'geometry.json').read_text(encoding='utf-8'))['geometry']
for key,expected in c['source_geometry_hashes'].items():
    if key not in g or g[key]['source_geometry_hash']!=expected:
        raise RuntimeError('Source geometry changed; review and update the example before rendering: '+key)
origin=np.array([-192300.,20000.,0.])
lo=np.array(c['roi_min_cm']); hi=np.array(c['roi_max_cm'])
by_kind={'architecture':[],'window':[],'unit':[],'mount':[]}
for item in c['instances']:
    mesh=g[item['mesh']]; r=item['transform']; name=c['meshes'][item['mesh']]['depot']
    vertices=(np.array(mesh['vertices'])*r[7:10])@Rotation.from_quat(r[3:7]).as_matrix().T+r[:3]
    for t in mesh['triangles']:
        tri=vertices[t[:3]]
        if np.any(tri.max(0)<lo) or np.any(tri.min(0)>hi):continue
        kind='mount' if '_mounting' in name else 'unit' if item['role']=='aircon' else 'window' if 'window' in mesh['materials'][t[3]]['metadata'].get('SourceShader','').lower() else 'architecture'
        by_kind[kind].append((tri-origin)/100)

colors={'architecture':'#bcc4cf','window':'#92b9ce','unit':'#dd743a','mount':'#4f46a5'}
labels={'architecture':'Assembled architecture','window':'Window material','unit':'Existing aircon body','mount':'Separate mounting piece'}

def clip(poly,low,high):
    for axis in range(3):
        for bound,sign in [(low[axis],1),(high[axis],-1)]:
            result=[]
            if len(poly)<3:return []
            for a,b in zip(poly,np.roll(poly,-1,axis=0)):
                da=(a[axis]-bound)*sign; db=(b[axis]-bound)*sign
                if da>=0:result.append(a)
                if (da>=0)!=(db>=0):result.append(a+(b-a)*da/(da-db))
            poly=np.array(result)
    return poly

fig=plt.figure(figsize=(16,8),facecolor='white')
for i,(elev,azim,title,limits) in enumerate([(20,-30,'Original assembled facade neighbourhood',None),(10,-8,'Existing units and their mounting pieces',([-1,3],[-3,4],[10,14]))]):
    ax=fig.add_subplot(1,2,i+1,projection='3d')
    low=np.array([x[0] for x in limits]) if limits else (lo-origin)/100
    high=np.array([x[1] for x in limits]) if limits else (hi-origin)/100
    polys=[]; shades=[]
    for kind in by_kind:
        for tri in by_kind[kind]:
            poly=clip(tri,low,high)
            if len(poly)<3:continue
            n=np.cross(tri[1]-tri[0],tri[2]-tri[0]); length=np.linalg.norm(n)
            light=.65+.35*abs(n@np.array([.8,.3,.5]))/length if length>1e-12 else .8
            polys.append(poly); shades.append(np.array(to_rgb(colors[kind]))*light)
    ax.add_collection3d(Poly3DCollection(polys,facecolors=shades,edgecolors='none',alpha=1.,zsort='average'))
    if limits:
        ax.set_xlim(*limits[0]);ax.set_ylim(*limits[1]);ax.set_zlim(*limits[2]);ax.set_box_aspect([4,7,4])
    else:
        ax.set_xlim((lo[0]-origin[0])/100,(hi[0]-origin[0])/100)
        ax.set_ylim((lo[1]-origin[1])/100,(hi[1]-origin[1])/100)
        ax.set_zlim(5,28);ax.set_box_aspect([12,21,23])
    ax.view_init(elev=elev,azim=azim)
    ax.set_title(title,fontsize=12,pad=12)
    ax.set_xlabel('Local X (m)');ax.set_ylabel('Local Y (m)');ax.set_zlabel('World Z (m)')
fig.suptitle('NightCity_WP: inspect the assembly before defining placement regions',fontsize=16,y=.98)
fig.legend(handles=[Patch(color=colors[k],label=labels[k]) for k in colors],loc='lower center',ncol=4,bbox_to_anchor=(.5,.045),frameon=False)
fig.text(.5,.015,'Cached original world transforms + current source LOD0 geometry. No green permission mask; building interior/exterior is not yet classified.',ha='center',fontsize=9,color='#4d5562')
fig.subplots_adjust(left=.01,right=.99,top=.9,bottom=.13,wspace=.03)
fig.savefig(OUT/'assembly-context.png',dpi=160);plt.close(fig)
print(str(OUT/'assembly-context.png'))
