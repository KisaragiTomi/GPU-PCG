"""Synthetic regressions for support holes, visibility, material semantics and normals."""
import json
import unittest
from BuildPlacementBatch import build, OUT
from shapely.geometry import Polygon, Point
from shapely.ops import unary_union


def source(rectangles):
    s={'mesh':'Test','path':'/Test/Test.Test','depot':'Test','loaded_instances':1,'source_geometry_hash':'0'*64,
       'vertices':[],'triangles':[],'triangle_normals':[],
       'materials':[{'metadata':{'SourceShader':'engine/materials/multilayered.mt'}},{'metadata':{'SourceShader':'base/materials/window_parallax_interior.mt'}},{'metadata':{'SourceShader':'base/materials/mesh_decal_parallax.mt'}}]}
    for x0,z0,x1,z1,y,material in rectangles:
        offset=len(s['vertices'])
        s['vertices'].extend([[x0,y,z0],[x1,y,z0],[x1,y,z1],[x0,y,z1]])
        s['triangles'].extend([[offset,offset+1,offset+2,material],[offset,offset+2,offset+3,material]])
        s['triangle_normals'].extend([[[0,1,0]]*3,[[0,1,0]]*3])
    return s


def coverage(profile):
    return unary_union([Polygon([[r['positions'][i][0],r['positions'][i][2]] for i in t]) for r in profile['regions'] for t in r['triangles']])


class PlacementGeometryTests(unittest.TestCase):
    def test_plain_wall_inset(self):
        p=build(source([(0,0,500,400,0,0)]))
        self.assertEqual(len(p['regions']),1)
        self.assertAlmostEqual(coverage(p).area,496*396)

    def test_window_hole_preserved(self):
        p=build(source([(0,0,150,400,0,0),(350,0,500,400,0,0),(150,0,350,100,0,0),(150,300,350,400,0,0),(150,100,350,300,0,1)]))
        self.assertEqual(len(p['regions'][0]['holes']),1)
        self.assertFalse(coverage(p).covers(Point(250,200)))
        self.assertEqual(coverage(p).intersection(Polygon([(150,100),(350,100),(350,300),(150,300)])).area,0)

    def test_forward_window_blocks_support(self):
        p=build(source([(0,0,500,400,0,0),(150,100,350,300,10,1)]))
        self.assertFalse(coverage(p).covers(Point(250,200)))
        self.assertTrue(coverage(p).covers(Point(50,200)))

    def test_rear_window_does_not_block(self):
        p=build(source([(0,0,500,400,0,0),(150,100,350,300,-10,1)]))
        self.assertAlmostEqual(coverage(p).area,496*396)

    def test_decal_does_not_block(self):
        p=build(source([(0,0,500,400,0,0),(150,100,350,300,10,2)]))
        self.assertAlmostEqual(coverage(p).area,496*396)

    def test_window_only_stays_pending(self):
        p=build(source([(0,0,500,400,0,1)]))
        self.assertEqual(p['state'],'PendingReview')
        self.assertEqual(p['regions'],[])

    def test_inconsistent_normals_rejected(self):
        s=source([(0,0,500,400,0,0)])
        s['triangle_normals']=[[[0,-1,0]]*3]*2
        self.assertEqual(build(s)['regions'],[])


if __name__=='__main__':
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(PlacementGeometryTests)
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    (OUT/'geometry_tests.json').write_text(json.dumps({'success':result.wasSuccessful(),'tests':result.testsRun,'failures':len(result.failures),'errors':len(result.errors)},indent=2)+'\n',encoding='utf-8')
    raise SystemExit(0 if result.wasSuccessful() else 1)
