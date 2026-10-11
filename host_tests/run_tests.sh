#!/bin/sh
# Host-side tests for the ESP32S3_K197 firmware (display, user interface, buttons, log, graph, settings).
# Needs only g++ (C++17). Usage: sh run_tests.sh [path to the sketch folder]
# (default: ../firmware/ESP32S3_K197). Deliberately kept OUTSIDE the sketch
# folder - Arduino would try to compile any .cpp it finds in there.
SKETCH="${1:-../firmware/ESP32S3_K197}"
HERE="$(cd "$(dirname "$0")" && pwd)"
set -e
cd "$HERE"
echo "== framebuffer.h: packing, palette, quantization error vs the old direct renderer, decimal point"
g++ -std=c++17 -Wall -Wextra -Istubs -I"$SKETCH" test_fb.cpp -o /tmp/k197_test_fb
/tmp/k197_test_fb | tail -4
echo "== display.ino: boot frame, state setters, statistics screen, threaded stress, tear detection"
g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -pthread -Istubs -I"$SKETCH" test_display.cpp -o /tmp/k197_test_display
/tmp/k197_test_display
echo "== ui.ino + datalog.ino + reading_math.ino + ui_format.h: statistics, formatting, buttons -> screens"
B=/tmp/k197_uibuild
rm -rf $B; mkdir -p $B
cp "$SKETCH"/* $B/
cp stubs/rtc_stub.h $B/rtc.h
g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-type-limits -Istubs -I$B test_ui.cpp -o /tmp/k197_test_ui
/tmp/k197_test_ui
echo "== buttons.ino: interrupt handlers, button task, debounce, events, REL/dB pulse and interlock (GPIO and timers simulated)"
g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -pthread -Istubs -I"$SKETCH" test_buttons.cpp -o /tmp/k197_test_buttons
/tmp/k197_test_buttons
echo "== settings.ino: deferred EEPROM write of the statistics interval, immediate SCPI saves, migration 0xA9 -> 0xAA"
g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Istubs -I"$SKETCH" test_settings.cpp -o /tmp/k197_test_settings
/tmp/k197_test_settings
echo "== dmm_bridge.ino: SPI slave capture vs a K197 that starts late, no-data watchdog (Err197, SPI restart)"
g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Istubs -I"$SKETCH" test_dmm.cpp -o /tmp/k197_test_dmm
/tmp/k197_test_dmm
echo "== debuglog.ino: debug messages are queued without ever waiting, gated by SYSTem:DEBUG, dropped when the queue is full"
g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -pthread -Istubs -I"$SKETCH" test_debuglog.cpp -o /tmp/k197_test_debuglog
/tmp/k197_test_debuglog
echo "== graph.ino (v14) + datalog.ino serial reading: columns vs an independent model, scrolling, gaps, OL, axis modes, FULL, ring wrap, settings screen, drawing"
B=/tmp/k197_graphbuild
rm -rf $B; mkdir -p $B
cp "$SKETCH"/* $B/
cp stubs/rtc_stub.h $B/rtc.h
g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-type-limits -Istubs -I$B test_graph.cpp -o /tmp/k197_test_graph
/tmp/k197_test_graph $K197_PREVIEW_DIR
echo "== v22: settling after a function change / after the K197 was switched on (real constants)"
g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-type-limits -Istubs -I$B test_settle.cpp -o /tmp/k197_test_settle
/tmp/k197_test_settle
