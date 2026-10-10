// debuglog.ino
// See debuglog.h.

#include <stdarg.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "debuglog.h"

// Declared in serial_out.h: true when SYSTem:DEBUG is on and no SCPI line is half typed.
bool serialOutDebugTraceAllowed();

// One queued message: the text, already formatted, with its newline.
struct DebugLogItem
{
    char text[DEBUGLOG_MESSAGE_CHARS + 3]; // + "\r\n" + terminator
};

static QueueHandle_t debugLogQueue = NULL;
static Stream *debugLogOut = &Serial; // v13: SYSTem:DEBUG:PORT
static bool debugLogPrefix = false;

void debugLogSetOutput(Stream *stream, bool prefix)
{
    debugLogOut = (stream != NULL) ? stream : &Serial;
    debugLogPrefix = prefix;
}
static volatile uint32_t debugLogDroppedTotal = 0;
static volatile uint32_t debugLogDroppedReported = 0;

void debugLogBegin()
{
    debugLogQueue = xQueueCreate(DEBUGLOG_QUEUE_LENGTH, sizeof(DebugLogItem));
}

static void debugLogEnqueue(const char *format, va_list args)
{
    if (debugLogQueue == NULL)
    {
        return;
    }
    DebugLogItem item;
    int length = vsnprintf(item.text, DEBUGLOG_MESSAGE_CHARS + 1, format, args);
    if (length < 0)
    {
        return;
    }
    if (length > DEBUGLOG_MESSAGE_CHARS)
    {
        length = DEBUGLOG_MESSAGE_CHARS; // cut
    }
    item.text[length] = '\r';
    item.text[length + 1] = '\n';
    item.text[length + 2] = '\0';
    // Timeout 0: never wait. A full queue means the communication task is
    // not keeping up (or is busy with a long SCPI reply) - the message is dropped.
    if (xQueueSend(debugLogQueue, &item, 0) != pdTRUE)
    {
        debugLogDroppedTotal = debugLogDroppedTotal + 1;
    }
}

void debugLogf(const char *format, ...)
{
    if (!serialOutDebugTraceAllowed())
    {
        return;
    }
    va_list args;
    va_start(args, format);
    debugLogEnqueue(format, args);
    va_end(args);
}

void debugLogAlways(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    debugLogEnqueue(format, args);
    va_end(args);
}

void debugLogDrain()
{
    if (debugLogQueue == NULL)
    {
        return;
    }
    DebugLogItem item;
    while (xQueueReceive(debugLogQueue, &item, 0) == pdTRUE)
    {
        if (debugLogPrefix)
        {
            debugLogOut->write((const uint8_t *)"# ", 2);
        }
        debugLogOut->write((const uint8_t *)item.text, strlen(item.text)); // one write per message
    }
    uint32_t dropped = debugLogDroppedTotal;
    if (dropped != debugLogDroppedReported)
    {
        char note[64];
        int length = snprintf(note, sizeof(note), "%s(%lu debug messages dropped)\r\n", debugLogPrefix ? "# " : "",
                              (unsigned long)(dropped - debugLogDroppedReported));
        debugLogOut->write((const uint8_t *)note, (size_t)length);
        debugLogDroppedReported = dropped;
    }
}

uint32_t debugLogDropped()
{
    return debugLogDroppedTotal;
}
