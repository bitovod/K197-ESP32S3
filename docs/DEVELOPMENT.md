# Development notes

**English** · [Česky](#poznámky-pro-vývoj)

## Building

- Arduino IDE 2.x, the Espressif **esp32** core. Board settings: ESP32S3 Dev Module (or your board),
  **PSRAM: OPI PSRAM**, **Partition scheme: Huge APP**, **USB CDC On Boot: Enabled**.
- Libraries: Adafruit GFX + BusIO, Vrekrer SCPI parser, Adafruit_NV3007 and DS3231Centisecond (both in `libraries/`), plus the
  core's Wire, SPI and EEPROM.
- The sketch folder name must equal the main file name: `firmware/ESP32S3_K197/ESP32S3_K197.ino`.
- The version is `SCPI_IDN_FIRMWARE` in `config.h` (`C2v20` …). It is shown in the help and returned by `*IDN?`.
- Size (v18): sketch ~1.28 MB of 2.0 MB, globals ~151 kB of 320 kB. The log takes the free PSRAM.

## Architecture

**Tasks** (priorities in `config.h`):

| Core | Task | Prio | Job |
|---|---|---|---|
| 0 | capture | 2 | SPI3 slave → decode the DMM frame → display setters, log, UI, alarm |
| 0 | blit | 1 | streams the finished frame to the TFT (the only task that touches the TFT) |
| 1 | buttons | 3 | woken by the key interrupts; debounce, events, REL/dB pulses, chord detection |
| 1 | render/UI | 2 | `uiPoll()` (screens, keys, statistics and graph in time slices) + composing a frame |
| 1 | comm | 1 | SCPI, RTC polling, deferred EEPROM writes, debug output |

**Display pipeline.** The setters only record the requested state; they are cheap and safe from any task. The
render task composes a whole 428 × 94 frame into one of two 6 bpp framebuffers (321 B per row, 64-colour palette =
background + three 21-step ramps: ghost, indicator, bright). The blit task sends the other one. The 94 content
rows sit at `SYSTem:YOFFset` on the 142-row panel. The LCD replica uses the theme as it is; the text screens
always use a black background (`fbStatsTheme()`). Rendering takes about 10 ms, a blit 25–37 ms.

**The log.** `LogRecord` is 13 bytes (RTC seconds, centiseconds, float value in base units, indicator bits, unit,
OL flag) in a PSRAM ring buffer of about 600 000 records. Readers that follow the log (the graph, the alarm
defaults, markers) use serial numbers (`datalogReadSerial()` etc.) so that the ring wrap does not disturb them. A
marker is bit 15 of `indicatorBits`, which the K197 does not use; bit 14 flags a reading that is still settling
after a change of the function (left out of statistics, graph and alarm).

**Statistics and graph** are computed in the render/UI task in time slices (about 3 ms per pass), so even a full
log never blocks the keys or the picture. The graph keeps min / max / sum / count for each of its 384 columns.
Column borders are on whole multiples of the column length, computed in integers. The ESP32-S3 has a
single-precision FPU only, and `double` is done in software.

**Settings** live in emulated EEPROM as one struct with a magic byte. Every new firmware version that adds fields
appends them and migrates the previous layout, so stored settings survive updates. Front-panel changes are saved
deferred (10 s after the last change), SCPI changes at once.

## Lessons learned (the Arduino build)

- **All `.ino` files are merged into one translation unit.** Static names must be unique across files, so every
  module uses a prefix (`ui…`, `graph…`, `alarm…`).
- **Generated prototypes.** Arduino inserts prototypes of all `.ino` functions right after the main sketch's
  `#include`s. A struct used as a parameter type must therefore be defined in a header, and every such header
  must also be included in the main `.ino`. The host tests compile files one by one and do not catch this.
- **Never print from time-critical tasks.** USB Serial can block for a long time. Debug messages go through a
  non-blocking queue (`debuglog.h`) that the comm task drains.
- **In an ISR**, no Serial and no `esp_timer_*`; notify a task instead. `portYIELD_FROM_ISR` differs between IDF
  versions.
- **The SPI3 slave** receive buffer must be a multiple of 4 bytes. Keep exactly one transaction queued in the
  driver.
- **The Vrekrer SCPI parser** keeps one flat pool of keyword tokens. If the short form of one token equals another
  token (`READ` vs `READing`), commands silently go to the wrong place. New tokens are checked against the whole
  pool, and the limits `SCPI_MAX_TOKENS/COMMANDS` are set in `config.h`.
- **`volatile` and `++`.** `++` / `|=` on `volatile` variables is deprecated in C++20; write `x = x + 1`.
- **Hardware.** Artefacts on the TFT can come from mechanical stress on the panel or its cable. The ESP32 start-up
  current can hang the K197 if both share its 5 V supply.

## Host tests

`cd host_tests && sh run_tests.sh` (only g++, C++17). The real firmware files are compiled against stand-ins for
Arduino, FreeRTOS, the TFT, GPIO, timers and EEPROM (`host_tests/stubs`):

| Test | What it covers |
|---|---|
| `test_fb` | framebuffer packing, palette, LCD composition against an independent reference, fonts, themes |
| `test_display` | the real `display.ino` with threads: tear detection, UI wake-up, all screens |
| `test_ui` | the real `ui.ino` + log: formatting, statistics jobs and time windows, every key on every screen, help texts (every line ≤ 47 characters, whole pages), alarm, markers |
| `test_buttons` | the real `buttons.ino` with simulated GPIO: debounce, events, REL/dB pulses, interlock, chord |
| `test_settings` | deferred EEPROM writes, migrations of every older layout |
| `test_dmm` | SPI slave capture, a DMM that starts late, the no-data watchdog |
| `test_debuglog` | the non-blocking debug queue |
| `test_graph` | graph columns against an independent model, scrolling, gaps, OL, axis modes, FULL, ring wrap, markers |

The timing-based checks of `test_buttons` can fail on a heavily loaded PC; run them again.

## Tools

| | |
|---|---|
| `tools/gen_font.py` | generates `font_data.h` (antialiased fonts of the text screens; default JetBrains Mono Bold, zero without the dot) |
| `tools/gen_pixel_font.py` | generates `font_pixel.h` (5×7 font of the graph, from Adafruit GFX) |
| `tools/gen_tag_shape.py` | generates `tag_shape.h` ("TAG" in the style of the original indicators, DejaVu Sans Condensed Bold) |
| `tools/make_screenshots.sh` | renders `docs/images/*.png` with the real drawing code |

## Code conventions

C/C++ in Allman style. Comments in English, `//` on every line (no `/* */`). Every module uses its own name prefix.
Display layout changes are tried as preview pictures first.

## Ideas / to do

- SD card: a continuous binary journal, reload into PSRAM after power-up, CSV export of the window shown in the
  graph, `MMEMory:*` commands.
- Calibration screen for the visible area behind the bezel.
- Battery option (the original Model 1978 pack), temperature sensors, a passive piezo with several tones.

---

# Poznámky pro vývoj

## Překlad

- Arduino IDE 2.x, jádro **esp32** od Espressifu. Nastavení desky: ESP32S3 Dev Module (nebo vaše deska),
  **PSRAM: OPI PSRAM**, **Partition scheme: Huge APP**, **USB CDC On Boot: Enabled**.
- Knihovny: Adafruit GFX + BusIO, Vrekrer SCPI parser, Adafruit_NV3007 a DS3231Centisecond (obě v `libraries/`) a Wire, SPI
  a EEPROM z jádra.
- Název složky sketche musí odpovídat hlavnímu souboru: `firmware/ESP32S3_K197/ESP32S3_K197.ino`.
- Verze je `SCPI_IDN_FIRMWARE` v `config.h` (`C2v20` …). Zobrazuje se v nápovědě a vrací ji `*IDN?`.
- Velikost (v18): sketch asi 1,28 MB z 2,0 MB, globální proměnné asi 151 kB z 320 kB. Log zabere volnou PSRAM.

## Architektura

**Tasky** (priority v `config.h`):

| Jádro | Task | Prio | Úloha |
|---|---|---|---|
| 0 | capture | 2 | SPI3 slave → dekódování rámce DMM → stav displeje, log, UI, alarm |
| 0 | blit | 1 | posílá hotový snímek do TFT (jediný task, který na TFT sahá) |
| 1 | buttons | 3 | probouzí ho přerušení tlačítek; odrušení zákmitů, události, pulsy REL/dB, detekce akordu |
| 1 | render/UI | 2 | `uiPoll()` (obrazovky, tlačítka, statistika a graf po časových dílech) + skládání snímku |
| 1 | comm | 1 | SCPI, čtení RTC, odložené zápisy do EEPROM, ladicí výstup |

**Vykreslování.** Settery jen zaznamenají požadovaný stav; jsou levné a bezpečné z libovolného tasku. Render task
složí celý snímek 428 × 94 do jednoho ze dvou 6bitových framebufferů (321 B na řádek, paleta 64 barev = pozadí +
tři rampy po 21 krocích: ghost, indicator, bright). Blit task posílá ten druhý. 94 řádků obsahu leží na panelu se
142 řádky od `SYSTem:YOFFset`. Replika LCD používá téma tak, jak je; textové obrazovky mají vždy černé pozadí
(`fbStatsTheme()`). Vykreslení trvá asi 10 ms, přenos 25–37 ms.

**Log.** `LogRecord` má 13 bajtů (sekundy RTC, setiny, hodnota float v základních jednotkách, bity indikátorů,
jednotka, příznak OL) v kruhovém bufferu v PSRAM pro asi 600 000 záznamů. Kdo log průběžně sleduje (graf, výchozí
meze alarmu, markery), používá sériová čísla (`datalogReadSerial()` atd.), aby mu nevadilo přetočení kruhu. Marker
je bit 15 v `indicatorBits`, který K197 nepoužívá; bit 14 označuje odečet, který se po změně funkce ještě ustaluje
(statistika, graf a alarm ho vynechají).

**Statistika a graf** se počítají v render/UI tasku po časových dílech (asi 3 ms na průchod), takže ani plný log
nikdy nezablokuje tlačítka ani obraz. Graf si pro každý ze 384 sloupců drží min / max / součet / počet. Hranice
sloupců leží na celých násobcích délky sloupce a počítají se v celých číslech. ESP32-S3 má FPU jen pro single
precision a `double` počítá softwarově.

**Nastavení** je v emulované EEPROM jako jedna struktura s magickým bajtem. Každá nová verze firmwaru, která přidá
položky, je připojí na konec a starší rozložení převede, takže uložené nastavení přežije aktualizace. Změny z panelu
se ukládají odloženě (10 s po poslední změně), změny přes SCPI hned.

## Poučení (překlad v Arduinu)

- **Všechny soubory `.ino` se spojí do jedné překladové jednotky.** Statická jména musí být unikátní napříč
  soubory, proto má každý modul předponu (`ui…`, `graph…`, `alarm…`).
- **Generované prototypy.** Arduino vkládá prototypy všech funkcí z `.ino` hned za `#include` hlavního sketche.
  Struktura použitá jako typ parametru proto musí být definovaná v hlavičce a každá taková hlavička musí být
  includovaná i v hlavním `.ino`. Testy na PC překládají soubory jednotlivě a tohle neodhalí.
- **Z časově kritických tasků nikdy nevypisovat.** USB Serial umí blokovat dlouho. Ladicí zprávy jdou přes
  neblokující frontu (`debuglog.h`), kterou vyprazdňuje comm task.
- **V ISR** žádný Serial ani `esp_timer_*`; místo toho probudit task. `portYIELD_FROM_ISR` se mezi verzemi IDF liší.
- **SPI3 slave:** přijímací buffer musí mít násobek 4 bajtů. V ovladači držet právě jednu transakci.
- **Parser Vrekrer SCPI** má jeden plochý seznam klíčových slov. Když se krátký tvar jednoho slova shoduje s jiným
  slovem (`READ` × `READing`), příkazy tiše skončí jinde. Nová slova se kontrolují proti celému seznamu a limity
  `SCPI_MAX_TOKENS/COMMANDS` jsou v `config.h`.
- **`volatile` a `++`.** `++` / `|=` na `volatile` je v C++20 zastaralé; psát `x = x + 1`.
- **Hardware.** Artefakty na TFT může způsobit mechanické pnutí panelu nebo kabelu. Proudový náraz ESP32 při
  startu umí zaseknout K197, pokud oba sdílejí jeho 5V napájení.

## Testy na PC

`cd host_tests && sh run_tests.sh` (stačí g++, C++17). Skutečné soubory firmwaru se překládají proti náhradám
Arduina, FreeRTOS, displeje, GPIO, časovačů a EEPROM (`host_tests/stubs`):

| Test | Co pokrývá |
|---|---|
| `test_fb` | balení framebufferu, paleta, skládání LCD proti nezávislé referenci, fonty, témata |
| `test_display` | skutečný `display.ino` s vlákny: trhání obrazu, probouzení UI, všechny obrazovky |
| `test_ui` | skutečný `ui.ino` + log: formátování, výpočty statistiky a časová okna, všechna tlačítka na všech obrazovkách, texty nápovědy (každý řádek ≤ 47 znaků, celé strany), alarm, markery |
| `test_buttons` | skutečný `buttons.ino` se simulovaným GPIO: odrušení zákmitů, události, pulsy REL/dB, blokování, akord |
| `test_settings` | odložené zápisy do EEPROM, převody všech starších rozložení |
| `test_dmm` | příjem SPI slave, DMM, který startuje později, hlídání chybějících dat |
| `test_debuglog` | neblokující fronta ladicích zpráv |
| `test_graph` | sloupce grafu proti nezávislému modelu, posun, mezery, OL, režimy osy, FULL, přetočení kruhu, markery |

Časové kontroly v `test_buttons` mohou na hodně vytíženém PC selhat; stačí je spustit znovu.

## Nástroje

| | |
|---|---|
| `tools/gen_font.py` | generuje `font_data.h` (vyhlazené fonty textových obrazovek; výchozí JetBrains Mono Bold, nula bez tečky) |
| `tools/gen_pixel_font.py` | generuje `font_pixel.h` (font 5×7 pro graf, z Adafruit GFX) |
| `tools/gen_tag_shape.py` | generuje `tag_shape.h` („TAG“ ve stylu původních indikátorů, DejaVu Sans Condensed Bold) |
| `tools/make_screenshots.sh` | vykreslí `docs/images/*.png` skutečným kreslicím kódem |

## Konvence kódu

C/C++ ve stylu Allman. Komentáře anglicky, `//` na každém řádku (žádné `/* */`). Každý modul má vlastní předponu
jmen. Změny rozvržení displeje se nejdřív zkoušejí na náhledových obrázcích.

## Nápady / k dodělání

- SD karta: průběžný binární žurnál, načtení zpět do PSRAM po zapnutí, export CSV okna zobrazeného v grafu,
  příkazy `MMEMory:*`.
- Kalibrační obrazovka viditelné plochy za rámečkem.
- Bateriové napájení (původní sada Model 1978), teplotní čidla, pasivní piezo s více tóny.
