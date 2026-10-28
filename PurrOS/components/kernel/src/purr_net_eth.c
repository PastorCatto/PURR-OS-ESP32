/*
 * purr_net_eth.c - the wired backend of purr_net.h: a W5500 SPI Ethernet controller through
 * ESP-IDF's esp_eth (plan4.md). Used by the VM flavor, where there is no usable radio, and by
 * any real board that carries a W5500 module.
 *
 * The radio is never touched: no esp_wifi_init, so no PHY bring-up. There is no SSID or password.
 * "connect" waits for the link and a DHCP address; "scan" finds nothing; the saved-network list
 * stays empty.
 */
#include "purr_net.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

#include "purr_board.h"
#include "purr_vm.h"

static const char *TAG = "net-eth";
#define BIT_GOT_IP BIT0

static esp_eth_handle_t s_eth;
static esp_netif_t *s_netif;
static EventGroupHandle_t s_eg;
static volatile uint8_t s_link;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == ETH_EVENT) {
        if (id == ETHERNET_EVENT_CONNECTED) {
            s_link = 1;
            ESP_LOGI(TAG, "link up");
        } else if (id == ETHERNET_EVENT_DISCONNECTED) {
            s_link = 0;
            xEventGroupClearBits(s_eg, BIT_GOT_IP);
            ESP_LOGW(TAG, "link down");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        xEventGroupSetBits(s_eg, BIT_GOT_IP);
    }
}

esp_err_t purr_net_init(purr_fs_t *fs)
{
    (void)fs;                                   /* nothing to save: there are no networks to remember */

    esp_err_t e = esp_netif_init();
    if (e != ESP_OK) {
        return e;
    }
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {      /* already created is fine */
        return e;
    }
    s_eg = xEventGroupCreate();
    if (s_eg == NULL) {
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_PURR_ETH_INT_GPIO >= 0
    e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        return e;
    }
#endif

    /* By default the W5500 sits on the board's shared SPI bus, which the kernel brought up for
     * the display (purr_kernel_init); the driver adds its own device to it. In a VM whose info
     * device says the controller has a bus of its own (purr_vm.h), open that bus instead: sharing
     * the display's bus is what the emulated SPI controller cannot do reliably (plan4.md). */
    const purr_board_t *board = purr_board();
    const purr_vm_info_t *vm = purr_vm_info();
    spi_host_device_t host = (spi_host_device_t)board->spi.host;
    int cs = CONFIG_PURR_ETH_CS_GPIO;
    if (vm->present && (vm->flags & PURR_VM_NIC_OWN_BUS)) {
        spi_bus_config_t bus = {
            .sclk_io_num = vm->nic_sclk, .mosi_io_num = vm->nic_mosi, .miso_io_num = vm->nic_miso,
            .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 1600,
        };
        /* The device reports the controller number (SPI3 = 3); IDF's host enum starts at SPI1 = 0. */
        spi_host_device_t nic_host = (spi_host_device_t)(vm->nic_host - 1);
        e = spi_bus_initialize(nic_host, &bus, SPI_DMA_CH_AUTO);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "the VM's Ethernet SPI bus did not start: %s", esp_err_to_name(e));
            return e;
        }
        host = nic_host;
        cs = vm->nic_cs;
        ESP_LOGI(TAG, "Ethernet controller on its own SPI host %d (VM)", (int)host);
    }
    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = CONFIG_PURR_ETH_SPI_MHZ * 1000 * 1000,
        .spics_io_num = cs,
        .queue_size = 20,
    };
    eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(host, &devcfg);
    w5500.int_gpio_num = CONFIG_PURR_ETH_INT_GPIO;
    w5500.poll_period_ms = CONFIG_PURR_ETH_POLL_MS;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = 1;
    phy_cfg.reset_gpio_num = CONFIG_PURR_ETH_RST_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
    if (mac == NULL || phy == NULL) {
        ESP_LOGE(TAG, "W5500 driver did not start (is the chip answering on CS %d?)", cs);
        return ESP_FAIL;
    }
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    e = esp_eth_driver_install(&eth_cfg, &s_eth);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "W5500 install failed: %s", esp_err_to_name(e));
        return e;
    }

    /* The W5500 has no burned-in MAC address; derive one from the chip's own. */
    uint8_t addr[6];
    esp_read_mac(addr, ESP_MAC_ETH);
    e = esp_eth_ioctl(s_eth, ETH_CMD_S_MAC_ADDR, addr);
    if (e != ESP_OK) {
        return e;
    }

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_cfg);
    if (s_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }
    e = esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_eth));
    if (e != ESP_OK) {
        return e;
    }
    esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID, &on_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_event, NULL, NULL);

    e = esp_eth_start(s_eth);
    if (e == ESP_OK) {
        ESP_LOGI(TAG, "W5500 on SPI host %d, CS %d, %s", (int)host, cs,
                 CONFIG_PURR_ETH_INT_GPIO >= 0 ? "interrupt-driven" : "polling");
    }
    return e;
}

esp_err_t purr_net_scan(purr_net_ap_t *out, int max, int *n)
{
    (void)out;
    (void)max;
    *n = 0;                                      /* a wire has no access points */
    return ESP_OK;
}

esp_err_t purr_net_connect(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    (void)ssid;                                  /* there is no network to pick and no password */
    (void)pass;
    if (s_eg == NULL || s_eth == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    EventBits_t bits = xEventGroupWaitBits(s_eg, BIT_GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & BIT_GOT_IP) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t purr_net_forget(const char *ssid)
{
    (void)ssid;
    return ESP_OK;
}

void purr_net_status(purr_net_status_t *out)
{
    memset(out, 0, sizeof(*out));
    strcpy(out->ip, "0.0.0.0");
    if (s_eg == NULL || !(xEventGroupGetBits(s_eg) & BIT_GOT_IP)) {
        return;
    }
    out->connected = 1;
    snprintf(out->ssid, sizeof(out->ssid), "ethernet");
    esp_netif_ip_info_t ip;
    if (s_netif != NULL && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip.ip));
    }
}

uint8_t purr_net_last_disconnect_reason(void)
{
    return 0;
}

int purr_net_saved(purr_net_ap_t *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}

int purr_net_wired(void)
{
    return 1;
}
