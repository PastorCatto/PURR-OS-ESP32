#include "power_mgr.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_clk_tree.h"

static const char *TAG = "power_mgr";
static int s_mhz = 240;

void power_mgr_init(int cpu_max_mhz) {
    s_mhz = cpu_max_mhz > 0 ? cpu_max_mhz : 240;
    // CPU frequency is fixed at boot via sdkconfig (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ).
    // Dynamic scaling would require CONFIG_PM_ENABLE; we just report here.
    uint32_t hz = 0;
    esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU,
                                 ESP_CLK_TREE_SRC_FREQ_PRECISION_APPROX, &hz);
    ESP_LOGI(TAG, "cpu ~%u MHz (target %d)", (unsigned)(hz / 1000000), s_mhz);
}

int power_mgr_cpu_mhz(void) { return s_mhz; }
