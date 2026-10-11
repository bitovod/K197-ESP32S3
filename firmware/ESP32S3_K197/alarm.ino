// alarm.ino
// See alarm.h.

#include "alarm.h"
#include "config.h"
#include "settings.h"
#include "beeper.h"
#include "display.h"     // setAlarmIndicator(), KEITHLEY_BIT_MINUS
#include "reading_math.h" // keithleyReadingToDouble(), keithleyIsOverload(), keithleyFunctionPrefix()
#include "ui_format.h"   // uiFunctionFilter(), uiDecimalsFor(), uiUnitMultiplier(), uiFormatNumber()
#include "datalog.h"    // v17: default limits from the recent log (MIN/MAX)
#include "rtc.h"        // rtcCompactTimestamp()
#include "framebuffer.h" // FB_ALARM_* (the symbol codes for setAlarmIndicator())
#include <esp_timer.h>
#include <math.h>
#include <strings.h> // strcasecmp()

static_assert(ALARM_FUNCTION_COUNT == SETTINGS_ALARM_FUNCTIONS, "one stored alarm configuration per function");


static const char *const alarmFunctionNames[ALARM_FUNCTION_COUNT] = {"DCV", "ACV", "DCI", "ACI", "OHM", "DB"};

// Display format of a function that has not been measured since power-up
// (only matters for SCPI-set limits shown before the first reading).
static const uint8_t alarmDefaultUnit[ALARM_FUNCTION_COUNT] = {KEITHLEY_UNIT_V, KEITHLEY_UNIT_V, KEITHLEY_UNIT_MA,
                                                               KEITHLEY_UNIT_MA, KEITHLEY_UNIT_KOHM, KEITHLEY_UNIT_DB};
static const uint8_t alarmDefaultDecimals[ALARM_FUNCTION_COUNT] = {5, 4, 4, 4, 4, 3};

// ---- guarded by alarmMux ----
static portMUX_TYPE alarmMux = portMUX_INITIALIZER_UNLOCKED;
static bool alarmOn[ALARM_FUNCTION_COUNT];          // not persisted: false after power-up
static uint8_t alarmStoredMode[ALARM_FUNCTION_COUNT]; // UPPER/LOWER/BOTH
static double alarmLow[ALARM_FUNCTION_COUNT];
static double alarmHigh[ALARM_FUNCTION_COUNT];
static uint8_t alarmSignal = CONFIG_ALARM_DEFAULT_SIGNAL;
static uint8_t alarmFuncUnit[ALARM_FUNCTION_COUNT];     // display format seen last per function, 0xFF = never
static uint8_t alarmFuncDecimals[ALARM_FUNCTION_COUNT];
// the newest reading
static uint8_t alarmLiveFunction = ALARM_FUNCTION_NONE;
static bool alarmLiveNumeric = false;  // a plain number (not OL, not self-test)
static bool alarmLiveOverload = false; // "OL" / "-OL"
static bool alarmLiveNegative = false; // minus sign (for OL)
static double alarmLiveValue = 0.0;    // base units
static uint8_t alarmLiveUnit = KEITHLEY_UNIT_NONE;
static uint8_t alarmLiveDecimals = 0;
static bool alarmLiveSettling = false; // v20/v22: the newest reading is still settling (datalog.ino decides) - no alarm

// The state last evaluated (written under alarmMux, read anywhere).
static volatile uint8_t alarmState = ALARM_STATE_NONE;
// Beep pattern (alarmTick(), esp_timer task only).
static esp_timer_handle_t alarmTimer = NULL;
static uint16_t alarmTickPhase = 0;
static volatile bool alarmRestartPattern = false; // set when a reading leaves the limits: first beep at the next tick
static volatile bool alarmBlinkVisible = true;    // the bright triangle of HIGH/LOW is shown (blink phase)
static volatile uint8_t alarmShownMode = ALARM_MODE_OFF; // mode of the live function, for the ARMED symbol

uint8_t alarmFunctionFromName(const char *name)
{
    if (name == NULL)
    {
        return ALARM_FUNCTION_NONE;
    }
    for (uint8_t f = 0; f < ALARM_FUNCTION_COUNT; f++)
    {
        if (strcasecmp(name, alarmFunctionNames[f]) == 0)
        {
            return f;
        }
    }
    return ALARM_FUNCTION_NONE;
}

const char *alarmFunctionName(uint8_t function)
{
    return (function < ALARM_FUNCTION_COUNT) ? alarmFunctionNames[function] : "";
}

static double alarmPow10(uint8_t decimals)
{
    double scale = 1.0;
    for (uint8_t i = 0; i < decimals; i++)
    {
        scale *= 10.0;
    }
    return scale;
}

// `baseValue` in counts of the last digit of a display in `unit` with `decimals`.
static long long alarmToCounts(double baseValue, uint8_t unit, uint8_t decimals)
{
    return llround(baseValue / uiUnitMultiplier(unit) * alarmPow10(decimals));
}

// The state for the newest reading. Caller holds alarmMux.
static uint8_t alarmEvaluateLocked()
{
    uint8_t f = alarmLiveFunction;
    if (f >= ALARM_FUNCTION_COUNT || !alarmOn[f])
    {
        return ALARM_STATE_NONE;
    }
    if (alarmLiveSettling)
    {
        return ALARM_STATE_ARMED; // v20: still settling after a change of the function / power-on - not judged yet
    }
    uint8_t mode = alarmStoredMode[f];
    bool watchHigh = (mode == ALARM_MODE_UPPER || mode == ALARM_MODE_BOTH) && !isnan(alarmHigh[f]);
    bool watchLow = (mode == ALARM_MODE_LOWER || mode == ALARM_MODE_BOTH) && !isnan(alarmLow[f]);
    if (alarmLiveOverload)
    {
        if (!alarmLiveNegative && watchHigh)
        {
            return ALARM_STATE_HIGH;
        }
        if (alarmLiveNegative && watchLow)
        {
            return ALARM_STATE_LOW;
        }
        return ALARM_STATE_ARMED;
    }
    if (!alarmLiveNumeric)
    {
        return ALARM_STATE_ARMED; // a function is known but no number (cannot happen often - kept simple)
    }
    long long reading = alarmToCounts(alarmLiveValue, alarmLiveUnit, alarmLiveDecimals);
    if (watchHigh && reading > alarmToCounts(alarmHigh[f], alarmLiveUnit, alarmLiveDecimals))
    {
        return ALARM_STATE_HIGH;
    }
    if (watchLow && reading < alarmToCounts(alarmLow[f], alarmLiveUnit, alarmLiveDecimals))
    {
        return ALARM_STATE_LOW;
    }
    return ALARM_STATE_ARMED;
}

// The symbol on the LCD replica (FB_ALARM_*) for a state.
static uint8_t alarmDisplayCode(uint8_t state, uint8_t mode, bool visible)
{
    switch (state)
    {
        case ALARM_STATE_HIGH:
            return visible ? FB_ALARM_HIGH : FB_ALARM_NONE;
        case ALARM_STATE_LOW:
            return visible ? FB_ALARM_LOW : FB_ALARM_NONE;
        case ALARM_STATE_ARMED:
            if (mode == ALARM_MODE_UPPER)
            {
                return FB_ALARM_ARMED_UPPER;
            }
            if (mode == ALARM_MODE_LOWER)
            {
                return FB_ALARM_ARMED_LOWER;
            }
            return FB_ALARM_ARMED_BOTH;
        default:
            return FB_ALARM_NONE;
    }
}

// Hands the current symbol to the display (cheap, it only redraws on a change).
static void alarmShow()
{
    setAlarmIndicator(alarmDisplayCode(alarmState, alarmShownMode, alarmBlinkVisible));
}

// Re-evaluates and publishes the state (any task).
static void alarmRefresh()
{
    portENTER_CRITICAL(&alarmMux);
    uint8_t before = alarmState;
    uint8_t now = alarmEvaluateLocked();
    alarmState = now;
    uint8_t f = alarmLiveFunction;
    alarmShownMode = (f < ALARM_FUNCTION_COUNT && alarmOn[f]) ? alarmStoredMode[f] : ALARM_MODE_OFF;
    bool wasOutside = (before == ALARM_STATE_HIGH || before == ALARM_STATE_LOW);
    bool isOutside = (now == ALARM_STATE_HIGH || now == ALARM_STATE_LOW);
    if (isOutside && !wasOutside)
    {
        alarmRestartPattern = true; // the next tick starts the pattern: beep + visible
        alarmBlinkVisible = true;   // shown at once, not only at the next tick
    }
    portEXIT_CRITICAL(&alarmMux);
    alarmShow();
}

static void alarmTimerCallback(void *arg)
{
    (void)arg;
    alarmTick();
}

void alarmTick()
{
    const uint16_t periodTicks = (uint16_t)((CONFIG_ALARM_BEEP_PERIOD_MS + CONFIG_ALARM_TICK_MS / 2) / CONFIG_ALARM_TICK_MS);
    const uint16_t visibleTicks = (uint16_t)((CONFIG_ALARM_BLINK_ON_MS + CONFIG_ALARM_TICK_MS / 2) / CONFIG_ALARM_TICK_MS);
    uint8_t state = alarmState;
    bool outside = (state == ALARM_STATE_HIGH || state == ALARM_STATE_LOW);
    if (!outside)
    {
        alarmTickPhase = 0;
        alarmBlinkVisible = true;
        return;
    }
    if (alarmRestartPattern)
    {
        alarmRestartPattern = false;
        alarmTickPhase = 0;
    }
    if (alarmTickPhase == 0 && alarmSignal == ALARM_SIGNAL_BEEP)
    {
        beeperBeepAsync(CONFIG_ALARM_BEEP_MS); // SYSTem:BEEP OFF keeps it silent
    }
    alarmBlinkVisible = (alarmTickPhase < visibleTicks); // blinks in step with the beeps
    alarmShow();
    alarmTickPhase = (uint16_t)(alarmTickPhase + 1);
    if (alarmTickPhase >= periodTicks || periodTicks == 0)
    {
        alarmTickPhase = 0;
    }
}

void alarmBegin()
{
    portENTER_CRITICAL(&alarmMux);
    for (uint8_t f = 0; f < ALARM_FUNCTION_COUNT; f++)
    {
        uint8_t mode;
        float low, high;
        settingsGetAlarm(f, &mode, &low, &high);
        alarmOn[f] = false;
        alarmStoredMode[f] = (mode >= ALARM_MODE_UPPER && mode <= ALARM_MODE_BOTH) ? mode : ALARM_MODE_BOTH;
        alarmLow[f] = isfinite(low) ? (double)low : NAN;
        alarmHigh[f] = isfinite(high) ? (double)high : NAN;
        alarmFuncUnit[f] = 0xFF;
        alarmFuncDecimals[f] = 0;
    }
    uint8_t signal = settingsGetAlarmSignal();
    alarmSignal = (signal < ALARM_SIGNAL_COUNT) ? signal : CONFIG_ALARM_DEFAULT_SIGNAL;
    portEXIT_CRITICAL(&alarmMux);
    setAlarmIndicator(FB_ALARM_NONE);

    esp_timer_create_args_t timerArgs = {};
    timerArgs.callback = &alarmTimerCallback;
    timerArgs.arg = NULL;
    timerArgs.dispatch_method = ESP_TIMER_TASK;
    timerArgs.name = "alarm";
    if (esp_timer_create(&timerArgs, &alarmTimer) == 0)
    {
        esp_timer_start_periodic(alarmTimer, (uint64_t)CONFIG_ALARM_TICK_MS * 1000ULL);
    }
}

void alarmUpdateReading(const char digits[KEITHLEY_DIGIT_COUNT], int8_t dpPos, uint16_t indicatorBits, uint8_t unit)
{
    char filter[8];
    uiFunctionFilter(filter, sizeof(filter), keithleyFunctionPrefix(unit, indicatorBits));
    uint8_t function = (unit == KEITHLEY_UNIT_NONE) ? ALARM_FUNCTION_NONE : alarmFunctionFromName(filter);
    bool overload = keithleyIsOverload(digits);
    double value = 0.0;
    bool numeric = !overload && keithleyReadingToDouble(digits, dpPos, indicatorBits, unit, &value);
    uint8_t decimals = uiDecimalsFor(dpPos);

    bool settling = datalogLastReadingSettling(); // v22: one rule with the log (dmmApplyFrame() logs first)

    portENTER_CRITICAL(&alarmMux);
    alarmLiveSettling = settling;
    alarmLiveFunction = function;
    alarmLiveNumeric = numeric;
    alarmLiveOverload = overload && function != ALARM_FUNCTION_NONE;
    alarmLiveNegative = (indicatorBits & KEITHLEY_BIT_MINUS) != 0;
    alarmLiveValue = value;
    alarmLiveUnit = unit;
    alarmLiveDecimals = decimals;
    if (function != ALARM_FUNCTION_NONE && numeric)
    {
        alarmFuncUnit[function] = unit;
        alarmFuncDecimals[function] = decimals;
    }
    portEXIT_CRITICAL(&alarmMux);
    alarmRefresh();
}

uint8_t alarmGetState()
{
    return alarmState;
}

uint8_t alarmGetLiveFunction()
{
    return alarmLiveFunction;
}

bool alarmGetLiveValue(uint8_t *function, uint8_t *unit, uint8_t *decimals, double *value)
{
    portENTER_CRITICAL(&alarmMux);
    bool numeric = alarmLiveNumeric && alarmLiveFunction != ALARM_FUNCTION_NONE;
    *function = alarmLiveFunction;
    *unit = alarmLiveUnit;
    *decimals = alarmLiveDecimals;
    if (numeric)
    {
        *value = alarmLiveValue;
    }
    portEXIT_CRITICAL(&alarmMux);
    return numeric;
}

bool alarmGetDisplayFormat(uint8_t function, uint8_t *unit, uint8_t *decimals)
{
    if (function >= ALARM_FUNCTION_COUNT)
    {
        *unit = KEITHLEY_UNIT_NONE;
        *decimals = 0;
        return false;
    }
    portENTER_CRITICAL(&alarmMux);
    bool seen = (alarmFuncUnit[function] != 0xFF);
    *unit = seen ? alarmFuncUnit[function] : alarmDefaultUnit[function];
    *decimals = seen ? alarmFuncDecimals[function] : alarmDefaultDecimals[function];
    portEXIT_CRITICAL(&alarmMux);
    return seen;
}

// Persists the configuration of one function. Takes the values under the lock.
static void alarmSave(uint8_t function, bool saveNow)
{
    portENTER_CRITICAL(&alarmMux);
    uint8_t mode = alarmStoredMode[function];
    float low = (float)alarmLow[function];
    float high = (float)alarmHigh[function];
    portEXIT_CRITICAL(&alarmMux);
    if (saveNow)
    {
        settingsSetAlarm(function, mode, low, high);
    }
    else
    {
        settingsSetAlarmDeferred(function, mode, low, high);
    }
}

uint8_t alarmGetMode(uint8_t function)
{
    if (function >= ALARM_FUNCTION_COUNT)
    {
        return ALARM_MODE_OFF;
    }
    portENTER_CRITICAL(&alarmMux);
    uint8_t mode = alarmOn[function] ? alarmStoredMode[function] : ALARM_MODE_OFF;
    portEXIT_CRITICAL(&alarmMux);
    return mode;
}

uint8_t alarmGetStoredMode(uint8_t function)
{
    return (function < ALARM_FUNCTION_COUNT) ? alarmStoredMode[function] : ALARM_MODE_BOTH;
}

bool alarmRecentRange(uint8_t function, double *minimum, double *maximum)
{
    if (function >= ALARM_FUNCTION_COUNT)
    {
        return false;
    }
    uint32_t end = datalogEndSerial(NULL);
    // At most this many records are looked at (the window holds ~35 at 3.5 readings per second).
    const uint32_t maxRecords = (uint32_t)(CONFIG_ALARM_DEFAULT_WINDOW_S * CONFIG_UI_FALLBACK_RECORDS_PER_SECOND * 4.0f) + 16;
    uint32_t start;
    uint32_t nowSeconds = 0;
    uint8_t nowCentis = 0;
    if (rtcCompactTimestamp(&nowSeconds, &nowCentis) && nowSeconds > CONFIG_ALARM_DEFAULT_WINDOW_S)
    {
        start = datalogFirstSerialAtOrAfter(nowSeconds - CONFIG_ALARM_DEFAULT_WINDOW_S);
    }
    else
    {
        // no RTC time: the usual number of readings in the window
        uint32_t estimate = (uint32_t)(CONFIG_ALARM_DEFAULT_WINDOW_S * CONFIG_UI_FALLBACK_RECORDS_PER_SECOND + 0.5f);
        start = (end > estimate) ? end - estimate : 0;
    }
    if (end > maxRecords && start < end - maxRecords)
    {
        start = end - maxRecords;
    }
    const char *name = alarmFunctionNames[function];
    uint32_t count = 0;
    double lo = 0.0, hi = 0.0;
    LogRecord chunk[16];
    uint32_t serial = start;
    while (serial < end)
    {
        uint32_t want = end - serial;
        if (want > 16)
        {
            want = 16;
        }
        uint32_t got = datalogReadSerial(&serial, chunk, want, NULL);
        if (got == 0)
        {
            break;
        }
        serial += got;
        for (uint32_t i = 0; i < got; i++)
        {
            if (datalogRecordSkipped(chunk[i]) || !datalogRecordMatchesFunction(name, &chunk[i]))
            {
                continue;
            }
            double v = (double)chunk[i].value;
            if (count == 0 || v < lo)
            {
                lo = v;
            }
            if (count == 0 || v > hi)
            {
                hi = v;
            }
            count++;
        }
    }
    if (count == 0)
    {
        return false;
    }
    *minimum = lo;
    *maximum = hi;
    return true;
}

void alarmSetMode(uint8_t function, uint8_t mode, bool saveNow)
{
    if (function >= ALARM_FUNCTION_COUNT || mode >= ALARM_MODE_COUNT)
    {
        return;
    }
    // v17: limits that are needed but not set yet start at the MAX / MIN of the last
    // CONFIG_ALARM_DEFAULT_WINDOW_S seconds of the log (outside the lock - it reads the log)
    double recentMin = NAN, recentMax = NAN;
    bool needHigh = (mode == ALARM_MODE_UPPER || mode == ALARM_MODE_BOTH) && isnan(alarmGetLimit(function, true));
    bool needLow = (mode == ALARM_MODE_LOWER || mode == ALARM_MODE_BOTH) && isnan(alarmGetLimit(function, false));
    if (needHigh || needLow)
    {
        alarmRecentRange(function, &recentMin, &recentMax);
    }
    portENTER_CRITICAL(&alarmMux);
    if (mode == ALARM_MODE_OFF)
    {
        alarmOn[function] = false;
    }
    else
    {
        alarmOn[function] = true;
        alarmStoredMode[function] = mode;
        // A limit that is needed but not set: MAX / MIN of the recent log, else the live reading (if it is one
        // of this function).
        bool liveHere = alarmLiveNumeric && alarmLiveFunction == function;
        if ((mode == ALARM_MODE_UPPER || mode == ALARM_MODE_BOTH) && isnan(alarmHigh[function]))
        {
            alarmHigh[function] = !isnan(recentMax) ? recentMax : (liveHere ? alarmLiveValue : NAN);
        }
        if ((mode == ALARM_MODE_LOWER || mode == ALARM_MODE_BOTH) && isnan(alarmLow[function]))
        {
            alarmLow[function] = !isnan(recentMin) ? recentMin : (liveHere ? alarmLiveValue : NAN);
        }
    }
    portEXIT_CRITICAL(&alarmMux);
    if (mode != ALARM_MODE_OFF)
    {
        alarmSave(function, saveNow); // OFF changes nothing that is persisted
    }
    alarmRefresh();
}

double alarmGetLimit(uint8_t function, bool upper)
{
    if (function >= ALARM_FUNCTION_COUNT)
    {
        return NAN;
    }
    portENTER_CRITICAL(&alarmMux);
    double value = upper ? alarmHigh[function] : alarmLow[function];
    portEXIT_CRITICAL(&alarmMux);
    return value;
}

void alarmSetLimit(uint8_t function, bool upper, double baseValue, bool saveNow)
{
    if (function >= ALARM_FUNCTION_COUNT)
    {
        return;
    }
    if (!isfinite(baseValue))
    {
        baseValue = NAN;
    }
    portENTER_CRITICAL(&alarmMux);
    if (upper)
    {
        alarmHigh[function] = baseValue;
    }
    else
    {
        alarmLow[function] = baseValue;
    }
    portEXIT_CRITICAL(&alarmMux);
    alarmSave(function, saveNow);
    alarmRefresh();
}

// Decimals uiFormatNumber() shows `baseValue` with (at most maxDecimals, fewer when
// the value would not fit into the display's counts) - the same loop.
static uint8_t alarmShownDecimals(double baseValue, uint8_t unit, uint8_t maxDecimals)
{
    double magnitude = fabs(baseValue / uiUnitMultiplier(unit));
    int decimals = (int)maxDecimals;
    double scale = alarmPow10(maxDecimals);
    while (decimals > 0 && floor(magnitude * scale + 0.5) > (double)UI_DISPLAY_MAX_COUNTS)
    {
        decimals--;
        scale /= 10.0;
    }
    return (uint8_t)decimals;
}

// The largest multiple of `step` (> 0) that is <= value.
static long long alarmFloorToStep(long long value, long long step)
{
    long long q = value / step;
    if (value % step != 0 && value < 0)
    {
        q--;
    }
    return q * step;
}

bool alarmStepLimit(uint8_t function, bool upper, int8_t direction, uint32_t stepCounts)
{
    if (function >= ALARM_FUNCTION_COUNT || direction == 0 || stepCounts == 0)
    {
        return false;
    }
    uint8_t unit, maxDecimals;
    alarmGetDisplayFormat(function, &unit, &maxDecimals);
    if (unit == KEITHLEY_UNIT_NONE)
    {
        return false;
    }
    double current = alarmGetLimit(function, upper);
    if (isnan(current))
    {
        uint8_t liveFunction, liveUnit, liveDecimals;
        double live = 0.0;
        current = (alarmGetLiveValue(&liveFunction, &liveUnit, &liveDecimals, &live) && liveFunction == function) ? live
                                                                                                                 : 0.0;
    }
    uint8_t decimals = alarmShownDecimals(current, unit, maxDecimals);
    double scale = alarmPow10(decimals);
    long long counts = alarmToCounts(current, unit, decimals);
    long long step = (long long)stepCounts;
    long long next;
    if (direction > 0)
    {
        next = alarmFloorToStep(counts + step, step); // 123 + 10 -> 130, -123 + 10 -> -120
    }
    else
    {
        next = -alarmFloorToStep(-(counts - step), step); // 123 - 10 -> 120, -123 - 10 -> -130
    }
    double limitCounts = (double)UI_DISPLAY_MAX_COUNTS * scale; // +-199999 counts at zero decimals
    if ((double)next > limitCounts)
    {
        next = (long long)limitCounts;
    }
    if ((double)next < -limitCounts)
    {
        next = -(long long)limitCounts;
    }
    if (next == counts && !isnan(alarmGetLimit(function, upper)))
    {
        return false; // already at the end of the range
    }
    alarmSetLimit(function, upper, (double)next / scale * uiUnitMultiplier(unit), false);
    return true;
}

void alarmFormatLimit(char *out, size_t size, uint8_t function, bool upper)
{
    double value = alarmGetLimit(function, upper);
    if (isnan(value))
    {
        snprintf(out, size, "--");
        return;
    }
    uint8_t unit, decimals;
    alarmGetDisplayFormat(function, &unit, &decimals);
    uiFormatNumber(out, size, value, unit, decimals);
}

uint8_t alarmGetSignal()
{
    return alarmSignal;
}

void alarmSetSignal(uint8_t signal, bool saveNow)
{
    if (signal >= ALARM_SIGNAL_COUNT)
    {
        return;
    }
    alarmSignal = signal;
    if (saveNow)
    {
        settingsSetAlarmSignal(signal);
    }
    else
    {
        settingsSetAlarmSignalDeferred(signal);
    }
}
