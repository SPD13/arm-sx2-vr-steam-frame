# Light gun model

`assets/lightgun.glb` is a low-poly (2,844 triangles, 92 KB) in-game model of the light gun. It is rebuilt from 3D scans of the two plastic shells of a PS1 light gun. Logos and markings are removed: the "namco" wordmarks on both sides of the slide, the PlayStation logo and wordmark above the grip, and the model/copyright text on the left front housing.

![preview](../../docs/images/lightgun-preview.png)

## Rebuild

```sh
tools/lightgun/run.sh "<dir with LeftScan.obj + RightScan.obj>" <work dir> [python]
```

You need Python 3.11 with `numpy scipy trimesh pymeshlab scikit-image rtree shapely mapbox-earcut pillow`. The output is deterministic: the same scans give a byte-identical `.glb`. The scans (about 400 MB) are not in the repo.

| Stage | Script | What it does |
|---|---|---|
| 1 | `load.py` | Reads each OBJ (about 1.1M vertices per shell). |
| 2 | `align.py` | Flips the right shell onto the left shell's parting plane, adds 1.0 mm for the stepped lip so the muzzle bore comes out round, and registers the outlines (IoU 0.996). |
| 3 | `fields.py` | Builds outer-surface height fields from ±X, ±Y and ±Z at 0.5 mm. |
| 4 | `solid.py` | Fills the screw and button holes, clamps deep openings, suppresses scan detail under 0.5 mm, Laplace-fills the logo boxes, then builds the solid as the intersection of the six bounds (marching cubes). |
| 5 | `decimate.py` | Quadric decimation to 2,600 faces. Mean deviation from the dense solid is 0.06 mm (p95 0.19 mm). |
| 6 | `build_glb.py` | Adds the parts that are not in the scans (trigger, side buttons), a lens material on the two front openings, and marker nodes. Writes the `.glb` with crease-split normals (35°). |
| 7 | `view_glb.py` | Reads the `.glb` back and renders the preview. Pass `pressed` as the last argument to see the trigger pulled. |

The logo boxes are in `solid.py`, as x/z ranges in mm in the scan frame. If you change the alignment, check them against a detail render of the height field.

## Contract for the host

- Units are metres. The axes follow glTF: +Y is up, −Z points forward along the bore, +X is the gun's right side.
- The root node `lightgun` has its origin at the grip centre.

| Node | Meaning |
|---|---|
| `body` | The shell, with two primitives: material `shell` and material `lens`. |
| `trigger` | Pivots at its node origin. To pull, rotate about local +X by a negative angle; about −12° of travel. |
| `button_left`, `button_right` | The side buttons on the front housing. To press, translate about 1 mm inward. |
| `muzzle` | The aim ray starts here, on the upper bore face. The ray direction is the node's −Z. |
| `grip` | Sits at the grip centre. Local −Z runs up the grip axis, which is raked 11° from vertical; +Y points forward. Use it to line up with the OpenXR grip pose. |

The materials are `shell` (near-black), `trim` (trigger and buttons) and `lens` (dark red).

## Known approximations

- The trigger and buttons are hand-modelled, because the scans contain only the shells.
- Total width (36.8 mm) depends on the 1.0 mm lip offset, which is inferred from the bore shape, not measured.
- Undercuts are approximated: the six-direction height fields cannot represent them.
- Slide serrations and shallow decorative grooves mostly decimate away at this budget.
