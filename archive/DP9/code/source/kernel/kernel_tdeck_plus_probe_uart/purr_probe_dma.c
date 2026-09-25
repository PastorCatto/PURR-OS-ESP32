// purr_probe_dma.c — kernel_tdeck_plus_probe_uart
//
// Deliberately a one-line wrapper, NOT a copy. This device differs from
// tdeck_plus_probe by exactly one sdkconfig choice (console on UART0 instead
// of USB-Serial-JTAG), so duplicating ~1500 lines of probe source to express
// that would guarantee the two drift the moment either is touched.
//
// purrstrap selects a kernel by globbing source/kernel/kernel_<device>/*.c,
// which is why this directory has to exist at all. Separate translation units
// rather than one file including all six: several of the probe sources define
// their own static rq()/wq() helpers, which would collide in a single TU.
#include "../kernel_tdeck_plus_probe/purr_probe_dma.c"
