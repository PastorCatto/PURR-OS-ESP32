/* The proximity link as a driver module (plan1.md, "Proximity rebuild"): a driver of class PURR_DRV_CLASS_LINK (purr_driver_class.h) for the virtual device "prox0" the kernel
 * declares on every board. All the work is the link manager in Modules/proximity/purr_px_link.c (host-tested; the PURR OS Mini links the same file); this file only joins it to
 * the link services of the bus table (bus ABI 10): the chip's radio link (ESP-NOW), UDP, a blob of storage, random bytes, the saved Wi-Fi networks, and to micro-ecc for the curve.
 * No libc (px_libc.c has the few functions the core needs).
 *
 * Built with: purrstrap modules build --kind driver --entry proximity_entry --source Modules/drivers/proximity_module.c,Modules/proximity/px_libc.c,Modules/proximity/uecc/px_uecc.c,
 *             Modules/proximity/purr_prox.c,Modules/proximity/purr_px_pair.c,Modules/proximity/purr_px_crypto.c,Modules/proximity/purr_px_link.c
 */
#include <string.h>

#include "purr_driver.h"
#include "purr_driver_bus.h"
#include "purr_driver_class.h"

#include "../proximity/purr_px_link.h"
#include "../proximity/uecc/uECC.h"

static const purr_driver_bus_table_t *s_bus;
static purr_pxl_t s_link;

static int rng_fn(uint8_t *dest, unsigned size) { return s_bus->rand_bytes(dest, size) == 0 ? 1 : 0; }
static void e_random(uint8_t *out, int n) { s_bus->rand_bytes(out, (size_t)n); }
static int e_keypair(uint8_t pub[64], uint8_t priv[32]) { return uECC_make_key(pub, priv, uECC_secp256r1()); }
static int e_shared(const uint8_t pub[64], const uint8_t priv[32], uint8_t out[32]) { return uECC_shared_secret(pub, priv, out, uECC_secp256r1()); }
static int e_sign(const uint8_t priv[32], const uint8_t hash[32], uint8_t sig[64]) { return uECC_sign(priv, hash, 32, sig, uECC_secp256r1()); }
static int e_verify(const uint8_t pub[64], const uint8_t hash[32], const uint8_t sig[64]) { return uECC_verify(pub, hash, 32, sig, uECC_secp256r1()); }
static const purr_px_ecc_t s_ecc = {.random = e_random, .keypair = e_keypair, .shared = e_shared, .sign = e_sign, .verify = e_verify};

/* the bus table's functions, as the link manager's io table wants them (a table the driver fills at probe, because the bus table is only known then) */
static purr_pxl_io_t s_io;

static int l_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    (void)dev;
    (void)board;
    if (s_link.in_use || s_bus->abi_version < 10 || s_bus->rand_bytes == NULL || s_bus->radio_send == NULL || s_bus->store_load == NULL) {
        return -1;
    }
    uECC_set_rng(rng_fn);
    s_io = (purr_pxl_io_t){
        .ecc = &s_ecc, .now_us = s_bus->now_us,
        .radio_send = s_bus->radio_send, .radio_recv = s_bus->radio_recv,
        .udp_open = s_bus->udp_open, .udp_send = s_bus->udp_send, .udp_recv = s_bus->udp_recv, .udp_close = s_bus->udp_close,
        .store_load = s_bus->store_load, .store_save = s_bus->store_save,
        .net_saved_get = s_bus->net_saved_get, .net_saved_add = s_bus->net_saved_add,
        .klass = PURR_PX_CLASS_TDECK, .caps = PURR_PX_CAP_DISPLAY_HOST | PURR_PX_CAP_SHARE, .headless = 0, .name_prefix = "PURR",
    };
    if (purr_pxl_init(&s_link, &s_io) != 0) {
        return -1;
    }
    *out_handle = &s_link;
    return 0;
}

static void l_remove(void *handle)
{
    purr_pxl_shutdown(handle);
}

static const purr_driver_t s_driver = {
    .name = "proximity", .compatible = "purr,proximity",
    .probe = l_probe, .remove = l_remove,
    .bus_abi = PURR_DRIVER_BUS_ABI_VERSION,
    .class_id = PURR_DRV_CLASS_LINK, .ops = &purr_pxl_ops,
};

__attribute__((used))
const purr_driver_t *proximity_entry(const purr_driver_bus_table_t *bus)
{
    s_bus = bus;
    return &s_driver;
}
