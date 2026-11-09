#pragma once
#include "esp_err.h"
esp_err_t purr_ui_service_run(void *(*fn)(void *), void *arg, void **result);
