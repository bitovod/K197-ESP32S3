// help_text.h
// The texts of the help screen (REL held for CONFIG_UI_HELP_HOLD_MS, see
// ui.ino). New in v12; v15: one text per SCREEN (context help) and per
// LANGUAGE (SYSTem:LANGuage, settings.h).
//
//   HELP_SECTION_DMM    opened from the LCD replica: preceded by the
//                       two-line preface (firmware version, build time,
//                       underlined - put in front by ui.ino), then the
//                       keys of the DMM screen, the help navigation, SCPI,
//                       v23: the SD card
//   HELP_SECTION_STAT   opened from the statistics screen: only its text
//   HELP_SECTION_GRAPH  opened from the graph screen: graph + its settings
//   HELP_SECTION_ALARM  v17: opened from the alarm settings screen
//
// Plain text, one screen line per source line, shown EXACTLY as written:
// no wrapping, no hyphenation, no justification. A line has room for 47
// characters (428 px / FONT_TINY advance 9); anything longer is cut off at
// the right edge of the panel - the PC test host_tests/test_ui.cpp checks
// every line of every language. Pages are 6 lines (the DMM section's first
// page 4, below the preface); the last line of a page ends with its page
// mark "n/m" flush right. Empty lines are kept. Only ASCII: the font has no
// diacritics, so Czech is written without them.
//
// Convention (writing only, no special drawing - everything is one color):
// a chapter heading is a line in CAPITALS ending with a colon, on its own.
//
// Adding a language: one more HELP_LANG_* index, its name in
// helpLanguageNames[] (what SYSTem:LANGuage accepts and reports) and one
// more row in helpTexts[] - the array sizes make a missing text a compile
// error. The graph settings screen has its own few translated words in
// graph.ino (graphSetupWords[]).
//
// Every line ends with "\n"; keep it that way, also on the last line.

#ifndef HELP_TEXT_H
#define HELP_TEXT_H

#define HELP_LANG_CZ    0
#define HELP_LANG_EN    1
#define HELP_LANG_COUNT 2

#define HELP_SECTION_DMM   0
#define HELP_SECTION_STAT  1
#define HELP_SECTION_GRAPH 2
#define HELP_SECTION_ALARM 3 // v17: the alarm settings screen (REL held on its ALARM or SIGNAL line)
#define HELP_SECTION_COUNT 4

static const char *const helpLanguageNames[HELP_LANG_COUNT] = {"CZ", "EN"};

static const char helpTextCzDmm[] =
"REL, dB: puvodni funkce multimetru K197.       \n"
"REL 1s: napoveda, dB 1s: alarm, REL+dB: TAG.   \n"
"STO: statistika, STO 1s: HOLD (zmrazeni).      \n"
"RCL: graf, RCL 1s: nastaveni DMM.           1/6\n"
"ALARM (dB 1s):                                 \n"
"Hlida dolni a/nebo horni mez, pro kazdou       \n"
"funkci zvlast. Po zapnuti DMM je vypnuty.      \n"
"Obrys nad minusem: hlidane meze; plny          \n"
"blikajici trojuhelnik: hodnota je mimo mez.    \n"
"RCL 1s na mezi: prevezme merenou hodnotu.   2/6\n"
"HOLD (STO 1s, druh v nastaveni DMM):           \n"
"RUCNI: zmrazi hodnotu z okamziku stisku + TAG. \n"
"AUTO HOLD: HOLD blika, ceka na stabilni        \n"
"hodnotu; zachycena: HOLD + AUTO, pipnuti, TAG. \n"
"Nula a OL se nezachytavaji. Konec: STO.        \n"
"                                            3/6\n"
"HELP:                                          \n"
"RCL: dalsi strana (dokola), REL/dB: zpet/vpred.\n"
"REL 1s: na zacatek, dB 1s: na konec.           \n"
"STO: odchod z napovedy.                        \n"
"Bez stisku tlacitka se zavre sama po 60 s.     \n"
"                                            4/6\n"
"SCPI:                                          \n"
"Pro vice informaci a dalsi nastaveni pouzijte  \n"
"USB virtualni seriovy port (rychlost 115200 Bd)\n"
"a v SCPI prikaz HELP?                          \n"
"Jazyk napovedy: SYST:LANG CZ|EN                \n"
"Alarm: CALC:LIM:LOW|UPP|STAT|FAIL?          5/6\n"
"SD KARTA (RCL 1s: nastaveni DMM):              \n"
"Log na kartu, novy soubor pri zmene funkce     \n"
"a po zapnuti K197. SD VYP: kartu lze vyjmout.  \n"
"SD! blika: karta chybi, je plna nebo chyba.    \n"
"Pipani zrusi prvni stisk libovolne klavesy.    \n"
"ZVUK: VSE, JEN ALARMY nebo VYP.             6/6\n";

static const char helpTextCzStat[] =
"STATISTIKA:                                    \n"
"Interval, datum a cas, aktualni hodnota.       \n"
"REL/dB: kratsi/delsi interval. RCL: dalsi      \n"
"strana (1-2-3), RCL 3s: smaze cely log.        \n"
"1: AVG prumer, MED median, MAX a MIN.          \n"
"STO: zpet na zakladni displej.              1/2\n"
"2: SDV smerodatna odchylka, REL = SDV/AVG,     \n"
"DIF = MAX - MIN, PTS pocet mereni.             \n"
"3: SEM = SDV/odmocnina(PTS) - nejistota AVG,   \n"
"TRD trend (primka) za minutu (/m) do 1 MIN,    \n"
"jinak za hodinu (/h), SDV v digitech           \n"
"posledniho mista, DIF = (MAX-MIN)/AVG.      2/2\n";

static const char helpTextCzGraph[] =
"GRAF:                                          \n"
"Log aktualni funkce: pas MIN/MAX a cara AVG.   \n"
"REL/dB: kratsi/delsi interval, REL+dB: TAG.    \n"
"RCL: dalsi osa Y (AUTO, ROBUST, ZERO, SUPP).   \n"
"RCL 1s: nastaveni grafu.                       \n"
"STO: zpet na zakladni displej.              1/3\n"
"GRAF - NASTAVENI:                              \n"
"RCL: dalsi radek, REL/dB: zmena hodnoty.       \n"
"INTERVAL: 10 SEC az 24 HRS, FULL = cely log.   \n"
"OSA Y: AUTO min..max, ZERO s nulou, SUPP pevna.\n"
"STOPA: co kreslit; ROZPETI Y: +- osy u SUPP.   \n"
"STO: zpet do grafu.                         2/3\n"
"OSA Y - ROBUST:                                \n"
"Jako AUTO, ale 2 % nejvyssich a 2 % nejnizsich \n"
"sloupcu se do rozsahu nepocita: jedna spicka   \n"
"nezmeni meritko, orizne se (tecka na okraji).  \n"
"Pod 20 sloupci s daty stejne jako AUTO.        \n"
"STO: zpet do grafu.                         3/3\n";

static const char helpTextCzAlarm[] =
"ALARM - NASTAVENI:                             \n"
"Meze zvlast pro kazdou funkci, po zapnuti      \n"
"DMM je alarm vypnuty. RCL: dalsi radek.        \n"
"ALARM: VYP, HORNI, DOLNI nebo OBE MEZE.        \n"
"Nova mez = MAX/MIN za poslednich 10 s.         \n"
"STO: zpet na zakladni displej.              1/2\n"
"MEZE:                                          \n"
"REL/dB: -/+ 1 digit, drzet = rychleji.         \n"
"RCL 1s: prevezme merenou hodnotu.              \n"
"SIGNAL: ZVUK + SIPKA nebo JEN SIPKA.           \n"
"Obrys nad minusem hlida, plny blika = mimo.    \n"
"REL 1s na ALARM/SIGNAL: tato napoveda.      2/2\n";

static const char helpTextEnDmm[] =
"REL, dB: original functions of the K197.       \n"
"REL 1s: help, dB 1s: alarm, REL+dB: TAG.       \n"
"STO: statistics, STO 1s: HOLD (freeze).        \n"
"RCL: graph, RCL 1s: DMM settings.           1/6\n"
"ALARM (dB 1s):                                 \n"
"Watches a lower and/or an upper limit, for     \n"
"every function separately. Off at power-up.    \n"
"Outline above the minus: limits watched;       \n"
"filled blinking triangle: reading outside.     \n"
"RCL 1s on a limit: takes the live reading.  2/6\n"
"HOLD (STO 1s, kind on the DMM settings):       \n"
"MANUAL: freezes the reading of the press + TAG.\n"
"AUTO HOLD: HOLD blinks, waits for a stable     \n"
"reading; captured: HOLD + AUTO, beep, TAG.     \n"
"Zero and OL are never captured. End: STO.      \n"
"                                            3/6\n"
"HELP:                                          \n"
"RCL: next page (wraps), REL/dB: back/forward.  \n"
"REL 1s: first page, dB 1s: last page.          \n"
"STO: leave the help.                           \n"
"Closes by itself after 60 s without a key.     \n"
"                                            4/6\n"
"SCPI:                                          \n"
"For more information and settings use the      \n"
"USB virtual serial port (115200 Bd)            \n"
"and the SCPI command HELP?                     \n"
"Help language: SYST:LANG CZ|EN                 \n"
"Alarm: CALC:LIM:LOW|UPP|STAT|FAIL?          5/6\n"
"SD CARD (RCL 1s: DMM settings):                \n"
"Log to the card, a new file at every change    \n"
"of the function and after the K197 is on again.\n"
"SD OFF: the card may be removed. SD! blinks:   \n"
"no card, card full or write error; the first   \n"
"key press silences the beep.                6/6\n";

static const char helpTextEnStat[] =
"STATISTICS:                                    \n"
"Interval, date and time, live value.           \n"
"REL/dB: shorter/longer interval. RCL: next     \n"
"page (1-2-3), RCL 3s: clears the whole log.    \n"
"1: AVG mean, MED median, MAX and MIN.          \n"
"STO: back to the main display.              1/2\n"
"2: SDV standard deviation, REL = SDV/AVG,      \n"
"DIF = MAX - MIN, PTS number of readings.       \n"
"3: SEM = SDV/sqrt(PTS) - uncertainty of AVG,   \n"
"TRD trend (fitted line) per minute (/m) up     \n"
"to 1 MIN, else per hour (/h), SDV in digits    \n"
"of the last place, DIF = (MAX-MIN)/AVG.     2/2\n";

static const char helpTextEnGraph[] =
"GRAPH:                                         \n"
"Log of the current function: MIN/MAX band, AVG.\n"
"REL/dB: shorter/longer interval, REL+dB: TAG.  \n"
"RCL: next Y axis (AUTO, ROBUST, ZERO, SUPP).   \n"
"RCL 1s: graph settings.                        \n"
"STO: back to the main display.              1/3\n"
"GRAPH SETTINGS:                                \n"
"RCL: next line, REL/dB: change the value.      \n"
"INTERVAL: 10 SEC to 24 HRS, FULL = whole log.  \n"
"Y AXIS: AUTO min..max, ZERO with 0, SUPP fixed.\n"
"TRACE: what to draw; Y SPAN: +- axis in SUPP.  \n"
"STO: back to the graph.                     2/3\n"
"Y AXIS - ROBUST:                               \n"
"Like AUTO, but the highest 2 % and the lowest  \n"
"2 % of the columns do not count for the range: \n"
"one spike keeps the scale, it is clipped (dot).\n"
"Below 20 columns with data it is plain AUTO.   \n"
"STO: back to the graph.                     3/3\n";

static const char helpTextEnAlarm[] =
"ALARM SETTINGS:                                \n"
"Limits for every function separately, the      \n"
"alarm is off at power-up. RCL: next line.      \n"
"ALARM: OFF, UPPER, LOWER or BOTH LIMITS.       \n"
"A new limit = MAX/MIN of the last 10 s.        \n"
"STO: back to the main display.              1/2\n"
"LIMITS:                                        \n"
"REL/dB: -/+ 1 digit, held = faster.            \n"
"RCL 1s: takes the live reading.                \n"
"SIGNAL: BEEP + ARROW or ARROW ONLY.            \n"
"Outline above the minus: watched, filled       \n"
"blinking = outside. REL 1s: this help.      2/2\n";

static const char *const helpTexts[HELP_LANG_COUNT][HELP_SECTION_COUNT] = {
    {helpTextCzDmm, helpTextCzStat, helpTextCzGraph, helpTextCzAlarm},
    {helpTextEnDmm, helpTextEnStat, helpTextEnGraph, helpTextEnAlarm},
};

#endif // HELP_TEXT_H
