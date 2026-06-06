// Minimal power/CPU management — pure C.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

void power_mgr_init(int cpu_max_mhz);
int  power_mgr_cpu_mhz(void);

#ifdef __cplusplus
}
#endif
