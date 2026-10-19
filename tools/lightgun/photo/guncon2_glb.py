"""Stage 3: assemble the PS2 light gun drawn from photos and write its .glb (guncon2_glb.py <work> <out.glb>).
Same node contract as the PS1 gun, plus the GunCon2's extra controls; see ../README.md."""
import sys, json, numpy as np, trimesh
import glb
from glb import ex, ey, ez
W, OUT = sys.argv[1], sys.argv[2]
body = trimesh.load(f'{W}/low_2600.ply', process=True)
F, px = glb.frame(W)
C = json.load(open(f'{sys.path[0]}/guncon2.json'))['controls']
sx, sz = F['px_to_mm']['sx'], F['px_to_mm']['sz']

def hit(o, d):
    """first point of the body along ray o + t d"""
    loc, _, _ = body.ray.intersects_location(np.array([o], float), np.array([d], float))
    return loc[np.argmin(((loc - o) ** 2).sum(1))]
def side_seat(p, side):
    """surface point on the gun's left (side=+1) or right (-1) face at the traced point p"""
    x, z = px(p); return hit([x, side * 40.0, z], [0, -side, 0])

lens = glb.lens_faces(body, F); print('lens faces', lens.sum())
up = F['tubes']['upper']; muzzle = np.array([up['front_x'], 0.0, up['center_z']])
trig, piv = glb.trigger(F)

# A and B: the oval buttons above the trigger, one on each side
ab = C['a_b']; ab_out = glb.stadium(ab['length_px'] * sx, ab['height_px'] * sz)
seatL, seatR = side_seat(ab['center'], +1), side_seat(ab['center'], -1)
btnA = glb.cap(ab_out, 0.7, seatL, ex, ez, ey, ab['top_mm']); btnB = glb.cap(ab_out, 0.7, seatR, -ex, ez, -ey, ab['top_mm'])
# SELECT (front) and START: the small round buttons on the left side
sel, st = C['select'], C['start']
seatSel, seatSt = side_seat(sel['center'], +1), side_seat(st['center'], +1)
btnSel = glb.cap(glb.circle(sel['diameter_px'] / 2 * sx), 0.5, seatSel, ex, ez, ey, sel['top_mm'])
btnSt = glb.cap(glb.circle(st['diameter_px'] / 2 * sx), 0.5, seatSt, ex, ez, ey, st['top_mm'])
# D-pad: a cross on the raked rear face
dp = C['dpad']; f0, f1 = px(dp['face']); u_d = np.array([f0[0] - f1[0], 0, f0[1] - f1[1]]); u_d /= np.linalg.norm(u_d)   # up the face
n_d = np.array([-u_d[2], 0, u_d[0]])   # out of the face: back and up
zc = px([0, dp['center_y']])[1]; t = (zc - f1[1]) / (f0[1] - f1[1]); on_face = f1 + t * (f0 - f1)
dpad_c = hit(np.array([on_face[0], 0, zc]) + 20 * n_d, -n_d)
dpad = glb.cap(glb.cross(dp['span_mm'], dp['arm_mm']), 0.6, dpad_c, -ey, u_d, n_d, dp['top_mm'], 3.5)
# C: the piece at the front of the grip's underside, pressed up into the grip
c = C['c']; c_prof = px(c['poly']); btnC = glb.extrude(c_prof, -c['width_mm'] / 2, c['width_mm'] / 2)
seatC = np.array([c_prof[:, 0].mean(), 0.0, c_prof[:, 1].max()])
print('seats A', seatL.round(1), 'B', seatR.round(1), 'select', seatSel.round(1), 'start', seatSt.round(1), 'dpad', dpad_c.round(1), 'C', seatC.round(1))

grip_c, ax = glb.grip_frame(F)
g = glb.Glb(grip_c, glb.MATERIALS)
shell = body.submesh([np.nonzero(~lens)[0]], append=True); lensm = body.submesh([np.nonzero(lens)[0]], append=True)
g.node('body', [(shell, grip_c, 0), (lensm, grip_c, 2)])
g.node('trigger', [(trig, piv, 1)], piv, note='pull = rotate about local +X by a negative angle (blade swings back toward the grip), ~12 deg travel')
g.node('button_left', [(btnA, seatL, 1)], seatL, note='A, on the gun\'s left side (-X) above the trigger; press = translate along +X by ~1 mm')
g.node('button_right', [(btnB, seatR, 1)], seatR, note='B, on the gun\'s right side (+X) above the trigger; press = translate along -X by ~1 mm')
g.control('button_select', btnSel, seatSel, ey, ez, 1, 'SELECT, the front one of the two small buttons on the left side; press = translate along local -Z by ~1 mm')
g.control('button_start', btnSt, seatSt, ey, ez, 1, 'START, the rear one of the two small buttons on the left side; press = translate along local -Z by ~1 mm')
g.control('button_c', btnC, seatC, -ez, ex, 1, 'C, under the front of the grip; press = translate along local -Z (up into the grip) by ~1 mm')
g.control('dpad', dpad, dpad_c, n_d, u_d, 1, 'D-pad on the rear face, local +Y up; press a direction = tilt that edge into the gun (about local X or Y), ~6 deg')
g.node('muzzle', at=muzzle, note='aim ray origin on the upper bore; aim direction = local -Z')
g.grip(ax)
g.write(OUT, 'ARMSX2 VR tools/lightgun/photo')
