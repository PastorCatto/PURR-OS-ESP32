#include "purr_resetnote_os.h"

#include "esp_log.h"

#include "purr_abi.h"
#include "purr_cfg.h"
#include "purr_cfgstore.h"
#include "purr_netrec.h"
#include "purr_resetnote.h"

static const char *TAG = "resetnote";

void purr_resetnote_record(int why, int detail, int had_data)
{
    purr_netrec_save_resetnote((uint8_t)why, (int16_t)detail, (uint8_t)(had_data != 0));
    purr_flash_t fl;
    purr_cfg_t cfg;
    if (purr_cfgstore_open(&fl) == 0 && purr_cfg_load(&fl, &cfg, NULL) >= 0) {
        purr_cfg_set_flag(&cfg, PURR_CFGF_DATA_RESET);
        purr_cfg_store(&fl, &cfg);
    }
    ESP_LOGW(TAG, "recorded: reason %d, detail %d, storage held data: %d", why, detail, had_data);
}

int purr_resetnote_pending(char *brief, size_t bcap, char *full, size_t fcap)
{
    uint8_t why, had;
    int16_t detail;
    uint32_t seq;
    if (purr_netrec_load_resetnote(&why, &detail, &had, &seq) != 0) {
        return 0;
    }
    purr_resetnote_text(why, detail, had, brief, bcap, full, fcap);
    return 1;
}

void purr_resetnote_dismiss(void)
{
    purr_netrec_clear_resetnote();
    purr_flash_t fl;
    purr_cfg_t cfg;
    if (purr_cfgstore_open(&fl) == 0 && purr_cfg_load(&fl, &cfg, NULL) >= 0) {
        purr_cfg_take_flag(&cfg, PURR_CFGF_DATA_RESET);
        purr_cfg_store(&fl, &cfg);
    }
}
