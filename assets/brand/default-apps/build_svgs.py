#!/usr/bin/env python3
"""Generates the six SVG sources in ./svg/ for the Foundation Sunshine default app artwork.

Inputs (next to this script, in assets/brand/default-apps/):
  marks/steam-logo-inverse.d     Valve's official white "steam(R)" logo path (from steam_brandAssets.eps, see README.assets.md)
  marks/xbox-sphere-FALLBACK.d   FALLBACK third-party (simple-icons, CC0) Xbox sphere path; official vector is behind a login wall
  ../bigscreen-cover.svg   existing Big Screen cover (shapes + mascot image reused)
  /usr/share/fonts/TTF/OpenSans-CondensedBold.ttf    open-licensed font (Apache-2.0); text is converted to outlines
"""
import os, re, sys
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'svg')
os.makedirs(OUT, exist_ok=True)

STEAM_D = open(os.path.join(HERE, 'marks/steam-logo-inverse.d')).read().strip()
# control-point bounding box of the official path (x0 y0 x1 y1), taken from the EPS-derived SVG
SX0, SY0, SX1, SY1 = 133.394531, 451.707031, 388.382812, 529.296875
SW, SH = SX1 - SX0, SY1 - SY0
XBOX_D = open(os.path.join(HERE, 'marks/xbox-sphere-FALLBACK.d')).read().strip()  # viewBox 0 0 24 24


def write(name, w, h, body, defs=''):
    s = (f'<?xml version="1.0" encoding="UTF-8"?>\n'
         f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
         f'width="{w}" height="{h}" viewBox="0 0 {w} {h}">\n<defs>\n{defs}</defs>\n{body}</svg>\n')
    open(os.path.join(OUT, name), 'w').write(s)


def steam_mark(cx, cy, width):
    k = width / SW
    tx = cx - (SX0 + SW / 2) * k
    ty = cy - (SY0 + SH / 2) * k
    return (f'<g transform="translate({tx:.3f} {ty:.3f}) scale({k:.5f})">'
            f'<path fill="#FFFFFF" fill-rule="nonzero" d="{STEAM_D}"/></g>\n')


def xbox_mark(cx, cy, diameter):
    k = diameter / 24.0
    return (f'<g transform="translate({cx - 12 * k:.3f} {cy - 12 * k:.3f}) scale({k:.5f})">'
            f'<path fill="#FFFFFF" d="{XBOX_D}"/></g>\n')


def field(w, h, stops, glow_c, glow_cx, glow_cy, glow_r, glow_op, vec=None):
    x2, y2 = vec if vec else (w, h)
    st = ''.join(f'<stop offset="{o}" stop-color="{c}"/>' for o, c in stops)
    defs = (f'<linearGradient id="bg" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="{x2}" y2="{y2}">{st}</linearGradient>\n'
            f'<radialGradient id="glow" gradientUnits="userSpaceOnUse" cx="{glow_cx}" cy="{glow_cy}" r="{glow_r}">'
            f'<stop offset="0" stop-color="{glow_c}" stop-opacity="{glow_op}"/>'
            f'<stop offset="0.45" stop-color="{glow_c}" stop-opacity="{glow_op * 0.45:.3f}"/>'
            f'<stop offset="1" stop-color="{glow_c}" stop-opacity="0"/></radialGradient>\n')
    body = (f'<rect width="{w}" height="{h}" fill="url(#bg)"/>\n'
            f'<rect width="{w}" height="{h}" fill="url(#glow)"/>\n')
    return defs, body


# ---------------------------------------------------------------- Steam
STEAM_STOPS = [(0, '#1b2838'), (0.62, '#2a475e'), (1, '#3b6a8c')]
d, b = field(600, 900, STEAM_STOPS, '#66c0f4', 300, 430, 300, 0.34)
write('steam.svg', 600, 900, b + steam_mark(300, 430, 400), d)
d, b = field(1920, 1080, STEAM_STOPS, '#66c0f4', 960, 540, 880, 0.34)
write('steam-hero.svg', 1920, 1080, b + steam_mark(960, 540, 820), d)

# ---------------------------------------------------------------- Xbox
XBOX_STOPS = [(0, '#0b3d0b'), (0.55, '#0d5e0d'), (1, '#107C10')]
d, b = field(600, 900, XBOX_STOPS, '#7ddf7d', 300, 430, 300, 0.40)
write('box.svg', 600, 900, b + xbox_mark(300, 430, 320), d)
d, b = field(1920, 1080, XBOX_STOPS, '#7ddf7d', 960, 540, 880, 0.40)
write('box-hero.svg', 1920, 1080, b + xbox_mark(960, 540, 470), d)

# ---------------------------------------------------------------- text outlines
FONT = TTFont('/usr/share/fonts/TTF/OpenSans-CondensedBold.ttf')
GS = FONT.getGlyphSet()
CMAP = FONT.getBestCmap()
UPM = FONT['head'].unitsPerEm
CAP = FONT['OS/2'].sCapHeight


def text_outline(text, size, tracking_em, x, baseline, anchor='start'):
    """Return (path d, width). Glyph outlines at `size` px, no kerning, uniform tracking."""
    s = size / UPM
    adv = [GS[CMAP[ord(c)]].width * s for c in text]
    track = tracking_em * size
    width = sum(adv) + track * (len(text) - 1)
    if anchor == 'middle':
        x -= width / 2
    ds = []
    pen_x = x
    for c, a in zip(text, adv):
        pen = SVGPathPen(GS, ntos=lambda v: f'{v:.2f}')
        tp = TransformPen(pen, (s, 0, 0, -s, pen_x, baseline))
        GS[CMAP[ord(c)]].draw(tp)
        ds.append(pen.getCommands())
        pen_x += a + track
    return ' '.join(ds), width


def monitor_icon(x, y, k):
    """The existing desktop.png icon (outer box 244x221 at 178,167) scaled by k, top-left at x,y."""
    def p(v, o):
        return v * k
    g = (f'<g transform="translate({x:.3f} {y:.3f}) scale({k})" fill="#FFFFFF">'
         f'<rect x="10" y="10" width="224" height="157" rx="14" fill="none" stroke="#FFFFFF" stroke-width="20"/>'
         f'<rect x="100" y="176" width="44" height="25"/>'
         f'<rect x="78" y="200" width="88" height="20"/></g>\n')
    return g


# ---------------------------------------------------------------- Desktop hero
DESK_STOPS = [(0, '#212121'), (0.12, '#212121'), (1, '#757575')]
d, b = field(1920, 1080, DESK_STOPS, '#FFFFFF', 960, 540, 1, 0)
b = b.replace('<rect width="1920" height="1080" fill="url(#glow)"/>\n', '')
K = 2.4
icon_w, icon_h = 244 * K, 221 * K
size = (57 * K) / (CAP / UPM)
tpath, tw = text_outline('DESKTOP', size, 0.09, 0, 0)
gap = 128
total = icon_w + gap + tw
x0 = (1920 - total) / 2
y0 = (1080 - icon_h) / 2
# text: cap-height centred on icon's monitor centre
mon_cy = y0 + (177 / 2) * K
baseline = mon_cy + (CAP / UPM * size) / 2
tpath, tw = text_outline('DESKTOP', size, 0.09, x0 + icon_w + gap, baseline)
write('desktop-hero.svg', 1920, 1080,
      b + monitor_icon(x0, y0, K) + f'<path fill="#FFFFFF" d="{tpath}"/>\n', d)

# ---------------------------------------------------------------- Big Screen hero
src = open(os.path.join(HERE, '..', 'bigscreen-cover.svg')).read()
img = re.search(r'<image [^>]*/>', src).group(0)
tv_inner = '''
    <rect x="40" y="76" width="520" height="336" rx="48" fill="#0B0920"/>
    <rect x="40" y="76" width="520" height="336" rx="48" fill="none" stroke="#6F63C9" stroke-opacity="0.55" stroke-width="3"/>
    <rect x="62" y="96" width="476" height="296" rx="34" fill="url(#screen)"/>
    <g clip-path="url(#screenclip)">
      <circle cx="300" cy="190" r="170" fill="#FFB94A" fill-opacity="0.22" filter="url(#soft)"/>
      <rect x="84" y="296" width="120" height="76" rx="22" fill="#5A4DB0" fill-opacity="0.75"/>
      <rect x="224" y="296" width="152" height="76" rx="26" fill="#7B6CE0"/>
      <rect x="224" y="296" width="152" height="76" rx="26" fill="none" stroke="#FFE08A" stroke-width="6"/>
      <rect x="396" y="296" width="120" height="76" rx="22" fill="#5A4DB0" fill-opacity="0.75"/>
      <circle cx="262" cy="334" r="14" fill="#FFE08A"/>
      <rect x="286" y="322" width="64" height="12" rx="6" fill="#F3EEFF" fill-opacity="0.9"/>
      <rect x="286" y="340" width="40" height="9" rx="4.5" fill="#F3EEFF" fill-opacity="0.55"/>
    </g>
    ''' + img + '''
    <rect x="236" y="412" width="128" height="22" rx="8" fill="#0B0920"/>
    <rect x="190" y="430" width="220" height="14" rx="7" fill="#0B0920"/>
'''
pad_inner = re.search(r'<g transform="translate\(300 600\)">(.*?)</g>\s*</svg>', src, re.S).group(1)
bs_defs = '''<linearGradient id="bg" x1="0" y1="0" x2="0.35" y2="1">
  <stop offset="0" stop-color="#2A2161"/><stop offset="0.55" stop-color="#1B1640"/><stop offset="1" stop-color="#0F0C26"/></linearGradient>
<radialGradient id="glow" cx="574" cy="540" r="540" gradientUnits="userSpaceOnUse">
  <stop offset="0" stop-color="#FFB94A" stop-opacity="0.55"/><stop offset="0.55" stop-color="#F26222" stop-opacity="0.16"/><stop offset="1" stop-color="#F26222" stop-opacity="0"/></radialGradient>
<linearGradient id="screen" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#3A2E7E"/><stop offset="1" stop-color="#241C58"/></linearGradient>
<linearGradient id="pad" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#F3EEFF"/><stop offset="1" stop-color="#CFC4F2"/></linearGradient>
<clipPath id="screenclip"><rect x="62" y="96" width="476" height="296" rx="34"/></clipPath>
<filter id="soft" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="14"/></filter>
'''
KT, KC = 1.7, 1.4
tv_w = 520 * KT
pad_w = 476 * KC
gapb = 105
x_left = (1920 - (tv_w + gapb + pad_w)) / 2
tv_tx = x_left - 40 * KT
tv_ty = 540 - 260 * KT
pad_cx = x_left + tv_w + gapb + pad_w / 2
pad_ty = 540 - 29 * KC + 10
bs_body = (f'<rect width="1920" height="1080" fill="url(#bg)"/>\n'
           f'<rect width="1920" height="1080" fill="url(#glow)"/>\n'
           f'<g transform="translate({tv_tx:.3f} {tv_ty:.3f}) scale({KT})">{tv_inner}</g>\n'
           f'<g transform="translate({pad_cx:.3f} {pad_ty:.3f}) scale({KC})">{pad_inner}</g>\n')
write('bigscreen-hero.svg', 1920, 1080, bs_body, bs_defs)
print('ok', sorted(os.listdir(OUT)), 'tv glow centre x', tv_tx + 300 * KT)
