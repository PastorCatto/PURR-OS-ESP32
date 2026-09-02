// wifi_mgr.c — PURR OS WiFi station-mode manager
//
// esp_wifi_init()/esp_netif_init()/esp_wifi_set_mode(WIFI_MODE_STA) already
// ran once at boot (kernel_tdp_boot.c) — this module starts the driver,
// drives scan/connect/disconnect, and tracks connection status, dispatching
// through purr_kernel_set_wifi_connected() so the status-bar WiFi icon
// (cupcake_ui.c/cardstack_ui.c) reflects real state for the first time.

#include "wifi_mgr.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/core/purr_module.h"
#include "soc/soc_caps.h"
#include "sdkconfig.h"

// Chips with no native radio (esp32p4/tab5): esp_wifi's headers exist but its
// library has no implementations, so any pulled-in esp_wifi_* call is an
// undefined reference at link time — and the Settings app's WiFi window calls
// this module's API unconditionally, which is what pulls it in even on devices
// whose device.pcat never registers wifi_mgr. Compile the whole module down to
// a not-supported stub there instead. When esp-hosted lands (Phase 2 of the
// tab5 port), CONFIG_ESP_WIFI_REMOTE_ENABLED flips this back to the real
// implementation with the C6 co-processor behind the same esp_wifi_* API.
#if !SOC_WIFI_SUPPORTED && !defined(CONFIG_ESP_WIFI_REMOTE_ENABLED)

#include "esp_log.h"

static const char *TAG = "wifi_mgr";

int  wifi_mgr_init(void) {
    ESP_LOGW(TAG, "no WiFi on this chip (and no esp_wifi_remote) — stubbed out");
    return PURR_MODULE_INIT_DECLINED;
}
void wifi_mgr_deinit(void) {}
int  wifi_mgr_scan(void) { return -1; }
int  wifi_mgr_scan_count(void) { return 0; }
bool wifi_mgr_scan_at(int idx, wifi_scan_result_t *out) { (void)idx; (void)out; return false; }
void wifi_mgr_connect(const char *ssid, const char *password) { (void)ssid; (void)password; }
void wifi_mgr_disconnect(void) {}
wifi_mgr_status_t wifi_mgr_status(void) { return WIFI_MGR_FAILED; }
const char *wifi_mgr_ip_str(void) { return ""; }

PURR_MODULE_REGISTER(wifi_mgr) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "wifi_mgr",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = wifi_mgr_init,
    .deinit            = wifi_mgr_deinit,
};

#else  // ── real implementation ─────────────────────────────────────────────

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wifi_mgr";
#define NVS_NS "wifi_sta"
#define MAX_SCAN_RESULTS 24
#define MAX_RETRIES 5
// Same public NTP pool every "protocols/sntp" IDF example defaults to.
// Not user-configurable yet — see wifi_mgr.h's doc comment.
#define SNTP_SERVER "pool.ntp.org"

static wifi_mgr_status_t  s_status = WIFI_MGR_IDLE;
static char               s_ip_str[16] = "";
static int                s_retries = 0;

static wifi_scan_result_t s_scan[MAX_SCAN_RESULTS];
static int                s_scan_count = 0;

static esp_event_handler_instance_t s_wifi_ev_inst;
static esp_event_handler_instance_t s_ip_ev_inst;
// Whether esp_netif_sntp_init() has run this boot — started once, on the
// first successful connection, not re-armed per reconnect. lwIP's SNTP
// client keeps retrying/resyncing on its own schedule for as long as it's
// initialised (CONFIG_LWIP_SNTP_UPDATE_DELAY, default hourly); a dropped
// and restored WiFi connection just means its own DNS/UDP attempts start
// succeeding again, nothing here needs to notice.
static bool s_sntp_started = false;

// ── SNTP ──────────────────────────────────────────────────────────────────────

static void on_sntp_synced(struct timeval *tv) {
    if (!tv) return;
    purr_kernel_time_set(PURR_TIME_SOURCE_NTP, (time_t)tv->tv_sec);
    ESP_LOGI(TAG, "SNTP synced (epoch=%lld)", (long long)tv->tv_sec);
}

static void start_sntp_once(void) {
    if (s_sntp_started) return;
    s_sntp_started = true;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_SERVER);
    cfg.sync_cb = on_sntp_synced;
    esp_err_t ret = esp_netif_sntp_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(ret));
        s_sntp_started = false;   // allow a retry on the next got_ip
    }
}

// ── NVS persistence ───────────────────────────────────────────────────────────

static void save_creds(const char *ssid, const char *password) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", password ? password : "");
    nvs_commit(h);
    nvs_close(h);
}

static bool load_creds(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t slen = ssid_sz, plen = pass_sz;
    esp_err_t r1 = nvs_get_str(h, "ssid", ssid, &slen);
    esp_err_t r2 = nvs_get_str(h, "pass", pass, &plen);
    nvs_close(h);
    return r1 == ESP_OK && r2 == ESP_OK && ssid[0];
}

// ── Event handling ────────────────────────────────────────────────────────────

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)data;
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        purr_kernel_set_wifi_connected(false);
        s_ip_str[0] = '\0';
        if (s_retries < MAX_RETRIES) {
            s_retries++;
            ESP_LOGW(TAG, "disconnected — retry %d/%d", s_retries, MAX_RETRIES);
            esp_wifi_connect();
            s_status = WIFI_MGR_CONNECTING;
        } else {
            ESP_LOGW(TAG, "disconnected — giving up after %d retries", MAX_RETRIES);
            s_status = WIFI_MGR_FAILED;
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)id;
    ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
    snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&evt->ip_info.ip));
    ESP_LOGI(TAG, "connected, IP=%s", s_ip_str);
    s_status  = WIFI_MGR_CONNECTED;
    s_retries = 0;
    purr_kernel_set_wifi_connected(true);
    start_sntp_once();
}

// ── Public API ────────────────────────────────────────────────────────────────

int wifi_mgr_scan(void) {
    wifi_scan_config_t cfg = { 0 };
    esp_err_t ret = esp_wifi_scan_start(&cfg, true);   // blocking
    if (ret != ESP_OK) { ESP_LOGW(TAG, "scan_start failed: %s", esp_err_to_name(ret)); return -1; }

    uint16_t n = MAX_SCAN_RESULTS;
    wifi_ap_record_t recs[MAX_SCAN_RESULTS];
    esp_wifi_scan_get_ap_records(&n, recs);

    s_scan_count = n;
    for (int i = 0; i < n; i++) {
        strncpy(s_scan[i].ssid, (const char *)recs[i].ssid, sizeof(s_scan[i].ssid) - 1);
        s_scan[i].ssid[sizeof(s_scan[i].ssid) - 1] = '\0';
        s_scan[i].rssi    = recs[i].rssi;
        s_scan[i].secured = (recs[i].authmode != WIFI_AUTH_OPEN);
    }
    return s_scan_count;
}

int  wifi_mgr_scan_count(void) { return s_scan_count; }

bool wifi_mgr_scan_at(int idx, wifi_scan_result_t *out) {
    if (idx < 0 || idx >= s_scan_count || !out) return false;
    *out = s_scan[idx];
    return true;
}

void wifi_mgr_connect(const char *ssid, const char *password) {
    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (password) strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);

    esp_wifi_disconnect();   // in case already connected/connecting elsewhere
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    s_retries = 0;
    s_status  = WIFI_MGR_CONNECTING;
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(ret));
        s_status = WIFI_MGR_FAILED;
        return;
    }
    // Persisted optimistically — on_ip_event only confirms the connection
    // came up, it doesn't need to also handle first-time persistence.
    save_creds(ssid, password);
}

void wifi_mgr_disconnect(void) {
    esp_wifi_disconnect();
    s_status = WIFI_MGR_IDLE;
    s_ip_str[0] = '\0';
    purr_kernel_set_wifi_connected(false);
}

wifi_mgr_status_t wifi_mgr_status(void) { return s_status; }
const char *wifi_mgr_ip_str(void)       { return s_ip_str; }

// ── Module lifecycle ──────────────────────────────────────────────────────────

// Brings up the underlying WiFi driver (STA mode, not connected to
// anything) if it isn't already — this file's own top comment used to
// assume kernel_tdp_boot.c (tdeck_plus's specialized boot file) always
// did this first, which is real and true THERE, but nothing did it on a
// device using the generic boot.c (confirmed: zero WiFi-related calls in
// it at all) — Heltec included, despite listing wifi_mgr in its own
// [modules] (via apply_radio_companion_defaults(), purrstrap.py, same
// server=true condition that already adds proximity). Real, confirmed
// consequence: wifi_mgr_init() below called esp_wifi_start() straight
// into a driver that was never initialized, and Server Manager's own
// WIFI_STATUS/SET handlers (server_mgr.c) reported this device as
// permanently "unsupported" as a result — the actual root cause behind
// remote WiFi setup never working on Heltec.
//
// Deliberately duplicates proximity_module.c's own ensure_wifi_ready()
// rather than depending on it — proximity is the radio-companion-
// specific module, wifi_mgr is the more fundamental one, and reaching
// "up" to depend on it would be backwards. Both are small (~15 lines)
// and idempotent (esp_wifi_get_mode() succeeding means someone already
// did this, safe to skip) — cheap enough that keeping them independent
// beats a new cross-module dependency for this little code. On
// tdeck_plus, kernel_tdp_boot.c already did this work, so this sees
// WiFi already up and returns immediately — a no-op, not a behavior
// change there.
static void ensure_wifi_stack_ready(void) {
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK) return;   // already up — nothing to do

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init() failed: %d", (int)err);
        return;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default() failed: %d", (int)err);
        return;
    }
    // Not idempotent — allocates a fresh netif every call — so this only
    // runs if no default STA netif exists yet (same guard proximity_
    // module.c's own copy uses).
    if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
        esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init() failed: %d", (int)err);
        return;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode() failed: %d", (int)err);
    }
    // esp_wifi_start() itself still happens below, in wifi_mgr_init() —
    // not duplicated here, so there's exactly one call site for it.
}

int wifi_mgr_init(void) {
    ensure_wifi_stack_ready();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, &s_wifi_ev_inst));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL, &s_ip_ev_inst));

    esp_err_t ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return -1;
    }

    char ssid[33] = "", pass[65] = "";
    if (load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "auto-reconnecting to saved network '%s'", ssid);
        wifi_mgr_connect(ssid, pass);
    }

    ESP_LOGI(TAG, "init complete");
    return 0;
}

void wifi_mgr_deinit(void) {
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_ev_inst);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_ev_inst);
    if (s_sntp_started) {
        esp_netif_sntp_deinit();
        s_sntp_started = false;
    }
    esp_wifi_stop();
}

// ── .purr module header ───────────────────────────────────────────────────────

PURR_MODULE_REGISTER(wifi_mgr) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "wifi_mgr",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = wifi_mgr_init,
    .deinit            = wifi_mgr_deinit,
};

#endif  // !SOC_WIFI_SUPPORTED && !CONFIG_ESP_WIFI_REMOTE_ENABLED
