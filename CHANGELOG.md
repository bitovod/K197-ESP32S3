# Changelog

**English** · [Česky](#historie-změn)

| Version | Changes |
|---|---|
| v25 | **Statistics on three pages** (RCL on the statistics screen): 1 AVG MED MAX MIN, 2 standard deviation SDV, MAX − MIN (DIF), SDV / AVG (REL), number of readings (PTS), 3 standard error of the mean SEM, trend TRD (least squares over the RTC time, per minute up to 1 MIN, otherwise per hour), SDV in digits of the last place, DIF relative to AVG. `LOG:SDEViation?`, `LOG:TREND?`. On the help screen RCL turns to the next page (like RCL = next line on the settings screens). |
| v24 | **DATA HOLD** (STO held 1 s on the main display): manual HOLD freezes the reading shown at the press and marks it; **AUTO HOLD** waits for a stable reading and captures it with a beep. "HOLD" / "AUTO" in place of STO / RCL. Choice on the DMM settings screen, `MEASure:HOLD`. Only the display freezes - log, statistics, graph, alarm and SCPI stay live. |
| v23 | **SD card logging** (SDMMC 1-bit): a CSV file per function and per K197 power-on, written every 30 s and at once on a marker; card problems shown as a blinking "SD!" in place of BAT with an alarm beep. Sound setting ALL / ALARMS ONLY / OFF. DMM settings screen with six lines (scrolling). `MMEMory:LOG`, `MMEMory:FORMat`, `MMEMory:STATe?`, `SYSTem:BEEP ALARm`. |
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
| v25 | **Statistika na třech stranách** (RCL na obrazovce statistiky): 1 AVG MED MAX MIN, 2 směrodatná odchylka SDV, MAX − MIN (DIF), SDV / AVG (REL), počet měření (PTS), 3 směrodatná chyba průměru SEM, trend TRD (metoda nejmenších čtverců podle času z RTC, za minutu do intervalu 1 MIN, jinak za hodinu), SDV v digitech posledního místa, DIF relativně k AVG. `LOG:SDEViation?`, `LOG:TREND?`. V nápovědě RCL listuje na další stranu (stejně jako RCL = další řádek na nastavovacích obrazovkách). |
| v24 | **DATA HOLD** (podržení STO 1 s na hlavním displeji): ruční HOLD zmrazí hodnotu zobrazenou při stisku a označí ji; **AUTO HOLD** čeká na ustálenou hodnotu a zachytí ji s pípnutím. „HOLD“ / „AUTO“ na místě STO / RCL. Volba na obrazovce nastavení DMM, `MEASure:HOLD`. Zmrazí se jen displej - log, statistika, graf, alarm i SCPI běží dál. |
| v23 | **Záznam na SD kartu** (SDMMC 1-bit): soubor CSV pro každou funkci a každé zapnutí K197, zápis každých 30 s a hned při markeru; potíže s kartou ukazuje blikající „SD!“ na místě BAT s pípáním. Zvuk VŠE / JEN ALARMY / VYP. Obrazovka nastavení DMM se šesti řádky (posouvá se). `MMEMory:LOG`, `MMEMory:FORMat`, `MMEMory:STATe?`, `SYSTem:BEEP ALARm`. |
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
