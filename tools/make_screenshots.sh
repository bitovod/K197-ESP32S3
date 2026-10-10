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
PY
rm -rf "$TMP"
