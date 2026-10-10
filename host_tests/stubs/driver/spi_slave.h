// Host stand-in for ESP-IDF's SPI slave driver, modelling what matters for
// dmm_bridge.ino: transactions stay in the driver until the master (here
// a simulated K197, sending a frame every FakeDmm::periodMs) clocks a frame
// into the OLDEST pending one; get_trans_result() with a timeout leaves a
// transaction in the driver - the ESP-IDF behaviour behind the v13 fix.
// Time is simulated: waiting advances millis() (fakeMillisOffset()).
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <string.h>
#include <deque>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_STATE 0x103
typedef int spi_host_device_t;
#define SPI3_HOST 2
#define SPI_DMA_CH_AUTO 3
struct spi_bus_config_t { int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num; };
struct spi_slave_interface_config_t { int spics_io_num; uint32_t flags; int queue_size; uint8_t mode; void (*post_setup_cb)(void *); void (*post_trans_cb)(void *); };
struct spi_slave_transaction_t { size_t length; size_t trans_len; const void *tx_buffer; void *rx_buffer; void *user; };

struct FakeSpiSlave
{
    bool initialized = false;
    int queueSize = 0; // driver queue depth: queued transactions (besides the one in progress) and finished results each
    std::deque<spi_slave_transaction_t *> pending; // front = the one the hardware fills next
    std::deque<spi_slave_transaction_t *> done;
    int inits = 0;
    int frees = 0;
    // simulated K197
    bool dmmOn = false;
    uint32_t nextFrameMs = 0;
    uint32_t periodMs = 285;
    uint8_t frame[17] = {0};
    int frameBytes = 17;
    int sent = 0;
    int lost = 0; // frames that found no pending transaction
    int droppedResults = 0;
};
inline FakeSpiSlave &fakeSpi() { static FakeSpiSlave s; return s; }
inline uint32_t fakeNow() { return (uint32_t)millis(); }
inline void fakeAdvanceTo(uint32_t t) { uint32_t n = fakeNow(); if ((int32_t)(t - n) > 0) fakeMillisOffset() += (t - n); }

// Delivers every frame due up to `untilMs`.
inline void fakeDeliverDue(uint32_t untilMs)
{
    FakeSpiSlave &s = fakeSpi();
    while (s.dmmOn && (int32_t)(s.nextFrameMs - untilMs) <= 0)
    {
        s.sent++;
        if (s.initialized && !s.pending.empty())
        {
            spi_slave_transaction_t *t = s.pending.front();
            s.pending.pop_front();
            memcpy(t->rx_buffer, s.frame, (size_t)s.frameBytes);
            t->trans_len = (size_t)s.frameBytes * 8;
            if ((int)s.done.size() < s.queueSize)
                s.done.push_back(t);
            else
                s.droppedResults++; // result queue full: the ISR cannot post it
        }
        else
        {
            s.lost++;
        }
        s.nextFrameMs += s.periodMs;
    }
}

// Waits (simulated) until `ready()` or the timeout; frames keep arriving meanwhile.
template <typename F>
inline bool fakeWait(uint32_t ticks, F ready)
{
    FakeSpiSlave &s = fakeSpi();
    uint32_t deadline = fakeNow() + ticks;
    for (;;)
    {
        fakeDeliverDue(fakeNow());
        if (ready()) return true;
        if (s.dmmOn && (int32_t)(s.nextFrameMs - deadline) <= 0)
        {
            fakeAdvanceTo(s.nextFrameMs);
            continue;
        }
        fakeAdvanceTo(deadline);
        fakeDeliverDue(fakeNow());
        return ready();
    }
}

inline esp_err_t spi_slave_initialize(spi_host_device_t, const spi_bus_config_t *, const spi_slave_interface_config_t *cfg, int)
{
    FakeSpiSlave &s = fakeSpi();
    if (s.initialized) return ESP_ERR_INVALID_STATE;
    s.initialized = true;
    s.queueSize = cfg->queue_size;
    s.pending.clear();
    s.done.clear();
    s.inits++;
    return ESP_OK;
}
inline esp_err_t spi_slave_free(spi_host_device_t)
{
    FakeSpiSlave &s = fakeSpi();
    s.initialized = false;
    s.pending.clear();
    s.done.clear();
    s.frees++;
    return ESP_OK;
}
inline esp_err_t spi_slave_queue_trans(spi_host_device_t, spi_slave_transaction_t *t, uint32_t ticks)
{
    FakeSpiSlave &s = fakeSpi();
    if (!s.initialized) return ESP_ERR_INVALID_STATE;
    if (!fakeWait(ticks, [&] { return (int)s.pending.size() < s.queueSize + 1; })) return ESP_ERR_TIMEOUT; // +1: the one in progress is no longer in the queue
    s.pending.push_back(t);
    return ESP_OK;
}
inline esp_err_t spi_slave_get_trans_result(spi_host_device_t, spi_slave_transaction_t **out, uint32_t ticks)
{
    FakeSpiSlave &s = fakeSpi();
    if (!s.initialized) return ESP_ERR_INVALID_STATE;
    if (!fakeWait(ticks, [&] { return !s.done.empty(); })) return ESP_ERR_TIMEOUT;
    *out = s.done.front();
    s.done.pop_front();
    return ESP_OK;
}
inline esp_err_t spi_slave_transmit(spi_host_device_t h, spi_slave_transaction_t *t, uint32_t ticks)
{
    esp_err_t e = spi_slave_queue_trans(h, t, ticks);
    if (e != ESP_OK) return e;
    spi_slave_transaction_t *r = nullptr;
    return spi_slave_get_trans_result(h, &r, ticks);
}
