"""Stage 3: assemble the PS1 light gun drawn from photos and write its .glb (guncon1_glb.py <work> <out.glb>).
Same node contract as before; see ../README.md."""
import sys, numpy as np, trimesh
import glb
W, OUT = sys.argv[1], sys.argv[2]
body = trimesh.load(f'{W}/low_2600.ply', process=True)
F, px = glb.frame(W)

lens = glb.lens_faces(body, F); print('lens faces', lens.sum())
up = F['tubes']['upper']; muzzle = np.array([up['front_x'], 0.0, up['center_z']])
trig, piv = glb.trigger(F)

def button(cx, cz, yfloor, ytop, side, r, seg=16):
    """chamfered cylinder along Y from the socket floor to its cap; side=+1 for the left (+Y) face"""
    a = np.linspace(0, 2 * np.pi, seg, endpoint=False)
    rings = [(r, yfloor), (r, ytop - 0.6), (r - 0.6, ytop)]
    v = []
    for rr, yy in rings:
        v += [[cx + rr * np.cos(t), side * yy, cz + rr * np.sin(t)] for t in a]
    v += [[cx, side * yfloor, cz], [cx, side * ytop, cz]]
    v = np.array(v); f = []
    for k in range(len(rings) - 1):
        for i in range(seg):
            j = (i + 1) % seg; a0, a1, b0, b1 = k * seg + i, k * seg + j, (k + 1) * seg + i, (k + 1) * seg + j
            f += [[a0, a1, b1], [a0, b1, b0]]
    cb, ct = len(v) - 2, len(v) - 1; top = (len(rings) - 1) * seg
    for i in range(seg):
        j = (i + 1) % seg; f += [[cb, j, i], [ct, top + i, top + j]]
    m = trimesh.Trimesh(v, f, process=True); m.fix_normals(); return m
b = F['button']; bc = np.array([b['x'], 0, b['z']]); ycap = b['face_y'] + 1.2
btnL = button(b['x'], b['z'], b['socket_floor_y'], ycap, +1, b['radius']); btnR = button(b['x'], b['z'], b['socket_floor_y'], ycap, -1, b['radius'])

grip_c, ax = glb.grip_frame(F)
g = glb.Glb(grip_c, glb.MATERIALS + [
    {'name': 'button', 'pbrMetallicRoughness': {'baseColorFactor': [0.6, 0.03, 0.04, 1], 'metallicFactor': 0.0, 'roughnessFactor': 0.3}}])
shell = body.submesh([np.nonzero(~lens)[0]], append=True); lensm = body.submesh([np.nonzero(lens)[0]], append=True)
g.node('body', [(shell, grip_c, 0), (lensm, grip_c, 2)])
g.node('trigger', [(trig, piv, 1)], piv, note='pull = rotate about local +X by a negative angle (blade swings back toward the grip), ~12 deg travel')
g.node('button_left', [(btnL, bc, 3)], bc, note='on the gun\'s left side (-X); press = translate along +X by ~1 mm')
g.node('button_right', [(btnR, bc, 3)], bc, note='on the gun\'s right side (+X); press = translate along -X by ~1 mm')
g.node('muzzle', at=muzzle, note='aim ray origin on the upper bore; aim direction = local -Z')
g.grip(ax)
g.write(OUT, 'ARMSX2 VR tools/lightgun/photo')
