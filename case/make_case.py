# Bambu-printable shell for the ES3C28P board.
# Board outline: 50.0 mm across, 85.8 mm from the MIC end to the USB end.
# Units are millimeters. STL vertices are written in millimeters.
import bpy
import math

PCB_L = 85.8
PCB_W = 50.0
# Glass is the full board width. Its top edge is 8.8 mm from the MIC end.
GLASS_L = 69.2
GLASS_W = 50.0
PCB_T = 1.5
STACK = 6.0  # board plus screen, from the glass face to the back of the board
SCREEN_T = STACK - PCB_T  # 4.5 mm, the screen standing off the board
GLASS_FROM_MIC = 8.8
# Buttons sit on the back, 1 mm in from the USB end and 10.8 mm in from each
# long edge. Body is 4.2 mm across, 3.3 mm toward the MIC, 2.7 mm tall.
# The opening covers that body either way around, so a swapped 4.2/3.3 still
# lines up under the hole.
BTN_W = 4.2
BTN_D = 3.3
BTN_H = 2.7
BTN_FROM_END = 1.0
BTN_FROM_SIDE = 10.8
WALL = 2.0  # was 1.0; the printed wall was too thin
TOL = 0.2  # extra clearance on every fit, per side
GAP = 0.4 + TOL
IN_L = PCB_L + GAP * 2
IN_W = PCB_W + GAP * 2
OUT_L = IN_L + WALL * 2
OUT_W = IN_W + WALL * 2
# Bezel face and glass face are the same plane. The board pocket starts at the
# back of the 4.5 mm screen and ends just past the 1.5 mm board.
FACE = SCREEN_T
FRONT_H = STACK + TOL
FLOOR = 2.0
# 2 mm floor + 15 mm behind the board: 7.4 mm speaker, adhesive, and about 3 mm extra.
BACK_H = 17.0

PX = WALL + GAP
PY = WALL + GAP

# Glass seat. Width matches the board pocket. Length has the same 0.2 mm extra.
# The opening is 0.1 mm shorter and 0.1 mm narrower than that seat.
GLASS_X0 = PX + GLASS_FROM_MIC - 0.3 - TOL + 0.05 + 0.1
GLASS_X1 = PX + GLASS_FROM_MIC + GLASS_L + 0.3 + TOL - 0.05 - 0.1
GLASS_Y0 = WALL + 0.05 + 0.1
GLASS_Y1 = WALL + IN_W - 0.05 - 0.1
# Corner holes are 3.3 mm across, about 3.5 mm from each board edge.
# 前盖柱是实心 Ø1.3，插进后壳柱顶的 Ø1.4 盲孔；离面板背面 1.7mm 处
# 有一圈 Ø3.3 的卡扣圈，对应后壳柱顶的 Ø3.3 沉孔（深 1.7mm）。
HOLE_D = 3.3
HOLE_INSET = 3.5
SOCK_R = 1.275  # 孔 Ø2.55：配合间隙 0.25（原 0.1 太紧）
PEG_PAST = 3.5
PEG_POST_R = 1.15  # 前盖柱 Ø2.3 固定，不再跟随 SOCK_R
COLLAR_R = HOLE_D / 2
COLLAR_OFF = 1.7
COLLAR_H = 0.3
COLLAR_SEAT = 1.7
pin_pts = [
    (PX + hx, PY + hy)
    for hx in (HOLE_INSET, PCB_L - HOLE_INSET)
    for hy in (HOLE_INSET, PCB_W - HOLE_INSET)
]


def box(name, x0, y0, z0, x1, y1, z1):
    bpy.ops.mesh.primitive_cube_add(size=1, location=((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2))
    obj = bpy.context.active_object
    obj.name = name
    obj.scale = (abs(x1 - x0), abs(y1 - y0), abs(z1 - z0))
    bpy.ops.object.transform_apply(scale=True)
    return obj


def tube(name, x, y, z, radius, depth, segments=48):
    bpy.ops.mesh.primitive_cylinder_add(vertices=segments, radius=radius, depth=depth, location=(x, y, z))
    obj = bpy.context.active_object
    obj.name = name
    return obj


def solver_name():
    item = bpy.types.BooleanModifier.bl_rna.properties["solver"].enum_items
    names = [entry.identifier for entry in item]
    for candidate in ("MANIFOLD", "EXACT", "FLOAT"):
        if candidate in names:
            return candidate
    return names[0]


SOLVER = solver_name()


def unite(target, piece):
    modifier = target.modifiers.new("add", "BOOLEAN")
    modifier.operation = "UNION"
    modifier.object = piece
    modifier.solver = SOLVER
    bpy.context.view_layer.objects.active = target
    target.select_set(True)
    bpy.ops.object.modifier_apply(modifier=modifier.name)
    bpy.data.objects.remove(piece, do_unlink=True)
    return target


def difference(target, cutter):
    modifier = target.modifiers.new("cut", "BOOLEAN")
    modifier.operation = "DIFFERENCE"
    modifier.object = cutter
    modifier.solver = SOLVER
    bpy.context.view_layer.objects.active = target
    target.select_set(True)
    bpy.ops.object.modifier_apply(modifier=modifier.name)
    bpy.data.objects.remove(cutter, do_unlink=True)
    return target


def cleanup(obj):
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.remove_doubles(threshold=0.02)
    bpy.ops.mesh.normals_make_consistent(inside=False)
    bpy.ops.object.mode_set(mode="OBJECT")


# Clear the default scene.
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

# Front bezel. Z = 0 is the outside face, printed face-down, flush with the glass.
front = box("front", 0, 0, 0, OUT_L, OUT_W, FRONT_H)
front = difference(front, box("front_cavity", WALL, WALL, FACE, WALL + IN_L, WALL + IN_W, FRONT_H + 1.0))
front = difference(front, box("glass_pocket", GLASS_X0, GLASS_Y0, -0.4, GLASS_X1, GLASS_Y1, FACE + 0.4))
# USB-C metal is 9 x 3.1 mm and centered on the short edge.
# It stands off the component face, so the slot is in the back shell.
# The front only loses the 0.2 mm lip that would otherwise cross the connector.
usb_len = 9.0 + 2 * TOL
usb_y0 = PY + (PCB_W - usb_len) / 2
usb_y1 = usb_y0 + usb_len
front = difference(
    front,
    box("usb_front", OUT_L - WALL - 0.4, usb_y0, FRONT_H - TOL, OUT_L + 0.6, usb_y1, FRONT_H + 0.6),
)
for i, (hx, hy) in enumerate(pin_pts):
    peg_z1 = FRONT_H + PEG_PAST
    # 实心 Ø1.3 柱，插进后壳母柱的 Ø1.4 盲孔
    front = unite(front, tube(f"peg_{i}", hx, hy, (1.0 + peg_z1) / 2, PEG_POST_R, peg_z1 - 1.0))
cleanup(front)

# Back tub. Z = 0 is the outside back, printed flat on the bed.
back = box("back", 0, 0, 0, OUT_L, OUT_W, BACK_H)
back = difference(back, box("back_cavity", WALL, WALL, FLOOR, WALL + IN_L, WALL + IN_W, BACK_H + 1.0))
back = difference(
    back,
    box("usb_back", OUT_L - WALL - 0.4, usb_y0, BACK_H - (3.1 + TOL), OUT_L + 0.6, usb_y1, BACK_H + 0.6),
)
for i, (hx, hy) in enumerate(pin_pts):
    # 母柱与壳沿齐平（BACK_H），不超出也不下凹
    sock_top = BACK_H
    back = unite(
        back,
        tube(f"pin_boss_{i}", hx, hy, (FLOOR - 0.2 + sock_top) / 2, 3.0, sock_top - (FLOOR - 0.2)),
    )
for i, (hx, hy) in enumerate(pin_pts):
    back = difference(back, tube(f"pin_sock_{i}", hx, hy, BACK_H - 1.6, SOCK_R, 4.6))
# Buttons are 1 mm from the USB end. A boss reaches toward each switch so the
# plunger is captive: the wide head stays inside, the nub sticks out the back.
btn_cx = PX + PCB_L - BTN_FROM_END - min(BTN_W, BTN_D) / 2
btn_cy_left = PY + BTN_FROM_SIDE + min(BTN_W, BTN_D) / 2
btn_cy_right = PY + PCB_W - BTN_FROM_SIDE - min(BTN_W, BTN_D) / 2
# PCB sits at the rim. Switch face is 2.7 mm below that.
# The stem and flange need more than TOL: a 0.4 mm nozzle prints holes small,
# and the old flange filled its pocket with no spare height.
switch_z = BACK_H - BTN_H
slide = 0.40
flange_h = 1.4
flange_r = 2.50
stem_r = 1.75  # 3.5 mm across
bore_r = 2.00  # hole stays 4.0 mm
boss_r = 3.05
seat_r = boss_r + 0.05
gap_to_switch = 0.40
pocket_slack = 0.60
boss_top = switch_z - gap_to_switch + 1.0  # 导向柱加高 1mm（用户要求），seat/柱塞联动 +1
seat_z = boss_top - flange_h - pocket_slack
for cy in (btn_cy_left, btn_cy_right):
    boss = tube(f"boss_{cy}", btn_cx, cy, (FLOOR - 0.2 + boss_top) / 2, boss_r, boss_top - (FLOOR - 0.2))
    back = unite(back, boss)
for cy in (btn_cy_left, btn_cy_right):
    back = difference(back, tube(f"btn_hole_{cy}", btn_cx, cy, (seat_z + 0.2 - 0.6) / 2, bore_r, seat_z + 0.8))
    seat_top = boss_top + 0.6
    back = difference(back, tube(f"btn_seat_{cy}", btn_cx, cy, (seat_z + seat_top) / 2, seat_r, seat_top - seat_z))

# Speaker grille over the oval driver, kept off the battery along y = 0.
gx0, gx1 = PX + 24.0, PX + 62.0
gy0, gy1 = PY + 16.0, PY + 42.0
slot = gy0
while slot + 1.4 < gy1:
    back = difference(back, box(f"slot_{slot:.1f}", gx0, slot, -1.0, gx1, slot + 1.4, FLOOR + 1.0))
    slot += 3.2

back.name = "back"
cleanup(back)


def plunger(name, x, y):
    # Flange prints on the bed. The stem drops through the boss and sticks out
    # well past the back so it can be trimmed to a comfortable press.
    nub = 2.4  # 总长 15.7
    stem_len = seat_z + nub
    head = tube(name, x, y, flange_h / 2, flange_r, flange_h)
    stem = tube(name + "_stem", x, y, flange_h + stem_len / 2, stem_r, stem_len)
    bpy.ops.object.select_all(action="DESELECT")
    head.select_set(True)
    stem.select_set(True)
    bpy.context.view_layer.objects.active = head
    bpy.ops.object.join()
    head.name = name
    return head


boot_btn = plunger("boot_button", 0, 0)
reset_btn = plunger("reset_button", 12, 0)

out_dir = "/Users/myidd007/My project/made/case"


def export_stl(objects, filename):
    bpy.ops.object.select_all(action="DESELECT")
    for obj in objects:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]
    bpy.ops.wm.stl_export(
        filepath=f"{out_dir}/{filename}",
        export_selected_objects=True,
        global_scale=1.0,
        apply_modifiers=True,
    )
    print("exported", filename)


def ray_open(obj, x, y):
    hit, loc, _normal, _index = obj.ray_cast((x, y, -2.0), (0.0, 0.0, 1.0), distance=30.0)
    print("ray", obj.name, round(x, 1), round(y, 1), "hit", hit, "z", round(loc.z, 2) if hit else None)
    return hit


print("solver", SOLVER)
print("outer", round(OUT_L, 2), round(OUT_W, 2), "front", FRONT_H, "back", BACK_H, "wall", WALL)
print("usb", round(usb_y0, 2), round(usb_y1, 2), "len", round(usb_len, 2), "h", round(3.1 + 2 * TOL, 2))
print(
    "glass",
    round(GLASS_X1 - GLASS_X0, 2),
    round(GLASS_Y1 - GLASS_Y0, 2),
    "hole",
    HOLE_D,
    "peg",
    round(PEG_POST_R * 2, 2),
    "collar",
    round(COLLAR_R * 2, 2),
)
for hx, hy in pin_pts:
    hit, loc, _normal, _index = back.ray_cast((hx, hy, BACK_H + 2), (0, 0, -1), distance=20)
    print("socket", round(hx, 2), round(hy, 2), "hit", hit, "z", round(loc.z, 2) if hit else None)
    hit, loc, _normal, _index = front.ray_cast((hx, hy, FRONT_H + PEG_PAST + 2), (0, 0, -1), distance=20)
    print("peg", round(hx, 2), round(hy, 2), "hit", hit, "z", round(loc.z, 2) if hit else None)
    hit, loc, _normal, _index = front.ray_cast((hx + 1.0, hy, FRONT_H + COLLAR_OFF + 1), (0, 0, -1), distance=8)
    print("head", round(hx, 2), round(hy, 2), "hit", hit, "z", round(loc.z, 2) if hit else None)
ray_open(front, (GLASS_X0 + GLASS_X1) / 2, PY + PCB_W / 2)
ray_open(front, PX + 2.2, PY + 2.2)
ray_open(front, PX + 4.0, PY + PCB_W / 2)
ray_open(front, 1.0, OUT_W / 2)
ray_open(back, (gx0 + gx1) / 2, gy0 + 0.7)
ray_open(back, btn_cx, btn_cy_left)
ray_open(back, btn_cx, btn_cy_right)
ray_open(back, PX + 20.0, PY + 8.0)
print(
    "buttons",
    round(btn_cx, 2),
    round(btn_cy_left, 2),
    round(btn_cy_right, 2),
    "stick-out",
    8.0,
    "stem dia",
    round(stem_r * 2, 2),
    "hole dia",
    round(bore_r * 2, 2),
    "flange dia",
    round(flange_r * 2, 2),
    "pocket",
    round(boss_top - seat_z, 2),
)

export_stl([front], "made_case_front.stl")
export_stl([back], "made_case_back.stl")
export_stl([boot_btn, reset_btn], "made_case_buttons.stl")

# Show the screen face and the open back side by side.
front.rotation_euler = (math.pi, 0, 0)
front.location = (0, OUT_W + 16, FRONT_H)
back.location = (0, 0, 0)
boot_btn.location = (OUT_L + 14, 8, 0)
reset_btn.location = (OUT_L + 14, 22, 0)

bpy.ops.object.light_add(type="SUN", location=(40, 20, 120))
sun = bpy.context.active_object
sun.data.energy = 3.5
bpy.ops.object.light_add(type="AREA", location=(-30, -40, 80))
fill = bpy.context.active_object
fill.data.energy = 250
fill.data.size = 40

bpy.ops.object.camera_add(location=(OUT_L / 2, -80, 70), rotation=(math.radians(62), 0, 0))
camera = bpy.context.active_object
camera.data.lens = 42
bpy.context.scene.camera = camera

scene = bpy.context.scene
scene.render.engine = "CYCLES"
scene.cycles.device = "CPU"
scene.cycles.samples = 48
scene.render.resolution_x = 1400
scene.render.resolution_y = 900
scene.render.filepath = f"{out_dir}/preview.png"
scene.render.film_transparent = False
world = scene.world or bpy.data.worlds.new("World")
scene.world = world
world.use_nodes = True
bg = world.node_tree.nodes.get("Background")
if bg:
    bg.inputs[0].default_value = (0.78, 0.80, 0.82, 1)
    bg.inputs[1].default_value = 0.8

for obj, color in (
    (front, (0.16, 0.45, 0.62, 1)),
    (back, (0.20, 0.24, 0.28, 1)),
    (boot_btn, (0.85, 0.55, 0.18, 1)),
    (reset_btn, (0.85, 0.55, 0.18, 1)),
):
    mat = bpy.data.materials.new(obj.name + "_mat")
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = color
    obj.data.materials.append(mat)

bpy.ops.render.render(write_still=True)
bpy.ops.wm.save_as_mainfile(filepath=f"{out_dir}/made_case.blend")
print("CASE_DONE", round(OUT_L, 2), round(OUT_W, 2), FRONT_H, BACK_H)
