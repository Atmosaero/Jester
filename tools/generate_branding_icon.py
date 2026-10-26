"""Rasterize the two authored SVG polygons into a multi-resolution Windows ICO.

Pillow is a development-only dependency. The SVG and ICO are checked in; normal
builds and the editor do not run Python or need Pillow.
"""
from pathlib import Path
import xml.etree.ElementTree as ET
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1]
source = root / 'Editor/resources/branding/jester.svg'
svg = ET.parse(source).getroot()
_, _, width, height = map(float, svg.attrib['viewBox'].split())
factor = 2048 / max(width, height)
offset = ((2048 - width * factor) / 2, (2048 - height * factor) / 2)
image = Image.new('RGBA', (2048, 2048))
draw = ImageDraw.Draw(image)
for polygon in svg.findall('{http://www.w3.org/2000/svg}polygon'):
    points = [tuple(map(float, pair.split(','))) for pair in polygon.attrib['points'].split()]
    draw.polygon([(x * factor + offset[0], y * factor + offset[1]) for x, y in points], fill=polygon.attrib['fill'])
image = image.resize((256, 256), Image.Resampling.LANCZOS)
image.save(source.with_suffix('.ico'), sizes=[(n, n) for n in [16, 20, 24, 32, 48, 64, 128, 256]])
preview = root / 'artifacts/branding/logo.png'
preview.parent.mkdir(parents=True, exist_ok=True)
image.save(preview)
print(source.with_suffix('.ico'))
