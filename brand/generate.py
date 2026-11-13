#!/usr/bin/env python3
"""Generates the Loom logo: the chosen mark ("Threads into Fabric") as the final set in this folder,
and the six concept tiles it was picked from in <repo>/.local/brand-concepts (not committed).

Colours follow React Native's own palette: cyan #61DAFB on the #20232A charcoal of its docs
(dark), and the deeper #087EA4 / #58C4DC cyans for light backgrounds. Each mark sits in a
rounded tile with its own background and a hairline border, like an app icon.
"""
import html
import os

HERE = os.path.dirname(os.path.abspath(__file__))
CONCEPTS_DIR = os.path.join(HERE, '..', '.local', 'brand-concepts')

PALETTES = {
    'dark': dict(bg='#20232A', border='#3A3F4B', warp='#61DAFB', weft='#E6EDF5', ink='#FFFFFF',
                 page='#16181D', card='#1C1F26', text='#F5F7FA', muted='#9AA3B2'),
    'light': dict(bg='#FFFFFF', border='#D8DEE6', warp='#087EA4', weft='#58C4DC', ink='#23272F',
                  page='#F3F5F8', card='#E9EDF2', text='#23272F', muted='#5B6472'),
}


def strands(spec, halo, width):
    out = []
    for x1, y1, x2, y2, colour in spec:
        out.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{halo}" stroke-width="{width + 6}"/>')
        out.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{colour}" stroke-width="{width}"/>')
    return ''.join(out)


def weave(p):
    bg, w, f = p['bg'], p['warp'], p['weft']
    v = [(x, 14, x, 82, w) for x in (28, 48, 68)]
    h = [(14, y, 82, y, f) for y in (28, 48, 68)]
    over = [(x, y - 9, x, y + 9, w) for x, y in ((28, 28), (68, 28), (48, 48), (28, 68), (68, 68))]
    return (f'<g stroke-linecap="round" fill="none">{strands(v, bg, 10)}{strands(h, bg, 10)}</g>'
            f'<g stroke-linecap="butt" fill="none">{strands(over, bg, 10)}</g>')


def shuttle(p):
    bg, w, f = p['bg'], p['warp'], p['weft']
    t = 'M56 48 C 66 48, 70 74, 92 78'
    return (f'<path d="M6 48 C 26 26, 70 26, 90 48 C 70 70, 26 70, 6 48 Z" fill="{w}"/>'
            f'<rect x="36" y="42" width="24" height="12" rx="6" fill="{bg}"/>'
            f'<rect x="40" y="45" width="16" height="6" rx="3" fill="{f}"/>'
            f'<path d="{t}" stroke="{bg}" stroke-width="9" fill="none" stroke-linecap="round"/>'
            f'<path d="{t}" stroke="{f}" stroke-width="4" fill="none" stroke-linecap="round"/>')


def woven_l(p):
    bg, w, f = p['bg'], p['warp'], p['weft']
    v = [(x, 12, x, 84, w) for x in (30, 46)]
    h = [(20, y, 84, y, f) for y in (60, 76)]
    over = [(30, 51, 30, 69, w), (46, 67, 46, 85, w)]
    return (f'<g stroke-linecap="round" fill="none">{strands(v, bg, 10)}{strands(h, bg, 10)}</g>'
            f'<g stroke-linecap="butt" fill="none">{strands(over, bg, 10)}</g>'
            f'<circle cx="46" cy="84" r="5" fill="{w}"/>')


def threads_into_fabric(p):
    bg, w, f = p['bg'], p['warp'], p['weft']
    loose = ''.join(f'<line x1="6" y1="{y}" x2="44" y2="{y}" stroke="{f}" stroke-width="4"/>' for y in (34, 48, 62))
    v = [(x, 22, x, 74, w) for x in (54, 68, 82)]
    h = [(44, y, 88, y, f) for y in (34, 48, 62)]
    over = [(54, 26, 54, 42, w), (82, 26, 82, 42, w), (68, 40, 68, 56, w), (54, 54, 54, 70, w), (82, 54, 82, 70, w)]
    return (f'<g stroke-linecap="round" fill="none">{loose}{strands(v, bg, 8)}{strands(h, bg, 8)}</g>'
            f'<g stroke-linecap="butt" fill="none">{strands(over, bg, 8)}</g>')


def loom_frame(p):
    bg, w, f, ink = p['bg'], p['warp'], p['weft'], p['ink']
    d = 'M18 48 Q22 40 28 40 Q35 40 35 48 Q35 56 42 56 Q49 56 49 48 Q49 40 56 40 Q63 40 63 48 Q63 56 70 56 Q76 56 79 50'
    warps = ''.join(f'<line x1="{x}" y1="22" x2="{x}" y2="74" stroke="{w}" stroke-width="5"/>' for x in (28, 42, 56, 70))
    over = [(42, 49, 42, 63, w), (70, 49, 70, 63, w)]
    return (f'<rect x="9" y="12" width="78" height="72" rx="12" fill="none" stroke="{ink}" stroke-width="6"/>'
            f'<g stroke-linecap="round" fill="none">{warps}'
            f'<path d="{d}" stroke="{bg}" stroke-width="12"/><path d="{d}" stroke="{f}" stroke-width="6"/></g>'
            f'<g stroke-linecap="butt" fill="none">{strands(over, bg, 5)}</g>')


def interlocked_oo(p):
    bg, w, f, ink = p['bg'], p['warp'], p['weft'], p['ink']
    arc = 'M96.21 57.57 A18 18 0 0 1 92.61 50.66'
    font = 'font-family="Space Grotesk, Inter, Helvetica, Arial, sans-serif" font-size="66" font-weight="600"'
    return (f'<text x="6" y="62" {font} fill="{ink}">l</text>'
            f'<circle cx="110" cy="46" r="18" fill="none" stroke="{bg}" stroke-width="15"/>'
            f'<circle cx="110" cy="46" r="18" fill="none" stroke="{f}" stroke-width="9"/>'
            f'<circle cx="78" cy="46" r="18" fill="none" stroke="{bg}" stroke-width="15"/>'
            f'<circle cx="78" cy="46" r="18" fill="none" stroke="{w}" stroke-width="9"/>'
            f'<path d="{arc}" fill="none" stroke="{bg}" stroke-width="15"/>'
            f'<path d="{arc}" fill="none" stroke="{f}" stroke-width="9"/>'
            f'<text x="136" y="62" {font} fill="{ink}">m</text>')


# slug, name, draw, mark size (w, h), tile size (w, h), corner radius
CONCEPTS = [
    ('01-warp-weft', 'Warp & Weft', weave, (96, 96), (128, 128), 28),
    ('02-shuttle', 'Shuttle', shuttle, (96, 96), (128, 128), 28),
    ('03-woven-l', 'Woven L', woven_l, (96, 96), (128, 128), 28),
    ('04-threads-into-fabric', 'Threads into Fabric', threads_into_fabric, (96, 96), (128, 128), 28),
    ('05-loom-frame', 'Loom Frame', loom_frame, (96, 96), (128, 128), 28),
    ('06-interlocked-oo', 'Interlocked oo', interlocked_oo, (200, 80), (248, 128), 28),
]


def tile(draw, mark, size, radius, p, name):
    tw, th = size
    dx, dy = (tw - mark[0]) / 2, (th - mark[1]) / 2
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {tw} {th}" width="{tw * 4}" height="{th * 4}" '
            f'role="img" aria-label="Loom logo concept: {html.escape(name)}">'
            f'<rect x="0.75" y="0.75" width="{tw - 1.5}" height="{th - 1.5}" rx="{radius}" '
            f'fill="{p["bg"]}" stroke="{p["border"]}" stroke-width="1.5"/>'
            f'<g transform="translate({dx} {dy})">{draw(p)}</g></svg>\n')


def main():
    os.chdir(HERE)
    os.makedirs(CONCEPTS_DIR, exist_ok=True)
    sections = []
    for theme, p in PALETTES.items():
        cards = []
        for slug, name, draw, mark, size, radius in CONCEPTS:
            with open(os.path.join(CONCEPTS_DIR, f'{slug}-{theme}.svg'), 'w') as out:
                out.write(tile(draw, mark, size, radius, p, name))
            word = '' if slug.startswith('06') else '<span class="word">loom</span>'
            cards.append(
                f'<div class="card" style="background:{p["card"]}">'
                f'<div class="lock"><img src="{slug}-{theme}.svg" alt="{html.escape(name)}" '
                f'style="height:120px">{word}</div>'
                f'<div><div class="num" style="color:{p["muted"]}">{slug[:2]}</div>'
                f'<div class="name">{html.escape(name)}</div></div></div>')
        sections.append(
            f'<section style="background:{p["page"]};color:{p["text"]}">'
            f'<h2>{theme.title()}</h2><div class="grid">{"".join(cards)}</div></section>')
    page = f'''<!doctype html><html lang="en"><head><meta charset="utf-8"><title>Loom logo concepts</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;600&family=Space+Grotesk:wght@600;700&display=swap" rel="stylesheet">
<style>
body{{margin:0;font-family:Inter,system-ui,sans-serif;background:#16181D}}
header{{padding:48px 64px 24px;color:#F5F7FA}}
h1{{margin:0;font-family:"Space Grotesk",sans-serif;font-size:40px;letter-spacing:-.02em}}
header p{{margin:8px 0 0;color:#9AA3B2}}
section{{padding:32px 64px 56px}} h2{{margin:0 0 20px;font-size:18px}}
.grid{{display:grid;grid-template-columns:repeat(auto-fill,minmax(340px,1fr));gap:24px}}
.card{{border-radius:20px;padding:28px;height:260px;box-sizing:border-box;display:flex;flex-direction:column;justify-content:space-between}}
.lock{{display:flex;align-items:center;gap:20px}}
.word{{font-family:"Space Grotesk",sans-serif;font-size:44px;font-weight:600;letter-spacing:-.03em}}
.num{{font-size:13px;font-weight:600}} .name{{font-size:17px;font-weight:600}}
</style></head><body>
<header><h1>Loom — logo concepts</h1>
<p>react-native-loom: a faster renderer for React Native. Colours from React Native's palette; each SVG here is a standalone tile.</p></header>
{"".join(sections)}</body></html>'''
    with open(os.path.join(CONCEPTS_DIR, 'index.html'), 'w') as out:
        out.write(page)
    final_set()



def threads_into_fabric_small(p):
    """The mark for 16-48 px: two loose threads and a 2x2 weave, strokes thick enough to survive."""
    bg, w, f = p['bg'], p['warp'], p['weft']
    loose = ''.join(f'<line x1="8" y1="{y}" x2="44" y2="{y}" stroke="{f}" stroke-width="8"/>' for y in (36, 60))
    v = [(x, 20, x, 76, w) for x in (56, 78)]
    h = [(44, y, 88, y, f) for y in (36, 60)]
    over = [(56, 27, 56, 45, w), (78, 51, 78, 69, w)]
    return (f'<g stroke-linecap="round" fill="none">{loose}{strands(v, bg, 12)}{strands(h, bg, 12)}</g>'
            f'<g stroke-linecap="butt" fill="none">{strands(over, bg, 12)}</g>')


FONT_IMPORT = ('<style>@import url("https://fonts.googleapis.com/css2?family=Space+Grotesk:wght@600;700'
               '&amp;family=Inter:wght@400;500&amp;display=swap");</style>')


def lockup(p):
    """Icon tile plus the "loom" wordmark, side by side."""
    tw, th = 128, 128
    w = 400
    icon = tile(threads_into_fabric, (96, 96), (tw, th), 28, p, 'Loom')
    inner = icon.split('>', 1)[1].rsplit('</svg>', 1)[0]
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {th}" width="{w * 2}" height="{th * 2}" '
            f'role="img" aria-label="Loom">{FONT_IMPORT}{inner}'
            f'<text x="152" y="94" font-family="Space Grotesk, Inter, Helvetica, Arial, sans-serif" '
            f'font-size="92" font-weight="600" letter-spacing="-2.5" fill="{p["ink"] if p["bg"] != "#FFFFFF" else "#23272F"}">loom</text>'
            f'</svg>\n')


def social_preview(p):
    """1280x640: GitHub's social preview size."""
    icon = tile(threads_into_fabric, (96, 96), (128, 128), 28, p, 'Loom')
    inner = icon.split('>', 1)[1].rsplit('</svg>', 1)[0]
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1280 640" width="1280" height="640" '
            f'role="img" aria-label="react-native-loom: a faster renderer for React Native">{FONT_IMPORT}'
            f'<rect width="1280" height="640" fill="#16181D"/>'
            f'<g transform="translate(112 192) scale(2)">{inner}</g>'
            f'<text x="408" y="290" font-family="Space Grotesk, Inter, sans-serif" font-size="76" font-weight="700" '
            f'letter-spacing="-2" fill="#FFFFFF">react-native-loom</text>'
            f'<text x="412" y="356" font-family="Inter, sans-serif" font-size="34" font-weight="500" fill="#61DAFB">'
            f'A faster renderer for React Native.</text>'
            f'<text x="412" y="410" font-family="Inter, sans-serif" font-size="26" fill="#9AA3B2">'
            f'Instant updates · native lists · JavaScript on the thread you choose.</text></svg>\n')


def final_set():
    for theme, p in PALETTES.items():
        with open(f'loom-icon-{theme}.svg', 'w') as out:
            out.write(tile(threads_into_fabric, (96, 96), (128, 128), 28, p, 'Loom'))
        with open(f'loom-icon-small-{theme}.svg', 'w') as out:
            out.write(tile(threads_into_fabric_small, (96, 96), (128, 128), 28, p, 'Loom'))
        with open(f'loom-lockup-{theme}.svg', 'w') as out:
            out.write(lockup(p))
    with open('loom-social-preview.svg', 'w') as out:
        out.write(social_preview(PALETTES['dark']))


if __name__ == '__main__':
    main()
