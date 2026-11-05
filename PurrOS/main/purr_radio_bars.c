/* purr_radio_bars.c - see purr_radio_bars.h. */
#include "purr_radio_bars.h"

#include "purr_driver.h"
#include "purr_driver_class.h"
#include "purr_statusicons.h"

int purr_radio_bars(void)
{
    purr_class_entry_t e;
    if (purr_class_find(PURR_DRV_CLASS_LORA, &e, 1) < 1 || e.ops == NULL) {
        return -1;
    }
    const purr_lora_ops_t *ops = e.ops;
    if (ops->version < 2 || ops->status == NULL) {      /* a driver from before the status call: its table ends at sleep */
        return -1;
    }
    purr_lora_status_t st;
    if (ops->status(e.handle, &st) != 0) {
        return -1;
    }
    return purr_si_lora_level(st.heard, st.last_rssi_dbm, st.last_age_ms);
}
