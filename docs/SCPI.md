# SCPI command reference

**English** · [Česky](#přehled-příkazů-scpi)

The instrument answers on the rear-panel UART (galvanically isolated, CH340 USB adapter, **115200 Bd, 8N1**) or on
the ESP32's own USB port (`SYSTem:COMM`). Lines end with CR, LF or both. Long and short forms work as usual in
SCPI (`SYSTem:FORMat` = `SYST:FORM`), case does not matter.

Every command that sets something replies `OK: …` or `ERROR: …`. An unknown command replies `ERROR`.
The instrument itself also lists every command with `HELP?` (one line each) and describes them in full with
`HELP:DETail?`.

**Parameters:** `<function>` is one of `DCV ACV DCI ACI OHM DB`. Log ranges `[<first> <last>]` and settings
parameters are separated by **spaces**. The `CALCulate:LIMit` commands accept spaces or commas.

**While the K197 is switched off** the ESP32 may be in light sleep (see the user guide). A start bit on the UART
wakes it, but that first command is usually lost: send it again (or send an empty line first). The native USB does
not answer in light sleep.

## Readings

| Command | Description |
|---|---|
| `*IDN?` | `KEITHLEY INSTRUMENTS INC.,MODEL 197,<serial>,<firmware>` (for compatibility, see the README) |
| `READ?` · `MEASure?` · `FETCh?` | the current reading in the format of `SYSTem:FORMat` |
| `SYSTem:FORMat <0\|1\|2>` / `?` | 0 = text as on the display (`3.14000 V AUTO`, `-OL`); 1 = status + function + number (`NDCV+1.23456E+0`; status N normal, O overload, Z REL active); 2 = number only (`+1.23456E+0`). Default 0 |
| `SYSTem:AUTO <ON\|OFF\|0\|n>` / `?` | sending without being asked: OFF; 0 = every measurement; n = every n seconds; ON = resume. The log uses the same interval (it logs regardless of ON/OFF). Default OFF, 0 |
| `MEASure:AUTO …` | the same as `SYSTem:AUTO` |
| `MEASure:TRIGger <INT\|EXT\|MARK>` / `?` | INT = sending per AUTO. EXT = AUTO paused; each falling edge on the rear EXTERNAL TRIGGER input sends the next complete reading once. MARK = the input is a marker key (foot switch): every press sets a marker; debounced, presses closer than 300 ms are ignored; AUTO as with INT. After power-up always INT |

The rear METERING COMPLETE output gives a pulse after every reading, in every mode.

## Reading log

Records are numbered 1 (oldest) … `LOG:POINts?` (newest). The optional parameters are the same for every `LOG:`
query: `[<function>] [<first> <last>]`, with the function always first.

- no numbers: the whole log;
- `0` on one side: no limit on that side;
- one positive number: that one record;
- `-N`: the last N records; `-N <last>`: the N records before `<last>`.

Examples: `LOG:DATA? DCV`, `LOG:DATA? OHM 10 50`, `LOG:DATA? -50`.

| Command | Description |
|---|---|
| `LOG:POINts? […]` | number of matching records |
| `LOG:CAPacity?` | capacity of the log (records), set by the free PSRAM at boot |
| `LOG:CLEar` | empties the whole log (the same as RCL held 3 s on the statistics screen) |
| `LOG:DATA? […]` | CSV: `record,timestamp,status,function,value,indicators`, e.g. `12,2026-10-10 14:03:07.42,N,DCV,+1.23456E+0,AUTO`. The timestamp comes from the RTC (`NO-RTC-TIME` before it was synced). Indicators: AUTO, STO, RCL, REM, BAT, **TAG** (a marker) and **SETTLE** (one of the first 7 readings (about 2 s) after a change of the function or the first 20 (about 6 s) after the K197 was switched on, left out of the statistics, the graph and the alarm) |
| `LOG:MARKer` | sets a marker on the newest record |
| `LOG:MARKer? […]` | the same CSV, only the marked records |
| `LOG:MIN? / LOG:MAX? / LOG:AVERage? / LOG:MEDian? […]` | one number over the range, OL and SETTLE records excluded. The median of more than 100 000 records is computed from an evenly spread sample |

A full log is ~30 MB of CSV and takes about 45 minutes at 115200 Bd.

## Limit alarm

| Command | Description |
|---|---|
| `CALCulate:LIMit:UPPer <value\|NONE> [<function>]` / `? [<function>]` | upper limit in base units (V, A, Ω, dB), e.g. `1.2`, `15E-3`; NONE = not set |
| `CALCulate:LIMit:LOWer <value\|NONE> [<function>]` / `? [<function>]` | lower limit |
| `CALCulate:LIMit:STATe <OFF\|ON\|UPPer\|LOWer\|BOTH> [<function>]` / `?` | which limits are watched; ON = the mode used last. Always OFF after power-up; limits and the last mode are kept |
| `CALCulate:LIMit:FAIL?` | 1 = the newest reading is above the upper or below the lower limit (OL = above, −OL = below), otherwise 0 |
| `CALCulate:LIMit:SIGNal <BEEP\|ARROW>` / `?` | BEEP = beep + blinking triangle, ARROW = triangle only |

Without `<function>` the commands work on the function being measured.

## Graph

| Command | Description |
|---|---|
| `GRAPh:INTerval <10S\|30S\|1M\|5M\|15M\|1H\|6H\|24H\|FULL>` / `?` | window of the graph (a plain number = seconds) |
| `GRAPh:YAXis <AUTO\|ROBust\|ZERO\|SUPP>` / `?` | Y axis mode (ROBust: AUTO without the highest and lowest 2 % of the columns) |
| `GRAPh:TRACe <BOTH\|AVG\|BAND>` / `?` | band + average, average only, band only |
| `GRAPh:SPAN <1\|2\|5\|10\|…\|10000>` / `?` | SUPP span, ± counts of the last digit |

## System

| Command | Description |
|---|---|
| `SYSTem:TIME <YYYY> <MM> <DD> <hh> <mm> <ss>` / `?` | date and time of the DS3231 RTC |
| `SYSTem:TIME:AGINg <-128..127>` / `?` | aging offset of the DS3231 (rate trim): one step ≈ 0.1 ppm ≈ 8.6 ms a day, positive = slower. Applied at once, kept in the settings, written back at boot if the RTC holds another value |
| `SYSTem:THEMe <0..7>` / `?` | colour theme (RED, GRN, ORG1, ORG2, VFD, LCD1, LCD2, LCD3) |
| `SYSTem:LANGuage <CZ\|EN>` / `?` | language of the help and the settings screens |
| `SYSTem:BEEP <ON\|OFF>` / `?` | all sounds on/off |
| `SYSTem:YOFFset <0..n>` / `?` | vertical position of the picture on the panel (default 24) |
| `SYSTem:COMM <UART\|USB>` / `?` | which port SCPI uses (switches at once) |
| `SYSTem:DEBUG <ON\|OFF>` / `?` | trace of every DMM frame (raw bytes + decoded reading) |
| `SYSTem:DEBUG:PORT <UART\|USB>` / `?` | where debug messages go (on UART each line starts with `# `) |
| `SYSTem:DIAGnostic?` | memory, stacks of all tasks, render/blit times, graph and statistics times, log fill, light sleeps while the K197 is off (count, time asleep, last wake-up) |
| `SYSTem:DIAGnostic:CLEar` | zeroes the maxima |

All settings except `MEASure:TRIGger` are kept in EEPROM. Changes from SCPI are saved at once, changes from the
front panel about 10 s after the last change.

---

# Přehled příkazů SCPI

Přístroj odpovídá na UARTu na zadním panelu (galvanicky oddělený, USB převodník CH340, **115200 Bd, 8N1**) nebo na
vlastním USB portu ESP32 (`SYSTem:COMM`). Řádek končí CR, LF nebo obojím. Dlouhé i krátké tvary fungují jako
v SCPI obvykle (`SYSTem:FORMat` = `SYST:FORM`), na velikosti písmen nezáleží.

Každý nastavovací příkaz odpoví `OK: …` nebo `ERROR: …`. Neznámý příkaz odpoví `ERROR`. Sám přístroj vypíše
všechny příkazy po jednom řádku na `HELP?` a podrobný popis na `HELP:DETail?`.

**Parametry:** `<funkce>` je jedna z `DCV ACV DCI ACI OHM DB`. Rozsahy logu `[<od> <do>]` a parametry nastavení se
oddělují **mezerou**. Příkazy `CALCulate:LIMit` přijmou mezeru i čárku.

**Když je K197 vypnutý**, může být ESP32 v light sleep (viz návod k obsluze). Start bit na UARTu ho probudí, ale
první příkaz se obvykle ztratí: pošlete ho znovu (nebo nejdřív prázdný řádek). Nativní USB v light sleep
neodpovídá.

## Měření

| Příkaz | Popis |
|---|---|
| `*IDN?` | `KEITHLEY INSTRUMENTS INC.,MODEL 197,<sériové číslo>,<firmware>` (kvůli kompatibilitě, viz README) |
| `READ?` · `MEASure?` · `FETCh?` | aktuální hodnota ve formátu podle `SYSTem:FORMat` |
| `SYSTem:FORMat <0\|1\|2>` / `?` | 0 = text jako na displeji (`3.14000 V AUTO`, `-OL`); 1 = stav + funkce + číslo (`NDCV+1.23456E+0`; stav N normální, O přetížení, Z zapnuté REL); 2 = jen číslo (`+1.23456E+0`). Výchozí 0 |
| `SYSTem:AUTO <ON\|OFF\|0\|n>` / `?` | posílání bez dotazu: OFF; 0 = každé měření; n = každých n sekund; ON = obnovit. Stejný interval používá log (loguje nezávisle na ON/OFF). Výchozí OFF, 0 |
| `MEASure:AUTO …` | totéž jako `SYSTem:AUTO` |
| `MEASure:TRIGger <INT\|EXT\|MARK>` / `?` | INT = posílání podle AUTO. EXT = AUTO pozastaveno; každá sestupná hrana na zadním vstupu EXTERNAL TRIGGER jednou odešle nejbližší kompletní měření. MARK = vstup je tlačítko markeru (nožní spínač): každý stisk zapíše marker; ošetřené odskakování, stisky blíž než 300 ms se ignorují; AUTO jako u INT. Po zapnutí vždy INT |

Zadní výstup METERING COMPLETE dává puls po každém měření, ve všech režimech.

## Log měření

Záznamy jsou číslované 1 (nejstarší) až `LOG:POINts?` (nejnovější). Nepovinné parametry jsou u všech dotazů `LOG:`
stejné: `[<funkce>] [<od> <do>]`, funkce vždy první.

- bez čísel: celý log;
- `0` na jedné straně: na té straně bez omezení;
- jedno kladné číslo: jen ten záznam;
- `-N`: posledních N záznamů; `-N <do>`: N záznamů před `<do>`.

Příklady: `LOG:DATA? DCV`, `LOG:DATA? OHM 10 50`, `LOG:DATA? -50`.

| Příkaz | Popis |
|---|---|
| `LOG:POINts? […]` | počet odpovídajících záznamů |
| `LOG:CAPacity?` | kapacita logu (záznamů), určená volnou PSRAM při startu |
| `LOG:CLEar` | smaže celý log (totéž jako podržení RCL 3 s na obrazovce statistiky) |
| `LOG:DATA? […]` | CSV: `record,timestamp,status,function,value,indicators`, např. `12,2026-10-10 14:03:07.42,N,DCV,+1.23456E+0,AUTO`. Čas je z RTC (`NO-RTC-TIME` před synchronizací). Indikátory: AUTO, STO, RCL, REM, BAT, **TAG** (marker) a **SETTLE** (jeden z prvních 7 odečtů (asi 2 s) po změně funkce nebo prvních 20 (asi 6 s) po zapnutí K197, vynechaný ze statistiky, grafu a alarmu) |
| `LOG:MARKer` | zapíše marker k nejnovějšímu záznamu |
| `LOG:MARKer? […]` | stejné CSV, jen označené záznamy |
| `LOG:MIN? / LOG:MAX? / LOG:AVERage? / LOG:MEDian? […]` | jedno číslo za rozsah, bez záznamů OL a SETTLE. Medián z více než 100 000 záznamů se počítá z rovnoměrně rozloženého vzorku |

Plný log je asi 30 MB CSV a při 115200 Bd se přenáší asi 45 minut.

## Alarm mezí

| Příkaz | Popis |
|---|---|
| `CALCulate:LIMit:UPPer <hodnota\|NONE> [<funkce>]` / `? [<funkce>]` | horní mez v základních jednotkách (V, A, Ω, dB), např. `1.2`, `15E-3`; NONE = nenastaveno |
| `CALCulate:LIMit:LOWer <hodnota\|NONE> [<funkce>]` / `? [<funkce>]` | dolní mez |
| `CALCulate:LIMit:STATe <OFF\|ON\|UPPer\|LOWer\|BOTH> [<funkce>]` / `?` | které meze se hlídají; ON = naposledy použitý režim. Po zapnutí vždy OFF; meze a poslední režim zůstávají |
| `CALCulate:LIMit:FAIL?` | 1 = nejnovější hodnota je nad horní nebo pod dolní mezí (OL = nad, −OL = pod), jinak 0 |
| `CALCulate:LIMit:SIGNal <BEEP\|ARROW>` / `?` | BEEP = pípání + blikající trojúhelník, ARROW = jen trojúhelník |

Bez `<funkce>` platí příkazy pro právě měřenou funkci.

## Graf

| Příkaz | Popis |
|---|---|
| `GRAPh:INTerval <10S\|30S\|1M\|5M\|15M\|1H\|6H\|24H\|FULL>` / `?` | okno grafu (samotné číslo = sekundy) |
| `GRAPh:YAXis <AUTO\|ROBust\|ZERO\|SUPP>` / `?` | režim osy Y (ROBust: AUTO bez 2 % nejvyšších a nejnižších sloupců) |
| `GRAPh:TRACe <BOTH\|AVG\|BAND>` / `?` | pás + průměr, jen průměr, jen pás |
| `GRAPh:SPAN <1\|2\|5\|10\|…\|10000>` / `?` | rozpětí pro SUPP, ± digitů poslední číslice |

## Systém

| Příkaz | Popis |
|---|---|
| `SYSTem:TIME <RRRR> <MM> <DD> <hh> <mm> <ss>` / `?` | datum a čas RTC DS3231 |
| `SYSTem:TIME:AGINg <-128..127>` / `?` | registr stárnutí (aging) DS3231, dolaďuje chod: jeden krok ≈ 0,1 ppm ≈ 8,6 ms za den, kladná hodnota = pomaleji. Platí hned, uloží se do nastavení a při startu se zapíše zpět, pokud RTC drží jinou hodnotu |
| `SYSTem:THEMe <0..7>` / `?` | barevné téma (RED, GRN, ORG1, ORG2, VFD, LCD1, LCD2, LCD3) |
| `SYSTem:LANGuage <CZ\|EN>` / `?` | jazyk nápovědy a nastavovacích obrazovek |
| `SYSTem:BEEP <ON\|OFF>` / `?` | všechny zvuky zap/vyp |
| `SYSTem:YOFFset <0..n>` / `?` | svislá poloha obrazu na panelu (výchozí 24) |
| `SYSTem:COMM <UART\|USB>` / `?` | port, na kterém běží SCPI (přepne hned) |
| `SYSTem:DEBUG <ON\|OFF>` / `?` | výpis každého rámce z DMM (surové bajty + dekódovaná hodnota) |
| `SYSTem:DEBUG:PORT <UART\|USB>` / `?` | kam jdou ladicí zprávy (na UARTu začíná každý řádek `# `) |
| `SYSTem:DIAGnostic?` | paměť, zásobníky všech tasků, časy vykreslování, grafu a statistiky, zaplnění logu, uspání při vypnutém K197 (počet, doba spánku, poslední probuzení) |
| `SYSTem:DIAGnostic:CLEar` | vynuluje maxima |

Všechna nastavení kromě `MEASure:TRIGger` se ukládají do EEPROM. Změny přes SCPI se ukládají hned, změny z panelu
asi 10 s po poslední změně.
