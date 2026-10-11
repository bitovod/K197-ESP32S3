# User guide

**English** · [Česky](#návod-k-obsluze)

The front panel has four new keys under the display: **REL**, **dB**, **STO** and **RCL**. The K197's own REL
and dB functions still work: the ESP32 passes a short press on to the meter through optocouplers. The original
rear-panel buttons stay connected in parallel.

A *click* is a press shorter than one second, a *hold* means keeping the key down for about one second
(3 s where noted). Holds act while the key is still down. A short beep confirms the holds that open a settings
screen.

## Screens and keys

### Main display (replica of the K197 LCD)

![LCD replica](images/lcd_grn.png)

| Key | Click | Hold 1 s |
|---|---|---|
| REL | REL of the K197 (sent at the release) | help for this screen |
| dB | dB of the K197 (sent at the release) | limit alarm settings of the function being measured |
| REL + dB | **marker** (TAG) on the reading shown, nothing is sent to the K197 | — |
| STO | statistics screen (with HOLD on: ends HOLD) | **DATA HOLD** (see below) |
| RCL | graph | DMM settings (language, theme, sound, SD card, CSV format, HOLD) |

REL and dB reach the K197 only at the release, so that a hold can open the help or the alarm settings instead.
The delay is the length of the press, typically under 0.2 s.

The REM indicator is never lit on this instrument, because there is no GPIB board. Its place shows **TAG**: dim
normally, lit for 2 s after a marker. When the K197 itself lights REM (during its self-test), REM is drawn.
In the same way STO and RCL (the K197's data-store keys are not wired) show **HOLD** and **AUTO**, and BAT shows
**SD!** (see the SD card below).

### Statistics (STO)

![Statistics](images/stats_grn.png)

Top line (the same on every page): the interval (between triangles), date and time, live value. Below, the
readings of the current function within the interval, on three pages (RCL click = next page). Intervals: 5 SEC,
10 SEC, 1 MIN, 5 MIN, 15 MIN, 1 HOUR, 6 HRS.

| Page | Left | Right |
|---|---|---|
| 1 | **AVG** average, **MED** median | **MAX**, **MIN** |
| 2 | **SDV** standard deviation, **REL** = SDV / AVG (ppm, % from 0.1 %) | **DIF** = MAX − MIN, **PTS** number of readings |
| 3 | **SEM** = SDV / √PTS (uncertainty of the average), **SDV** in digits of the last place (dgt) | **TRD** trend, **DIF** = (MAX − MIN) / AVG |

![Statistics, page 2](images/stats_page2.png)
![Statistics, page 3](images/stats_page3.png)

SDV, SEM and TRD have three significant digits and an SI prefix (nV, µV, mV …). **TRD** is the slope of a line
fitted through the readings (least squares over the RTC time): per minute (`/m`) up to the 1 MIN interval, per
hour (`/h`) for the longer ones. In dB there are no relative values (REL and the relative DIF show dashes).
The page stays when the interval changes and when you come back from the help; the statistics opened anew start
on page 1.

| Key | Action |
|---|---|
| REL / dB | shorter / longer interval (REL and dB do not reach the K197 here) |
| RCL click | next page (1 → 2 → 3 → 1) |
| RCL hold 3 s | **clears the whole log** (long confirmation beep) |
| REL hold | help |
| STO | back to the main display |

`--.----` (on page 3 `---`) means "no value": the computation is still running after a change, or there are
not enough readings in the interval yet (SDV needs 2, the trend 3 with an RTC time).

### Graph (RCL)

![Graph with markers](images/graph_markers_5min.png)

The graph shows the log of the function being measured. A band runs from minimum to maximum and the line is the
average. Each of the 384 pixel columns covers 1/384 of the window. The header shows the function and unit, the
interval, the Y axis mode (with "..." while older records are still being read), the time and the live value.
Markers are small triangles at the top edge. An OL reading is a mark at the top edge.

| Key | Action |
|---|---|
| REL / dB click | shorter / longer interval (at the release) |
| REL + dB | marker on the reading shown |
| RCL click | next Y axis mode: AUTO → ROBUST → ZERO → SUPP → AUTO |
| RCL hold | graph settings |
| REL hold | help |
| STO | back to the main display |

Intervals: 10 SEC, 30 SEC, 1 MIN, 5 MIN, 15 MIN, 1 HOUR, 6 HRS, 24 HRS, FULL (the whole log).
Y axis: **AUTO** = from the lowest to the highest value; **ROBUST** = like AUTO, but the highest 2 % and the
lowest 2 % of the columns do not count for the range, so one short spike does not set the scale (it is clipped and
marked by a dot on the edge; below 20 columns with data it is plain AUTO); **ZERO** = the axis always includes
zero; **SUPP** (suppressed zero) = a fixed centre (the newest value at the moment SUPP is chosen) ± a chosen span,
plus a 10 % reserve on each side. Values outside the axis are clipped and marked by a dot on the edge.

![ROBUST: one spike, the noise band still fills the screen](images/graph_robust.png)

**Graph settings:** INTERVAL, Y AXIS, TRACE (band + average, average only, band only), Y SPAN (the SUPP span,
±1 … ±10000 counts of the last digit).

![Graph settings](images/graph_setup_en.png)

### Settings screens (graph, DMM, alarm)

![DMM settings](images/setup_dmm.png)

On every settings screen: **RCL** moves to the next line, **REL / dB** change the value on the cursor line, and
**STO** goes back. The screen also closes by itself after 60 s without a key. Changes apply at once. They are
saved to EEPROM about 10 s after the last change, so stepping through values costs a single flash write.

**DMM settings** (RCL held on the main display), six lines, four at a time (small triangles at the right edge
show that the list goes on): LANGUAGE (CZ/EN), THEME (8 colour themes, the display changes at once), SOUND (ALL,
ALARMS ONLY = only the limit and SD card alarms, OFF = really everything off), SD CARD (ON / OFF, with the card
state), CSV FORMAT (`1.234 ,` or `1,234 ;`), HOLD (MANUAL / AUTO HOLD).

#### Colour themes

Each theme sets four colours: background, ghost (unlit segments), indicator and bright (lit digits). The LCD
replica uses them as they are. The text screens (statistics, graph, help, settings) always have a black
background; a theme with a light background (ORG2, LCD1, LCD2) is used there the other way round. Every picture
shows the main display (AUTO and REL lit, alarm on both limits) above the statistics screen. The pictures are
rendered by the firmware's own drawing code (`tools/make_screenshots.sh`).

![All colour themes](images/themes.png)

| Theme | Look |
|---|---|
| RED | red LED (the default theme) |
| GRN | green LED |
| ORG1 | orange LED / amber |
| ORG2 | inverted: black digits on an orange background |
| VFD | vacuum fluorescent display, blue-green |
| LCD1 | reflective LCD with a green tint |
| LCD2 | reflective LCD, white |
| LCD3 | white on black |

Single pictures: [RED](images/theme_red.png) · [GRN](images/theme_grn.png) · [ORG1](images/theme_org1.png) ·
[ORG2](images/theme_org2.png) · [VFD](images/theme_vfd.png) · [LCD1](images/theme_lcd1.png) ·
[LCD2](images/theme_lcd2.png) · [LCD3](images/theme_lcd3.png). New themes can be added to `themes[]` in
`config.h`.

### Help

![Help](images/help_en.png)

Hold REL on the main display, the statistics, the graph or on the ALARM/SIGNAL lines of the alarm settings to get
the help for that screen. RCL = next page (after the last one back to the first, like RCL = next line on the
settings screens), REL / dB click = previous / next page, REL / dB hold = first / last page, STO = back. The help
closes by itself after 60 s without a key.

## Limit alarm

![Alarm settings](images/setup_alarm.png)

Hold dB on the main display. The alarm settings open for the function being measured (DCV, ACV, DCI, ACI, OHM,
dB); every function has its own limits.

| Line | Values |
|---|---|
| ALARM | OFF, UPPER, LOWER, BOTH LIMITS |
| LOW LIMIT / HIGH LIMIT | the limit in the unit of the display |
| SIGNAL | BEEP + ARROW, ARROW ONLY |

- **Switching a limit on** sets it, if it has no value yet, to the MAX (upper) / MIN (lower) of the last 10 s of
  the log, or to the live reading.
- **On a limit line** REL / dB change the value by one count of the last digit. Held, the value repeats five times
  a second, and the step grows tenfold every 3 s (1, 10, 100 … counts). After the release it starts at one count
  again. **RCL held** on a limit line takes the live reading. RCL click moves to the next line.
- **REL held on the ALARM or SIGNAL line** opens the help. On a limit line a held REL repeats the step instead.
- **Persistence:** the limits and the last mode of every function are kept, but the alarm is **always off after
  power-up**.

The symbol above the minus sign:

| Symbol | Meaning |
|---|---|
| ![](images/lcd_alarm_both.png) | outline (two triangles = both limits, one triangle = upper or lower): watched, the reading is inside |
| ![](images/lcd_alarm_high.png) | filled triangle up, blinking: above the upper limit |
| ![](images/lcd_alarm_low.png) | filled triangle down, blinking: below the lower limit |

Outside a limit the triangle blinks and, with SIGNAL = BEEP and the sound on, a short beep sounds twice a
second. The reading is compared in counts of the displayed value, so 1.20000 V is not "above" a limit of
1.2 V. OL counts as above, −OL as below. With REL on the K197, the displayed (relative) value is compared.

## Markers

A marker flags the reading that was **on the display at the moment you pressed**, so it can be found again in the
log. The time you need to press the second key does not matter. Ways to set one:

- **REL + dB** together (in any order) on the main display or the graph;
- a press on a switch at the rear **EXTERNAL TRIGGER** input (e.g. a foot switch), after `MEASure:TRIGger MARK`;
- the SCPI command `LOG:MARKer`.

You get a double beep and **TAG** lit for 2 s (on the main display). In the graph a triangle appears at the top
edge. Over SCPI, `LOG:DATA?` shows `TAG` among the indicators of a marked record, and `LOG:MARKer?` lists only the
marked records.

## DATA HOLD

Hold **STO** for 1 s on the main display. Only the display freezes: the log, the statistics, the graph, the alarm,
the SD card and SCPI `READ?` keep working with the live readings. What HOLD does is chosen on the DMM settings
screen (HOLD line, also `MEASure:HOLD ON|AUTO|OFF`):

- **MANUAL**: the reading shown at the **moment of the press** freezes at once and **HOLD** lights (in place of
  STO). If you let go before 1 s it was just a click (the statistics open). After 1 s a short beep confirms it,
  the reading is marked in the log and TAG lights.
- **AUTO HOLD**: HOLD blinks while it waits; the first **stable** reading (4 readings in a row within ±2 counts,
  same function and range, not settling, not OL and at least 100 counts from zero) freezes, **HOLD + AUTO** light,
  a 200 ms beep sounds and the reading is marked. It keeps watching: another stable reading that differs is
  captured the same way, so you can go from one test point to the next without touching the meter. Lifting the
  probes (zero) or OL never replaces a captured reading. A change of the function drops it and waits again.

![Manual HOLD](images/lcd_hold.png)
![AUTO HOLD: a reading captured](images/lcd_auto_hold.png)

A short STO ends HOLD (live display again), STO held again starts it over. Switching the K197 off ends HOLD.
Constants in `config.h`: `CONFIG_UI_HOLD_HOLD_MS`, `CONFIG_HOLD_AUTO_*`, `CONFIG_HOLD_CAPTURE_BEEP_MS`.

## SD card

With **SD CARD = ON** on the DMM settings screen (or `MMEMory:LOG ON`) every reading also goes into a CSV file on
a microSD card (FAT32, SDMMC 1-bit, pins in `config.h`):

- **Files**: `/K197/YYYY-MM/K197_YYYY-MM-DD_hh-mm-ss_FUN.csv` (FUN = DCV, ACV, DCI, ACI, OHM, dB). A **new file**
  at every change of the function, after the K197 was switched on and after a card was inserted; not at midnight
  (an overnight measurement stays in one file). Without an RTC time: `/K197/NO-RTC/K197_nnnn_FUN.csv`.
- **Content**: four comment lines (`# firmware`, function and unit, start, format), a header and lines like
  `LOG:DATA?` (record number within the file, timestamp, status, function, value, indicators incl. `TAG` and
  `SETTLE`). **CSV FORMAT**: `1.234 ,` (comma separated, decimal point) or `1,234 ;` (semicolon separated, decimal
  comma - for a Czech Excel).
- **Writing**: in blocks every 30 s, at once on a marker; on a change of the function, when the K197 is switched
  off and when logging is switched OFF everything is written and the file closed. **SD CARD = OFF** = the card can
  be removed safely.
- **Problems** (no card, card full, write error): **SD!** in place of BAT blinks and a 0.5 s beep sounds every 5 s
  (with SOUND = ALL or ALARMS ONLY). The **first press of any key** only silences the beep (that press does nothing
  else); SD! keeps blinking. Readings not yet written stay in PSRAM and go to a new file on the next card.
- `MMEMory:STATe?` shows the state, the file, the records written and the free space; `SYSTem:DIAGnostic?` the
  write times.

![SD card problem: SD! lit](images/lcd_sd.png)

## The log

Every reading goes into the log in PSRAM, about 600 000 readings (~50 h at 3.3 readings/s; then the oldest are
overwritten). Each reading has its date and time from the RTC. The log is **lost when the instrument is switched
off**. Read it out with `LOG:DATA?` before that, or keep it on the SD card (see above). The log is used by the statistics, the
graph, the alarm defaults and SCPI (`LOG:*`).

**Changing the function.** The statistics and the graph always show only the function being measured (DCV, ACV,
DCI, ACI, OHM, dB). After a switch the graph is rebuilt with the new function: the time spent on the other one
stays a gap, and switching back shows the earlier readings again. A change of range within one function (mV ↔ V)
changes nothing. The first **7 readings (about 2 s) after a change of the function** are still settling, and the
first **20 readings (about 6 s) after the K197 was switched on** (or the ESP32 started). They are logged with the
flag `SETTLE`, but the statistics, the graph and the alarm leave them out (`CONFIG_DATALOG_SETTLE_READINGS`,
`CONFIG_DATALOG_SETTLE_POWERON_READINGS` in `config.h`).

## When the K197 is switched off

If the ESP32 has its own supply (USB, or a battery added later), the log stays while the K197 is switched off.

![oFF197](images/lcd_off.png)

- **4 s** without a usable frame: the main display shows **`oFF197`** when the K197 sends nothing at all (CS quiet,
  the K197 is off), or **`Err197`** when something arrives that cannot be used (a fault). With `oFF197` every
  screen returns to the main display.
- The graph and the statistics keep the last function, so the log can still be looked at (the live value shows
  `--.----`). When the K197 comes back, the gap stays visible in the graph and the older readings stay.
- After **60 s** without a key the ESP32 goes into light sleep. The display keeps its picture (`oFF197`), the log
  stays in PSRAM.
- It wakes up when the K197 is switched on, on any key (that press only wakes it; then all screens work for 60 s)
  and on a start bit on the SCPI UART (the first command is usually lost, send it again). In light sleep the
  native USB (CDC) does not answer.
- `SYSTem:DIAGnostic?` shows the number of light sleeps, the time asleep and the last wake-up cause.
- Settings in `config.h`: `CONFIG_DMM_NO_DATA_MS`, `CONFIG_DMM_OFF_QUIET_MS`, `CONFIG_POWER_*`
  (`CONFIG_POWER_SLEEP_ENABLE 0` = never sleep).

## RTC accuracy

`SYSTem:TIME:AGINg <-128..127>` trims the rate of the DS3231: one step is about 0.1 ppm (about 8.6 ms a day), a
**positive value slows the clock down**. A clock gaining 1 s a day needs about +116 more. The value is applied at
once, kept in the settings and written back into the RTC at boot if the RTC holds another one (e.g. after a
battery change).

---

# Návod k obsluze

Pod displejem jsou čtyři nová tlačítka: **REL**, **dB**, **STO** a **RCL**. Původní funkce REL a dB přístroje K197
fungují dál: krátký stisk předá ESP32 multimetru přes optočleny. Původní tlačítka na zadním panelu zůstávají
zapojená paralelně.

*Klik* je stisk kratší než sekunda, *podržení* znamená držet tlačítko asi sekundu (kde je uvedeno, 3 s). Podržení
se provede ještě během držení. Podržení, která otevírají nastavovací obrazovku, potvrdí krátké
pípnutí.

## Obrazovky a tlačítka

### Hlavní displej (replika LCD K197)

| Tlačítko | Klik | Podržení 1 s |
|---|---|---|
| REL | REL multimetru (odešle se při uvolnění) | nápověda k této obrazovce |
| dB | dB multimetru (odešle se při uvolnění) | nastavení alarmu mezí pro měřenou funkci |
| REL + dB | **marker** (TAG) na zobrazenou hodnotu, do K197 se nic neposílá | — |
| STO | obrazovka statistiky (při zapnutém HOLD: ukončí HOLD) | **DATA HOLD** (viz níže) |
| RCL | graf | nastavení DMM (jazyk, téma, zvuk, SD karta, formát CSV, HOLD) |

REL a dB dostane K197 až při uvolnění, aby podržení mohlo místo toho otevřít nápovědu nebo nastavení alarmu.
Zpoždění odpovídá délce stisku, obvykle pod 0,2 s.

Indikátor REM se na tomto přístroji nikdy nerozsvítí, protože chybí deska GPIB. Na jeho místě je **TAG**:
normálně tlumený, 2 s po markeru rozsvícený. Když REM rozsvítí sám K197 (při self-testu), vykreslí se REM.
Stejně tak STO a RCL (tlačítka paměti K197 nejsou zapojená) ukazují **HOLD** a **AUTO** a BAT ukazuje **SD!**
(viz SD karta níže).

### Statistika (STO)

Horní řádek (na všech stranách stejný): interval (mezi trojúhelníky), datum a čas, aktuální hodnota. Pod ním
odečty aktuální funkce v intervalu na třech stranách (klik RCL = další strana). Intervaly: 5 SEC, 10 SEC, 1 MIN,
5 MIN, 15 MIN, 1 HOUR, 6 HRS.

| Strana | Vlevo | Vpravo |
|---|---|---|
| 1 | **AVG** průměr, **MED** medián | **MAX**, **MIN** |
| 2 | **SDV** směrodatná odchylka, **REL** = SDV / AVG (ppm, od 0,1 % v %) | **DIF** = MAX − MIN, **PTS** počet měření |
| 3 | **SEM** = SDV / √PTS (nejistota průměru), **SDV** v digitech posledního místa (dgt) | **TRD** trend, **DIF** = (MAX − MIN) / AVG |

SDV, SEM a TRD mají tři platné číslice a předponu SI (nV, µV, mV …). **TRD** je sklon přímky proložené odečty
(metoda nejmenších čtverců podle času z RTC): za minutu (`/m`) do intervalu 1 MIN, za hodinu (`/h`) u delších.
V dB nejsou relativní hodnoty (REL a relativní DIF ukazují pomlčky). Strana zůstává při změně intervalu i po
návratu z nápovědy; nově otevřená statistika začíná na straně 1.

| Tlačítko | Akce |
|---|---|
| REL / dB | kratší / delší interval (REL a dB se tu do K197 neposílají) |
| RCL klik | další strana (1 → 2 → 3 → 1) |
| RCL podržet 3 s | **smaže celý log** (dlouhé potvrzovací pípnutí) |
| REL podržet | nápověda |
| STO | zpět na hlavní displej |

`--.----` (na straně 3 `---`) znamená „bez hodnoty“: po změně ještě běží výpočet, nebo v intervalu zatím není
dost odečtů (SDV potřebuje 2, trend 3 s časem z RTC).

### Graf (RCL)

Graf zobrazuje log měřené funkce. Pás vede od minima k maximu a čára je průměr. Každý z 384 pixelových sloupců
pokrývá 1/384 okna. V záhlaví je funkce a jednotka, interval, režim osy Y (s „...“, dokud se čtou starší záznamy),
čas a aktuální hodnota. Markery jsou malé trojúhelníky na horní hraně. Odečet OL je značka na horní hraně.

| Tlačítko | Akce |
|---|---|
| REL / dB klik | kratší / delší interval (při uvolnění) |
| REL + dB | marker na zobrazenou hodnotu |
| RCL klik | další režim osy Y: AUTO → ROBUST → ZERO → SUPP → AUTO |
| RCL podržet | nastavení grafu |
| REL podržet | nápověda |
| STO | zpět na hlavní displej |

Intervaly: 10 SEC, 30 SEC, 1 MIN, 5 MIN, 15 MIN, 1 HOUR, 6 HRS, 24 HRS, FULL (celý log).
Osa Y: **AUTO** = od nejnižší po nejvyšší hodnotu; **ROBUST** = jako AUTO, ale 2 % nejvyšších a 2 % nejnižších
sloupců se do rozsahu nepočítá, takže jedna krátká špička nezmění měřítko (ořízne se a na okraji ji označí tečka;
pod 20 sloupci s daty je to obyčejné AUTO); **ZERO** = osa vždy obsahuje nulu; **SUPP** (potlačená nula) = pevný
střed (nejnovější hodnota v okamžiku volby SUPP) ± zvolené rozpětí a 10% rezerva na každé straně. Hodnoty mimo osu
se oříznou a na okraji je označí tečka.

![ROBUST: jedna špička, pás šumu přesto vyplní obrazovku](images/graph_robust.png)

**Nastavení grafu:** INTERVAL, OSA Y, STOPA (pás + průměr, jen průměr, jen pás), ROZPETI Y (rozpětí pro SUPP,
±1 až ±10000 digitů poslední číslice).

### Nastavovací obrazovky (graf, DMM, alarm)

Na každé nastavovací obrazovce **RCL** posune kurzor na další řádek, **REL / dB** mění hodnotu na řádku s kurzorem
a **STO** vrátí zpět. Obrazovka se také sama zavře po 60 s bez stisku. Změny platí hned. Do EEPROM se uloží asi
10 s po poslední změně, takže procházení hodnot stojí jediný zápis do flash.

**Nastavení DMM** (podržení RCL na hlavním displeji), šest řádků, vidět jsou čtyři (malé trojúhelníky u pravého
okraje ukazují, že seznam pokračuje): JAZYK (CZ/EN), TEMA (8 barevných témat, displej se změní hned), ZVUK (VSE,
JEN ALARMY = jen alarm mezí a SD karty, VYP = opravdu vše vypnuto), SD KARTA (ZAP / VYP, se stavem karty), FORMAT
CSV (`1.234 ,` nebo `1,234 ;`), HOLD (RUCNI / AUTO HOLD).

#### Barevná témata

Každé téma určuje čtyři barvy: pozadí, ghost (nesvítící segmenty), indikátor a jas (svítící číslice). Replika LCD
je používá přímo. Textové obrazovky (statistika, graf, nápověda, nastavení) mají vždy černé pozadí; téma se
světlým pozadím (ORG2, LCD1, LCD2) se tam použije obráceně. Každý obrázek ukazuje hlavní displej (svítí AUTO a REL,
alarm na obě meze) a pod ním obrazovku statistiky. Obrázky vykreslil přímo kreslicí kód firmwaru
(`tools/make_screenshots.sh`).

![Všechna barevná témata](images/themes.png)

| Téma | Vzhled |
|---|---|
| RED | červený LED (výchozí téma) |
| GRN | zelený LED |
| ORG1 | oranžový LED / jantarový |
| ORG2 | inverzní: černé číslice na oranžovém pozadí |
| VFD | vakuový fluorescenční displej, modrozelený |
| LCD1 | reflexní LCD se zeleným nádechem |
| LCD2 | reflexní LCD, bílý |
| LCD3 | bílá na černé |

Jednotlivé obrázky: [RED](images/theme_red.png) · [GRN](images/theme_grn.png) · [ORG1](images/theme_org1.png) ·
[ORG2](images/theme_org2.png) · [VFD](images/theme_vfd.png) · [LCD1](images/theme_lcd1.png) ·
[LCD2](images/theme_lcd2.png) · [LCD3](images/theme_lcd3.png). Další témata lze přidat do `themes[]`
v `config.h`.

### Nápověda

Podržením REL na hlavním displeji, ve statistice, v grafu nebo na řádcích ALARM/SIGNAL nastavení alarmu se otevře
nápověda k dané obrazovce. RCL = další strana (po poslední zase první, stejně jako RCL = další řádek na
nastavovacích obrazovkách), REL / dB klik = předchozí / další strana, REL / dB podržet = první / poslední strana,
STO = zpět. Nápověda se sama zavře po 60 s bez stisku.

## Alarm mezí

Podržením dB na hlavním displeji se otevře nastavení alarmu pro měřenou funkci (DCV, ACV, DCI, ACI, OHM, dB).
Každá funkce má své vlastní meze.

| Řádek | Hodnoty |
|---|---|
| ALARM | VYP, HORNI, DOLNI, OBE MEZE |
| DOLNI MEZ / HORNI MEZ | mez v jednotce displeje |
| SIGNAL | ZVUK + SIPKA, JEN SIPKA |

- **Zapnutí meze**, která ještě nemá hodnotu, ji nastaví na MAX (horní) / MIN (dolní) z posledních 10 s logu,
  případně na aktuální hodnotu.
- **Na řádku meze** mění REL / dB hodnotu o jeden digit poslední číslice. Při držení se hodnota opakuje pětkrát
  za sekundu a krok se každé 3 s zvětší desetkrát (1, 10, 100 … digitů). Po uvolnění začíná krok znovu od
  jednoho digitu. **Podržení RCL** na řádku meze převezme aktuální hodnotu. Klik RCL přejde na další řádek.
- **Podržení REL na řádku ALARM nebo SIGNAL** otevře nápovědu. Na řádku meze podržení REL místo toho opakuje
  krok.
- **Ukládání:** meze a poslední režim každé funkce se ukládají, ale alarm je **po zapnutí přístroje vždy
  vypnutý**.

Symbol nad znaménkem mínus:

| Symbol | Význam |
|---|---|
| obrys (dva trojúhelníky = obě meze, jeden = horní nebo dolní) | hlídá se, hodnota je v mezích |
| plný trojúhelník nahoru, bliká | nad horní mezí |
| plný trojúhelník dolů, bliká | pod dolní mezí |

Mimo mez trojúhelník bliká a při SIGNAL = ZVUK a zapnutém zvuku dvakrát za sekundu krátce pípne. Hodnota se
porovnává v digitech zobrazené hodnoty, takže 1,20000 V není „nad“ mezí 1,2 V. OL se bere jako nad mezí, −OL jako
pod mezí. Při zapnutém REL na K197 se porovnává zobrazená (relativní) hodnota.

## Markery

Marker označí hodnotu, která byla **na displeji v okamžiku stisku**, aby se dala v logu znovu najít. Na tom, jak
dlouho trvá stisknout druhé tlačítko, nezáleží. Možnosti:

- **REL + dB** současně (v libovolném pořadí) na hlavním displeji nebo v grafu;
- stisk spínače na zadním vstupu **EXTERNAL TRIGGER** (např. nožní spínač) po příkazu `MEASure:TRIGger MARK`;
- příkaz SCPI `LOG:MARKer`.

Následuje dvojpípnutí a na hlavním displeji se na 2 s rozsvítí **TAG**. V grafu se objeví trojúhelník na horní
hraně. Přes SCPI ukazuje `LOG:DATA?` u označeného záznamu mezi indikátory `TAG` a `LOG:MARKer?` vypíše jen
označené záznamy.

## DATA HOLD

Podržte **STO** 1 s na hlavním displeji. Zmrazí se jen displej: log, statistika, graf, alarm, SD karta i SCPI
`READ?` pracují dál s živými odečty. Co HOLD dělá, se volí na obrazovce nastavení DMM (řádek HOLD, také
`MEASure:HOLD ON|AUTO|OFF`):

- **RUCNI**: hodnota zobrazená v **okamžiku stisku** se hned zmrazí a rozsvítí se **HOLD** (na místě STO). Když
  tlačítko pustíte dřív než za 1 s, byl to jen klik (otevře se statistika). Po 1 s to potvrdí krátké pípnutí,
  hodnota se v logu označí a rozsvítí se TAG.
- **AUTO HOLD**: HOLD bliká a čeká; první **ustálená** hodnota (4 odečty po sobě v rozmezí ±2 digity, stejná
  funkce a rozsah, ne při ustalování, ne OL a aspoň 100 digitů od nuly) se zmrazí, rozsvítí se **HOLD + AUTO**,
  zazní pípnutí 200 ms a hodnota se označí. Hlídá dál: další ustálenou hodnotu, která se liší, zachytí stejně, takže
  jde měřit bod za bodem bez sahání na přístroj. Zvednutí hrotů (nula) ani OL zachycenou hodnotu nepřepíšou. Změna
  funkce ji zahodí a čeká znovu.

Krátký stisk STO HOLD ukončí (displej znovu živý), další podržení STO začne znovu. Vypnutí K197 HOLD ukončí.
Konstanty v `config.h`: `CONFIG_UI_HOLD_HOLD_MS`, `CONFIG_HOLD_AUTO_*`, `CONFIG_HOLD_CAPTURE_BEEP_MS`.

## SD karta

Při **SD KARTA = ZAP** na obrazovce nastavení DMM (nebo `MMEMory:LOG ON`) jde každý odečet také do souboru CSV na
kartě microSD (FAT32, SDMMC 1-bit, piny v `config.h`):

- **Soubory**: `/K197/RRRR-MM/K197_RRRR-MM-DD_hh-mm-ss_FUN.csv` (FUN = DCV, ACV, DCI, ACI, OHM, dB). **Nový soubor**
  při každé změně funkce, po zapnutí K197 a po vložení karty; o půlnoci ne (měření přes noc zůstane v jednom
  souboru). Bez času z RTC: `/K197/NO-RTC/K197_nnnn_FUN.csv`.
- **Obsah**: čtyři řádky komentáře (`# firmware`, funkce a jednotka, začátek, formát), záhlaví a řádky jako
  `LOG:DATA?` (číslo záznamu v souboru, čas, stav, funkce, hodnota, indikátory včetně `TAG` a `SETTLE`). **FORMAT
  CSV**: `1.234 ,` (oddělovač čárka, desetinná tečka) nebo `1,234 ;` (oddělovač středník, desetinná čárka - pro
  český Excel).
- **Zápis**: po blocích každých 30 s, hned při markeru; při změně funkce, vypnutí K197 a vypnutí záznamu se vše
  zapíše a soubor zavře. **SD KARTA = VYP** = kartu lze bezpečně vyjmout.
- **Potíže** (chybí karta, plná karta, chyba zápisu): **SD!** na místě BAT bliká a každých 5 s zazní pípnutí 0,5 s
  (při ZVUK = VSE nebo JEN ALARMY). **První stisk libovolného tlačítka** pípání jen umlčí (nic jiného neudělá);
  SD! bliká dál. Odečty, které se ještě nezapsaly, zůstanou v PSRAM a na další kartu se zapíší do nového souboru.
- `MMEMory:STATe?` ukazuje stav, soubor, počet zapsaných záznamů a volné místo; `SYSTem:DIAGnostic?` časy zápisu.

## Log

Každý odečet se ukládá do logu v PSRAM, asi 600 000 odečtů (asi 50 h při 3,3 odečtu/s, pak se přepisují
nejstarší). Každý odečet má datum a čas z RTC. Log se **při vypnutí přístroje ztratí**. Před vypnutím ho můžete
vyčíst příkazem `LOG:DATA?`, nebo ho průběžně ukládat na SD kartu (viz výše). Log používá statistika, graf, výchozí meze alarmu a SCPI
(`LOG:*`).

**Změna funkce.** Statistika i graf vždy ukazují jen právě měřenou funkci (DCV, ACV, DCI, ACI, OHM, dB). Po
přepnutí se graf přestaví pro novou funkci: doba měření jiné funkce zůstane jako mezera a po přepnutí zpět jsou
starší odečty znovu vidět. Změna rozsahu v rámci jedné funkce (mV ↔ V) nic nemění. Prvních **7 odečtů (asi 2 s) po změně
funkce** se ještě ustaluje, po **zapnutí K197** (nebo startu ESP32) prvních **20 odečtů (asi 6 s)**. Do logu se
zapíší s příznakem `SETTLE`, ale statistika, graf i alarm je vynechají (`CONFIG_DATALOG_SETTLE_READINGS`,
`CONFIG_DATALOG_SETTLE_POWERON_READINGS` v `config.h`).

## Když je K197 vypnutý

Pokud má ESP32 vlastní napájení (USB, později baterii), log při vypnutí K197 zůstane.

![oFF197](images/lcd_off.png)

- **4 s** bez použitelného snímku: hlavní displej ukáže **`oFF197`**, když K197 neposílá vůbec nic (CS v klidu,
  K197 je vypnutý), nebo **`Err197`**, když chodí něco nepoužitelného (porucha). Při `oFF197` se každá obrazovka
  vrátí na hlavní displej.
- Graf a statistika drží poslední funkci, takže se v logu dá dál listovat (živá hodnota ukazuje `--.----`). Po
  zapnutí K197 zůstane v grafu vidět mezera a starší odečty zůstanou.
- Po **60 s** bez stisku přejde ESP32 do light sleep. Displej si drží obraz (`oFF197`), log zůstává v PSRAM.
- Probudí ho zapnutí K197, libovolné tlačítko (ten stisk jen probudí, pak 60 s fungují všechny obrazovky) a start
  bit na SCPI UART (první příkaz se obvykle ztratí, pošlete ho znovu). V light sleep neodpovídá nativní USB (CDC).
- `SYSTem:DIAGnostic?` ukazuje počet uspání, celkovou dobu spánku a příčinu posledního probuzení.
- Nastavení v `config.h`: `CONFIG_DMM_NO_DATA_MS`, `CONFIG_DMM_OFF_QUIET_MS`, `CONFIG_POWER_*`
  (`CONFIG_POWER_SLEEP_ENABLE 0` = nikdy nespat).

## Přesnost RTC

`SYSTem:TIME:AGINg <-128..127>` dolaďuje chod DS3231: jeden krok je asi 0,1 ppm (asi 8,6 ms za den), **kladná
hodnota hodiny zpomaluje**. Hodiny, které se předbíhají o 1 s za den, potřebují asi +116 navíc. Hodnota platí
hned, uloží se do nastavení a při startu se zapíše zpět do RTC, pokud v něm je jiná (např. po výměně baterie).
