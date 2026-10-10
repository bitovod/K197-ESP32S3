// Host stand-in for the parts of ESP-IDF's GPIO driver dmm_bridge.ino uses
// (edge counter interrupts). Handlers are remembered so a test can play edges.
#pragma once
#include <stdint.h>
typedef int gpio_num_t;
enum gpio_int_type_t { GPIO_INTR_DISABLE = 0, GPIO_INTR_POSEDGE = 1, GPIO_INTR_NEGEDGE = 2 };
typedef void (*gpio_isr_t)(void *);
struct FakeGpioIntr { gpio_isr_t handler = nullptr; void *arg = nullptr; bool enabled = false; int type = 0; };
inline FakeGpioIntr &fakeGpioIntr(int pin) { static FakeGpioIntr p[64]; return p[pin & 63]; }
inline int gpio_install_isr_service(int) { return 0; }
inline int gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t t) { fakeGpioIntr(pin).type = t; return 0; }
inline int gpio_isr_handler_add(gpio_num_t pin, gpio_isr_t h, void *arg) { fakeGpioIntr(pin).handler = h; fakeGpioIntr(pin).arg = arg; return 0; }
inline int gpio_intr_enable(gpio_num_t pin) { fakeGpioIntr(pin).enabled = true; return 0; }
inline int gpio_intr_disable(gpio_num_t pin) { fakeGpioIntr(pin).enabled = false; return 0; }
// Test helper: n edges on a pin (only counted while its interrupt is enabled).
inline void fakeGpioEdges(int pin, int n) { for (int i = 0; i < n; i++) { FakeGpioIntr &p = fakeGpioIntr(pin); if (p.enabled && p.handler) p.handler(p.arg); } }
