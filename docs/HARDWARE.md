# Hardware

**English** · [Česky](#hardware-česky)

> This page describes the prototype as built. The power supply, the rear-panel trigger connectors and the SD card
> slot are still being worked on; their sections will be completed.

## Safety first

The Keithley 197 is a floating meter: its digital circuitry, and with it the ESP32, the display, the keys and
everything else in this project, is referenced to the **measurement input LO**. When the meter measures with LO
at a high voltage, all of that is at the same voltage.

- Anything that leaves the instrument must be galvanically isolated: the SCPI UART, the trigger I/O, a PC on USB.
  **Do not connect the ESP32's USB port to a PC while measuring.** It is only meant for flashing and debugging with
  the inputs disconnected.
- Parts that a user can touch from outside (an SD card slot, for example) must be recessed or covered.
- The instrument contains mains voltage. Work on it only when it is unplugged, and only if you know what you are
  doing.

## Parts

| Part | Notes |
|---|---|
| ESP32-S3 board with **8 MB OPI PSRAM + 16 MB flash** | developed on an "ESP32-S3-Nano" (ESP32-S3R8, W25Q128) |
| NV3007 TFT, 2.79", 428 × 142 px, IPS | SPI; mounting matters, because mechanical stress on the panel or its flex cable shows as artefacts |
| DS3231 RTC module | I2C; time stamps of the log, resolution 10 ms (library `DS3231Centisecond`) |
| 4 push buttons | front panel, under the display |
| 2 × PC817 optocoupler | REL/dB towards the K197 |
| active buzzer | own oscillator, driven HIGH/LOW |
| optocouplers for the rear trigger I/O | METERING COMPLETE out, EXTERNAL TRIGGER in |
| isolated USB-UART (CH340) | SCPI on the rear panel |
| resistors 10 k / 20 k | dividers for the DMM bus tap |

## Pins (`config.h`)

| GPIO | Use |
|---|---|
| 2, 10, 12, 11 (9) | TFT: CS, DC, SCK, MOSI (RST on 9, not wired) |
| 4, 5, 6 | DMM bus tap: SCK, DATA (MOSI), CS — SPI3 in slave mode, input only |
| 17, 18 | SCPI UART TX, RX |
| 39, 40, 41, 42 | keys REL, dB, STO, RCL (active low, internal pull-up; the former JTAG pins) |
| 13, 14 | REL / dB pulse to the K197 (active high, through PC817) |
| 15, 16 | I2C SDA, SCL (DS3231) |
| 48 | beeper (shared with the on-board LED) |
| 7 | METERING COMPLETE output (pulse after every reading) |
| 8 | EXTERNAL TRIGGER input (falling edge, internal pull-up) |

Pins that are probably still free: **1, 21, 38, 47**; 9 if the TFT RST stays unconnected; 43/44 (UART0) if the
board brings them out. Avoid the strapping pins 0, 3, 45, 46, the USB pins 19/20, the flash pins 26–32, and on
S3R8 modules the octal PSRAM pins 33–37.

## Connections to the K197

**DMM bus tap.** The K197's CPU talks to the µPD7225 LCD driver over a one-way serial bus (5 V TTL). Each of the
three lines (clock, data, chip select) goes through a **10 k / 20 k divider** (5 V → 3.3 V, about 170 µA per line)
to the ESP32. The ESP32 only listens. Bring-up can be done with the original LCD still connected in parallel.
Each frame has 9 … 17 bytes and comes about 3.5 times per second. Without valid frames for 10 s the replica shows
`Err197` and the SPI slave restarts.

**REL / dB.** The original keys connect a sense line to +5 V. One PC817 per line emulates that: LED from the GPIO
through 220 Ω to GND (about 10 mA), collector to +5 V, emitter to the K197 sense line. Even at the worst-case CTR
there is about 10× more current than the line needs. The original buttons, moved to the rear panel, stay in
parallel.

**Common ground.** The TFT, the DMM tap and the UART need the K197 logic ground as their ground.

## Power

The ESP32 and the TFT must not be fed from the K197's 5 V logic supply. The start-up current made the K197 supply
sag and its CPU hang (seen as a BROWNOUT reset of the ESP32 and no DMM frames until the meter was power cycled).
The solution being built: a separate 5 V supply from the spare winding of the K197 transformer that was meant for
the GPIB option. It uses the original GPIB supply design: bridge rectifier, 250 µF / 25 V, 7805 on a heat sink,
2.2 mF + 2 × 100 nF. Its ground is tied to the K197 logic ground at one point.

## Planned: SD card

SDMMC in 1-bit mode needs 3 GPIOs (CLK, CMD, D0) plus an optional card-detect pin. It needs pull-ups of 10 k on
CMD, D0 and D3, and 10 … 47 µF close to the socket. Use a plain socket or breakout: the usual "micro SD modules"
with a level shifter work in SPI mode only. Keep the cable short.

---

# Hardware (česky)

> Stránka popisuje prototyp tak, jak je postavený. Napájení, zadní konektory triggeru a slot pro SD kartu jsou
> rozpracované; jejich části budou doplněny.

## Nejdřív bezpečnost

Keithley 197 je plovoucí měřidlo: jeho digitální část, a s ní ESP32, displej, tlačítka a vše ostatní z tohoto
projektu, je vztažená ke **vstupu LO**. Když přístroj měří s LO na vysokém napětí, je na stejném napětí i tohle vše.

- Cokoli vede ven z přístroje, musí být galvanicky oddělené: SCPI UART, vstupy a výstupy triggeru, PC na USB.
  **USB port ESP32 během měření nepřipojujte k PC.** Je jen pro nahrávání firmwaru a ladění s odpojenými vstupy.
- Části, na které uživatel dosáhne zvenku (např. slot SD karty), musí být zapuštěné nebo zakryté.
- V přístroji je síťové napětí. Pracujte na něm jen odpojeném ze sítě a jen tehdy, když víte, co děláte.

## Součástky

| Součástka | Poznámka |
|---|---|
| deska s ESP32-S3 s **8 MB OPI PSRAM + 16 MB flash** | vyvíjeno na „ESP32-S3-Nano“ (ESP32-S3R8, W25Q128) |
| TFT NV3007, 2,79", 428 × 142 px, IPS | SPI; záleží na uchycení, mechanické pnutí panelu nebo jeho flex kabelu se projeví artefakty |
| modul RTC DS3231 | I2C; časové značky logu v rozlišení 10 ms (knihovna `DS3231Centisecond`) |
| 4 tlačítka | čelní panel pod displejem |
| 2 × optočlen PC817 | REL/dB směrem do K197 |
| aktivní bzučák | vlastní oscilátor, řízení HIGH/LOW |
| optočleny pro zadní vstupy/výstupy triggeru | výstup METERING COMPLETE, vstup EXTERNAL TRIGGER |
| oddělený převodník USB-UART (CH340) | SCPI na zadním panelu |
| rezistory 10 k / 20 k | děliče pro odposlech sběrnice DMM |

## Piny (`config.h`)

| GPIO | Použití |
|---|---|
| 2, 10, 12, 11 (9) | TFT: CS, DC, SCK, MOSI (RST na 9, nezapojený) |
| 4, 5, 6 | odposlech sběrnice DMM: SCK, DATA (MOSI), CS — SPI3 v režimu slave, jen vstup |
| 17, 18 | SCPI UART TX, RX |
| 39, 40, 41, 42 | tlačítka REL, dB, STO, RCL (aktivní v nule, interní pull-up; bývalé piny JTAG) |
| 13, 14 | puls REL / dB do K197 (aktivní v jedničce, přes PC817) |
| 15, 16 | I2C SDA, SCL (DS3231) |
| 48 | bzučák (sdílený s LED na desce) |
| 7 | výstup METERING COMPLETE (puls po každém měření) |
| 8 | vstup EXTERNAL TRIGGER (sestupná hrana, interní pull-up) |

Pravděpodobně ještě volné piny: **1, 21, 38, 47**; pin 9, pokud RST displeje zůstane nezapojený; 43/44 (UART0),
pokud je deska vyvádí. Nepoužívat strapping piny 0, 3, 45, 46, piny USB 19/20, piny flash 26–32 a u modulů S3R8
piny oktální PSRAM 33–37.

## Propojení s K197

**Odposlech sběrnice DMM.** Procesor K197 komunikuje s budičem LCD µPD7225 po jednosměrné sériové sběrnici (5 V
TTL). Každá ze tří linek (hodiny, data, chip select) jde přes **dělič 10 k / 20 k** (5 V → 3,3 V, asi 170 µA na
linku) do ESP32. ESP32 jen poslouchá. Oživovat jde i s původním LCD zapojeným paralelně. Rámec má 9 až 17 bajtů
a přichází asi 3,5krát za sekundu. Když 10 s nepřijde platný rámec, replika ukáže `Err197` a SPI slave se
restartuje.

**REL / dB.** Původní tlačítka připojují snímací linku na +5 V. To napodobuje jeden PC817 na linku: LED z GPIO přes
220 Ω na GND (asi 10 mA), kolektor na +5 V, emitor na snímací linku K197. I při nejhorším CTR je k dispozici asi
10× víc proudu, než linka potřebuje. Původní tlačítka, přesunutá na zadní panel, zůstávají zapojená paralelně.

**Společná zem.** Displej, odposlech DMM a UART potřebují jako zem logickou zem K197.

## Napájení

ESP32 a displej se nesmí napájet z 5V logického napájení K197. Proudový náraz při startu stáhl napájení K197 a jeho
procesor se zasekl (projevilo se resetem ESP32 z důvodu BROWNOUT a žádnými rámci z DMM až do vypnutí a zapnutí
multimetru). Řešení ve stavbě: samostatných 5 V z rezervního vinutí transformátoru K197 určeného pro volitelnou
desku GPIB. Používá původní zapojení napájení GPIB: usměrňovací můstek, 250 µF / 25 V, 7805 na chladiči,
2,2 mF + 2 × 100 nF. Zem je spojená s logickou zemí K197 v jednom bodě.

## V plánu: SD karta

SDMMC v jednobitovém režimu potřebuje 3 GPIO (CLK, CMD, D0) a volitelně pin pro detekci karty. Potřebuje pull-upy
10 k na CMD, D0 a D3 a 10 až 47 µF u slotu. Použijte holý slot nebo breakout: běžné „micro SD moduly“ s převodníkem
úrovní fungují jen v režimu SPI. Kabel držte krátký.
