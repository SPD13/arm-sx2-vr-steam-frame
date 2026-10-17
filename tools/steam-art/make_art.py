"""Build the Steam library artwork for "ARMSX2 VR Steam Frame Edition" into assets/steam/, the regular set without the
Alpha badge into assets/steam-final/, and the installer page's hero.jpg, logo.png and favicon.png.
Usage: python make_art.py <repo root>
Sources: the ARMSX2 app icon (glowing pillar), the Bebas Neue font from the ARMSX2 Android app
and assets/lightgun.glb. Needs Python 3 with numpy and pillow."""
import os, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gun

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '../..'))
ARMSX2 = os.path.join(ROOT, 'external/armsx2')
PILLAR = os.path.join(ARMSX2, 'platforms/ios/app/src/main/assets/app_icons/icon.png')
BEBAS = os.path.join(ARMSX2, 'platforms/android/app/src/main/res/font/bebas_neue.ttf')
GLB = os.path.join(ROOT, 'assets/lightgun.glb')
OUT = os.path.join(ROOT, 'assets/steam')
FINAL = os.path.join(ROOT, 'assets/steam-final')
UI = os.path.join(ROOT, 'tools/steam-frame/installer/ui')

# While the project is in Alpha, the art that ships (assets/steam/, the installer page) carries an ALPHA badge.
# Leaving Alpha: set this to False and rerun (or copy assets/steam-final/ over assets/steam/).
ALPHA = True

TITLE, SUBTITLE = 'ARMSX2 VR', 'STEAM FRAME EDITION'
CYAN = (90, 200, 255); VIOLET = (170, 80, 255); RED = (255, 40, 30); AMBER = (255, 170, 30)

# ---------- helpers ----------

def background(w, h, centre=(0.5, 0.4), seed=1):
    """ARMSX2 navy: radial glow from `centre` (fractions) to near-black, with fine grain."""
    y, x = np.mgrid[0:h, 0:w].astype(float)
    cx, cy = centre[0] * w, centre[1] * h
    r = np.hypot((x - cx) / max(w, h), (y - cy) / max(w, h))
    t = np.clip(r / 0.75, 0, 1) ** 0.8
    inner, outer = np.array([14, 32, 105]), np.array([2, 3, 16])
    img = inner * (1 - t[..., None]) + outer * t[..., None]
    img += np.random.default_rng(seed).normal(0, 2.2, (h, w, 1))
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), 'RGB').convert('RGBA')

def add(base, layer, pos=(0, 0), gain=1.0):
    """Additive blend of an RGB(A) layer (black = no light) at pos."""
    lay = Image.new('RGB', base.size, 0)
    lay.paste(layer.convert('RGB'), pos)
    if gain != 1.0:
        lay = Image.fromarray(np.clip(np.asarray(lay).astype(float) * gain, 0, 255).astype(np.uint8))
    rgb = ImageChops.add(base.convert('RGB'), lay)
    out = rgb.convert('RGBA'); out.putalpha(base.getchannel('A')); return out

def pillar(height):
    """The ARMSX2 pillar (with its orbit ring and glow) cut from the app icon, edges faded to black."""
    src = Image.open(PILLAR).convert('RGB').crop((112, 0, 912, 1024))
    w, h = src.size
    y, x = np.mgrid[0:h, 0:w].astype(float)
    fade = np.clip(1 - np.hypot((x - w / 2) / (w * 0.52), (y - h / 2) / (h * 0.55)), 0, 1) ** 0.6
    a = np.asarray(src).astype(float) * fade[..., None]
    a = np.clip(a - 3, 0, 255)  # drop the icon's near-black floor
    im = Image.fromarray(a.astype(np.uint8))
    return im.resize((round(w * height / h), height), Image.LANCZOS)

def glow(img_rgba, radius, colour, strength=1.0):
    """Coloured blur of an RGBA image's alpha, as an additive RGB layer."""
    a = img_rgba.getchannel('A').filter(ImageFilter.GaussianBlur(radius))
    arr = np.asarray(a).astype(float)[..., None] / 255 * np.array(colour) * strength
    return Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8))

def text_layer(text, font, tracking=0, fill=(255, 255, 255, 255)):
    """Text rendered on a transparent layer cropped to its ink, with letter spacing in px."""
    asc, desc = font.getmetrics()
    widths = [font.getlength(ch) for ch in text]
    w = int(sum(widths) + tracking * (len(text) - 1)) + 4
    im = Image.new('RGBA', (w, asc + desc + 4), (0, 0, 0, 0))
    d = ImageDraw.Draw(im); x = 2
    for ch, cw in zip(text, widths):
        d.text((x, 2), ch, font=font, fill=fill); x += cw + tracking
    return im.crop(im.getbbox())

def title_block(width, colour_sub=CYAN):
    """'ARMSX2 VR' over a tracked 'STEAM FRAME EDITION', both centred, `width` px wide (transparent)."""
    big = text_layer(TITLE, ImageFont.truetype(BEBAS, 400))
    big = big.resize((width, round(big.height * width / big.width)), Image.LANCZOS)
    sub_font = ImageFont.truetype(BEBAS, 200)
    sub = text_layer(SUBTITLE, sub_font, tracking=28, fill=colour_sub + (255,))
    sw = round(width * 0.98)
    sub = sub.resize((sw, round(sub.height * sw / sub.width)), Image.LANCZOS)
    gap = round(big.height * 0.16)
    pad = round(width * 0.08)
    blk = Image.new('RGBA', (width + 2 * pad, big.height + gap + sub.height + 2 * pad), (0, 0, 0, 0))
    blk.alpha_composite(big, (pad, pad))
    blk.alpha_composite(sub, (pad + (width - sw) // 2, pad + big.height + gap))
    return blk

def put_title(canvas, blk, pos, glow_r):
    """Title block with a cyan halo and a dark drop shadow so it reads on any background."""
    shadow = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    sh = Image.new('RGBA', blk.size, (0, 0, 10, 0)); sh.putalpha(blk.getchannel('A').point(lambda v: v * 0.85))
    shadow.alpha_composite(sh, (pos[0], pos[1] + max(2, glow_r // 3)))
    canvas.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(glow_r * 0.6)))
    canvas = add(canvas, glow(blk, glow_r, CYAN, 0.55), pos)
    canvas.alpha_composite(blk, pos)
    return canvas

def alpha_badge(height):
    """Amber 'ALPHA' pill, `height` px tall, with a transparent margin of half its height for its halo."""
    k = 4  # drawn 4x larger, then scaled down, for smooth edges
    h = height * k
    txt = text_layer('ALPHA', ImageFont.truetype(BEBAS, 200), tracking=22, fill=(8, 12, 40, 255))
    th = round(h * 0.54)
    txt = txt.resize((round(txt.width * th / txt.height), th), Image.LANCZOS)
    w, m = txt.width + round(h * 0.95), h // 2
    pill = Image.new('RGBA', (w + 2 * m, h + 2 * m), (0, 0, 0, 0))
    ImageDraw.Draw(pill).rounded_rectangle((m, m, m + w - 1, m + h - 1), radius=h // 2, fill=AMBER + (255,))
    pill.alpha_composite(txt, (m + (w - txt.width) // 2, m + (h - txt.height) // 2))
    return pill.resize((pill.width // k, pill.height // k), Image.LANCZOS)

def put_badge(canvas, height, right, top):
    """The Alpha badge with its top-right corner at (right, top): a dark shadow, an amber halo, then the pill."""
    b = alpha_badge(height)
    m = height // 2
    pos = (right - b.width + m, top - m)
    shadow = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    sh = Image.new('RGBA', b.size, (0, 0, 10, 0)); sh.putalpha(b.getchannel('A').point(lambda v: v * 0.8))
    shadow.alpha_composite(sh, (pos[0], pos[1] + max(2, height // 12)))
    canvas.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(height / 6)))
    canvas = add(canvas, glow(b, height / 4, AMBER, 0.6), pos)
    canvas.alpha_composite(b, pos)
    return canvas

def portal(canvas, box, width, pillar_h=None):
    """The volumetric window: a glowing frame with the ARMSX2 pillar standing inside it."""
    x0, y0, x1, y1 = box
    inner = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    ImageDraw.Draw(inner).rounded_rectangle(box, radius=width * 3, fill=(0, 10, 40, 150))
    canvas.alpha_composite(inner.filter(ImageFilter.GaussianBlur(width)))
    if pillar_h:
        p = pillar(pillar_h)
        canvas = add(canvas, p, ((x0 + x1 - p.width) // 2, (y0 + y1 - p.height) // 2 + pillar_h // 30))
    frame = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    ImageDraw.Draw(frame).rounded_rectangle(box, radius=width * 3, outline=CYAN + (255,), width=width)
    canvas = add(canvas, glow(frame, width * 6, CYAN, 1.4))
    canvas = add(canvas, glow(frame, width * 2, (150, 225, 255), 1.0))
    canvas.alpha_composite(frame)
    return canvas

def place_gun(canvas, shot, pos, target=None, beam_w=3):
    """Composite a gun render with a soft shadow, lens bloom and (optionally) the aim beam to `target`."""
    g = shot.image()
    canvas = add(canvas, glow(g, max(4, g.width // 40), VIOLET, 0.35), pos)
    if target is not None:
        m = shot.project(shot.nodes['muzzle']) + np.array(pos)
        beam = Image.new('RGBA', canvas.size, (0, 0, 0, 0)); d = ImageDraw.Draw(beam)
        d.line([tuple(m), tuple(target)], fill=RED + (255,), width=beam_w)
        canvas = add(canvas, glow(beam, beam_w * 4, RED, 1.2))
        canvas.alpha_composite(beam)
        dot = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
        r = beam_w * 3; tx, ty = target
        ImageDraw.Draw(dot).ellipse((tx - r, ty - r, tx + r, ty + r), fill=(255, 220, 210, 255))
        canvas = add(canvas, glow(dot, r * 3, RED, 2.0))
        canvas.alpha_composite(dot)
    canvas.alpha_composite(g, pos)
    lens = shot.lens_glow()
    bloom = Image.merge('RGB', [lens.point(lambda v: v * c / 255) for c in RED]).filter(ImageFilter.GaussianBlur(max(3, g.width // 60)))
    canvas = add(canvas, bloom, pos, gain=1.6)
    return canvas

def aim_error(shot, pos, target):
    """Signed screen angle (degrees) from the bore direction to the muzzle->target direction."""
    m = shot.project(shot.nodes['muzzle']) + np.array(pos)
    d = shot.direction([0, 0, -1])
    v = np.array(target) - m
    return np.degrees(np.arctan2(d[0] * v[1] - d[1] * v[0], d @ v))

def solve_shot(yaw, pitch, size, fill, pos, target):
    """Pick the camera roll that makes the bore point at `target` (projection only), then render."""
    def err(r): return aim_error(gun.Shot(GLB, yaw, pitch, r, size=size, fill=fill, raster=False), pos, target)
    rolls = np.radians(np.arange(-90, 91, 2.0))
    errs = np.array([err(r) for r in rolls])
    r = rolls[np.argmin(np.abs(errs))]
    for step in (np.radians(1), np.radians(0.25)):
        cand = [r - step, r, r + step]; r = min(cand, key=lambda x: abs(err(x)))
    return gun.Shot(GLB, yaw, pitch, r, size=size, fill=fill)

# ---------- the five images ----------

def capsule(badge=False):
    """Library capsule, 600x900 (<appid>p.png)."""
    W, H = 600, 900
    c = background(W, H, (0.5, 0.33))
    box = (60, 60, 470, 420)
    c = portal(c, box, 4, pillar_h=340)
    target, size, pos = (225, 290), (500, 400), (W - 500 + 70, 345)
    s = solve_shot(-0.6, 0.3, size, 0.95, pos, target)
    c = place_gun(c, s, pos, target, beam_w=3)
    blk = title_block(470)
    c = put_title(c, blk, ((W - blk.width) // 2, H - blk.height - 6), 14)
    return put_badge(c, 40, W - 22, 10) if badge else c

def wide(badge=False):
    """Wide capsule / library header, 920x430 (<appid>.png)."""
    W, H = 920, 430
    c = background(W, H, (0.68, 0.45))
    box = (420, 40, 720, 330)
    c = portal(c, box, 3, pillar_h=270)
    target, size, pos = (570, 200), (330, 270), (W - 330 + 10, H - 270 + 15)
    s = solve_shot(-0.7, 0.3, size, 0.95, pos, target)
    c = place_gun(c, s, pos, target, beam_w=3)
    blk = title_block(330)
    c = put_title(c, blk, (22, (H - blk.height) // 2 - 5), 10)
    return put_badge(c, 34, W - 20, 18) if badge else c

def hero(badge=False):
    """Game page hero, 1920x620 (<appid>_hero.png). No text: Steam draws the logo over its left side."""
    W, H = 1920, 620
    c = background(W, H, (0.6, 0.45))
    box = (900, 60, 1420, 540)
    c = portal(c, box, 5, pillar_h=450)
    target, size, pos = (1160, 300), (600, 480), (W - 600 - 30, H - 480 + 40)
    s = solve_shot(-0.7, 0.3, size, 0.95, pos, target)
    c = place_gun(c, s, pos, target, beam_w=4)
    # darken the left third so the overlaid logo always reads
    y, x = np.mgrid[0:H, 0:W]
    shade = np.clip(1 - x / (W * 0.45), 0, 1) ** 1.5 * 0.55
    dark = Image.fromarray((shade * 255).astype(np.uint8), 'L')
    black = Image.new('RGBA', (W, H), (1, 2, 12, 255)); black.putalpha(dark)
    c.alpha_composite(black)
    # inset so it survives Steam cropping the hero to the window's shape
    return put_badge(c, 56, W - 115, 62) if badge else c

def logo(badge=False):
    """Transparent logo for the game page (<appid>_logo.png): the title with a soft dark halo."""
    blk = title_block(1100)
    halo = Image.new('RGBA', blk.size, (0, 0, 8, 0))
    halo.putalpha(blk.getchannel('A').filter(ImageFilter.GaussianBlur(10)).point(lambda v: min(255, v * 1.2)))
    c = Image.new('RGBA', blk.size, (0, 0, 0, 0))
    c.alpha_composite(halo, (0, 5)); c.alpha_composite(blk)
    if badge:  # under the right end of STEAM FRAME EDITION, in the block's bottom padding
        pad, bh = round(1100 * 0.08), 64
        room = Image.new('RGBA', (c.width, c.height + bh), (0, 0, 0, 0)); room.alpha_composite(c)
        c = put_badge(room, bh, c.width - pad - 4, c.height - pad + 30)
    return c.crop(c.getbbox())

def icon(badge=False):
    """Square icon, 256x256: the ARMSX2 pillar with the light gun in front."""
    S = 512
    c = Image.open(PILLAR).convert('RGBA').resize((S, S), Image.LANCZOS)
    s = gun.Shot(GLB, 0.7, 0.2, np.radians(-8), size=(330, 260), fill=0.98)
    c = place_gun(c, s, (S - 340, S - 250))
    if badge:
        c = put_badge(c, 84, S - 18, 18)
    return c.resize((256, 256), Image.LANCZOS)

def save(im, folder, name):
    if name != 'logo.png' and name != 'icon.png': im = im.convert('RGB')
    im.save(os.path.join(folder, name), optimize=True)
    print(os.path.relpath(os.path.join(folder, name), ROOT), im.size)

if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True); os.makedirs(FINAL, exist_ok=True)
    shipped = {}
    for name, fn in [('capsule.png', capsule), ('wide.png', wide), ('hero.png', hero), ('logo.png', logo), ('icon.png', icon)]:
        plain = fn()
        save(plain, FINAL, name)
        shipped[name] = fn(badge=True) if ALPHA else plain
        save(shipped[name], OUT, name)
    # The installer page shows the hero behind its header and the logo in it (at 96 px tall)
    h = shipped['hero.png'].convert('RGB').resize((1500, 484), Image.LANCZOS)
    h.save(os.path.join(UI, 'hero.jpg'), quality=88, optimize=True)
    lg = shipped['logo.png']
    lg = lg.resize((round(lg.width * 228 / lg.height), 228), Image.LANCZOS)
    lg.save(os.path.join(UI, 'logo.png'), optimize=True)
    # The page's favicon: the icon without the badge, which would be unreadable at this size
    fav = icon().resize((64, 64), Image.LANCZOS)
    fav.save(os.path.join(UI, 'favicon.png'), optimize=True)
    print('installer ui: hero.jpg', h.size, 'logo.png', lg.size, 'favicon.png', fav.size)
