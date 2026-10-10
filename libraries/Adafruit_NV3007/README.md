# Adafruit_NV3007

**English** · [Česky](#česky)

Arduino driver for TFT IPS displays with the **NV3007** controller (NewVision), for example the 2.79" 142 × 428 px
panel used by [K197-ESP32S3](../../README.md). The class is derived from `Adafruit_SPITFT`, so the whole
[Adafruit GFX](https://github.com/adafruit/Adafruit-GFX-Library) API works: drawing, fonts, bitmaps and the fast
`startWrite()` / `setAddrWindow()` / `writePixels()` path that the firmware uses for its framebuffer.

> **Not an official Adafruit product.** The name only follows the convention of the Adafruit drivers it is modelled
> on (`Adafruit_ST7789` and others). It is not made, reviewed or supported by Adafruit Industries.

### Features

- Hardware SPI (`SPIClass*`), default hardware SPI or software SPI constructors, SPI mode 3.
- `setRotation()` 0–3 with the panel offsets (`NV3007_COL_OFFSET`, `NV3007_ROW_OFFSET` = 14 for 142 × 428).
- `invertDisplay()`, `enableTearingEffect()`.
- Panel size and offsets can be overridden with `#define` before the include.

### Installation

Copy the `Adafruit_NV3007` folder into your Arduino `libraries` folder. Dependencies from the Library Manager:
**Adafruit GFX Library** and **Adafruit BusIO**.

### Use

```cpp
#include <SPI.h>
#include <Adafruit_NV3007.h>

Adafruit_NV3007 tft(&SPI, TFT_CS, TFT_DC, TFT_RST);

void setup() {
  SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);
  tft.begin();          // 80 MHz by default
  tft.setRotation(1);   // landscape 428 x 142
  tft.fillScreen(0);
}
```

See [examples/HelloWorld](examples/HelloWorld/HelloWorld.ino).

### SPI clock

`begin()` without an argument runs the bus at **80 MHz**. The K197-ESP32S3 firmware runs at this speed with short
wiring. The controller documentation is more conservative (about 40–50 MHz), so with longer wires or artefacts use
`tft.begin(40000000UL)`.

### Origin of the init sequence

The power, gamma and GOA registers of the initialization sequence come from a working ESP-IDF reference driver for
the 142 × 428 IVO panel (2.66" / 2.79"). Another panel (for example 168 × 428) may need other offsets and possibly
other gamma and power values from its datasheet.

### License

MIT, the same as the rest of the repository ([LICENSE](../../LICENSE)). It needs Adafruit GFX Library and Adafruit
BusIO, which are under the BSD license and are not included here.

---

## Česky

Ovladač pro Arduino pro TFT IPS displeje s řadičem **NV3007** (NewVision), například panel 2,79" 142 × 428 px
použitý v projektu [K197-ESP32S3](../../README.md). Třída je odvozená od `Adafruit_SPITFT`, takže funguje celé API
[Adafruit GFX](https://github.com/adafruit/Adafruit-GFX-Library): kreslení, fonty, bitmapy i rychlá cesta
`startWrite()` / `setAddrWindow()` / `writePixels()`, kterou firmware používá pro svůj framebuffer.

> **Nejde o oficiální produkt firmy Adafruit.** Název jen dodržuje zvyklost ovladačů Adafruit, podle kterých je
> knihovna udělaná (`Adafruit_ST7789` a další). Adafruit Industries ji nevyrábí, nekontroluje ani nepodporuje.

### Vlastnosti

- Konstruktory pro hardwarové SPI (`SPIClass*`), výchozí hardwarové SPI i softwarové SPI, SPI režim 3.
- `setRotation()` 0–3 s posuny panelu (`NV3007_COL_OFFSET`, `NV3007_ROW_OFFSET` = 14 pro 142 × 428).
- `invertDisplay()`, `enableTearingEffect()`.
- Rozměry a posuny panelu lze změnit pomocí `#define` před vložením hlavičky.

### Instalace

Složku `Adafruit_NV3007` zkopírujte do složky `libraries` Arduina. Závislosti z Library Manageru:
**Adafruit GFX Library** a **Adafruit BusIO**.

### Použití

Viz příklad výše a [examples/HelloWorld](examples/HelloWorld/HelloWorld.ino).

### Takt SPI

`begin()` bez parametru nastaví sběrnici na **80 MHz**. Firmware K197-ESP32S3 na této rychlosti běží s krátkým
propojením. Dokumentace řadiče je opatrnější (asi 40–50 MHz), takže při delších vodičích nebo artefaktech použijte
`tft.begin(40000000UL)`.

### Původ inicializační sekvence

Registry napájení, gamma a GOA v inicializační sekvenci pocházejí z funkčního referenčního ovladače pro ESP-IDF pro
panel IVO 142 × 428 (2,66" / 2,79"). Jiný panel (například 168 × 428) může potřebovat jiné posuny a případně jiné
hodnoty gammy a napájení podle svého datasheetu.

### Licence

MIT, stejně jako zbytek repozitáře ([LICENSE](../../LICENSE)). Potřebuje knihovny Adafruit GFX Library a Adafruit
BusIO, které jsou pod licencí BSD a nejsou zde přiloženy.
