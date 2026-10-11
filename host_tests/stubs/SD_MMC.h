#pragma once
// v23: stand-in for the Arduino ESP32 SD_MMC library - an in-memory card the test controls: present or not,
// size, failing writes. File::write() collects the bytes, flush() / close() put them "on the card" (only while
// the card is present), so what a removed card loses is exactly what was not synced.
#include <Arduino.h>
#include <map>
#include <string>

#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 3 // core 3.x: SD_MMC.readRAW() exists (the card probe)
#endif
#define FILE_READ  "r"
#define FILE_WRITE "w"

typedef enum
{
    CARD_NONE,
    CARD_MMC,
    CARD_SD,
    CARD_SDHC,
    CARD_UNKNOWN
} sdcard_type_t;

struct FakeSdCard
{
    bool present = false;
    bool mounted = false;
    bool failWrites = false;
    uint64_t total = 1024ULL * 1024ULL * 1024ULL;
    uint64_t used = 0;
    int beginCalls = 0;
    int opens = 0;
    std::map<std::string, std::string> files; // what is on the card
    std::map<std::string, bool> dirs;
};
inline FakeSdCard &fakeSd()
{
    static FakeSdCard card;
    return card;
}

class File
{
public:
    size_t write(const uint8_t *buf, size_t size)
    {
        if (!isOpen || !fakeSd().present || !fakeSd().mounted || fakeSd().failWrites)
            return 0;
        pending.append((const char *)buf, size);
        return size;
    }
    void flush()
    {
        if (isOpen && fakeSd().present && fakeSd().mounted)
        {
            fakeSd().files[path] += pending;
            fakeSd().used += pending.size();
        }
        pending.clear();
    }
    void close()
    {
        flush();
        isOpen = false;
    }
    explicit operator bool() const { return isOpen; }
    std::string path;
    std::string pending;
    bool isOpen = false;
};

class SDMMCFS
{
public:
    bool setPins(int, int, int) { return true; }
    bool begin(const char *, bool, bool, int, int)
    {
        fakeSd().beginCalls++;
        if (!fakeSd().present)
            return false;
        fakeSd().mounted = true;
        return true;
    }
    void end() { fakeSd().mounted = false; }
    sdcard_type_t cardType() { return fakeSd().present ? CARD_SDHC : CARD_NONE; }
    uint64_t totalBytes() { return fakeSd().total; }
    uint64_t usedBytes() { return fakeSd().used; }
    bool exists(const char *path) { return fakeSd().files.count(path) != 0 || fakeSd().dirs.count(path) != 0; }
    bool mkdir(const char *path)
    {
        fakeSd().dirs[path] = true;
        return true;
    }
    File open(const char *path, const char *)
    {
        File f;
        if (!fakeSd().present || !fakeSd().mounted)
            return f;
        f.path = path;
        f.isOpen = true;
        fakeSd().files[path]; // created (empty)
        fakeSd().opens++;
        return f;
    }
    bool readRAW(uint8_t *, uint32_t) { return fakeSd().present && fakeSd().mounted; }
};
inline SDMMCFS SD_MMC;
