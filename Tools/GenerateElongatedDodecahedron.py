"""Удлинённый додекаэдр - ячейка Вороного ОЦК-подрешётки, растянутой по Z вдвое.

Четвёртая из пяти фёдоровских форм (CellShapePresets.cpp). Строится не по
списку граней, а выпуклой оболочкой 18 вершин, и уже потом сверяется с
контрольными числами пресета: так ошибка в порядке обхода грани невозможна в
принципе, а несовпадение с ожидаемым останавливает скрипт до экспорта.

Система координат - единицы шага решётки в плоскости (S = 1):
    |x| <= 1, |y| <= 1, |z| <= (3 - |x| - |y|) / 2
Габарит 2 x 2 x 3, объём ровно 8. Рендерер нормирует меш по X-габариту
(InstancedMeshCellGridRenderer.cpp), поэтому важны только пропорции.
"""

import sys
import bpy
import bmesh
from mathutils import Vector

EXPECTED_VERTS = 18
EXPECTED_EDGES = 28
EXPECTED_FACES = 12
EXPECTED_SIDE_COUNTS = {4: 8, 6: 4}   # 8 ромбов + 4 шестиугольника
EXPECTED_VOLUME = 8.0
EXPECTED_AABB = (2.0, 2.0, 3.0)
EPS = 1e-6

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
out_fbx = argv[0] if argv else "ElongatedDodecahedron.fbx"

# --- 18 вершин ---------------------------------------------------------------
verts = []
for sx in (1, -1):
    for sy in (1, -1):
        for sz in (1, -1):
            verts.append(Vector((sx * 1.0, sy * 1.0, sz * 0.5)))   # 8 углов
for sz in (1, -1):
    verts.append(Vector((0.0, 0.0, sz * 1.5)))                     # 2 апекса
    for s in (1, -1):
        verts.append(Vector((s * 1.0, 0.0, sz * 1.0)))             # 4 на +-X
        verts.append(Vector((0.0, s * 1.0, sz * 1.0)))             # 4 на +-Y

assert len(verts) == EXPECTED_VERTS, f"вершин {len(verts)}, а не {EXPECTED_VERTS}"

# Каждая вершина обязана лежать НА границе тела, иначе оболочка её проглотит.
for v in verts:
    limit = (3.0 - abs(v.x) - abs(v.y)) / 2.0
    on_surface = (abs(abs(v.x) - 1.0) < EPS or abs(abs(v.y) - 1.0) < EPS
                  or abs(abs(v.z) - limit) < EPS)
    assert on_surface, f"вершина {v[:]} не на границе"

# --- выпуклая оболочка + слияние компланарных треугольников -------------------
bm = bmesh.new()
for v in verts:
    bm.verts.new(v)
bm.verts.ensure_lookup_table()
bmesh.ops.convex_hull(bm, input=bm.verts, use_existing_faces=False)
bmesh.ops.dissolve_limit(bm, angle_limit=0.001,
                         verts=bm.verts[:], edges=bm.edges[:], delimit=set())
bm.normal_update()

# --- сверка с пресетом (до экспорта) ------------------------------------------
side_counts = {}
for f in bm.faces:
    side_counts[len(f.verts)] = side_counts.get(len(f.verts), 0) + 1

volume = bm.calc_volume()
xs = [v.co.x for v in bm.verts]
ys = [v.co.y for v in bm.verts]
zs = [v.co.z for v in bm.verts]
aabb = (max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs))

problems = []
if len(bm.verts) != EXPECTED_VERTS:
    problems.append(f"вершин {len(bm.verts)} вместо {EXPECTED_VERTS}")
if len(bm.edges) != EXPECTED_EDGES:
    problems.append(f"рёбер {len(bm.edges)} вместо {EXPECTED_EDGES}")
if len(bm.faces) != EXPECTED_FACES:
    problems.append(f"граней {len(bm.faces)} вместо {EXPECTED_FACES}")
if side_counts != EXPECTED_SIDE_COUNTS:
    problems.append(f"грани по числу сторон {side_counts} вместо {EXPECTED_SIDE_COUNTS}")
if abs(volume - EXPECTED_VOLUME) > 1e-5:
    problems.append(f"объём {volume:.6f} вместо {EXPECTED_VOLUME}")
for axis, got, want in zip("XYZ", aabb, EXPECTED_AABB):
    if abs(got - want) > 1e-5:
        problems.append(f"габарит по {axis} = {got:.6f} вместо {want}")

# Ромбы обязаны быть именно ромбами: все четыре стороны равны.
for f in bm.faces:
    if len(f.verts) == 4:
        lens = [e.calc_length() for e in f.edges]
        if max(lens) - min(lens) > 1e-6:
            problems.append(f"четырёхугольник не ромб: стороны {lens}")
            break

print("=" * 62)
print(f"вершин {len(bm.verts)} / рёбер {len(bm.edges)} / граней {len(bm.faces)}")
print(f"граней по числу сторон: {side_counts}")
print(f"объём {volume:.6f}   габарит {aabb[0]:.4f} x {aabb[1]:.4f} x {aabb[2]:.4f}")
print("=" * 62)

if problems:
    for p in problems:
        print("НЕСОВПАДЕНИЕ: " + p)
    sys.exit(1)

# --- UV: в UV.x лежит РАССТОЯНИЕ ДО РЕБРА, а не текстурная координата ---------
# Узел материала берёт d = UV.x напрямую и рисует кант там, где d < W (с
# антиалиасингом по ddx/ddy и затуханием, когда клетка мельче канта). То есть
# расстояние обязан поставлять меш.
#
# ПОЧЕМУ ВЕЕРОМ ОТ ЦЕНТРА. Расстояние до БЛИЖАЙШЕГО ребра - это минимум по
# рёбрам, функция не линейная, и по вершинам её не задать: вершины лежат НА
# рёбрах, значит во всех них d = 0, а интерполяция дала бы ноль по всей грани.
# Но расстояние до ОДНОГО ребра линейно, поэтому грань разбивается веером
# (bmesh.ops.poke), и в каждом треугольнике d задаётся по своему основанию: 0 в
# двух граничных вершинах и высота треугольника в центральной. Барицентрическая
# интерполяция такой функции даёт ТОЧНОЕ перпендикулярное расстояние до этого
# ребра. У края, где кант и рисуется, ближайшее ребро - как раз основание
# своего треугольника, так что приближение точно именно там, где важно.
#
# Центральная вершина у всех треугольников грани общая, но UV в Blender живут
# НА ПЕТЛЯХ, а не на вершинах, поэтому каждый треугольник кладёт в неё свою
# высоту - и рёбра с разным расстоянием до центра (у шестиугольника они разные)
# получают одинаковую ширину канта.
#
# d измеряется в единицах шага решётки в плоскости: меш нормируется по
# X-габариту, так что W = 0.04 значит кант шириной 4% шага решётки.
#
# Прежний вариант (аффинная развёртка ромба в единичный квадрат под формулу
# min(u,1-u,v,1-v)) отсюда убран: он давал кант максимум на четырёх сторонах из
# шести и требовал от материала предположения, что грань - четырёхугольник.
face_centers = [f.calc_center_median().copy() for f in bm.faces]
bmesh.ops.poke(bm, faces=bm.faces[:])
bm.verts.ensure_lookup_table()

uv_layer = bm.loops.layers.uv.new("UVMap")
max_edge_distance = 0.0
for f in bm.faces:
    if len(f.verts) != 3:
        print(f"НЕСОВПАДЕНИЕ: после веера грань из {len(f.verts)} вершин, а не треугольник")
        sys.exit(1)

    # Центральная вершина - та, что появилась от poke, то есть совпала с ранее
    # запомненным центром грани. Две остальные лежат на исходном ребре.
    apex_loop = None
    for loop in f.loops:
        if any((loop.vert.co - c).length < EPS for c in face_centers):
            apex_loop = loop
            break
    if apex_loop is None:
        print("НЕСОВПАДЕНИЕ: у треугольника не нашлось центральной вершины")
        sys.exit(1)

    base = [l for l in f.loops if l is not apex_loop]
    edge_dir = (base[1].vert.co - base[0].vert.co)
    edge_len = edge_dir.length
    to_apex = apex_loop.vert.co - base[0].vert.co
    # Высота треугольника над основанием - она же расстояние от центра грани до
    # этого ребра. Через площадь, чтобы не строить нормаль отдельно.
    height = to_apex.cross(edge_dir).length / max(edge_len, 1e-12)
    max_edge_distance = max(max_edge_distance, height)

    # u = расстояние/2, чтобы диапазон был 0..0.5 - тот же, что даёт
    # min(u, 1-u, v, 1-v) на кубе (в центре грани ровно 0.5).
    #
    # v держится у 0.5 и разъезжается лишь на +-0.01. Причина в том, что
    # материал считает минимум ПО ЧЕТЫРЁМ величинам, и член min(v, 1-v) обязан
    # никогда не выигрывать: при v = 0 и v = 1 он обращается в ноль по углам
    # основания, и грань покрывают огромные тёмные клинья от углов - это и была
    # исходная поломка. При v в [0.49, 0.51] член по v не меньше 0.49, то есть
    # больше любого разумного канта, и формула тождественно сводится к u.
    #
    # Совсем постоянным v сделать нельзя: тогда треугольник вырождается в
    # отрезок в UV, и тангенциальный базис становится неопределённым. Отсюда
    # маленький, но ненулевой разброс.
    base[0][uv_layer].uv = (0.0, 0.49)
    base[1][uv_layer].uv = (0.0, 0.51)
    apex_loop[uv_layer].uv = (height * 0.5, 0.5)

# Контроль: на любом ребре d обязан быть нулём, а в центре грани - строго
# положительным. Ошибка здесь не роняет ни импорт, ни рендер - она молча
# стирает кант, что и было исходной проблемой, поэтому проверка явная.
zero_at_edge = all(
    abs(l[uv_layer].uv.x) < EPS
    for f in bm.faces for l in f.loops
    if not any((l.vert.co - c).length < EPS for c in face_centers))
if not zero_at_edge or max_edge_distance <= 0.0:
    print("НЕСОВПАДЕНИЕ: расстояние до ребра записано неверно")
    sys.exit(1)
print(f"UV.x = расстояние до ребра, максимум {max_edge_distance:.4f} "
      f"(в единицах шага решётки); треугольников {len(bm.faces)}")

for f in bm.faces:
    f.smooth = False   # плоское затенение: грани клетки должны читаться гранями

# --- в сцену и на экспорт -----------------------------------------------------
bpy.ops.wm.read_factory_settings(use_empty=True)
mesh = bpy.data.meshes.new("ElongatedDodecahedron")
bm.to_mesh(mesh)
bm.free()
obj = bpy.data.objects.new("ElongatedDodecahedron", mesh)
# Материальный слот нужен не ради самого материала (в движке назначается свой),
# а ради полигон-группы: FBX без единой группы даёт на импорте
# "LogStaticMesh: Error: Bad MeshDescription", потому что у описания меша не
# оказывается ни одной секции.
mesh.materials.append(bpy.data.materials.new("CellSurface"))
bpy.context.collection.objects.link(obj)
bpy.context.view_layer.objects.active = obj
obj.select_set(True)

bpy.ops.export_scene.fbx(
    filepath=out_fbx,
    use_selection=True,
    apply_unit_scale=True,
    axis_forward="X",
    axis_up="Z",          # UE тоже Z-up: экспортируем как есть, без поворота
    mesh_smooth_type="FACE",
)
print(f"ЭКСПОРТ: {out_fbx}")
