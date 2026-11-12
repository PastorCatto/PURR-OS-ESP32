/*
 * purr_driver_class.h - what a driver can be, and the interface each kind of driver offers the OS (Drivers/SPEC.md section 6).
 *
 * A driver is a freestanding module (purr_driver_t) that reaches hardware only through the bus table (purr_driver_bus.h). From bus ABI 6 it can also
 * say what KIND of driver it is: it sets `class_id` and points `ops` at one of the tables below, and the OS finds it by class ("give me the GPS")
 * without caring which chip or bus is behind it. Drivers built for ABI 4 and 5 have neither field and keep working exactly as before.
 *
 * Every ops table starts with `uint32_t version`, the table's own version. New functions are only ever added at the end, so an OS that
 * knows version 2 still runs a version-1 driver by never calling what is missing. Every function takes the handle probe() returned. Functions
 * return >= 0 for success (a count where that makes sense) and < 0 for an error. None of them may block longer than the timeout given.
 *
 * Plain C, no ESP-IDF, no libc: the same header is compiled into drivers, the kernel and the host tests.
 */
#ifndef PURR_DRIVER_CLASS_H
#define PURR_DRIVER_CLASS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_DRV_CLASS_NONE   0     /* a driver with no class: sensors, displays and the like keep using read() */
#define PURR_DRV_CLASS_GNSS   1     /* GPS and other satellite positioning */
#define PURR_DRV_CLASS_LORA   2     /* a LoRa packet radio */
#define PURR_DRV_CLASS_MODEM  3     /* a cellular modem (LTE, LTE-M, NB-IoT, GSM) */
#define PURR_DRV_CLASS_WIFI   4     /* a Wi-Fi interface that is not the chip's own */
#define PURR_DRV_CLASS_HCI    5     /* a Bluetooth controller, as an HCI transport */
#define PURR_DRV_CLASS_BLOCK  6     /* a block device: an SD card, an eMMC, a flash chip with sectors */
#define PURR_DRV_CLASS_ETHERNET 7   /* a wired network chip that moves raw Ethernet frames (the kernel's own TCP/IP stack sits on top) */

const char *purr_drv_class_name(uint32_t class_id);   /* "gnss", "lora", "modem", "wifi", "hci", "block", "none", "?" */

/* ---------------------------------------------------------------- GNSS (GPS) */

typedef struct {
    uint8_t valid;              /* a position fix is held (the receiver says it has one) */
    uint8_t quality;            /* 0 no fix, 1 GPS, 2 differential, 4/5 RTK */
    uint8_t sats_used, sats_view;
    uint8_t time_valid;         /* year..second below are the satellites' UTC time */
    uint8_t month, day, hour, minute, second;
    uint16_t year;
    int32_t lat_e7, lon_e7;     /* degrees x 10^7, south and west negative */
    int32_t alt_mm;             /* above mean sea level */
    uint32_t speed_mmps;        /* over the ground */
    int32_t course_cdeg;        /* degrees x 100, true north */
    uint16_t hdop_x10;          /* horizontal dilution of precision x 10 (0 = unknown) */
    uint32_t age_ms;            /* how long ago the fix was last updated, by the driver's clock */
} purr_gnss_fix_t;

typedef struct {
    uint32_t version;                                       /* 1 */
    int (*power)(void *h, int on);                          /* receiver on or off (a power pin, or a command); 0 or < 0 */
    int (*poll)(void *h);                                   /* reads what the receiver has sent and parses it; the number of sentences handled */
    int (*get_fix)(void *h, purr_gnss_fix_t *out);          /* poll(), then the latest fix: 1 if valid, 0 if there is none yet, < 0 on error */
    int (*set_rate_hz)(void *h, int hz);                    /* position updates per second; 0 or < 0 */
} purr_gnss_ops_t;

/* ---------------------------------------------------------------- LoRa */

typedef struct {
    uint32_t freq_hz;           /* carrier, 150 MHz to 960 MHz depending on the chip */
    uint32_t bw_hz;             /* 7800 ... 500000 */
    uint8_t sf;                 /* spreading factor 5 ... 12 */
    uint8_t cr;                 /* coding rate 5 ... 8, meaning 4/5 ... 4/8 */
    int8_t tx_dbm;              /* transmit power; the driver clamps it to what the chip allows */
    uint8_t crc_on;
    uint8_t implicit_header;
    uint8_t reserved;
    uint16_t preamble;          /* symbols */
    uint16_t sync_word;         /* 0x1424 private, 0x3444 public (LoRaWAN) */
} purr_lora_cfg_t;

typedef struct {
    int16_t rssi_dbm;           /* of the packet received */
    int8_t snr_db;
    uint8_t reserved;
} purr_lora_rxinfo_t;

/* What the radio has heard, for the status bar. `heard` is 0 until a packet has been received since the driver started. */
typedef struct {
    uint8_t heard;
    int8_t last_snr_db;
    int16_t last_rssi_dbm;      /* of the last packet received */
    uint32_t last_age_ms;       /* how long ago it arrived */
    uint32_t rx_count;          /* packets received since the driver started */
} purr_lora_status_t;

typedef struct {
    uint32_t version;                                       /* 1; 2 adds status (a version 1 table ends at sleep: never read past it) */
    int (*configure)(void *h, const purr_lora_cfg_t *cfg);  /* applies a configuration; 0 or < 0 */
    int (*send)(void *h, const uint8_t *data, size_t len, uint32_t timeout_ms);       /* one packet; 0 when it has gone out */
    int (*receive)(void *h, uint8_t *buf, size_t cap, uint32_t timeout_ms, purr_lora_rxinfo_t *info);  /* bytes of one packet, 0 if none arrived in time */
    int (*sleep)(void *h);                                  /* lowest power until the next call */
    int (*status)(void *h, purr_lora_status_t *out);        /* version 2: what the radio last heard; 0 or < 0. Never touches the chip. */
} purr_lora_ops_t;

/* ---------------------------------------------------------------- sockets, shared by the offload devices (a modem, an AT Wi-Fi module) */

typedef struct {
    uint32_t version;                                       /* 1 */
    int (*open)(void *h, const char *host, uint16_t port, int tcp, uint32_t timeout_ms);   /* a socket id >= 0, or < 0 */
    int (*send)(void *h, int sock, const uint8_t *data, size_t len);                        /* bytes sent, or < 0 */
    int (*recv)(void *h, int sock, uint8_t *buf, size_t cap, uint32_t timeout_ms);          /* bytes received, 0 if none in time, < 0 closed or error */
    int (*close)(void *h, int sock);
} purr_sock_ops_t;

/* ---------------------------------------------------------------- cellular modem */

#define PURR_MODEM_TECH_NONE  0
#define PURR_MODEM_TECH_GSM   1
#define PURR_MODEM_TECH_LTE   2
#define PURR_MODEM_TECH_LTEM  3
#define PURR_MODEM_TECH_NBIOT 4

typedef struct {
    uint8_t registered;         /* on a network (home or roaming) */
    uint8_t roaming;
    uint8_t tech;               /* PURR_MODEM_TECH_* */
    uint8_t data_up;            /* a data session is up */
    int16_t rssi_dbm;           /* 0 when unknown */
    char operator_name[20];
    char imei[18];
    char ip[16];                /* the session's address, dotted, empty when none */
} purr_modem_status_t;

typedef struct {
    uint32_t version;                                       /* 1 */
    int (*power)(void *h, int on);                          /* power key and reset line, then wait for the modem to answer; 0 or < 0 */
    int (*status)(void *h, purr_modem_status_t *out);       /* 0 or < 0 */
    int (*connect)(void *h, const char *apn, const char *user, const char *pass);   /* bring the data session up; 0 or < 0 */
    int (*disconnect)(void *h);
    int (*at)(void *h, const char *cmd, char *resp, size_t cap, uint32_t timeout_ms);  /* a raw command and its reply, for diagnostics; length or < 0 */
    const purr_sock_ops_t *sockets;                         /* the modem's own sockets, or NULL */
} purr_modem_ops_t;

/* ---------------------------------------------------------------- Wi-Fi that is not the chip's own */

typedef struct {
    char ssid[33];
    int8_t rssi_dbm;
    uint8_t channel;
    uint8_t secure;             /* 0 open, 1 any security */
} purr_wifi_ap_t;

typedef struct {
    uint8_t connected;
    int8_t rssi_dbm;
    uint8_t channel;
    uint8_t reserved;
    char ssid[33];
    char ip[16];
} purr_wifi_status_t;

typedef struct {
    uint32_t version;                                       /* 1 */
    int (*power)(void *h, int on);
    int (*scan)(void *h, purr_wifi_ap_t *out, int max, uint32_t timeout_ms);       /* how many were found, or < 0 */
    int (*connect)(void *h, const char *ssid, const char *pass, uint32_t timeout_ms);   /* 0 once an address is held, or < 0 */
    int (*disconnect)(void *h);
    int (*status)(void *h, purr_wifi_status_t *out);
    const purr_sock_ops_t *sockets;                         /* an offload module's own sockets, or NULL */
    int (*send_frame)(void *h, const uint8_t *frame, size_t len);                    /* a raw Ethernet frame, for a MAC-level chip; NULL when not offered */
    int (*recv_frame)(void *h, uint8_t *buf, size_t cap, uint32_t timeout_ms);       /* a frame, 0 if none in time; NULL when not offered */
} purr_wifi_ops_t;

/* ---------------------------------------------------------------- wired Ethernet, a MAC-level chip (a W5500 in raw-frame mode) */

#define PURR_ETH_LINK_UP   0x01     /* a cable and a partner */
#define PURR_ETH_SPEED_100 0x02     /* 100 Mbit/s (clear: 10) */
#define PURR_ETH_FULL      0x04     /* full duplex (clear: half) */
#define PURR_ETH_MAX_FRAME 1514     /* a frame without its CRC: the longest send_frame() takes and recv_frame() returns */

typedef struct {
    uint32_t version;                                       /* 1 */
    int (*start)(void *h, const uint8_t mac[6]);            /* resets the chip, checks it answers, sets the station address, opens the receive path; 0 or < 0 */
    int (*stop)(void *h);                                   /* closes it; 0 or < 0 */
    int (*link)(void *h);                                   /* PURR_ETH_* bits (0 = no link), or < 0 when the chip does not answer. Cheap: it is polled */
    int (*send_frame)(void *h, const uint8_t *frame, size_t len);              /* one frame, 14 to PURR_ETH_MAX_FRAME bytes, sent before it returns; 0 or < 0 */
    int (*recv_frame)(void *h, uint8_t *buf, size_t cap, uint32_t timeout_ms); /* one frame (its length), 0 if none arrived in time, < 0 on a chip error. A longer frame is cut to cap */
} purr_eth_ops_t;

/* ---------------------------------------------------------------- Bluetooth: an HCI transport */

#define PURR_HCI_COMMAND 1
#define PURR_HCI_ACL     2
#define PURR_HCI_SCO     3
#define PURR_HCI_EVENT   4
#define PURR_HCI_ISO     5

typedef struct {
    uint32_t version;                                       /* 1 */
    int (*reset)(void *h);                                  /* resets the controller (HCI_Reset and waits for it); 0 or < 0 */
    int (*write)(void *h, uint8_t type, const uint8_t *data, size_t len);          /* one HCI packet, `type` is PURR_HCI_*; 0 or < 0 */
    int (*read)(void *h, uint8_t *buf, size_t cap, uint32_t timeout_ms);           /* one packet with its type as the first byte; its length, 0 if none in time */
} purr_hci_ops_t;

/* ---------------------------------------------------------------- block devices (SD cards) */

#define PURR_BLOCK_CARD_NONE 0
#define PURR_BLOCK_CARD_SDSC 1      /* up to 2 GB, byte addressed on the wire */
#define PURR_BLOCK_CARD_SDHC 2      /* SDHC and SDXC */

typedef struct {
    uint32_t sectors;           /* how many */
    uint32_t sector_size;       /* bytes, 512 for an SD card */
    uint8_t removable;
    uint8_t writable;
    uint8_t card;               /* PURR_BLOCK_CARD_* */
    uint8_t reserved;
} purr_block_info_t;

typedef struct {
    uint32_t version;                                       /* 1 */
    /* The device's size, starting the card first if it has not been. 0, or < 0 when there is no card (or it did not answer). A card that was taken out and
     * put back is found again by the next call. */
    int (*info)(void *h, purr_block_info_t *out);
    int (*read)(void *h, uint32_t lba, uint8_t *buf, uint32_t count);               /* count sectors; 0 or < 0 */
    int (*write)(void *h, uint32_t lba, const uint8_t *buf, uint32_t count);        /* 0 or < 0 */
    int (*sync)(void *h);                                   /* waits until the card is done writing */
} purr_block_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* PURR_DRIVER_CLASS_H */
