// serial_out.ino
// SCPI-style command interface, using the Vrekrer_scpi_parser library
// (https://github.com/Vrekrer/Vrekrer_scpi_parser) - install it via
// the Arduino Library Manager (search "Vrekrer SCPI parser") before
// building; it's a third-party dependency, not bundled in this
// snapshot.
//
// Commands:
//   *IDN?           standard SCPI identification query
//   *HLP / *HLP? / HELP / HELP?   brief, one-line-per-command list -
//                    see HandleHelp() below
//   *HLP:DETail? / HELP:DETail?   full command reference - see
//                    HandleHelpDetail() below, kept as the single
//                    source of truth for exact command syntax/behavior
//   READ?           the currently decoded DMM reading, formatted per
//                    SYSTem:FORMat - see printCurrentReading() below
//   SYSTem:FORMat <0|1|2> / ?   output format for READ? and auto-send
//                    (named FORMat, not READing/READ - the obvious
//                    "READ" abbreviation collides with the top-level
//                    READ? command's own token, see
//                    HandleSystemFormatSet()'s comment for why)
//   SYSTem:AUTO <ON|OFF|0|NNN> / ?   automatic/periodic sending
//   SYSTem:THEMe <0..themeCount-1> / ?   display color theme
//   SYSTem:YOFFset <0..N> / ?   vertical pixel offset where display
//                    drawing starts - see setDisplayYOffset()
//                    (display.h) for why this needs to be adjustable
//   SYSTem:DEBUG <ON|OFF> / ?   live trace on the debug Serial: raw
//                    hex bytes of every captured frame, then the
//                    decoded reading - see HandleHelpDetail() below
//   SYSTem:TIME <YYYY> <MM> <DD> <hh> <mm> <ss> / ?   DS3231 RTC
//                    date/time (rtc.h/rtc.ino), SPACE-separated (not
//                    comma) - see HandleSystemTimeSet()'s own comment
//                    for why, and HandleHelpDetail() below for the
//                    reply format and error cases (no RTC wired up,
//                    RTC lost power)
//   SYSTem:BEEP <ON|OFF> / ?   enables/disables every acoustic signal
//                    (beeper.h/beeper.ino) - currently just the
//                    one-shot startup chirp
//   SYSTem:LANGuage <CZ|EN> / ?   (v15) language of the help screens and
//                    of the graph settings screen on the display (not of
//                    this SCPI help, which stays English)
//   GRAPh:INTerval <10S|30S|1M|5M|15M|1H|6H|24H|FULL> / ?   (v15) graph window
//   GRAPh:YAXis <AUTO|ZERO|SUPP> / ?   (v15) graph Y axis mode
//   GRAPh:TRACe <BOTH|AVG|BAND> / ?    (v15) what the graph draws
//   GRAPh:SPAN <10|20|50|...|10000> / ?   (v15) SUPP span, +- counts
//                    All four save at once (the front panel saves them
//                    deferred); the graph adopts them on its next pass.
//   SYSTem:DIAGnostic?   (v16) memory, stacks and timings for bring-up:
//                    internal RAM / PSRAM free + low-water + largest
//                    block, free stack of every task, render/blit/UI
//                    loop times, graph build and statistics job times,
//                    log fill, debug queue drops
//   SYSTem:DIAGnostic:CLEar   (v16) zeroes the maxima above
//   SYSTem:COMM <UART|USB> / ?   which physical port the SCPI
//                    interface itself talks on - takes effect
//                    immediately, no reboot needed
//   LOG:POINts? [<function>] [<first> <last>]   how many matching
//                                 records are in the PSRAM log
//   LOG:CAPacity?                 total ring buffer capacity (records),
//                                 sized from free PSRAM at boot - see
//                                 datalog.h
//   LOG:CLEar                    empties the log
//   LOG:DATA? [<function>] [<first> <last>]   matching records
//                                 first..last (1-based, 0 on either
//                                 side = "open end"; negative <first>
//                                 = tail mode, "last |first| records
//                                 before <last>" - see datalog.h; no
//                                 parameters = the whole log) as CSV -
//                                 see datalog.h for the exact
//                                 numbering and the optional function
//                                 filter ("DCV"/"ACV"/"DCI"/"ACI"/
//                                 "OHM"/"DB")
//   LOG:MIN?/MAX?/AVERage?/MEDian? [<function>] [<first> <last>]
//                                 statistics over the same
//                                 range/numbering/filter
// All six SYSTem:* settings that have one (FORMat/AUTO/THEMe/DEBUG/
// BEEP/COMM) persist across power cycles via settings.h/settings.ino
// (emulated EEPROM); the reading log itself does not (see datalog.h).
// SYSTem:TIME is the one exception - it has no entry in settings.h at
// all, because the DS3231 itself (battery/supercap-backed) is already
// the persistent store; mirroring it into EEPROM too would just be a
// second, potentially stale copy of the same fact. See rtc.h.
//
// Every SET command and a totally unrecognized command both reply
// with an explicit "OK: ..." or "ERROR: ..." line (see
// HandleUnknownCommand(), SetErrorHandler(), and each Handle*Set()
// below) - added after a real command silently doing nothing (the
// SYST:READ/READing token collision, see HandleSystemFormatSet())
// went unnoticed for a while with no feedback either way.
//
// dmm_bridge.ino calls serialOutUpdateReading() once per decoded
// frame to keep the cached reading current; that alone does NOT send
// anything (that's what an earlier, pre-SCPI version of this file
// did - see docs/DEVELOPMENT.md). A reply goes out only when the
// outside world asks for one (READ?, LOG:*) or when SYSTem:AUTO is
// enabled (see scpiAutoSendCheck(), called from serialOutProcess()).

#include "serial_out.h"
#include "settings.h"
#include "display.h"
#include "reading_math.h"
#include "datalog.h"
#include "rtc.h"
#include "trigger_io.h"
#include "debuglog.h"
#include "help_text.h" // v15: helpLanguageNames[] for SYSTem:LANGuage
#include "graph.h"     // v15: GRAPh:* (interval/span tables, GRAPH_* constants)
#include "ui.h"        // v16: SYSTem:DIAGnostic? (statistics job timing)
#include "buttons.h"   // v16: SYSTem:DIAGnostic? (button task handle)
#include "alarm.h"     // v17: CALCulate:LIMit:*
#include "marker.h"    // v18: LOG:MARKer, MEASure:TRIGger MARK
#include <esp_heap_caps.h>
#include <Vrekrer_scpi_parser.h>
#include <math.h>

// Which physical port the SCPI interface talks on - UART (Serial1)
// or the USB/debug Serial - is now a runtime choice, persisted via
// SYSTem:COMM (settings.h/HandleSystemCommSet() below), not a
// compile-time switch. Both ports are always begun (see
// serialOutBegin()) so switching between them takes effect
// immediately, with no reboot. scpiStream points at whichever one is
// currently active; every place that used to reference a compile-time
// SCPI_STREAM macro now dereferences this instead.
static Stream *scpiStream = nullptr;

// UART pins and baud - freely reassignable, adjust to suit your
// wiring; nothing else depends on the exact values. Defined here
// with a fallback so the file still compiles standalone, but
// ESP32S3_K197.ino defining them first (same pattern as TFT_*)
// takes priority, since Arduino concatenates .ino files with the
// sketch-folder-named file first.
#ifndef SERIAL_OUT_TX_PIN
#define SERIAL_OUT_TX_PIN 17
#endif
#ifndef SERIAL_OUT_RX_PIN
#define SERIAL_OUT_RX_PIN 18
#endif
#ifndef SERIAL_OUT_BAUD
#define SERIAL_OUT_BAUD 115200
#endif

// *IDN? fields - SCPI_IDN_MANUFACTURER/MODEL/SERIAL/FIRMWARE now live
// in config.h (pulled in via serial_out.h) - that is the file to
// edit if this snapshot ever gets reused for a different physical
// unit.

static SCPI_Parser scpiParser;

// SYSTem:DEBUG - prints every decoded reading to the fixed USB/debug
// Serial port (always Serial, regardless of what SYSTem:COMM has the
// SCPI interface itself routed to - see serialOutUpdateReading()
// below), in the same raw/mirror-of-
// the-display format printReading() already uses for
// SYSTem:FORMat=0, so it's useful as a live "what's actually on the
// display" trace independent of whatever numeric format SCPI itself
// is currently configured to answer with. dmm_bridge.ino's
// dmmApplyFrame() also checks serialOutDebugTraceAllowed() (declared
// in serial_out.h) to print the raw captured frame bytes just before
// the decoded reading, so a full SYSTem:DEBUG pass shows both the hex
// packet and what it decoded to - restored after the author noticed
// the raw byte dump had quietly gone missing once this replaced an
// earlier, pre-SCPI debug mechanism.
//
// Persisted via settings.h (settingsGetDebugEnabled()/
// settingsSetDebugEnabled()) - originally this was a plain local
// `static bool`, deliberately NOT persisted, on the theory that a
// debug trace silently surviving a power cycle was the wrong default.
// In practice, debugging a real problem often needs several restarts
// in a row, and having to re-enable SYSTem:DEBUG after every one of
// them meant missing exactly the frames right after power-up that
// matter most - so it moved into settings.h alongside the other three
// SYSTem:* settings instead.

// Hand-rolled line accumulator for serialOutProcess() (see that
// function's own comment further down for why it doesn't use the
// library's ProcessInput()). Declared up here, not just above
// serialOutProcess() itself, because serialOutUpdateReading() below
// also needs to check `lineLength` (to avoid printing a debug trace
// line in the middle of a command someone hasn't finished typing yet)
// before it's otherwise defined further down in the file.
static char lineBuffer[SCPI_BUFFER_LENGTH];
static uint8_t lineLength = 0;

// Set by a LOG:* handler right before it prints a data dump, to
// suppress the one scpiAutoSendCheck() opportunity that would
// otherwise immediately follow it within the same serialOutProcess()
// call (see that function's tail) - without this, an active
// SYSTem:AUTO would reliably (not just occasionally) append one
// unsolicited reading right after every LOG:DATA?/MIN?/etc. reply,
// corrupting a block the person wants to treat as clean CSV. It's a
// one-shot flag: serialOutProcess() clears it again right after
// honoring it once, so it never suppresses auto-send beyond the
// single pass a dump just happened on.
static bool suppressUnsolicitedOutput = false;

// Cached latest decoded reading - written by serialOutUpdateReading()
// (called from dmm_bridge.ino once per decoded frame), read back by
// the READ? handler below. Initial values match a blank display.
// Prefixed scpi* (rather than e.g. currentDigits/currentUnit) because
// Arduino concatenates every .ino file in the sketch into one
// translation unit - display.h/display.ino already define their own
// globals named currentDigits/currentUnit for their own internal
// state, and a plain "static" here would NOT have avoided colliding
// with those (a name can only be defined once at file scope in one
// translation unit, static or not) - this bit us for real the first
// time this was built for actual hardware, see docs/DEVELOPMENT.md.
//
// Cross-core note (post dual-core split, v6): written on
// core 0 (dmm_bridge.ino's dmmApplyFrame(), via
// serialOutUpdateReading()), read on core 1 (READ?/SYSTem:READing
// here). Deliberately left WITHOUT a mutex - a considered choice, not
// an oversight: these four fields are small (10 bytes total) and
// change at DMM update speed (~285 ms), so the worst a torn read can
// produce is one READ? reply mixing digits from one frame with an
// indicator/unit from the very next - cosmetic, self-corrects on the
// following read, and not worth the locking overhead on every single
// reading update given how much more frequently this is touched than
// the LOG:* ring buffer (which DOES use a mutex - see datalog.ino).
// Revisit with a real mutex if that ever turns out to matter more
// than this reasoning assumes.
static char scpiDigits[KEITHLEY_DIGIT_COUNT] = {' ', ' ', ' ', ' ', ' ', ' '};
static int8_t scpiDpPos = -1;
static uint16_t scpiIndicatorBits = 0;
static uint8_t scpiUnit = KEITHLEY_UNIT_NONE;

// Auto-send bookkeeping (SYSTem:AUTO) - see scpiAutoSendCheck().
// scpiFrameCounter is bumped once per serialOutUpdateReading() call
// (i.e. once per decoded DMM frame), independent of whether anyone's
// listening - "did a new measurement arrive since we last sent one"
// for the interval==0 (every measurement) case, and a stable time
// reference for the interval>0 (every N seconds) case.
static uint32_t scpiFrameCounter = 0;
static uint32_t scpiLastSentFrameCounter = 0;
static uint32_t scpiLastSentMillis = 0;

// v13: MEASure:TRIGger INT|EXT. INT = sending follows SYSTem:AUTO /
// MEASure:AUTO as before. EXT = AUTO sending is suspended; instead, each
// EXTERNAL TRIGGER edge (trigger_io.h) makes the FIRST frame decoded after
// it be sent once. Not persisted: every power-up starts in INT (an
// instrument that silently waits for a trigger nobody sends would look
// dead). Several edges before the next frame give one send.
// The capture task (serialOutUpdateReading(), core 0) turns edges into
// "triggered frames" (scpiExtTrigFrames); the comm task (scpiAutoSendCheck(),
// core 1) sends one reading per new triggered frame. Plain counters, each
// with one writer, so no lock is needed.
// v18: MARK = the input is a marker key (marker.h); sending follows AUTO as with INT.
#define SCPI_TRIGGER_INT  0
#define SCPI_TRIGGER_EXT  1
#define SCPI_TRIGGER_MARK 2
static volatile uint8_t scpiTriggerSource = SCPI_TRIGGER_INT;
static volatile uint32_t scpiExtTrigSeen = 0;   // trigger edge count already turned into a frame (capture task)
static volatile uint32_t scpiExtTrigFrames = 0; // triggered frames so far (capture task)
static uint32_t scpiExtTrigSent = 0;            // triggered frames already sent (comm task)

// Indexed by KeithleyUnit (keithley_shapes.h). Empty string = don't
// print a unit suffix at all (KEITHLEY_UNIT_NONE, and
// KEITHLEY_UNIT_CHESS which decodeUnit() never actually returns).
static const char *const keithleyUnitNames[KEITHLEY_UNIT_COUNT] = {
    "",     // KEITHLEY_UNIT_NONE
    "mV",   // KEITHLEY_UNIT_MV
    "V",    // KEITHLEY_UNIT_V
    "uA",   // KEITHLEY_UNIT_UA
    "mA",   // KEITHLEY_UNIT_MA
    "A",    // KEITHLEY_UNIT_A
    "OHM",  // KEITHLEY_UNIT_OHM
    "KOHM", // KEITHLEY_UNIT_KOHM
    "MOHM", // KEITHLEY_UNIT_MOHM
    "",     // KEITHLEY_UNIT_CHESS - unused
    "dB",   // KEITHLEY_UNIT_DB
};

struct IndicatorName
{
    const char *name;
    uint16_t bit;
};

// Mirrors the KEITHLEY_BIT_* macros in display.h, minus MINUS -
// that one is handled separately below as the reading's sign rather
// than printed as a word.
static const IndicatorName indicatorNames[] = {
    {"AUTO", KEITHLEY_BIT_AUT},
    {"REL",  KEITHLEY_BIT_REL},
    {"STO",  KEITHLEY_BIT_STO},
    {"RCL",  KEITHLEY_BIT_RCL},
    {"AC",   KEITHLEY_BIT_AC},
    {"REM",  KEITHLEY_BIT_REM},
    {"BAT",  KEITHLEY_BIT_BAT},
};
#define INDICATOR_NAME_COUNT (sizeof(indicatorNames) / sizeof(indicatorNames[0]))

// Formats the cached reading (scpiDigits/scpiDpPos/
// scpiIndicatorBits/scpiUnit) onto `interface` - same format
// the pre-SCPI version of this file used to stream unsolicited:
// optional leading '-', digits with '.' inserted at the decimal
// point (or the word "OL" if any digit is 'L'), then a
// space-separated unit name and any active indicator names.
static void printReading(Stream &interface)
{
    bool overload = false;
    for (uint8_t i = 0; i < KEITHLEY_DIGIT_COUNT; i++)
    {
        if (scpiDigits[i] == 'L')
            overload = true;
    }

    if (scpiIndicatorBits & KEITHLEY_BIT_MINUS)
        interface.print('-');

    if (overload)
    {
        interface.print("OL");
    }
    else
    {
        for (uint8_t i = 0; i < KEITHLEY_DIGIT_COUNT; i++)
        {
            if (scpiDigits[i] != ' ')
                interface.print(scpiDigits[i]);
            if (i == scpiDpPos)
                interface.print('.');
        }
    }

    if (scpiUnit < KEITHLEY_UNIT_COUNT && keithleyUnitNames[scpiUnit][0] != '\0')
    {
        interface.print(' ');
        interface.print(keithleyUnitNames[scpiUnit]);
    }

    for (uint8_t i = 0; i < INDICATOR_NAME_COUNT; i++)
    {
        if (scpiIndicatorBits & indicatorNames[i].bit)
        {
            interface.print(' ');
            interface.print(indicatorNames[i].name);
        }
    }

    interface.println();
}

// SYSTem:FORMat=1 (includePrefix=true) and =2 (includePrefix=false).
// Every numeric reply with a prefix starts with a one-character
// status, directly in front of the function prefix (no separator) -
// see keithleyPrintStatusPrefixValue() (reading_math.ino) for exactly
// which letter and why; "OL" no longer prints as bare text here
// either way, it goes through KEITHLEY_OVERLOAD_SENTINEL like any
// other numeric reading (sign still from the minus indicator, which
// stays valid during "-OL"). SYSTem:FORMat=2 has no prefix at all by
// definition, so it skips straight to the value with no status
// character either - the status is conceptually part of the prefix.
static void printReadingNumeric(Stream &interface, bool includePrefix)
{
    bool overload = keithleyIsOverload(scpiDigits);
    bool ac = (scpiIndicatorBits & KEITHLEY_BIT_AC) != 0;

    double value;
    if (overload)
    {
        value = (scpiIndicatorBits & KEITHLEY_BIT_MINUS)
                    ? -KEITHLEY_OVERLOAD_SENTINEL
                    : KEITHLEY_OVERLOAD_SENTINEL;
    }
    else if (!keithleyReadingToDouble(scpiDigits, scpiDpPos, scpiIndicatorBits, scpiUnit, &value))
    {
        // Not overload, and not an ordinary numeric reading either
        // (no decimal point at all - e.g. self-test content) - fall
        // back to the raw text rather than invent a number.
        printReading(interface);
        return;
    }

    if (includePrefix)
        keithleyPrintStatusPrefixValue(interface, value, scpiUnit, scpiIndicatorBits, overload);
    else
        keithleyPrintScientific(interface, value, ac);
    interface.println();
}

// Formats the cached reading per the current SYSTem:FORMat setting -
// the single entry point both READ? and auto-send (scpiAutoSendCheck)
// go through, so they're always in sync.
static void printCurrentReading(Stream &interface)
{
    switch (settingsGetReadingFormat())
    {
        case SETTINGS_READING_PREFIXED:
            printReadingNumeric(interface, true);
            break;
        case SETTINGS_READING_NUMERIC:
            printReadingNumeric(interface, false);
            break;
        default:
            printReading(interface);
            break;
    }
}

// See the declaration comment in serial_out.h - only meaningful once
// serialOutBegin() has set scpiStream.
Stream &serialOutActiveStream()
{
    return *scpiStream;
}

// Shared by HandleIdentify() below and ESP32S3_K197.ino's setup()
// (which prints this once at boot instead of a plain "starting up"
// banner - see that file for why) so the exact identification string
// only has one place it's assembled.
void serialOutPrintIdn(Stream &interface)
{
    interface.println(F(SCPI_IDN_MANUFACTURER "," SCPI_IDN_MODEL ","
                         SCPI_IDN_SERIAL "," SCPI_IDN_FIRMWARE));
}

static void HandleIdentify(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    serialOutPrintIdn(interface);
}

// Registered via SetErrorHandler() (serialOutBegin()) - the library
// calls this whenever a command's keywords don't match any token it
// knows at all (see GetCommandCode_()/Execute() in
// Vrekrer_scpi_parser_code.h: this is the `unknown_hash` case
// specifically). NOTE this does NOT catch every possible mistake: a
// command whose individual keywords are each independently
// recognized, but combined in a way that was never actually
// registered (e.g. a typo'd abbreviation that happens to collide with
// a different command's token, the way "SYST:READ" used to silently
// match "READ?"'s own token before that got renamed - see
// HandleSystemFormatSet()'s comment), computes a real hash that
// simply matches nothing in the registered set, which the library
// treats the same as an unrecognized-but-harmless no-op rather than
// routing through here. Genuine typos in a single keyword (e.g.
// "SYSTEM:REEDING") DO get caught here.
static void HandleUnknownCommand(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    suppressUnsolicitedOutput = true;
    interface.println(F("ERROR: unknown command - try HELP? for a command list, HELP:DETail? for full details"));
}

// *HLP / *HLP? / HELP / HELP? (serialOutBegin()) - the brief, one-
// line-per-command list. Kept as its own function (rather than just
// the first few lines of HandleHelpDetail() below) so it stays a
// quick scan even as HandleHelpDetail() keeps growing - HELP used to
// mean the full multi-page reference, which got unwieldy to scroll
// past just to remember a command's exact spelling. Every line here
// should have a matching, more detailed entry in HandleHelpDetail()
// below; nothing here should be the ONLY place a command is
// documented.
static void HandleHelp(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    suppressUnsolicitedOutput = true;

    interface.println(F("=== Keithley 197 SCPI commands (brief - HELP:DETail? for the full reference) ==="));
    interface.println(F("*IDN?                                              identification string"));
    interface.println(F("*HLP / *HLP? / HELP / HELP?                       this list"));
    interface.println(F("*HLP:DETail? / HELP:DETail?                       full reference, one command at a time"));
    interface.println(F("READ?                                              current decoded reading"));
    interface.println(F("MEASure? / FETCh?                                 aliases of READ?"));
    interface.println(F("MEASure:TRIGger <INT|EXT|MARK> / ?                INT = AUTO, EXT = send on rear trigger, MARK = marker"));
    interface.println(F("MEASure:AUTO <ON|OFF|0|NNN> / ?                   same as SYSTem:AUTO"));
    interface.println(F("SYSTem:FORMat <0|1|2> / ?                         READ?/AUTO output format"));
    interface.println(F("SYSTem:AUTO <ON|OFF|0|NNN> / ?                    automatic/periodic sending"));
    interface.println(F("SYSTem:THEMe <0..N-1> / ?                         display color theme"));
    interface.println(F("SYSTem:YOFFset <0..N> / ?                         display vertical pixel offset"));
    interface.println(F("SYSTem:DEBUG <ON|OFF> / ?                         raw+decoded frame trace"));
    interface.println(F("SYSTem:DEBUG:PORT <UART|USB> / ?                  where debug messages go"));
    interface.println(F("SYSTem:TIME <YYYY> <MM> <DD> <hh> <mm> <ss> / ?   DS3231 RTC date/time"));
    interface.println(F("SYSTem:BEEP <ON|OFF> / ?                          acoustic signalling on/off"));
    interface.println(F("SYSTem:COMM <UART|USB> / ?                        which port SCPI itself uses"));
    interface.println(F("SYSTem:LANGuage <CZ|EN> / ?                       language of the help on the display"));
    interface.println(F("SYSTem:DIAGnostic? / SYSTem:DIAGnostic:CLEar      memory, stacks, timings / zero the maxima"));
    interface.println(F("GRAPh:INTerval <10S|30S|1M|5M|15M|1H|6H|24H|FULL> / ?   graph window"));
    interface.println(F("GRAPh:YAXis <AUTO|ZERO|SUPP> / ?                  graph Y axis mode"));
    interface.println(F("GRAPh:TRACe <BOTH|AVG|BAND> / ?                   graph trace: band + average, average, band"));
    interface.println(F("GRAPh:SPAN <10|20|50|...|10000> / ?               SUPP span, +- counts of the last digit"));
    interface.println(F("CALCulate:LIMit:UPPer <value> [<function>] / ?    alarm upper limit (base units)"));
    interface.println(F("CALCulate:LIMit:LOWer <value> [<function>] / ?    alarm lower limit (base units)"));
    interface.println(F("CALCulate:LIMit:STATe <OFF|ON|UPPer|LOWer|BOTH> [<function>] / ?   limit alarm"));
    interface.println(F("CALCulate:LIMit:FAIL?                             0 = inside / off, 1 = outside a limit"));
    interface.println(F("CALCulate:LIMit:SIGNal <BEEP|ARROW> / ?           alarm with beep + arrow, or arrow only"));
    interface.println(F("LOG:POINts? [<function>] [<first> <last>]        matching record count"));
    interface.println(F("LOG:CAPacity?                                     total ring buffer capacity"));
    interface.println(F("LOG:CLEar                                         empty the log"));
    interface.println(F("LOG:DATA? [<function>] [<first> <last>]           CSV dump"));
    interface.println(F("LOG:MARKer / LOG:MARKer? [<function>] [<first> <last>]   set a marker / CSV of the marked records"));
    interface.println(F("LOG:MIN?/MAX?/AVERage?/MEDian? [<function>] [<first> <last>]   statistics"));
}

// *HLP:DETail? / HELP:DETail? (serialOutBegin()) - the full command
// reference, kept here in one place specifically so it can never
// silently drift out of sync with the header comment at the top of
// this file (that comment is for someone reading the source; this is
// for someone at a terminal who doesn't have the source open) - if a
// command's behavior changes, both need updating, but at least
// there's only one runtime copy to remember. Uses
// suppressUnsolicitedOutput like every LOG:* reply does, for the same
// reason: this is a long, multi-line dump, and an active SYSTem:AUTO
// could otherwise interleave a reading into the middle of it.
static void HandleHelpDetail(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    suppressUnsolicitedOutput = true;

    interface.println(F("=== Keithley 197 SCPI command reference ==="));
    interface.println();

    interface.println(F("*IDN?"));
    interface.println(F("  Standard SCPI identification query."));
    interface.println(F("  Reply: <manufacturer>,<model>,<serial>,<firmware>"));
    interface.println();

    interface.println(F("*HLP / *HLP? / HELP / HELP?"));
    interface.println(F("  Brief, one-line-per-command list - see HandleHelp()."));
    interface.println();

    interface.println(F("*HLP:DETail? / HELP:DETail?"));
    interface.println(F("  This text - the full reference, one command at a time."));
    interface.println();

    interface.println(F("READ?"));
    interface.println(F("  The currently decoded DMM reading, formatted per"));
    interface.println(F("  SYSTem:FORMat below."));
    interface.println();

    interface.println(F("MEASure? / FETCh?"));
    interface.println(F("  Aliases of READ? (compatibility - the K197 measures continuously,"));
    interface.println(F("  there is no separate trigger/fetch step)."));
    interface.println();

    interface.println(F("MEASure:TRIGger <INT|EXT|MARK>"));
    interface.println(F("MEASure:TRIGger?"));
    interface.println(F("  INT - readings are sent per SYSTem:AUTO / MEASure:AUTO."));
    interface.println(F("  EXT - AUTO sending is suspended; a falling edge on the rear"));
    interface.println(F("        EXTERNAL TRIGGER input makes the next complete DMM frame"));
    interface.println(F("        be sent once (emulated trigger). Several edges before"));
    interface.println(F("        that frame give one reading."));
    interface.println(F("  MARK - (v18) the input is a marker key, e.g. a foot switch:"));
    interface.println(F("        each press puts a marker (TAG) on the reading shown,"));
    interface.println(F("        like REL + dB on the front panel; debounced, presses"));
    interface.println(F("        closer than 300 ms are ignored. AUTO sends as with INT."));
    interface.println(F("  Default INT after every power-up (not persisted)."));
    interface.println(F("  The rear METERING COMPLETE output pulses after every frame"));
    interface.println(F("  in all modes."));
    interface.println();

    interface.println(F("MEASure:AUTO <ON|OFF|0|NNN>"));
    interface.println(F("MEASure:AUTO?"));
    interface.println(F("  Same command and setting as SYSTem:AUTO below."));
    interface.println();

    interface.println(F("SYSTem:FORMat <0|1|2>"));
    interface.println(F("SYSTem:FORMat?"));
    interface.println(F("  (Named FORMat, not READing - \"READ\" as an abbreviation"));
    interface.println(F("  would collide with the top-level READ? command.)"));
    interface.println(F("  Output format for READ? and SYSTem:AUTO's sends."));
    interface.println(F("    0 = raw text, exactly as shown on the display,"));
    interface.println(F("        e.g. \"3.14000 V AUTO\" or \"-OL\""));
    interface.println(F("    1 = status + function + scientific notation,"));
    interface.println(F("        e.g. \"NDCV+1.23456E+2\", \"ZOHM+1.23456E+3\","));
    interface.println(F("             \"ODCV-9.99999E+37\" (overload sentinel)"));
    interface.println(F("        status: N=normal, O=overload, Z=REL active"));
    interface.println(F("        function: DCV / ACV / DCI / ACI / OHM / \" dB\""));
    interface.println(F("    2 = scientific notation only, e.g. \"+1.23456E+2\""));
    interface.println(F("  Default 0. Persists across power cycles."));
    interface.println();

    interface.println(F("SYSTem:AUTO <ON|OFF|0|NNN>"));
    interface.println(F("SYSTem:AUTO?"));
    interface.println(F("  Automatic/periodic sending of READ?'s reply, unasked:"));
    interface.println(F("    OFF - no automatic sending, only on an explicit READ?"));
    interface.println(F("    0   - send once per DMM measurement"));
    interface.println(F("    NNN - send every NNN seconds"));
    interface.println(F("    ON  - resume at whichever interval was last set"));
    interface.println(F("  Default OFF, interval 0. Persists across power cycles."));
    interface.println(F("  The reading log (LOG:* below) uses this same interval"));
    interface.println(F("  independently - it logs regardless of ON/OFF here."));
    interface.println();

    interface.println(F("SYSTem:THEMe <0..N-1>"));
    interface.println(F("SYSTem:THEMe?"));
    interface.println(F("  Display color theme. Query replies \"<n>,<name>\"."));
    interface.println(F("  Persists across power cycles."));
    interface.println();

    interface.println(F("SYSTem:YOFFset <0..N>"));
    interface.println(F("SYSTem:YOFFset?"));
    interface.println(F("  Vertical pixel offset from the top of the panel where"));
    interface.println(F("  display drawing starts - does NOT change the visible"));
    interface.println(F("  content's height, only where it starts, to correct for"));
    interface.println(F("  how this particular panel sits behind the meter's"));
    interface.println(F("  plastic bezel once mounted. Valid range is 0..N, where N"));
    interface.println(F("  depends on the panel's actual height - query with no"));
    interface.println(F("  prior context to see it (an out-of-range SET reply"));
    interface.println(F("  states it too). Default 24. Persists across power"));
    interface.println(F("  cycles, so a real unit's correction only needs setting"));
    interface.println(F("  once."));
    interface.println();

    interface.println(F("SYSTem:DEBUG:PORT <UART|USB>"));
    interface.println(F("SYSTem:DEBUG:PORT?"));
    interface.println(F("  Where debug messages go (the SYSTem:DEBUG trace and the"));
    interface.println(F("  always-on boot / DMM watchdog / error messages): UART ="));
    interface.println(F("  this SCPI UART, each line starting with \"# \"; USB = the"));
    interface.println(F("  ESP32's own USB port. Persists across power cycles."));
    interface.println();

    interface.println(F("SYSTem:DEBUG <ON|OFF>"));
    interface.println(F("SYSTem:DEBUG?"));
    interface.println(F("  Live trace to the debug port (SYSTem:DEBUG:PORT), for every"));
    interface.println(F("  captured DMM frame: the raw bytes as hex"));
    interface.println(F("  (\"frame (17 bytes): 20 30 4A ...\"), then the decoded"));
    interface.println(F("  reading (SYSTem:FORMat=0 format). Independent of"));
    interface.println(F("  which port SYSTem:COMM currently has SCPI itself"));
    interface.println(F("  routed to. Default OFF. Persists across power"));
    interface.println(F("  cycles - on purpose, so a debugging session needing"));
    interface.println(F("  frequent restarts doesn't lose the trace right when"));
    interface.println(F("  it matters most (the first frames after power-up)."));
    interface.println(F("  Remember to turn it back OFF when done."));
    interface.println();

    interface.println(F("SYSTem:TIME <YYYY> <MM> <DD> <hh> <mm> <ss>"));
    interface.println(F("SYSTem:TIME?"));
    interface.println(F("  DS3231 RTC date/time. SET input is 24-hour, SPACE-"));
    interface.println(F("  separated (not comma) - on purpose, so the whole thing"));
    interface.println(F("  arrives as one unsplit block of text rather than hitting"));
    interface.println(F("  the SCPI parser library's small, fixed limit on comma-"));
    interface.println(F("  separated values per command, e.g. \"SYSTem:TIME 2026 9"));
    interface.println(F("  23 14 30 0\". Both the OK confirmation and the query"));
    interface.println(F("  reply print it human-readable and zero-padded instead:"));
    interface.println(F("  \"2026-09-23, 14:30:00\". Both reply ERROR if no RTC is"));
    interface.println(F("  wired up on the I2C bus; query also ERRORs if the RTC"));
    interface.println(F("  reports it lost power (battery/supercap depleted) - set"));
    interface.println(F("  the time again to clear that. The RTC chip itself is the"));
    interface.println(F("  persistent store - nothing here is also mirrored into"));
    interface.println(F("  EEPROM."));
    interface.println();

    interface.println(F("SYSTem:BEEP <ON|OFF>"));
    interface.println(F("SYSTem:BEEP?"));
    interface.println(F("  Enables/disables every acoustic signal: start-up chirp,"));
    interface.println(F("  key clicks, log-clear and settings confirmations, the"));
    interface.println(F("  limit alarm and the marker double beep. Default ON."));
    interface.println(F("  Persists across power cycles."));
    interface.println();

    interface.println(F("SYSTem:COMM <UART|USB>"));
    interface.println(F("SYSTem:COMM?"));
    interface.println(F("  Which physical port the SCPI interface itself talks"));
    interface.println(F("  on: the dedicated second UART, or the same USB/debug"));
    interface.println(F("  Serial port used for setup()'s startup banner and"));
    interface.println(F("  SYSTem:DEBUG's trace. Takes effect immediately, no"));
    interface.println(F("  reboot needed - this reply itself still goes out on"));
    interface.println(F("  whichever port the command arrived on. Default UART."));
    interface.println(F("  Persists across power cycles."));
    interface.println();

    interface.println(F("SYSTem:LANGuage <CZ|EN>"));
    interface.println(F("SYSTem:LANGuage?"));
    interface.println(F("  Language of the help screens (REL held 1 s) and of the"));
    interface.println(F("  graph settings screen on the display: CZ = Czech"));
    interface.println(F("  (without diacritics), EN = English. This SCPI help"));
    interface.println(F("  stays English. Default CZ. Persists across power cycles."));
    interface.println();

    interface.println(F("SYSTem:DIAGnostic?"));
    interface.println(F("  Bring-up figures, one \"name: value\" line each: uptime and"));
    interface.println(F("  reset reason, internal RAM and PSRAM (free now, lowest free"));
    interface.println(F("  since boot, largest free block), the lowest free stack of"));
    interface.println(F("  every task (bytes) next to its configured size, render /"));
    interface.println(F("  blit / UI loop times (last, max, microseconds), the last and"));
    interface.println(F("  longest graph build and statistics job (milliseconds), log"));
    interface.println(F("  fill and covered time, dropped debug messages."));
    interface.println(F("SYSTem:DIAGnostic:CLEar"));
    interface.println(F("  Zeroes the maxima (render/blit/UI loop, graph build,"));
    interface.println(F("  statistics job) - e.g. before a test. Stack and heap"));
    interface.println(F("  low-water marks cannot be reset (FreeRTOS / heap)."));
    interface.println();

    interface.println(F("--- Graph screen (RCL on the display) ---"));
    interface.println(F("GRAPh:INTerval <10S|30S|1M|5M|15M|1H|6H|24H|FULL> / ?"));
    interface.println(F("  Time window of the graph, ending now (FULL = the whole"));
    interface.println(F("  log). A number without a unit is taken as seconds."));
    interface.println(F("GRAPh:YAXis <AUTO|ZERO|SUPP> / ?"));
    interface.println(F("  AUTO = lowest..highest value of the window, ZERO = the"));
    interface.println(F("  axis always includes 0, SUPP = suppressed zero: centre"));
    interface.println(F("  = newest value when chosen, +- GRAPh:SPAN (+10 % reserve)."));
    interface.println(F("GRAPh:TRACe <BOTH|AVG|BAND> / ?"));
    interface.println(F("  BOTH = MIN/MAX band + average line, AVG, BAND."));
    interface.println(F("GRAPh:SPAN <10|20|50|100|200|500|1000|2000|5000|10000> / ?"));
    interface.println(F("  SUPP span, +- counts of the last displayed digit."));
    interface.println(F("  All four persist at once (front-panel changes deferred)."));
    interface.println();

    interface.println(F("--- Limit alarm (dB held on the display) ---"));
    interface.println(F("CALCulate:LIMit:UPPer <value> [<function>]"));
    interface.println(F("CALCulate:LIMit:UPPer? [<function>]"));
    interface.println(F("CALCulate:LIMit:LOWer <value> [<function>]"));
    interface.println(F("CALCulate:LIMit:LOWer? [<function>]"));
    interface.println(F("  Limit in BASE units (V, A, Ohm, dB), e.g. 1.2 or 15E-3;"));
    interface.println(F("  NONE clears it. Query: scientific notation or NONE."));
    interface.println(F("CALCulate:LIMit:STATe <OFF|ON|UPPer|LOWer|BOTH> [<function>]"));
    interface.println(F("CALCulate:LIMit:STATe? [<function>]"));
    interface.println(F("  Which limits are watched. ON = the mode used last."));
    interface.println(F("  Always OFF after power-up; limits and the last mode persist."));
    interface.println(F("  <function>: DCV, ACV, DCI, ACI, OHM or DB - default: the"));
    interface.println(F("  function being measured now. Value and function may be"));
    interface.println(F("  separated by a space or a comma."));
    interface.println(F("CALCulate:LIMit:FAIL?"));
    interface.println(F("  1 if the newest reading is above the upper or below the"));
    interface.println(F("  lower limit (OL counts as above, -OL as below), else 0."));
    interface.println(F("  Compared in counts of the last displayed digit; with REL"));
    interface.println(F("  on the K197 the relative value is compared."));
    interface.println(F("CALCulate:LIMit:SIGNal <BEEP|ARROW> / ?"));
    interface.println(F("  BEEP = intermittent beep + blinking triangle above the"));
    interface.println(F("  minus sign, ARROW = triangle only (SYSTem:BEEP OFF also"));
    interface.println(F("  silences the beep)."));
    interface.println();

    interface.println(F("--- Reading log ---"));
    interface.println(F("Records are numbered 1 (oldest) .. LOG:POINts? (newest)."));
    interface.println(F("Every LOG:* command below takes the same optional"));
    interface.println(F("parameters, space-separated (not comma):"));
    interface.println(F("  [<function>] [<first> <last>]"));
    interface.println();
    interface.println(F("<function>, if given, must be the FIRST token: DCV, ACV,"));
    interface.println(F("DCI, ACI, OHM, or DB (case-insensitive) - restricts to"));
    interface.println(F("matching records only; omit it to match every function."));
    interface.println();
    interface.println(F("<first> <last>, both optional:"));
    interface.println(F("  no numbers at all       -> the whole log"));
    interface.println(F("  0 on either side        -> \"no bound\" that side -"));
    interface.println(F("                             first=0: from the oldest"));
    interface.println(F("                             last=0:  to the newest"));
    interface.println(F("  one positive number      -> just that one record"));
    interface.println(F("  first < 0 (tail mode)     -> Unix tail-style:"));
    interface.println(F("    -N          the last N records"));
    interface.println(F("    -N <last>   the N records immediately before <last>"));
    interface.println(F("Out-of-range numbers are clamped, never an error."));
    interface.println(F("Examples: \"LOG:DATA? DCV\", \"LOG:DATA? OHM 10 50\","));
    interface.println(F("          \"LOG:DATA? -50\", \"LOG:DATA? -50 200\""));
    interface.println();

    interface.println(F("LOG:POINts? [<function>] [<first> <last>]"));
    interface.println(F("  Count of matching records; no parameters = total count."));
    interface.println();

    interface.println(F("LOG:CAPacity?"));
    interface.println(F("  Total ring buffer capacity in records, sized from free"));
    interface.println(F("  PSRAM at boot - not a fixed number."));
    interface.println();

    interface.println(F("LOG:CLEar"));
    interface.println(F("  Empties the whole log. No partial/filtered clear."));
    interface.println();

    interface.println(F("LOG:DATA? [<function>] [<first> <last>]"));
    interface.println(F("  CSV: a header row, then one row per matching record:"));
    interface.println(F("    record,timestamp,status,function,value,indicators"));
    interface.println(F("  e.g. \"12,2026-10-10 14:03:07.42,N,DCV,+1.23456E+0,AUTO\""));
    interface.println(F("  timestamp: DS3231 RTC time of the reading (10 ms),"));
    interface.println(F("  NO-RTC-TIME for readings made before the RTC was synced."));
    interface.println(F("  Record numbers are always the position in the WHOLE"));
    interface.println(F("  log - a function filter skips non-matching rows, it"));
    interface.println(F("  never renumbers the ones that do match."));
    interface.println(F("  status: N=normal, O=overload, Z=REL active."));
    interface.println(F("  indicators: space-separated AUTO/STO/RCL/REM/BAT, only"));
    interface.println(F("  whichever are set (AC/REL/MINUS aren't repeated here,"));
    interface.println(F("  they're already reflected in status/the value's sign);"));
    interface.println(F("  TAG = a marker was set on this record; SETTLE = one of the"));
    interface.print(F("  first "));
    interface.print(CONFIG_DATALOG_SETTLE_READINGS);
    interface.println(F(" readings after a change of the function (left"));
    interface.println(F("  out of the statistics, the graph and the alarm)."));
    interface.println();

    interface.println(F("LOG:MARKer"));
    interface.println(F("  Puts a marker (TAG) on the newest record - like REL + dB"));
    interface.println(F("  together on the display or MEASure:TRIGger MARK."));
    interface.println(F("LOG:MARKer? [<function>] [<first> <last>]"));
    interface.println(F("  The same CSV as LOG:DATA?, only the marked records."));
    interface.println();

    interface.println(F("LOG:MIN? / LOG:MAX? / LOG:AVERage? / LOG:MEDian?"));
    interface.println(F("       [<function>] [<first> <last>]"));
    interface.println(F("  A single scientific-notation number over the range,"));
    interface.println(F("  e.g. \"+1.23456E+2\" - no status/function prefix."));
    interface.println(F("  Overload and SETTLE records are excluded from all four."));
    interface.println(F("  MEDian over a very large range (> 100,000 records)"));
    interface.println(F("  is computed from an evenly-spread sample rather than"));
    interface.println(F("  every record, to bound its memory use - MIN/MAX/"));
    interface.println(F("  AVERage are always exact, whatever the range size."));
    interface.println();

    interface.println(F("--- General ---"));
    interface.println(F("Line ending: any mix of CR and LF is accepted."));
    interface.println(F("Every SET command (SYSTem:*, LOG:CLEar) replies either"));
    interface.println(F("\"OK: <what was set>\" or \"ERROR: <why not>\" - a missing,"));
    interface.println(F("non-numeric, or out-of-range parameter is an ERROR, it no"));
    interface.println(F("longer just does nothing silently. A completely"));
    interface.println(F("unrecognized command also replies ERROR. One residual"));
    interface.println(F("gap: a command whose individual words are each valid but"));
    interface.println(F("combined in a way that was never registered (the old"));
    interface.println(F("SYST:READ/READ? collision was exactly this) can still go"));
    interface.println(F("silent - if a command you expect to work produces no"));
    interface.println(F("reply at all, try the exact spelling shown above instead"));
    interface.println(F("of an abbreviation."));
}

// Arduino's String::toInt() returns 0 both for a genuine "0" AND for
// anything it can't parse at all ("abc", "", "12x") - meaning every
// SET handler that used it directly would silently accept garbage as
// if it were a valid "0" (e.g. "SYSTem:AUTO abc" would have quietly
// enabled auto-send with a 0-second interval, no different from
// "SYSTem:AUTO 0"). This checks the string is non-empty and every
// character is an ASCII digit before trusting toInt()'s result, so
// non-numeric input gets treated as the parameter error it actually
// is instead of silently degrading into "0".
static bool parseStrictUInt(const String &param, long *outValue)
{
    if (param.length() == 0)
        return false;
    for (uint16_t i = 0; i < param.length(); i++)
    {
        if (!isDigit(param[i]))
            return false;
    }
    *outValue = param.toInt();
    return true;
}

static void HandleRead(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    printCurrentReading(interface);
}

// SYSTem:FORMat <0|1|2> - see printCurrentReading() above for what
// each value produces. Named FORMat rather than the more obvious
// READing: that name's natural 4-letter SCPI abbreviation ("READ")
// is already taken by the top-level READ? command, and the library's
// flat, tree-unaware token matching (see GetCommandCode_() in
// Vrekrer_scpi_parser_code.h) can't tell the two apart by context -
// "SYST:READ 1" silently matched READ?'s own token instead and did
// nothing (see HandleUnknownCommand()'s comment for the full
// mechanism). Simply changing the required/optional letter split
// (e.g. requiring "READI") would have worked too, but a 5-letter
// minimum abbreviation isn't real SCPI style (3 or 4 letters is the
// convention) - renaming to FORMat, a real SCPI mnemonic in its own
// right, gives a clean 4-letter "FORM" abbreviation with no collision
// at all, which is also a more accurate name for what this actually
// controls.
//
// Missing/non-numeric/out-of-range parameters print an ERROR line and
// change nothing; success prints an OK line echoing the value that
// was actually set (not just "OK" alone) - the author specifically
// asked for both, having run into the silent-failure behavior this
// used to have.
static void HandleSystemFormatSet(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    if (parameters.Size() < 1)
    {
        interface.println(F("ERROR: SYSTem:FORMat needs a value: 0, 1, or 2"));
        return;
    }
    String param(parameters[0]);
    param.trim();
    long format;
    if (!parseStrictUInt(param, &format)
        || format < SETTINGS_READING_RAW || format > SETTINGS_READING_NUMERIC)
    {
        interface.print(F("ERROR: SYSTem:FORMat value must be 0, 1, or 2, got \""));
        interface.print(param);
        interface.println('"');
        return;
    }
    settingsSetReadingFormat((uint8_t)format);
    interface.print(F("OK: SYSTem:FORMat "));
    interface.println(format);
}

static void HandleSystemFormatQuery(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    interface.println(settingsGetReadingFormat());
}

// SYSTem:AUTO <ON|OFF|0|NNN> - controls automatic/periodic sending of
// READ?'s answer without being asked:
//   OFF   no automatic sending - only in response to READ?
//   ON    resumes sending at whatever interval was last configured
//         (defaults to 0, i.e. every measurement, until an explicit
//         interval has ever been set)
//   0     send every DMM measurement
//   NNN   send every NNN seconds
// A bare number (0 or NNN) both sets the interval AND turns sending
// on, same as most instruments' streaming-rate commands - "ON" only
// re-enables the interval already configured, it never changes it.
// This ON-resumes-vs-number-sets-and-enables split isn't fully
// spelled out by "ON | OFF | 0 | NNN" on its own - it's the reading
// of that grammar this settled on, flagged here since a different
// interpretation (e.g. "ON" always meaning "every measurement", same
// as "0") is equally defensible from the request alone.
static void HandleSystemAutoSet(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    if (parameters.Size() < 1)
    {
        interface.println(F("ERROR: SYSTem:AUTO needs a value: ON, OFF, 0, or NNN"));
        return;
    }
    String param(parameters[0]);
    param.trim();
    if (param.equalsIgnoreCase("OFF"))
    {
        settingsSetAuto(false, settingsGetAutoIntervalSeconds());
        interface.println(F("OK: SYSTem:AUTO OFF"));
        return;
    }
    if (param.equalsIgnoreCase("ON"))
    {
        settingsSetAuto(true, settingsGetAutoIntervalSeconds());
        interface.print(F("OK: SYSTem:AUTO ON (interval "));
        interface.print(settingsGetAutoIntervalSeconds());
        interface.println(')');
        return;
    }
    long seconds;
    if (!parseStrictUInt(param, &seconds) || seconds > 65535)
    {
        interface.print(F("ERROR: SYSTem:AUTO value must be ON, OFF, or 0..65535, got \""));
        interface.print(param);
        interface.println('"');
        return;
    }
    settingsSetAuto(true, (uint16_t)seconds);
    // Restart the interval timer from now rather than from whenever
    // the last (differently-timed) auto-send happened, so changing
    // NNN doesn't fire immediately if a send under the old interval
    // was already close to due.
    scpiLastSentMillis = millis();
    interface.print(F("OK: SYSTem:AUTO "));
    interface.println(seconds);
}

static void HandleSystemAutoQuery(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    if (!settingsGetAutoEnabled())
        interface.println("OFF");
    else
        interface.println(settingsGetAutoIntervalSeconds());
}

// MEASure:TRIGger <INT|EXT> (v13) - see scpiTriggerSource above.
// Accepts INT/INTernal and EXT/EXTernal, case-insensitive. Switching to
// EXT forgets edges seen before the switch, so only a trigger that comes
// after the command produces a send.
static void HandleMeasTriggerSet(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    if (parameters.Size() < 1)
    {
        interface.println(F("ERROR: MEASure:TRIGger needs a value: INT, EXT or MARK"));
        return;
    }
    String param(parameters[0]);
    param.trim();
    if (param.equalsIgnoreCase("INT") || param.equalsIgnoreCase("INTERNAL"))
    {
        scpiTriggerSource = SCPI_TRIGGER_INT;
        scpiLastSentFrameCounter = scpiFrameCounter; // AUTO resumes with the next frame, not with an old one
        scpiLastSentMillis = millis();
        interface.println(F("OK: MEASure:TRIGger INT"));
        return;
    }
    if (param.equalsIgnoreCase("EXT") || param.equalsIgnoreCase("EXTERNAL"))
    {
        // Order matters: the capture task only looks at these while the
        // source is EXT, so they are set before the source flips.
        scpiExtTrigSeen = triggerIoExternalCount();
        scpiExtTrigSent = scpiExtTrigFrames;
        scpiTriggerSource = SCPI_TRIGGER_EXT;
        interface.println(F("OK: MEASure:TRIGger EXT"));
        return;
    }
    if (param.equalsIgnoreCase("MARK") || param.equalsIgnoreCase("MARKER"))
    {
        triggerIoKeySync(); // edges from before do not count
        scpiTriggerSource = SCPI_TRIGGER_MARK;
        scpiLastSentFrameCounter = scpiFrameCounter; // AUTO as with INT, from the next frame
        scpiLastSentMillis = millis();
        interface.println(F("OK: MEASure:TRIGger MARK"));
        return;
    }
    interface.print(F("ERROR: MEASure:TRIGger value must be INT, EXT or MARK, got \""));
    interface.print(param);
    interface.println('"');
}

static void HandleMeasTriggerQuery(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    interface.println(scpiTriggerSource == SCPI_TRIGGER_EXT ? "EXT" : (scpiTriggerSource == SCPI_TRIGGER_MARK ? "MARK" : "INT"));
}

// SYSTem:THEMe <0..themeCount-1> - see themes[] in display.h. Applies
// immediately (setTheme() redraws right away) and persists.
static void HandleSystemThemeSet(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    if (parameters.Size() < 1)
    {
        interface.print(F("ERROR: SYSTem:THEMe needs a value: 0.."));
        interface.println(themeCount - 1);
        return;
    }
    String param(parameters[0]);
    param.trim();
    long theme;
    if (!parseStrictUInt(param, &theme) || theme >= themeCount)
    {
        interface.print(F("ERROR: SYSTem:THEMe value must be 0.."));
        interface.print(themeCount - 1);
        interface.print(F(", got \""));
        interface.print(param);
        interface.println('"');
        return;
    }
    setTheme((uint8_t)theme);
    settingsSetTheme((uint8_t)theme);
    interface.print(F("OK: SYSTem:THEMe "));
    interface.print(theme);
    interface.print(',');
    interface.println(currentThemeName());
}

static void HandleSystemThemeQuery(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    interface.print(settingsGetTheme());
    interface.print(',');
    interface.println(currentThemeName());
}

// SYSTem:YOFFset <n> / ? - see setDisplayYOffset() (display.h). Valid
// range is [0, getDisplayMaxYOffset()] - queried at request time
// rather than hardcoded here, since it depends on the actual panel's
// tft.height(), which only display.ino knows.
static void HandleSystemYOffsetSet(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    int16_t maxOffset = getDisplayMaxYOffset();
    if (parameters.Size() < 1)
    {
        interface.print(F("ERROR: SYSTem:YOFFset needs a value: 0.."));
        interface.println(maxOffset);
        return;
    }
    String param(parameters[0]);
    param.trim();
    long offset;
    if (!parseStrictUInt(param, &offset) || offset > maxOffset)
    {
        interface.print(F("ERROR: SYSTem:YOFFset value must be 0.."));
        interface.print(maxOffset);
        interface.print(F(", got \""));
        interface.print(param);
        interface.println('"');
        return;
    }
    setDisplayYOffset((int16_t)offset);
    settingsSetDisplayYOffset((int16_t)offset);
    interface.print(F("OK: SYSTem:YOFFset "));
    interface.println(offset);
}

static void HandleSystemYOffsetQuery(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    interface.println(getDisplayYOffset());
}

// SYSTem:DEBUG <ON|OFF> / ? - see the persistence comment above.
static void HandleSystemDebugSet(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    if (parameters.Size() < 1)
    {
        interface.println(F("ERROR: SYSTem:DEBUG needs a value: ON or OFF"));
        return;
    }
    String param(parameters[0]);
    param.trim();
    if (param.equalsIgnoreCase("ON"))
    {
        settingsSetDebugEnabled(true);
        interface.println(F("OK: SYSTem:DEBUG ON"));
    }
    else if (param.equalsIgnoreCase("OFF"))
    {
        settingsSetDebugEnabled(false);
        interface.println(F("OK: SYSTem:DEBUG OFF"));
    }
    else
    {
        interface.print(F("ERROR: SYSTem:DEBUG value must be ON or OFF, got \""));
        interface.print(param);
        interface.println('"');
    }
}

static void HandleSystemDebugQuery(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    interface.println(settingsGetDebugEnabled() ? "ON" : "OFF");
}

// v13: points the debug output (debuglog.h) at the port SYSTem:DEBUG:PORT
// selects. On the UART every line gets a "# " prefix, since SCPI replies
// share that port.
static void scpiApplyDebugPort()
{
    if (settingsGetDebugPort() == SETTINGS_COMM_UART)
    {
        debugLogSetOutput(&Serial1, true);
    }
    else
    {
        debugLogSetOutput(&Serial, false);
    }
}

// SYSTem:DEBUG:PORT <UART|USB> (v13) - where debug messages go: the frame
// trace of SYSTem:DEBUG ON and the always-on messages (boot report, DMM
// watchdog, errors). UART = the SCPI UART (CH340, keeps its USB link while
// the instrument is off, so the power-up messages are not missed), USB =
// the ESP32's own USB CDC port. Persisted; takes effect at once.
static void HandleSystemDebugPortSet(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    if (parameters.Size() < 1)
    {
        interface.println(F("ERROR: SYSTem:DEBUG:PORT needs a value: UART or USB"));
        return;
    }
    String param(parameters[0]);
    param.trim();
    uint8_t port;
    if (param.equalsIgnoreCase("UART"))
    {
        port = SETTINGS_COMM_UART;
    }
    else if (param.equalsIgnoreCase("USB"))
    {
        port = SETTINGS_COMM_USB;
    }
    else
    {
        interface.print(F("ERROR: SYSTem:DEBUG:PORT value must be UART or USB, got \""));
        interface.print(param);
        interface.println('"');
        return;
    }
    settingsSetDebugPort(port);
    scpiApplyDebugPort();
    interface.println(port == SETTINGS_COMM_UART ? F("OK: SYSTem:DEBUG:PORT UART") : F("OK: SYSTem:DEBUG:PORT USB"));
}

static void HandleSystemDebugPortQuery(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    interface.println(settingsGetDebugPort() == SETTINGS_COMM_UART ? "UART" : "USB");
}

// v13: collects what printReading() writes, so the SYSTem:DEBUG frame trace
// goes through the debug queue (and so to SYSTem:DEBUG:PORT) instead of
// being written to Serial directly from the capture task.
class ScpiLineCollector : public Stream
{
public:
    char text[DEBUGLOG_MESSAGE_CHARS + 1];
    size_t length = 0;
    ScpiLineCollector()
    {
        text[0] = '\0';
    }
    size_t write(uint8_t c) override
    {
        if (c == '\r' || c == '\n' || length >= DEBUGLOG_MESSAGE_CHARS)
        {
            return 1;
        }
        text[length++] = (char)c;
        text[length] = '\0';
        return 1;
    }
    int available() override
    {
        return 0;
    }
    int read() override
    {
        return -1;
    }
    int peek() override
    {
        return -1;
    }
};

static bool isLeapYear(uint16_t year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static uint8_t daysInMonth(uint16_t year, uint8_t month)
{
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && isLeapYear(year))
        return 29;
    return days[month - 1];
}

// Prints value zero-padded to 2 digits (5 -> "05", 23 -> "23") - used
// by HandleSystemTimeSet()/Query() below to format month/day/hour/
// minute/second as a human-readable timestamp. Every one of those
// fields fits in 0-99, so a fixed 2-digit pad is all this needs - the
// year is printed plain (always already 4 digits, 2000-2099, see
// rtc.h), no separate 4-digit version required.
static void print2(Stream &s, uint8_t value)
{
    if (value < 10)
        s.print('0');
    s.print(value);
}

// SYSTem:TIME <YYYY> <MM> <DD> <hh> <mm> <ss> / ? - see rtc.h.
// Space-separated, not comma-separated - deliberately, and not just a
// style choice: Vrekrer_scpi_parser auto-splits a command's parameter
// text into its SCPI_Parameters array only on ',' (confirmed from its
// actual source, SCPI_Parameters' constructor in
// Vrekrer_scpi_arrays_code.h - strtok(parameter, ",")), and that
// array has a small, fixed compile-time size (SCPI_ARRAY_SYZE) that
// six comma-separated values overran in practice (a first version of
// this command used commas and failed with exactly that). Six
// SPACE-separated values have no comma anywhere in them, so the
// library sees them as a single unsplit parameter string
// (parameters.Size() == 1) and never touches SCPI_ARRAY_SYZE at all -
// splitTokensN() below then does the actual splitting by hand, same
// technique this project's LOG:* commands already use for their own
// space-separated arguments (see the comment above that function).
// This sidesteps the library's limit entirely, for any number of
// fields, rather than just fitting under it for six - the general fix,
// not merely a workaround sized to today's six.
static void HandleSystemTimeSet(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    if (!rtcIsPresent())
    {
        interface.println(F("ERROR: SYSTem:TIME - no RTC detected on the I2C bus"));
        return;
    }

    String tokens[6];
    if (splitTokensN(parameters, tokens, 6) != 6)
    {
        interface.println(F("ERROR: SYSTem:TIME needs 6 space-separated values: YYYY MM DD hh mm ss"));
        return;
    }

    long year = tokens[0].toInt();
    long month = tokens[1].toInt();
    long day = tokens[2].toInt();
    long hour = tokens[3].toInt();
    long minute = tokens[4].toInt();
    long second = tokens[5].toInt();

    if (year < 2000 || year > 2099)
    {
        interface.print(F("ERROR: SYSTem:TIME year must be 2000-2099, got "));
        interface.println(year);
        return;
    }
    if (month < 1 || month > 12)
    {
        interface.print(F("ERROR: SYSTem:TIME month must be 1-12, got "));
        interface.println(month);
        return;
    }
    if (day < 1 || day > daysInMonth((uint16_t)year, (uint8_t)month))
    {
        interface.print(F("ERROR: SYSTem:TIME day must be 1-"));
        interface.print(daysInMonth((uint16_t)year, (uint8_t)month));
        interface.print(F(" for that month/year, got "));
        interface.println(day);
        return;
    }
    if (hour < 0 || hour > 23)
    {
        interface.print(F("ERROR: SYSTem:TIME hour must be 0-23, got "));
        interface.println(hour);
        return;
    }
    if (minute < 0 || minute > 59)
    {
        interface.print(F("ERROR: SYSTem:TIME minute must be 0-59, got "));
        interface.println(minute);
        return;
    }
    if (second < 0 || second > 59)
    {
        interface.print(F("ERROR: SYSTem:TIME second must be 0-59, got "));
        interface.println(second);
        return;
    }

    if (!rtcSetDateTime((uint16_t)year, (uint8_t)month, (uint8_t)day,
                         (uint8_t)hour, (uint8_t)minute, (uint8_t)second))
    {
        interface.println(F("ERROR: SYSTem:TIME - RTC write failed"));
        return;
    }

    interface.print(F("OK: SYSTem:TIME "));
    interface.print(year);
    interface.print('-');
    print2(interface, (uint8_t)month);
    interface.print('-');
    print2(interface, (uint8_t)day);
    interface.print(F(", "));
    print2(interface, (uint8_t)hour);
    interface.print(':');
    print2(interface, (uint8_t)minute);
    interface.print(':');
    print2(interface, (uint8_t)second);
    interface.println();
}

// Query has no input-parameter-count concern (it's SCPI_ARRAY_SYZE on
// the way IN that this command was designed around, not on the way
// out) - replies in the same human-readable YYYY-MM-DD, hh:mm:ss
// format as HandleSystemTimeSet()'s own OK confirmation above, zero-
// padded via print2() (e.g. "2026-09-23, 08:05:00", not "2026-9-23,
// 8:5:0"), rather than the bare comma-separated numbers most other
// queries in this file use - a timestamp is read by a person far more
// often than machine-parsed, so this favors that over easy splitting.
static void HandleSystemTimeQuery(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    if (!rtcIsPresent())
    {
        interface.println(F("ERROR: SYSTem:TIME? - no RTC detected on the I2C bus"));
        return;
    }
    if (rtcHasLostPower())
    {
        interface.println(F("ERROR: SYSTem:TIME? - RTC lost power, time is not valid - set it with SYSTem:TIME"));
        return;
    }

    uint16_t year;
    uint8_t month, day, hour, minute, second;
    if (!rtcGetDateTime(&year, &month, &day, &hour, &minute, &second))
    {
        interface.println(F("ERROR: SYSTem:TIME? - RTC read failed"));
        return;
    }

    interface.print(year);
    interface.print('-');
    print2(interface, month);
    interface.print('-');
    print2(interface, day);
    interface.print(F(", "));
    print2(interface, hour);
    interface.print(':');
    print2(interface, minute);
    interface.print(':');
    print2(interface, second);
    interface.println();
}

// SYSTem:BEEP <ON|OFF> / ? - see the persistence comment above and
// beeper.h. Same shape as HandleSystemDebugSet()/Query() above, on
// purpose - copy that pair's pattern for any future SYSTem:<ON|OFF>
// setting rather than reinventing it a third way.
static void HandleSystemBeepSet(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    if (parameters.Size() < 1)
    {
        interface.println(F("ERROR: SYSTem:BEEP needs a value: ON or OFF"));
        return;
    }
    String param(parameters[0]);
    param.trim();
    if (param.equalsIgnoreCase("ON"))
    {
        settingsSetBeepEnabled(true);
        interface.println(F("OK: SYSTem:BEEP ON"));
    }
    else if (param.equalsIgnoreCase("OFF"))
    {
        settingsSetBeepEnabled(false);
        interface.println(F("OK: SYSTem:BEEP OFF"));
    }
    else
    {
        interface.print(F("ERROR: SYSTem:BEEP value must be ON or OFF, got \""));
        interface.print(param);
        interface.println('"');
    }
}

static void HandleSystemBeepQuery(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    interface.println(settingsGetBeepEnabled() ? "ON" : "OFF");
}

// SYSTem:COMM <UART|USB> / ? - see scpiStream's own comment near the
// top of this file. Same shape as HandleSystemDebugSet()/Query()
// above, except the SET side also has to flip the live scpiStream
// pointer, not just the persisted setting - everything else here
// (serialOutProcess() below) reads and writes through *scpiStream, so
// that one assignment is what actually moves the interface to the
// new port. `interface` (this call's own reply target) stays bound
// to whichever port the command arrived on, unaffected by that
// reassignment - see serialOutProcess()'s call to scpiParser.Execute()
// for why.
static void HandleSystemCommSet(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    if (parameters.Size() < 1)
    {
        interface.println(F("ERROR: SYSTem:COMM needs a value: UART or USB"));
        return;
    }
    String param(parameters[0]);
    param.trim();
    if (param.equalsIgnoreCase("UART"))
    {
        settingsSetCommPort(SETTINGS_COMM_UART);
        scpiStream = &Serial1;
        interface.println(F("OK: SYSTem:COMM UART"));
    }
    else if (param.equalsIgnoreCase("USB"))
    {
        settingsSetCommPort(SETTINGS_COMM_USB);
        scpiStream = &Serial;
        interface.println(F("OK: SYSTem:COMM USB"));
    }
    else
    {
        interface.print(F("ERROR: SYSTem:COMM value must be UART or USB, got \""));
        interface.print(param);
        interface.println('"');
    }
}

static void HandleSystemCommQuery(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    interface.println(settingsGetCommPort() == SETTINGS_COMM_USB ? "USB" : "UART");
}

// SYSTem:LANGuage <CZ|EN> (v15) - language of the help screens and of the
// graph settings screen (help_text.h: helpLanguageNames[], HELP_LANG_*).
// Saved at once; a help screen already open keeps its language until it is
// opened again, the graph settings follow at their next redraw. "CS" (the
// ISO 639 code of Czech) is accepted as well as "CZ".
static void HandleSystemLanguageSet(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    if (parameters.Size() < 1)
    {
        interface.println(F("ERROR: SYSTem:LANGuage needs a value: CZ or EN"));
        return;
    }
    String param(parameters[0]);
    param.trim();
    int language = -1;
    for (uint8_t i = 0; i < HELP_LANG_COUNT; i++)
    {
        if (param.equalsIgnoreCase(helpLanguageNames[i]))
        {
            language = i;
        }
    }
    if (language < 0 && param.equalsIgnoreCase("CS"))
    {
        language = HELP_LANG_CZ;
    }
    if (language < 0)
    {
        interface.print(F("ERROR: SYSTem:LANGuage value must be CZ or EN, got \""));
        interface.print(param);
        interface.println('"');
        return;
    }
    settingsSetLanguage((uint8_t)language);
    interface.print(F("OK: SYSTem:LANGuage "));
    interface.println(helpLanguageNames[language]);
}

// ---- SYSTem:DIAGnostic? (v16) ----
// Read-only bring-up figures from the other tasks: plain 32-bit values and
// FreeRTOS/heap queries that are safe from any task - at worst one update
// stale. Nothing here takes a lock the capture task waits for, except the two
// single-record reads of the log (datalogReadSerial(), microseconds).

static void scpiDiagStack(Stream &interface, const char *name, TaskHandle_t task, uint32_t configuredBytes)
{
    interface.print(F("stack "));
    interface.print(name);
    interface.print(F(": "));
    if (task == NULL)
    {
        interface.println(F("(no task)"));
        return;
    }
    // ESP-IDF's FreeRTOS counts stack in bytes (StackType_t is uint8_t), so this is already bytes.
    interface.print((uint32_t)uxTaskGetStackHighWaterMark(task));
    interface.print(F(" B free (lowest), of "));
    interface.print(configuredBytes);
    interface.println(F(" B"));
}

static void scpiDiagHeap(Stream &interface, const char *name, uint32_t caps)
{
    interface.print(name);
    interface.print(F(": free "));
    interface.print((uint32_t)heap_caps_get_free_size(caps));
    interface.print(F(" B, lowest free "));
    interface.print((uint32_t)heap_caps_get_minimum_free_size(caps));
    interface.print(F(" B, largest block "));
    interface.print((uint32_t)heap_caps_get_largest_free_block(caps));
    interface.println(F(" B"));
}

// Timestamp (seconds since 2000) of the record with `serial`, 0 if none / not stamped.
static uint32_t scpiDiagRecordTime(uint32_t serial)
{
    LogRecord rec;
    uint32_t s = serial;
    if (datalogReadSerial(&s, &rec, 1, NULL) != 1)
    {
        return 0;
    }
    return rec.timestampSeconds2000;
}

static void HandleSystemDiagQuery(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    suppressUnsolicitedOutput = true; // one clean block (like the LOG:* replies)

    interface.print(F("firmware: "));
    interface.println(SCPI_IDN_FIRMWARE);
    interface.print(F("uptime: "));
    interface.print((uint32_t)(millis() / 1000UL));
    interface.print(F(" s, reset reason "));
    interface.println(bootResetReasonName());

    scpiDiagHeap(interface, "internal RAM", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    scpiDiagHeap(interface, "PSRAM", MALLOC_CAP_SPIRAM);

    scpiDiagStack(interface, "capture (core 0)", mainCaptureTaskHandle, CONFIG_CORE0_TASK_STACK_BYTES);
    scpiDiagStack(interface, "blit (core 0)", displayGetBlitTask(), CONFIG_DISPLAY_BLIT_TASK_STACK_BYTES);
    scpiDiagStack(interface, "buttons (core 1)", buttonsGetTask(), CONFIG_BUTTONS_TASK_STACK_BYTES);
    scpiDiagStack(interface, "render/UI (core 1)", displayGetRenderTask(), CONFIG_DISPLAY_RENDER_TASK_STACK_BYTES);
    scpiDiagStack(interface, "comm/SCPI (core 1)", mainCommTaskHandle, CONFIG_CORE1_TASK_STACK_BYTES);

    DisplayStats ds = displayGetStats();
    interface.print(F("render: last "));
    interface.print(ds.renderLastUs);
    interface.print(F(" us, max "));
    interface.print(ds.renderMaxUs);
    interface.print(F(" us, count "));
    interface.print(ds.renderCount);
    interface.print(F(", waited for blit "));
    interface.println(ds.renderWaitedForBlit);
    interface.print(F("blit: last "));
    interface.print(ds.blitLastUs);
    interface.print(F(" us, max "));
    interface.print(ds.blitMaxUs);
    interface.print(F(" us, count "));
    interface.println(ds.blitCount);
    interface.print(F("UI loop pass: max "));
    interface.print(ds.uiLoopMaxUs);
    interface.println(F(" us"));

    GraphDebug gd = graphGetDebug();
    interface.print(F("graph build: last "));
    interface.print(gd.lastBuildMs);
    interface.print(F(" ms ("));
    interface.print(gd.lastBuildRecords);
    interface.print(F(" records), max "));
    interface.print(gd.maxBuildMs);
    interface.print(F(" ms, window "));
    interface.print(gd.spanSeconds);
    interface.print(F(" s, rebuilds "));
    interface.println(gd.rebuilds);
    uint32_t statsLast = 0, statsMax = 0, statsRecords = 0;
    uiGetStatsTiming(&statsLast, &statsMax, &statsRecords);
    interface.print(F("statistics job: last "));
    interface.print(statsLast);
    interface.print(F(" ms ("));
    interface.print(statsRecords);
    interface.print(F(" records), max "));
    interface.print(statsMax);
    interface.println(F(" ms"));

    uint32_t count = datalogCount();
    uint32_t capacity = datalogCapacity();
    interface.print(F("log: "));
    interface.print(count);
    interface.print(F(" of "));
    interface.print(capacity);
    interface.print(F(" records ("));
    interface.print((uint32_t)sizeof(LogRecord));
    interface.print(F(" B each)"));
    uint32_t end = datalogEndSerial(NULL);
    uint32_t oldest = datalogFirstSerialAtOrAfter(1);
    uint32_t t0 = (oldest < end) ? scpiDiagRecordTime(oldest) : 0;
    uint32_t t1 = (end > 0) ? scpiDiagRecordTime(end - 1) : 0;
    if (t0 != 0 && t1 >= t0 && count > 1)
    {
        double hours = (double)(t1 - t0) / 3600.0;
        interface.print(F(", covers "));
        interface.print(hours, 2);
        interface.print(F(" h"));
        if (t1 > t0 && count < capacity)
        {
            // full log at the rate seen so far
            double perHour = (double)(end - oldest) / hours;
            interface.print(F(", full at this rate: "));
            interface.print((double)capacity / perHour, 1);
            interface.print(F(" h"));
        }
    }
    interface.println();
    interface.print(F("debug queue: dropped "));
    interface.println(debugLogDropped());
    // (suppressUnsolicitedOutput is one-shot: serialOutProcess() clears it after this pass)
}

static void HandleSystemDiagClear(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    displayResetStatsMax();
    graphDiagClear();
    uiDiagClear();
    interface.println(F("OK: SYSTem:DIAGnostic:CLEar (maxima zeroed)"));
}

// ---- GRAPh:* (v15) ----
// The graph settings as SCPI. Saved at once with settingsSetGraph(); the
// graph itself (render/UI task) adopts them on its next pass, so this task
// never touches the graph's own state.

// "10S" / "1M" / "1H" / "FULL" for interval `index`.
static void scpiPrintGraphInterval(Stream &interface, uint8_t index)
{
    uint32_t seconds = graphIntervalSecondsAt(index);
    if (seconds == 0)
    {
        interface.println(F("FULL"));
    }
    else if (seconds % 3600 == 0)
    {
        interface.print(seconds / 3600);
        interface.println('H');
    }
    else if (seconds % 60 == 0)
    {
        interface.print(seconds / 60);
        interface.println('M');
    }
    else
    {
        interface.print(seconds);
        interface.println('S');
    }
}

// Graph interval index of "FULL" / "<n>[S|M|H]" (plain number = seconds), -1 if there is none.
static int scpiGraphIntervalFromText(String text)
{
    text.trim();
    text.toUpperCase();
    if (text.equals("FULL"))
    {
        return GRAPH_INTERVAL_FULL;
    }
    uint32_t multiplier = 1;
    if (text.endsWith("S") || text.endsWith("M") || text.endsWith("H"))
    {
        char unit = text.charAt(text.length() - 1);
        multiplier = (unit == 'H') ? 3600 : ((unit == 'M') ? 60 : 1);
        text = text.substring(0, text.length() - 1);
    }
    long value = text.toInt();
    if (value <= 0)
    {
        return -1;
    }
    uint32_t seconds = (uint32_t)value * multiplier;
    for (uint8_t i = 0; i < GRAPH_INTERVAL_COUNT; i++)
    {
        if (graphIntervalSecondsAt(i) == seconds)
        {
            return i;
        }
    }
    return -1;
}

static void HandleGraphIntervalSet(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    int index = (parameters.Size() < 1) ? -1 : scpiGraphIntervalFromText(String(parameters[0]));
    if (index < 0)
    {
        interface.println(F("ERROR: GRAPh:INTerval value must be 10S, 30S, 1M, 5M, 15M, 1H, 6H, 24H or FULL"));
        return;
    }
    settingsSetGraph((uint8_t)index, settingsGetGraphYMode(), settingsGetGraphTrace(), settingsGetGraphSpan());
    interface.print(F("OK: GRAPh:INTerval "));
    scpiPrintGraphInterval(interface, (uint8_t)index);
}

static void HandleGraphIntervalQuery(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    scpiPrintGraphInterval(interface, settingsGetGraphInterval());
}

// Index of `text` in names[count] (case-insensitive), -1 if none.
static int scpiFindName(SCPI_P parameters, const char *const *names, uint8_t count)
{
    if (parameters.Size() < 1)
    {
        return -1;
    }
    String text(parameters[0]);
    text.trim();
    for (uint8_t i = 0; i < count; i++)
    {
        if (text.equalsIgnoreCase(names[i]))
        {
            return i;
        }
    }
    return -1;
}

static const char *const scpiGraphYModeNames[GRAPH_YMODE_COUNT] = {"AUTO", "ZERO", "SUPP"};
static const char *const scpiGraphTraceNames[GRAPH_TRACE_COUNT] = {"BOTH", "AVG", "BAND"};

static void HandleGraphYAxisSet(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    int mode = scpiFindName(parameters, scpiGraphYModeNames, GRAPH_YMODE_COUNT);
    if (mode < 0)
    {
        interface.println(F("ERROR: GRAPh:YAXis value must be AUTO, ZERO or SUPP"));
        return;
    }
    settingsSetGraph(settingsGetGraphInterval(), (uint8_t)mode, settingsGetGraphTrace(), settingsGetGraphSpan());
    interface.print(F("OK: GRAPh:YAXis "));
    interface.println(scpiGraphYModeNames[mode]);
}

static void HandleGraphYAxisQuery(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    uint8_t mode = settingsGetGraphYMode();
    interface.println(scpiGraphYModeNames[(mode < GRAPH_YMODE_COUNT) ? mode : 0]);
}

static void HandleGraphTraceSet(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    int trace = scpiFindName(parameters, scpiGraphTraceNames, GRAPH_TRACE_COUNT);
    if (trace < 0)
    {
        interface.println(F("ERROR: GRAPh:TRACe value must be BOTH, AVG or BAND"));
        return;
    }
    settingsSetGraph(settingsGetGraphInterval(), settingsGetGraphYMode(), (uint8_t)trace, settingsGetGraphSpan());
    interface.print(F("OK: GRAPh:TRACe "));
    interface.println(scpiGraphTraceNames[trace]);
}

static void HandleGraphTraceQuery(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    uint8_t trace = settingsGetGraphTrace();
    interface.println(scpiGraphTraceNames[(trace < GRAPH_TRACE_COUNT) ? trace : 0]);
}

static void HandleGraphSpanSet(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    int index = -1;
    if (parameters.Size() >= 1)
    {
        String text(parameters[0]);
        text.trim();
        long counts = text.toInt();
        for (uint8_t i = 0; i < GRAPH_SPAN_COUNT; i++)
        {
            if ((long)graphSpanCountsAt(i) == counts)
            {
                index = i;
            }
        }
    }
    if (index < 0)
    {
        interface.println(F("ERROR: GRAPh:SPAN value must be 10, 20, 50, 100, 200, 500, 1000, 2000, 5000 or 10000"));
        return;
    }
    settingsSetGraph(settingsGetGraphInterval(), settingsGetGraphYMode(), settingsGetGraphTrace(), (uint8_t)index);
    interface.print(F("OK: GRAPh:SPAN "));
    interface.println(graphSpanCountsAt((uint8_t)index));
}

static void HandleGraphSpanQuery(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    interface.println(graphSpanCountsAt(settingsGetGraphSpan()));
}

static void HandleSystemLanguageQuery(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    uint8_t language = settingsGetLanguage();
    interface.println(helpLanguageNames[(language < HELP_LANG_COUNT) ? language : 0]);
}

// The Vrekrer library only auto-splits parameters on ',' (confirmed
// from its actual source, SCPI_Parameters' constructor in
// Vrekrer_scpi_arrays_code.h - strtok(parameter, ",")). This
// project's LOG:* commands take an optional function name and/or two
// range numbers separated by spaces instead
// ("LOG:DATA? DCV 10 20"), per the author's own preferred syntax, so
// a bare space-separated group arrives as ONE unsplit string in
// parameters[0] - this splits that by hand.
//
// Takes `parameters` by value, not `const SCPI_P&` - the library's
// own handler signature (SCPI_caller_t) already passes it by value,
// and at least one installed copy of this library (Arduino Library
// Manager's, as opposed to the GitHub master branch used to check
// the CR/LF behavior elsewhere in this project) has Size()/
// operator[]() declared without `const`, which fails to compile
// against a const reference ("discards qualifiers"). Passing by value
// sidesteps the mismatch regardless of which copy of the library this
// ends up built against.
//
// Splits parameters[0] into up to maxTokens whitespace-separated
// tokens. Extra tokens beyond maxTokens are silently ignored, same
// "don't error on odd input" style as the rest of this project's SCPI
// handlers.
static uint8_t splitTokensN(SCPI_P parameters, String tokens[], uint8_t maxTokens)
{
    if (parameters.Size() < 1)
        return 0;
    String combined(parameters[0]);
    combined.trim();
    if (combined.length() == 0)
        return 0;

    uint8_t count = 0;
    int start = 0;
    int len = combined.length();
    while (count < maxTokens)
    {
        while (start < len && combined[start] == ' ')
            start++;
        if (start >= len)
            break;
        int end = combined.indexOf(' ', start);
        if (end < 0)
            end = len;
        tokens[count++] = combined.substring(start, end);
        start = end;
    }
    return count;
}

// 3-token convenience wrapper - see splitTokensN() above. Kept as its
// own name (rather than every caller writing splitTokensN(p, t, 3))
// since parseLogArgs() below reads better this way.
static uint8_t splitTokens(SCPI_P parameters, String tokens[3])
{
    return splitTokensN(parameters, tokens, 3);
}

// The six function names LOG:*'s optional filter argument accepts -
// deliberately the same strings keithleyFunctionPrefix()
// (reading_math.ino) can produce, compared case-insensitively and
// trimmed (so "dcv", "DCV", " OHM " etc. all work; the leading space
// keithleyFunctionPrefix() puts on " dB" for column-alignment
// purposes elsewhere doesn't need to be typed here).
static bool isFunctionToken(const String &tokenUpper)
{
    return tokenUpper == "DCV" || tokenUpper == "ACV" || tokenUpper == "DCI" ||
           tokenUpper == "ACI" || tokenUpper == "OHM" || tokenUpper == "DB";
}

// Shared by every LOG:* handler below: parses the optional
// "[<function>] [<first> <last>]" grammar (see datalog.h's function-
// filter comment) into a function filter string (empty = no filter,
// i.e. every function) and the (first, last) pair datalog.h's range
// numbering expects. A single bare POSITIVE number (no function, one
// range token) means "just that one record", same as before this
// session's filter addition. A single bare NEGATIVE number is a tail
// request instead ("LOG:DATA? -50" - see resolveRange() in
// datalog.ino) and is passed through as-is, `last` staying at its
// already-set default of 0 ("anchor = end") - only a positive lone
// number collapses first==last, since -50 meaning "record -50" would
// be meaningless and isn't what a bare negative number is for here.
// The function name, if given, is always the FIRST token -
// "LOG:DATA? 10 20 DCV" is not accepted, only "LOG:DATA? DCV 10 20".
static void parseLogArgs(SCPI_P parameters, String *functionFilter, long *first, long *last)
{
    String tokens[3];
    uint8_t n = splitTokens(parameters, tokens);

    *functionFilter = "";
    *first = 0;
    *last = 0;

    uint8_t numberStart = 0;
    if (n > 0)
    {
        String firstUpper = tokens[0];
        firstUpper.toUpperCase();
        if (isFunctionToken(firstUpper))
        {
            *functionFilter = firstUpper;
            numberStart = 1;
        }
    }

    uint8_t numbersFound = n - numberStart;
    if (numbersFound >= 1)
        *first = tokens[numberStart].toInt();
    if (numbersFound >= 2)
        *last = tokens[numberStart + 1].toInt();
    else if (numbersFound == 1 && *first > 0)
        *last = *first; // single positive number -> just that one record
    // else (negative, or the literal "0 records" edge case): *last
    // stays 0, its already-set default - correct for both "no bound"
    // (plain range mode) and "anchor = end" (tail mode).
}

// Every LOG:* handler sets suppressUnsolicitedOutput before printing
// anything - see that flag's own comment (near the top of the file)
// for why. Applied uniformly to all seven, not just LOG:DATA?, even
// though the single-line replies (POINts?/CLEar/MIN?/MAX?/AVERage?/
// MEDian?) are much less likely to actually collide with an
// auto-send line in practice - consistent behavior across the whole
// LOG:* family is simpler to reason about than only guarding the one
// command that's most likely to need it.
//
// LOG:POINts? [<function>] [<first> <last>] - unfiltered total record
// count if called with no parameters at all (unchanged from before
// this session), or a filtered/ranged count otherwise. Added as a
// small, cheap extension alongside the other four commands' new
// filter support - not explicitly requested, but directly useful for
// deciding what range to ask LOG:DATA? for once a function filter is
// in play.
static void HandleLogPoints(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    suppressUnsolicitedOutput = true;
    String functionFilter;
    long first, last;
    parseLogArgs(parameters, &functionFilter, &first, &last);
    interface.println(datalogCountFiltered(functionFilter.c_str(), first, last));
}

// LOG:CAPacity? - total ring buffer capacity in records, determined
// at boot from actually-free PSRAM (datalogBegin(), datalog.ino) -
// worth exposing over SCPI specifically because it's no longer a
// fixed number known ahead of time the way it used to be.
static void HandleLogCapacity(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    suppressUnsolicitedOutput = true;
    interface.println(datalogCapacity());
}

static void HandleLogClear(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    suppressUnsolicitedOutput = true;
    datalogClear();
    interface.println(F("OK: LOG:CLEar"));
}

// LOG:DATA? [<function>] [<first> <last>] - e.g. "LOG:DATA? DCV",
// "LOG:DATA? DCV 10 20", or the original "LOG:DATA? 10 20" with no
// function filter at all. Record numbers in the CSV output are always
// the original position in the whole (unfiltered) log, even when a
// function filter skips some of them - see datalog.h.
static void HandleLogData(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    suppressUnsolicitedOutput = true;
    String functionFilter;
    long first, last;
    parseLogArgs(parameters, &functionFilter, &first, &last);
    datalogPrintRange(interface, functionFilter.c_str(), first, last);
}

// v18 LOG:MARKer - a marker on the newest record.
static void HandleLogMarkerSet(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    suppressUnsolicitedOutput = true;
    if (markerAdd((uint32_t)millis(), MARKER_SOURCE_SCPI))
    {
        interface.print(F("OK: LOG:MARKer on record "));
        interface.println(datalogCount());
    }
    else
    {
        interface.println(F("ERROR: LOG:MARKer - the log has no record yet"));
    }
}

// v18 LOG:MARKer? [<function>] [<first> <last>] - the marked records, LOG:DATA? format.
static void HandleLogMarkerQuery(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    suppressUnsolicitedOutput = true;
    String functionFilter;
    long first, last;
    parseLogArgs(parameters, &functionFilter, &first, &last);
    datalogPrintMarked(interface, functionFilter.c_str(), first, last);
}

static void HandleLogMin(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    suppressUnsolicitedOutput = true;
    String functionFilter;
    long first, last;
    parseLogArgs(parameters, &functionFilter, &first, &last);
    datalogPrintMin(interface, functionFilter.c_str(), first, last);
}

static void HandleLogMax(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    suppressUnsolicitedOutput = true;
    String functionFilter;
    long first, last;
    parseLogArgs(parameters, &functionFilter, &first, &last);
    datalogPrintMax(interface, functionFilter.c_str(), first, last);
}

static void HandleLogAverage(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    suppressUnsolicitedOutput = true;
    String functionFilter;
    long first, last;
    parseLogArgs(parameters, &functionFilter, &first, &last);
    datalogPrintAverage(interface, functionFilter.c_str(), first, last);
}

static void HandleLogMedian(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    suppressUnsolicitedOutput = true;
    String functionFilter;
    long first, last;
    parseLogArgs(parameters, &functionFilter, &first, &last);
    datalogPrintMedian(interface, functionFilter.c_str(), first, last);
}

// ---- CALCulate:LIMit:* (v17, limit alarm - alarm.h) ----

// All parameter tokens, whether separated by commas (the library splits those)
// or by spaces: up to `maxTokens`.
static uint8_t scpiAllTokens(SCPI_P parameters, String tokens[], uint8_t maxTokens)
{
    uint8_t count = 0;
    for (uint8_t p = 0; p < parameters.Size() && count < maxTokens; p++)
    {
        String text(parameters[p]);
        text.trim();
        int start = 0;
        int len = text.length();
        while (count < maxTokens && start < len)
        {
            while (start < len && text[start] == ' ')
                start++;
            if (start >= len)
                break;
            int end = text.indexOf(' ', start);
            if (end < 0)
                end = len;
            tokens[count++] = text.substring(start, end);
            start = end;
        }
    }
    return count;
}

// Splits "<value> [<function>]" / "[<function>]": the function token (if any)
// goes to *function, the first other token to *value. Without a function token
// the function being measured is used; false (with an ERROR printed) if there is
// none.
static bool scpiAlarmArgs(SCPI_P parameters, Stream &interface, uint8_t *function, String *value)
{
    String tokens[3];
    uint8_t n = scpiAllTokens(parameters, tokens, 3);
    *function = ALARM_FUNCTION_NONE;
    *value = "";
    for (uint8_t i = 0; i < n; i++)
    {
        String upper = tokens[i];
        upper.toUpperCase();
        if (isFunctionToken(upper))
        {
            *function = alarmFunctionFromName(upper.c_str());
        }
        else if (value->length() == 0)
        {
            *value = tokens[i];
        }
    }
    if (*function == ALARM_FUNCTION_NONE)
    {
        *function = alarmGetLiveFunction();
    }
    if (*function == ALARM_FUNCTION_NONE)
    {
        interface.println(F("ERROR: no function is being measured - add one: DCV, ACV, DCI, ACI, OHM or DB"));
        return false;
    }
    return true;
}

// A number such as "1.2", "-0.5", "15E-3" - the whole text must be the number.
static bool parseStrictDouble(const String &text, double *out)
{
    if (text.length() == 0)
    {
        return false;
    }
    char *end = NULL;
    double value = strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !isfinite(value))
    {
        return false;
    }
    *out = value;
    return true;
}

static void scpiPrintAlarmLimit(Stream &interface, double value)
{
    if (isnan(value))
    {
        interface.println(F("NONE"));
        return;
    }
    keithleyPrintScientific(interface, value, false);
    interface.println();
}

static void scpiAlarmLimitSet(SCPI_P parameters, Stream &interface, bool upper)
{
    uint8_t function;
    String text;
    if (!scpiAlarmArgs(parameters, interface, &function, &text))
    {
        return;
    }
    double value = NAN;
    if (!text.equalsIgnoreCase("NONE") && !parseStrictDouble(text, &value))
    {
        interface.print(upper ? F("ERROR: CALCulate:LIMit:UPPer") : F("ERROR: CALCulate:LIMit:LOWer"));
        interface.println(F(" needs a number in base units (e.g. 1.2, -0.5, 15E-3) or NONE"));
        return;
    }
    alarmSetLimit(function, upper, value, true);
    interface.print(upper ? F("OK: CALCulate:LIMit:UPPer ") : F("OK: CALCulate:LIMit:LOWer "));
    interface.print(alarmFunctionName(function));
    interface.print(' ');
    scpiPrintAlarmLimit(interface, alarmGetLimit(function, upper));
}

static void scpiAlarmLimitQuery(SCPI_P parameters, Stream &interface, bool upper)
{
    uint8_t function;
    String text;
    if (scpiAlarmArgs(parameters, interface, &function, &text))
    {
        scpiPrintAlarmLimit(interface, alarmGetLimit(function, upper));
    }
}

static void HandleLimitUpperSet(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    scpiAlarmLimitSet(parameters, interface, true);
}

static void HandleLimitUpperQuery(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    scpiAlarmLimitQuery(parameters, interface, true);
}

static void HandleLimitLowerSet(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    scpiAlarmLimitSet(parameters, interface, false);
}

static void HandleLimitLowerQuery(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    scpiAlarmLimitQuery(parameters, interface, false);
}

static const char *const scpiAlarmModeNames[ALARM_MODE_COUNT] = {"OFF", "UPPER", "LOWER", "BOTH"};

static void HandleLimitStateSet(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    uint8_t function;
    String text;
    if (!scpiAlarmArgs(parameters, interface, &function, &text))
    {
        return;
    }
    text.toUpperCase();
    int mode = -1;
    if (text == "OFF" || text == "0")
        mode = ALARM_MODE_OFF;
    else if (text == "ON" || text == "1")
        mode = alarmGetStoredMode(function);
    else if (text == "UPP" || text == "UPPER")
        mode = ALARM_MODE_UPPER;
    else if (text == "LOW" || text == "LOWER")
        mode = ALARM_MODE_LOWER;
    else if (text == "BOTH")
        mode = ALARM_MODE_BOTH;
    if (mode < 0)
    {
        interface.println(F("ERROR: CALCulate:LIMit:STATe value must be OFF, ON, UPPer, LOWer or BOTH"));
        return;
    }
    alarmSetMode(function, (uint8_t)mode, true);
    interface.print(F("OK: CALCulate:LIMit:STATe "));
    interface.print(alarmFunctionName(function));
    interface.print(' ');
    interface.println(scpiAlarmModeNames[alarmGetMode(function)]);
}

static void HandleLimitStateQuery(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    uint8_t function;
    String text;
    if (scpiAlarmArgs(parameters, interface, &function, &text))
    {
        interface.println(scpiAlarmModeNames[alarmGetMode(function)]);
    }
}

static void HandleLimitFailQuery(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    uint8_t state = alarmGetState();
    interface.println((state == ALARM_STATE_HIGH || state == ALARM_STATE_LOW) ? 1 : 0);
}

static const char *const scpiAlarmSignalNames[ALARM_SIGNAL_COUNT] = {"BEEP", "ARROW"};

static void HandleLimitSignalSet(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    int signal = scpiFindName(parameters, scpiAlarmSignalNames, ALARM_SIGNAL_COUNT);
    if (signal < 0)
    {
        interface.println(F("ERROR: CALCulate:LIMit:SIGNal value must be BEEP or ARROW"));
        return;
    }
    alarmSetSignal((uint8_t)signal, true);
    interface.print(F("OK: CALCulate:LIMit:SIGNal "));
    interface.println(scpiAlarmSignalNames[signal]);
}

static void HandleLimitSignalQuery(SCPI_C commands, SCPI_P parameters, Stream &interface)
{
    uint8_t signal = alarmGetSignal();
    interface.println(scpiAlarmSignalNames[(signal < ALARM_SIGNAL_COUNT) ? signal : 0]);
}

void serialOutBegin()
{
    // Always begun now, regardless of which port SYSTem:COMM has
    // active - see scpiStream's own comment near the top of this
    // file for why (switching ports at runtime needs both ready).
    // Serial (USB/debug mode) is already begun elsewhere (setup(),
    // ESP32S3_K197.ino) - no .begin() call needed or wanted for it
    // here.
    Serial1.begin(SERIAL_OUT_BAUD, SERIAL_8N1, SERIAL_OUT_RX_PIN, SERIAL_OUT_TX_PIN);
    scpiApplyDebugPort(); // v13: debug messages to SYSTem:DEBUG:PORT (queued ones from earlier in setup() too)
    scpiStream = (settingsGetCommPort() == SETTINGS_COMM_USB) ? (Stream *)&Serial : (Stream *)&Serial1;

    scpiParser.RegisterCommand(F("*IDN?"), &HandleIdentify);
    scpiParser.RegisterCommand(F("READ?"), &HandleRead);
    // v13: MEASure? and FETCh? are plain aliases of READ? (compatibility
    // with programs written for other meters; there is no separate
    // "start a measurement" step - the K197 measures continuously).
    scpiParser.RegisterCommand(F("MEASure?"), &HandleRead);
    scpiParser.RegisterCommand(F("FETCh?"), &HandleRead);
    scpiParser.RegisterCommand(F("*HLP"), &HandleHelp);
    scpiParser.RegisterCommand(F("*HLP?"), &HandleHelp);
    scpiParser.RegisterCommand(F("HELP"), &HandleHelp);
    scpiParser.RegisterCommand(F("HELP?"), &HandleHelp);
    scpiParser.SetCommandTreeBase(F("*HLP"));
    scpiParser.RegisterCommand(F(":DETail"), &HandleHelpDetail);
    scpiParser.RegisterCommand(F(":DETail?"), &HandleHelpDetail);
    scpiParser.SetCommandTreeBase(F("HELP"));
    scpiParser.RegisterCommand(F(":DETail"), &HandleHelpDetail);
    scpiParser.RegisterCommand(F(":DETail?"), &HandleHelpDetail);
    scpiParser.SetErrorHandler(&HandleUnknownCommand);

    // v13: MEASure:TRIGger, and MEASure:AUTO as the new name of
    // SYSTem:AUTO (same handlers, same setting - SYSTem:AUTO stays for
    // compatibility).
    scpiParser.SetCommandTreeBase(F("MEASure"));
    scpiParser.RegisterCommand(F(":TRIGger"), &HandleMeasTriggerSet);
    scpiParser.RegisterCommand(F(":TRIGger?"), &HandleMeasTriggerQuery);
    scpiParser.RegisterCommand(F(":AUTO"), &HandleSystemAutoSet);
    scpiParser.RegisterCommand(F(":AUTO?"), &HandleSystemAutoQuery);

    scpiParser.SetCommandTreeBase(F("SYSTem"));
    // ":FORMat", not ":READing" - see the long comment above
    // HandleSystemFormatSet() for the full story of why this isn't
    // called READing.
    scpiParser.RegisterCommand(F(":FORMat"), &HandleSystemFormatSet);
    scpiParser.RegisterCommand(F(":FORMat?"), &HandleSystemFormatQuery);
    scpiParser.RegisterCommand(F(":AUTO"), &HandleSystemAutoSet);
    scpiParser.RegisterCommand(F(":AUTO?"), &HandleSystemAutoQuery);
    scpiParser.RegisterCommand(F(":THEMe"), &HandleSystemThemeSet);
    scpiParser.RegisterCommand(F(":THEMe?"), &HandleSystemThemeQuery);
    scpiParser.RegisterCommand(F(":YOFFset"), &HandleSystemYOffsetSet);
    scpiParser.RegisterCommand(F(":YOFFset?"), &HandleSystemYOffsetQuery);
    scpiParser.RegisterCommand(F(":DEBUG"), &HandleSystemDebugSet);
    scpiParser.RegisterCommand(F(":DEBUG?"), &HandleSystemDebugQuery);
    scpiParser.RegisterCommand(F(":TIME"), &HandleSystemTimeSet);
    scpiParser.RegisterCommand(F(":TIME?"), &HandleSystemTimeQuery);
    scpiParser.RegisterCommand(F(":BEEP"), &HandleSystemBeepSet);
    scpiParser.RegisterCommand(F(":BEEP?"), &HandleSystemBeepQuery);
    scpiParser.RegisterCommand(F(":DEBUG:PORT"), &HandleSystemDebugPortSet);
    scpiParser.RegisterCommand(F(":DEBUG:PORT?"), &HandleSystemDebugPortQuery);
    scpiParser.RegisterCommand(F(":COMM"), &HandleSystemCommSet);
    scpiParser.RegisterCommand(F(":COMM?"), &HandleSystemCommQuery);
    scpiParser.RegisterCommand(F(":LANGuage"), &HandleSystemLanguageSet); // v15
    scpiParser.RegisterCommand(F(":LANGuage?"), &HandleSystemLanguageQuery);
    scpiParser.RegisterCommand(F(":DIAGnostic?"), &HandleSystemDiagQuery); // v16
    scpiParser.RegisterCommand(F(":DIAGnostic:CLEar"), &HandleSystemDiagClear);

    // v15: graph settings (tokens GRAPh, INTerval, YAXis, TRACe, SPAN - none of them equals an existing token
    // or its short form, see the SYSTem:FORMat note on the parser's flat token pool)
    scpiParser.SetCommandTreeBase(F("GRAPh"));
    scpiParser.RegisterCommand(F(":INTerval"), &HandleGraphIntervalSet);
    scpiParser.RegisterCommand(F(":INTerval?"), &HandleGraphIntervalQuery);
    scpiParser.RegisterCommand(F(":YAXis"), &HandleGraphYAxisSet);
    scpiParser.RegisterCommand(F(":YAXis?"), &HandleGraphYAxisQuery);
    scpiParser.RegisterCommand(F(":TRACe"), &HandleGraphTraceSet);
    scpiParser.RegisterCommand(F(":TRACe?"), &HandleGraphTraceQuery);
    scpiParser.RegisterCommand(F(":SPAN"), &HandleGraphSpanSet);
    scpiParser.RegisterCommand(F(":SPAN?"), &HandleGraphSpanQuery);

    scpiParser.SetCommandTreeBase(F("CALCulate:LIMit")); // v17
    scpiParser.RegisterCommand(F(":UPPer"), &HandleLimitUpperSet);
    scpiParser.RegisterCommand(F(":UPPer?"), &HandleLimitUpperQuery);
    scpiParser.RegisterCommand(F(":LOWer"), &HandleLimitLowerSet);
    scpiParser.RegisterCommand(F(":LOWer?"), &HandleLimitLowerQuery);
    scpiParser.RegisterCommand(F(":STATe"), &HandleLimitStateSet);
    scpiParser.RegisterCommand(F(":STATe?"), &HandleLimitStateQuery);
    scpiParser.RegisterCommand(F(":FAIL?"), &HandleLimitFailQuery);
    scpiParser.RegisterCommand(F(":SIGNal"), &HandleLimitSignalSet);
    scpiParser.RegisterCommand(F(":SIGNal?"), &HandleLimitSignalQuery);

    scpiParser.SetCommandTreeBase(F("LOG"));
    scpiParser.RegisterCommand(F(":POINts?"), &HandleLogPoints);
    scpiParser.RegisterCommand(F(":CAPacity?"), &HandleLogCapacity);
    scpiParser.RegisterCommand(F(":CLEar"), &HandleLogClear);
    scpiParser.RegisterCommand(F(":DATA?"), &HandleLogData);
    scpiParser.RegisterCommand(F(":MARKer"), &HandleLogMarkerSet); // v18
    scpiParser.RegisterCommand(F(":MARKer?"), &HandleLogMarkerQuery);
    scpiParser.RegisterCommand(F(":MIN?"), &HandleLogMin);
    scpiParser.RegisterCommand(F(":MAX?"), &HandleLogMax);
    scpiParser.RegisterCommand(F(":AVERage?"), &HandleLogAverage);
    scpiParser.RegisterCommand(F(":MEDian?"), &HandleLogMedian);

    // settingsBegin() (main sketch's setup(), before this call) has
    // already loaded the persisted theme and Y offset - apply both
    // now that the TFT is up (displayBegin() already ran too).
    // readingFormat/auto are just read on demand from settings.h
    // elsewhere, nothing to "apply" for those at startup.
    setTheme(settingsGetTheme());
    setDisplayYOffset(settingsGetDisplayYOffset());
}

// True if SYSTem:DEBUG is ON and nothing is sitting half-typed in the
// line buffer - see the declaration comment in serial_out.h.
bool serialOutDebugTraceAllowed()
{
    return settingsGetDebugEnabled() && lineLength == 0;
}

void serialOutUpdateReading(const char digits[KEITHLEY_DIGIT_COUNT], int8_t dpPos,
                             uint16_t indicatorBits, uint8_t unit)
{
    for (uint8_t i = 0; i < KEITHLEY_DIGIT_COUNT; i++)
        scpiDigits[i] = digits[i];
    scpiDpPos = dpPos;
    scpiIndicatorBits = indicatorBits;
    scpiUnit = unit;
    scpiFrameCounter++;

    // v13 MEASure:TRIGger EXT: this is the first complete frame after any
    // trigger edge(s) not handled yet - mark it for one send.
    if (scpiTriggerSource == SCPI_TRIGGER_EXT)
    {
        uint32_t edges = triggerIoExternalCount();
        if (edges != scpiExtTrigSeen)
        {
            scpiExtTrigSeen = edges;
            scpiExtTrigFrames = scpiExtTrigFrames + 1; // single writer (this task); no ++ on a volatile (C++20)
        }
    }

    // Prints the decoded-reading trace - dmm_bridge.ino's
    // dmmApplyFrame() separately checks serialOutDebugTraceAllowed()
    // itself to print the raw captured frame bytes just before this
    // gets called, so with SYSTem:DEBUG ON a full pass looks like:
    // raw hex bytes, then what they decoded to.
    if (serialOutDebugTraceAllowed())
    {
        ScpiLineCollector line;
        printReading(line);
        debugLogf("%s", line.text);
    }
}

// Hand-rolled replacement for SCPI_Parser::ProcessInput(). The
// library's own line-buffering (GetMessage(), in
// Vrekrer_scpi_parser_code.h) ends a message with
// `strstr(msg_buffer_, term_chars) != NULL` - an EXACT substring
// match, not "any one of these characters ends the message" as
// originally assumed here before the library's actual source was
// available to check (see docs/DEVELOPMENT.md - that first attempt,
// passing term_chars="\r\n", would only have matched a literal CRLF
// pair; a bare "\n", a bare "\r", or "\n\r" would NOT have ended a
// message with it).
//
// This does the equivalent job by hand instead: read characters one
// at a time, treat EITHER '\r' or '\n' as ending the current message,
// and ignore a terminator character that arrives while the buffer is
// already empty - so any run of CR/LF in any order/combination
// ("\r\n", "\n\r", repeats, ...) collapses into ending just the one
// message that preceded it, never producing extra empty commands in
// between. Once a full line is assembled, it's handed to the
// library's own Execute(), so all the actual SCPI command parsing and
// dispatch still goes through the library - this only replaces its
// line-buffering step. (`lineBuffer`/`lineLength` themselves are
// declared up near the top of the file - see the comment there.)

// SYSTem:AUTO's periodic/every-measurement sending. Called once per
// serialOutProcess() call (i.e. once per loop() iteration), so it's
// checked often regardless of the DMM's own update rate. Defined
// before serialOutProcess() (which calls it) rather than relying on
// Arduino's auto-generated prototypes, same reasoning as the
// SCPI_C/SCPI_P include-ordering issue noted in
// ESP32S3_K197.ino - better not to depend on that mechanism
// again for something so easy to just order correctly by hand.
static void scpiAutoSendCheck()
{
    if (scpiTriggerSource == SCPI_TRIGGER_EXT)
    {
        // External trigger: one reading per triggered frame, AUTO suspended.
        uint32_t frames = scpiExtTrigFrames;
        if (frames != scpiExtTrigSent)
        {
            scpiExtTrigSent = frames;
            printCurrentReading(*scpiStream);
        }
        return;
    }

    if (!settingsGetAutoEnabled())
        return;

    uint16_t intervalSeconds = settingsGetAutoIntervalSeconds();
    if (intervalSeconds == 0)
    {
        // Every measurement: fire once per new frame, not once per
        // loop() pass - scpiFrameCounter only changes when
        // serialOutUpdateReading() runs (dmm_bridge.ino, once per
        // decoded DMM frame).
        if (scpiFrameCounter != scpiLastSentFrameCounter)
        {
            printCurrentReading(*scpiStream);
            scpiLastSentFrameCounter = scpiFrameCounter;
        }
    }
    else
    {
        uint32_t nowMs = millis();
        if (nowMs - scpiLastSentMillis >= (uint32_t)intervalSeconds * 1000UL)
        {
            printCurrentReading(*scpiStream);
            scpiLastSentMillis = nowMs;
        }
    }
}

void serialOutProcess()
{
    // v18 MEASure:TRIGger MARK: the rear input as a marker key (debounced in trigger_io.ino)
    uint32_t keyMs = 0;
    if (scpiTriggerSource == SCPI_TRIGGER_MARK && triggerIoTakeKeyPress(&keyMs))
    {
        markerAdd(keyMs, MARKER_SOURCE_EXT);
    }

    while (scpiStream->available())
    {
        char c = scpiStream->read();
        if (c == '\r' || c == '\n')
        {
            if (lineLength > 0)
            {
                lineBuffer[lineLength] = '\0';
                scpiParser.Execute(lineBuffer, *scpiStream);
                lineLength = 0;
            }
            continue;
        }
        if (lineLength >= sizeof(lineBuffer) - 1)
        {
            // Longer than the library's own buffer would accept -
            // discard rather than silently truncate a command into
            // something else.
            lineLength = 0;
            continue;
        }
        lineBuffer[lineLength++] = c;
    }

    if (suppressUnsolicitedOutput)
    {
        // A LOG:* handler just finished a dump during the Execute()
        // call(s) above - skip auto-send for this one pass so nothing
        // gets appended right after it, then clear the flag so it
        // doesn't suppress anything beyond that.
        suppressUnsolicitedOutput = false;
    }
    else if (lineLength == 0)
    {
        // No dump just happened, and nothing is sitting half-typed in
        // the line buffer either - safe to check whether an
        // unsolicited SYSTem:AUTO line is due. (If lineLength > 0, the
        // person has started typing a command but hasn't pressed
        // Enter yet - holding off here means an auto-send line can't
        // land in the middle of whatever they're typing, LOG:* or
        // otherwise.)
        scpiAutoSendCheck();
    }
}

