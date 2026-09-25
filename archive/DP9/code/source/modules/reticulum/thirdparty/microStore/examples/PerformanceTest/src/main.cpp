/*
 * main.cpp — FlashDB KV storage example
 *
 * Demonstrates persistent key-value storage on MCU internal flash using
 * FlashDB + FAL, targeting both ESP32 (Heltec WiFi LoRa 32 V4) and
 * nRF52840 (RAK4630 / WisBlock RAK4631).
 *
 * What this sketch does
 * ─────────────────────
 *  setup()
 *    1. Initialise FlashDB (formats flash on the very first boot)
 *    2. Increment and persist a boot counter
 *    3. Persist a device name string (set once, readable across reboots)
 *    4. Persist a calibration struct (set once with defaults, updateable)
 *    5. Dump the full database contents to Serial
 *
 *  loop()
 *    6. Every 5 s, update an uptime counter and a simulated sensor reading
 *    7. Every 30 s, print the current database state
 *
 * Flash storage layout
 * ────────────────────
 *  ESP32  (8 MB):  1 MB dedicated "flashdb" partition at 0x300000
 *                  256 sectors × 4 KB = 99.9 % utilisation for KV data
 *  nRF52  (1 MB):  128 KB region 0xD0000–0xEFFFF (32 × 4 KB sectors)
 *                  Safely below the UF2 bootloader at 0xF4000
 */

/*
 * On nRF52840 (Adafruit/RAK BSP) the TinyUSB library provides the Serial
 * (= SerialTinyUSB = Adafruit_USBD_CDC) object.  PlatformIO's LDF only
 * compiles the library when its top-level header is directly included from
 * user code — a transitive pull via Arduino.h is not sufficient.  This
 * include MUST come before Arduino.h so that the CDC class is fully defined
 * before Arduino.h's extern declaration is processed.
 */
#ifdef PLATFORM_NRF52
#include <Adafruit_TinyUSB.h>
#endif

#if defined(ARDUINO)
#include <Arduino.h>
#endif

#include "utility.h"
#include "microStoreTest.h"

/* ── Clear-on-boot option ───────────────────────────────────────────────────
 * Uncomment the line below (or add -DCLEAR_DB_ON_BOOT to build_flags) to
 * wipe the entire FlashDB partition and reinitialise it with factory defaults
 * on the next boot.  Re-comment (or remove the flag) before the next upload
 * so the database is not erased on every subsequent reboot.
 */
//#define CLEAR_DB_ON_BOOT

/* ── Stress-test ────────────────────────────────────────────────────────────
 *
 * Simulates a growing workload by writing and deleting 1 KB records:
 *   • ADD  ~2 records / second  (every 500 ms)
 *   • DEL  ~1 record  / second  (every 1000 ms, random victim)
 *
 * Net effect: +1 KB/s until the FlashDB partition is exhausted, at which
 * point adds stop and only deletes continue.
 *
 * Active record IDs are tracked in the RAM array st_ids[].  The maximum
 * number of records the array can hold is STRESS_MAX_RECORDS; the flash
 * partition will typically fill before that limit is reached.
 */

/* ── Stress-test load profile ───────────────────────────────────────────────
 *
 *  STRESS_ADD_INTERVAL_MS   Minimum gap between successive add operations.
 *                           500 → target 2 adds/s.  0 → add as fast as flash
 *                           allows (every loop tick).
 *
 *  STRESS_DEL_EVERY_N_ADDS  Delete one random record after every N adds.
 *                           2 → 1 del per 2 adds (steady net growth).
 *                           1 → del on every add (zero net growth / GC-only
 *                               endurance test).
 *                           0 → never delete (fill flash as fast as possible).
 *
 * Note: deletion is count-based (not time-based) so the 2:1 ratio is
 * maintained exactly even when flash writes block for >500 ms.
 */
//#define STRESS_ADD_INTERVAL_MS   500   /* ms between add attempts             */
#define STRESS_ADD_INTERVAL_MS   100   /* ms between add attempts             */
#define STRESS_DEL_EVERY_N_ADDS    2   /* delete 1 record per N adds (0=off)  */

struct StressRecord {
    uint32_t id;           /* matches the numeric suffix in the key          */
    uint32_t seq;          /* monotonically increasing write counter         */
    uint8_t  data[1016];   /* padding — filled with (id & 0xFF)             */
};
static_assert(sizeof(StressRecord) == 1024, "StressRecord must be exactly 1024 bytes");

//static const uint16_t STRESS_MAX_RECORDS = 1024; /* RAM tracking ceiling    */
static const uint16_t STRESS_MAX_RECORDS = 16384; /* RAM tracking ceiling    */
static uint32_t st_ids[STRESS_MAX_RECORDS];       /* IDs of live records     */
static uint16_t st_count   = 0;                   /* live record count       */
static uint32_t st_next_id = 0;                   /* next ID to assign       */
static uint32_t st_seq     = 0;                   /* write sequence counter  */
static bool     st_full    = false;               /* DB has rejected a write */
static uint32_t st_get_hits   = 0;   /* kvdb_get() returned KVDB_OK            */
static uint32_t st_get_misses = 0;   /* kvdb_get() returned KVDB_ERR_NOT_FOUND */

static void stress_add()
{
    if (st_full || st_count >= STRESS_MAX_RECORDS) {
        if (!st_full) {
            printf("[stress] RAM tracking limit reached — no more adds.\n");
            st_full = true;
        }
        return;
    }

    char key[20];
    uint32_t id = st_next_id;
    snprintf(key, sizeof(key), "s_%lu", id);

    static StressRecord rec;
    rec.id  = id;
    rec.seq = st_seq++;
    memset(rec.data, (uint8_t)(id & 0xFF), sizeof(rec.data));

    uint32_t t0_add = microStore::millis();
    kvdb_err_t add_rc = kvdb_set(key, (uint8_t*)&rec, sizeof(rec));
    uint32_t add_ms = microStore::millis() - t0_add;

    if (add_rc == KVDB_OK) {
        st_ids[st_count++] = id;
        st_next_id++;
        printf("[stress] ADD  %-12s  id=%-8u  live=%-8u  +%lums\n",
                      key, id, (unsigned)st_count, add_ms);
        /* Persist high-water mark every 10 adds so the next boot can clean up.
         * Writing it on every add would double flash traffic; accepting up to
         * 10 extra records to clean on the next boot is a fine trade-off. */
        if (st_next_id % 10 == 0) {
            kvdb_set("st_hwm", (uint8_t*)&st_next_id, sizeof(st_next_id));
        }
    } else {
        printf("[stress] ADD FAILED %-12s (DB full after %u records,"
                      " ~%u KB used)  +%lums\n",
                      key, (unsigned)st_count,
                      (unsigned)st_count /* each ~1 KB */,
                      add_ms);
        st_full = true;
    }
}

static void stress_del()
{
    if (st_count == 0) return;

    /* Pick a random victim from the live pool */
    uint16_t slot = (uint16_t)(rand_int((int)st_count));
    char key[20];
    uint32_t id = st_ids[slot];
    snprintf(key, sizeof(key), "s_%lu", id);

    uint32_t t0_del = microStore::millis();
    kvdb_err_t rc = kvdb_del(key);
    uint32_t del_ms = microStore::millis() - t0_del;

    if (rc == KVDB_OK || rc == KVDB_ERR_NOT_FOUND) {
        /* Swap-remove: fill the gap with the last element */
        st_ids[slot] = st_ids[--st_count];
        if (rc == KVDB_OK && st_full) {
            /* Space freed — allow adds again */
            st_full = false;
            printf("[stress] DEL  %-12s  id=%-8u  live=%-8u  +%lums  (space freed, adds resume)\n",
                          key, id, (unsigned)st_count, del_ms);
        } else {
            printf("[stress] DEL  %-12s  id=%-8u  live=%-8u  +%lums\n",
                          key, id, (unsigned)st_count, del_ms);
        }
    } else {
        printf("[stress] DEL FAILED %s  +%lums\n", key, del_ms);
    }
}

static void stress_get()
{
    if (st_next_id == 0) return;   /* nothing has been written yet */

    /* Pick a random ID from the full [0, st_next_id) range.
     * ~50% of these will have been deleted, producing the expected miss rate. */
    uint32_t id = (uint32_t)(rand_long((long)st_next_id));
    char key[20];
    snprintf(key, sizeof(key), "s_%lu", id);

    static StressRecord rec;
    size_t len = sizeof(rec);

    uint32_t t0 = microStore::millis();
    kvdb_err_t rc = kvdb_get(key, (uint8_t*)&rec, &len);
    uint32_t get_ms = microStore::millis() - t0;

    if (rc == KVDB_OK) {
        st_get_hits++;
        if (rec.id == id) {
            printf("[stress] GET  %-12s  id=%-8u  HIT     +%lums\n",
                        key, rec.id, get_ms);
        }
        else {
            printf("[stress] GET  %-12s  id=%-8u  CORRUPT +%lums\n",
                        key, rec.id, get_ms);
        }
    } else {
        st_get_misses++;
        printf("[stress] GET  %-12s  id=          MISS    +%lums\n",
                      key, get_ms);
    }
}

/* ── Application data types ────────────────────────────────────────────────*/

struct CalibData {
    float    temp_offset;   /* °C correction applied to raw reading      */
    float    temp_scale;    /* multiplicative gain                       */
    uint16_t adc_zero;      /* ADC counts at 0 °C                        */
    uint16_t crc16;         /* simple integrity check (sum of fields)    */
};

struct SensorReading {
    float    temperature;
    float    humidity;
    uint32_t timestamp_s;   /* seconds since boot                        */
};

/* ── Helpers ────────────────────────────────────────────────────────────────*/

static uint16_t calib_crc(const CalibData &c)
{
    /* Trivial checksum — replace with CRC-16 for production */
    return (uint16_t)((uint32_t)(c.temp_offset * 100)
                    + (uint32_t)(c.temp_scale  * 1000)
                    + c.adc_zero);
}

static void print_banner(void)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════╗\n");
    printf("║  microStore KV Example  — persistent MCU flash KV  ║\n");
#if defined(PLATFORM_ESP32)
    printf("║  Platform: ESP32 (Heltec WiFi LoRa 32 V4)          ║\n");
#elif defined(PLATFORM_NRF52)
    printf("║  Platform: nRF52840 (RAK4630 / WisBlock RAK4631).  ║\n");
#endif
    printf("╚════════════════════════════════════════════════════╝\n");
    printf("\n");
}

static void mem_info() {
#if defined(ESP32)
	printf("\nHeap Stats\n");
	printf("  Heap size:       %u\n", ESP.getHeapSize());
	printf("  Heap free:       %u (%u%% free)\n", ESP.getFreeHeap(), (unsigned)((double)ESP.getFreeHeap() / (double)ESP.getHeapSize() * 100.0));
	//printf("  Heap free:       %u (%u%% free)\n", xPortGetFreeHeapSize(), (unsigned)((double)xPortGetFreeHeapSize() / (double)xPort * 100.0));
	printf("  Heap min free:   %u\n", ESP.getMinFreeHeap());
	//printf("  Heap min free:   %u\n", xPortGetMinimumEverFreeHeapSize());
	printf("  Heap max alloc:  %u (%u%% fragmented)\n", ESP.getMaxAllocHeap(), (unsigned)(100.0 - (double)ESP.getMaxAllocHeap() / (double)ESP.getFreeHeap() * 100.0));
	//printf("  Heap max alloc:  %u (%u%% fragmented)\n", ESP.getMaxAllocHeap(), (unsigned)(100.0 - (double)ESP.getMaxAllocHeap() / (double)xPortGetFreeHeapSize() * 100.0));
	printf("  PSRAM size:      %u\n", ESP.getPsramSize());
	printf("  PSRAM free:      %u (%u%% free)\n", ESP.getFreePsram(), (ESP.getPsramSize() > 0) ? (unsigned)((double)ESP.getFreePsram() / (double)ESP.getPsramSize() * 100.0) : 0);
	printf("  PSRAM min free:  %u\n", ESP.getMinFreePsram());
	printf("  PSRAM max alloc: %u (%u%% fragmented)\n", ESP.getMaxAllocPsram(), (ESP.getFreePsram() > 0) ? (unsigned)(100.0 - (double)ESP.getMaxAllocPsram() / (double)ESP.getFreePsram() * 100.0) : 0);
    printf("\n");
#elif defined(ARDUINO_ARCH_NRF52) || defined(ARDUINO_NRF52_ADAFRUIT)
	printf("\nHeap Stats\n");
    dbgMemInfo();
#endif
}

/* ── setup ──────────────────────────────────────────────────────────────────*/
void setup()
{
#if defined(ARDUINO)
	Serial.begin(115200);
	while (!Serial) {
		if (::millis() > 5000)
			break;
		delay(500);
	}
#endif

    print_banner();

    /* ── 1. Initialise database ───────────────────────────────────────────*/
    printf("[main] Initializing KVDB...\n");
#ifdef CLEAR_DB_ON_BOOT
    printf("[main] CLEAR_DB_ON_BOOT defined — clearing storage...\n");
    if (kvdb_init(true) != KVDB_OK) {
#else
    if (kvdb_init() != KVDB_OK) {
#endif
        printf("[main] ERROR: KVDB init failed — halting.\n");
#ifdef ARDUINO
        while (true) delay(1000);
#else
        exit(1);
#endif
    }
    printf("[main] KVDB ready.\n");

    mem_info();
    kvdb_info();

    /* ── 2. Boot counter ──────────────────────────────────────────────────*/
    uint32_t boot_cnt = 0;
    size_t len = sizeof(boot_cnt);
    kvdb_get("boot_cnt", (uint8_t*)&boot_cnt, &len);
    boot_cnt++;
    kvdb_set("boot_cnt", (uint8_t*)&boot_cnt, len);
    printf("[main] Boot count: %lu\n", boot_cnt);

    /* ── 3. Device name ───────────────────────────────────────────────────*/
    char dev_name[32] = {};
    len = sizeof(dev_name);
    kvdb_get_str("dev_name", dev_name, &len);

    if (strcmp(dev_name, "unnamed") == 0) {
        /* First meaningful boot — personalise the device */
        kvdb_set_str("dev_name", "SensorNode-01");
        kvdb_get_str("dev_name", dev_name, &len);
        printf("[main] Device name set: %s\n", dev_name);
    } else {
        printf("[main] Device name:  %s\n", dev_name);
    }

    /* ── 4. Calibration struct ────────────────────────────────────────────*/
    CalibData cal = {};
    len = sizeof(cal);
    if (kvdb_get("cal", (uint8_t*)&cal, &len) != KVDB_OK
        || cal.crc16 != calib_crc(cal))
    {
        /* No valid calibration found — write factory defaults */
        cal = { 0.50f, 1.020f, 2048, 0 };
        cal.crc16 = calib_crc(cal);
        len = sizeof(cal);
        kvdb_set("cal", (uint8_t*)&cal, len);
        printf("[main] Calibration: factory defaults written.\n");
    } else {
        printf("[main] Calibration: offset=%.2f°C  scale=%.3f"
                      "  adc_zero=%u\n",
                      cal.temp_offset, cal.temp_scale, cal.adc_zero);
    }

    /* ── 5. Database dump ─────────────────────────────────────────────────*/
    printf("\n");
    kvdb_dump();

    /* ── 6. Clean up stress records left on flash from the previous run ───
     *
     * Deleted KV entries are not immediately reclaimed; they remain as
     * physical garbage until FlashDB's GC collects them.  Records from a
     * previous run (s_0 … s_(hwm-1)) therefore still occupy flash sectors
     * and cause GC to fire on every few writes of the current run.
     * Calling kvdb_del() on each one marks them for collection up-front so
     * the GC can discard them without reading or moving them.
     * Keys that were already deleted return KVDB_ERR_NOT_FOUND (harmless).
     * ── */
    uint32_t prev_hwm = 0;
    len = sizeof(prev_hwm);
    kvdb_get("st_hwm", (uint8_t*)&prev_hwm, &len);
    if (prev_hwm > 0) {
/*
        printf("[stress] Cleaning up %lu stress record(s) from previous"
                      " run (s_0 … s_%lu) …\n", prev_hwm, prev_hwm - 1);
        uint32_t cleaned = 0;
        for (uint32_t i = 0; i < prev_hwm; i++) {
            char key[20];
            snprintf(key, sizeof(key), "s_%lu", i);
            if (kvdb_del(key) == KVDB_OK) cleaned++;
        }
        printf("[stress] Cleanup done: %lu record(s) deleted.\n", cleaned);
*/
        printf("[stress] NOTE: Found %lu stress record(s) from previous"
                    " run (s_0 … s_%lu) …\n", prev_hwm, prev_hwm - 1);
    }

    printf("\n");
    printf("[stress] Stress test starting:\n");
    printf("[stress]   ADD 1 record per loop tick (count-based 2:1 ratio)\n");
    printf("[stress]   DB will grow by 1 record every 2 adds until full.\n");
    printf("\n");
}

/* ── loop ───────────────────────────────────────────────────────────────────*/

void loop()
{
    static uint32_t last_sensor_ms = 0;
    static uint32_t last_add_ms    = 0;
    static uint32_t last_report_ms = 0;
    static uint32_t uptime_s       = 0;
    static uint32_t total_adds     = 0;
    static uint32_t total_dels     = 0;
    static uint32_t total_gets     = 0;
    static uint32_t add_count      = 0; /* total adds this session          */

    //try {

    uint32_t now = microStore::millis();

    /* ── Every 5 s: print uptime and sensor reading (no flash writes) ───
     * Writing these two keys every 5 s would leave constant garbage in the
     * KVDB sectors, causing GC to fire after just 2–3 stress records and
     * forcing each GC cycle to move both live copies.  During the stress
     * test we print them but do NOT persist them so the only flash traffic
     * is from the stress records themselves.
     * ── */
    if (now - last_sensor_ms >= 5000UL) {
        last_sensor_ms = now;
        uptime_s += 5;

        float temp = 22.0f + (float)(uptime_s % 10) * 0.3f;
        float hum  = 55.0f + (float)(uptime_s % 7)  * 0.5f;
        //printf("[loop] t=%lus  temp=%.1f°C  hum=%.1f%%\n", uptime_s, temp, hum);
    }

    /* ── Stress: ADD rate-limited by STRESS_ADD_INTERVAL_MS
     *           DEL count-based every STRESS_DEL_EVERY_N_ADDS adds
     *
     * Wall-clock timers can't enforce a fixed add:del ratio when each 1 KB
     * flash write blocks for longer than the timer interval: by the time
     * loop() returns, both the add and del timers have already expired,
     * producing a 1:1 ratio and zero net growth.  Count-based scheduling
     * guarantees the configured ratio regardless of write latency.
     * ── */
    if (now - last_add_ms >= STRESS_ADD_INTERVAL_MS) {
        last_add_ms = now;
        stress_add();
        total_adds++;
        add_count++;

        if (STRESS_DEL_EVERY_N_ADDS > 0 && add_count % STRESS_DEL_EVERY_N_ADDS == 0) {
            stress_del();
            total_dels++;
        }

        stress_get();
        total_gets++;

        /* Refresh now: stress_add/del can block 1–2 s each; the timestamp
         * captured at the top of loop() is too stale to use for the report
         * timer below, causing the 10 s interval to fire late and then
         * immediately again on the following iteration. */
        now = microStore::millis();
    }

    /* ── Every 60 s: log stress-test progress + DB summary ──────────────*/
    if (now - last_report_ms >= 60000UL) {
        printf("[stress] t=%lus  adds=%lu  dels=%lu"
                      "  gets=%lu(hit=%lu miss=%lu)"
                      "  live=%lu  next_id=%lu  full=%ls\n",
                      uptime_s,
                      total_adds, total_dels,
                      total_gets, st_get_hits, st_get_misses,
                      (unsigned)st_count,
                      (unsigned)st_next_id,
                      st_full ? "YES" : "no");
        mem_info();
        kvdb_info();
        last_report_ms = microStore::millis();
    }

    kvdb_loop();

    //}
    //catch (std::exception& e) {
	//	printf("EXCEPTION: %s", e.what());
    //}
}

#if defined(ARDUINO)

int _write(int file, char *ptr, int len) {
    size_t wrote = Serial.write(ptr, len);
    Serial.flush();
    return wrote;
}

#else

int main(void) {
    setup();
    while (true) {
        loop();
    }
    return 0;
}

#endif
