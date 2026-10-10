#!/bin/sh
# Renders the screenshots in docs/images with the real drawing code of the
# firmware (host_tests/render_screens.cpp and the pictures test_graph writes).
# Needs g++ and python3 with Pillow. Usage: sh tools/make_screenshots.sh
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SKETCH="$ROOT/firmware/ESP32S3_K197"
OUT="$ROOT/docs/images"
TMP="$(mktemp -d)"
cd "$ROOT/host_tests"
g++ -std=c++17 -w -Istubs -I"$SKETCH" render_screens.cpp -o "$TMP/render_screens"
"$TMP/render_screens" "$TMP"
B="$TMP/graphbuild"
mkdir -p "$B"
cp "$SKETCH"/* "$B/"
cp stubs/rtc_stub.h "$B/rtc.h"
g++ -std=c++17 -w -Istubs -I"$B" test_graph.cpp -o "$TMP/test_graph"
"$TMP/test_graph" "$TMP" > /dev/null
mkdir -p "$OUT"
python3 - "$TMP" "$OUT" <<'PY'
import sys
from PIL import Image
src, out = sys.argv[1], sys.argv[2]
names = ['lcd_grn', 'lcd_lcd1', 'lcd_ohm', 'lcd_ac', 'lcd_alarm_both', 'lcd_alarm_high', 'lcd_alarm_low', 'lcd_tag',
         'stats_grn', 'stats_lcd1', 'help_cz', 'help_en', 'setup_dmm', 'setup_alarm',
         'graph_markers_5min', 'graph_10sec', 'graph_setup_en']
for n in names:
    im = Image.open('%s/%s.ppm' % (src, n))
    im.resize((im.width * 2, im.height * 2), Image.NEAREST).save('%s/%s.png' % (out, n))
    print('docs/images/%s.png' % n)
# all color themes: one picture per theme (LCD replica above, statistics below)
# and an overview sheet with the theme names
from PIL import ImageDraw, ImageFont
NAMES = ['RED', 'GRN', 'ORG1', 'ORG2', 'VFD', 'LCD1', 'LCD2', 'LCD3']
try:
    font = ImageFont.truetype('DejaVuSans-Bold.ttf', 22)
except OSError:
    font = ImageFont.load_default()
tiles = []
for i, n in enumerate(NAMES):
    a = Image.open('%s/theme_%d_lcd.ppm' % (src, i))
    b = Image.open('%s/theme_%d_ui.ppm' % (src, i))
    t = Image.new('RGB', (a.width, a.height + b.height + 4), (40, 40, 40))
    t.paste(a, (0, 0))
    t.paste(b, (0, a.height + 4))
    t = t.resize((t.width * 2, t.height * 2), Image.NEAREST)
    t.save('%s/theme_%s.png' % (out, n.lower()))
    print('docs/images/theme_%s.png' % n.lower())
    tiles.append((n, t))
pad, cap = 16, 34
w, h = tiles[0][1].width // 2, tiles[0][1].height // 2
sheet = Image.new('RGB', (pad + 2 * (w + pad), pad + 4 * (h + cap + pad)), (24, 24, 24))
d = ImageDraw.Draw(sheet)
for i, (n, t) in enumerate(tiles):
    x = pad + (i % 2) * (w + pad)
    y = pad + (i // 2) * (h + cap + pad)
    d.text((x, y), n, fill=(230, 230, 230), font=font)
    sheet.paste(t.resize((w, h), Image.NEAREST), (x, y + cap))
sheet.save('%s/themes.png' % out)
print('docs/images/themes.png')
PY
rm -rf "$TMP"
