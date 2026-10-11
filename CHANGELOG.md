# Changelog

**English** · [Česky](#historie-změn)

| Version | Changes |
|---|---|
| v22 | **K197 switched off**: `oFF197` (instead of `Err197`) when the K197 sends nothing, every screen goes back to the main display, the graph and statistics keep the last function; light sleep after 60 s without a key, woken by the K197, a key or the SCPI UART (`power.ino`). The first 20 readings (~6 s) after the K197 is switched on are flagged `SETTLE`. No-data time 4 s (was 10 s). `SYSTem:TIME:AGINg` (DS3231 rate trim, kept in the settings). |
| v21 | Graph Y axis **ROBUST** (AUTO without the highest / lowest 2 % of the columns: one spike does not set the scale). SUPP span from ±1 count. |
| v20 | The first 7 readings (~2 s, `CONFIG_DATALOG_SETTLE_READINGS`) after a change of the function (and after power-up / the DMM self-test) are flagged `SETTLE`: still logged, but left out of the statistics, the graph, `LOG:MIN?/MAX?/AVERage?/MEDian?`, the alarm and its default limits. |
| v19 | Slower limit editing on the alarm settings (a step every 200 ms, ×10 every 3 s). SCPI help texts brought up to date (RTC timestamps in `LOG:DATA?`, `SYSTem:BEEP`). |
| v18 | **Markers**: REL + dB together, `MEASure:TRIGger MARK` (foot switch on the rear trigger input), `LOG:MARKer` / `LOG:MARKer?`. "TAG" in place of REM, double beep, triangles in the graph. On the graph dB acts at the release. |
| v17 | **Limit alarm** per function (dB held 1 s): limits, outline / blinking triangle above the minus, beep, `CALCulate:LIMit:*`. Default limits from MIN/MAX of the last 10 s. dB is sent to the K197 at the release. |
| v16 | `SYSTem:DIAGnostic?` (memory, stacks, timings). Smaller PSRAM reserve (≈ +2 h of log). Median adapts to the free memory. |
| v15 | Context help in Czech and English (`SYSTem:LANGuage`). DMM settings screen (language, theme, sound). Graph intervals 10 s and 30 s. `GRAPh:*` commands. Settings screens close after 60 s. |
| v14 | **Graph screen**: MIN/MAX band + average, 10 s … 24 h and FULL, Y axis AUTO / ZERO / SUPP, settings screen, 5×7 pixel font. |
| v13 | DMM watchdog (`Err197`, SPI restart), `SYSTem:DEBUG:PORT`, boot report, `MEASure:TRIGger`. |
| v12 | Help screen. REL sent at the release. Black background on the text screens. Rear trigger I/O (METERING COMPLETE, EXTERNAL TRIGGER). |
| v8 – v11 | Statistics screen (STO), button task with debouncing, statistics computed in time slices, persistent interval. |
| v6 – v7 | Dual-core split (capture on core 0). Frame-based rendering into a 6 bpp framebuffer with a separate blit task. |
| ≤ v5 | Bring-up: decoding the DMM bus, the LCD replica on the TFT, SCPI, the PSRAM log, the DS3231 RTC. |

---

## Historie změn

| Verze | Změny |
|---|---|
| v22 | **Vypnutý K197**: `oFF197` (místo `Err197`), když K197 neposílá nic; každá obrazovka se vrátí na hlavní displej, graf a statistika drží poslední funkci; po 60 s bez stisku light sleep, probudí ho K197, tlačítko nebo UART SCPI (`power.ino`). Prvních 20 odečtů (asi 6 s) po zapnutí K197 má příznak `SETTLE`. Doba bez dat 4 s (dříve 10 s). `SYSTem:TIME:AGINg` (dolaďování chodu DS3231, uloží se do nastavení). |
| v21 | Osa Y grafu **ROBUST** (AUTO bez 2 % nejvyšších / nejnižších sloupců: jedna špička nezmění měřítko). Rozpětí SUPP od ±1 digitu. |
| v20 | Prvních 7 odečtů (asi 2 s, `CONFIG_DATALOG_SETTLE_READINGS`) po změně funkce (a po zapnutí / self-testu DMM) mají příznak `SETTLE`: zapíší se do logu, ale statistika, graf, `LOG:MIN?/MAX?/AVERage?/MEDian?`, alarm i jeho výchozí meze je vynechají. |
| v19 | Pomalejší nastavování mezí alarmu (krok každých 200 ms, ×10 každé 3 s). Aktualizované texty nápovědy SCPI (čas z RTC v `LOG:DATA?`, `SYSTem:BEEP`). |
| v18 | **Markery**: REL + dB současně, `MEASure:TRIGger MARK` (nožní spínač na zadním vstupu triggeru), `LOG:MARKer` / `LOG:MARKer?`. „TAG“ na místě REM, dvojpípnutí, trojúhelníčky v grafu. V grafu dB reaguje až při uvolnění. |
| v17 | **Alarm mezí** pro každou funkci (podržení dB 1 s): meze, obrys / blikající trojúhelník nad mínusem, pípání, `CALCulate:LIMit:*`. Výchozí meze z MIN/MAX posledních 10 s. dB se do K197 posílá až při uvolnění. |
| v16 | `SYSTem:DIAGnostic?` (paměť, zásobníky, časy). Menší rezerva PSRAM (≈ +2 h logu). Medián se přizpůsobí volné paměti. |
| v15 | Kontextová nápověda česky a anglicky (`SYSTem:LANGuage`). Obrazovka nastavení DMM (jazyk, téma, zvuk). Intervaly grafu 10 s a 30 s. Příkazy `GRAPh:*`. Nastavovací obrazovky se zavřou po 60 s. |
| v14 | **Obrazovka grafu**: pás MIN/MAX + průměr, 10 s až 24 h a FULL, osa Y AUTO / ZERO / SUPP, obrazovka nastavení, pixelový font 5×7. |
| v13 | Hlídání DMM (`Err197`, restart SPI), `SYSTem:DEBUG:PORT`, hlášení při startu, `MEASure:TRIGger`. |
| v12 | Obrazovka nápovědy. REL se posílá při uvolnění. Černé pozadí textových obrazovek. Zadní vstup a výstup triggeru (METERING COMPLETE, EXTERNAL TRIGGER). |
| v8 – v11 | Obrazovka statistiky (STO), task tlačítek s odrušením zákmitů, výpočet statistiky po časových dílech, ukládání intervalu. |
| v6 – v7 | Rozdělení na dvě jádra (příjem na jádře 0). Vykreslování po celých snímcích do 6bitového framebufferu se samostatným přenosovým taskem. |
| ≤ v5 | Oživení: dekódování sběrnice DMM, replika LCD na TFT, SCPI, log v PSRAM, RTC DS3231. |
