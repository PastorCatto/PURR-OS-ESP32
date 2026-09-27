/*
 * purr_fetch.h - a small blocking HTTPS GET, for the recovery loader (no filesystem, no
 * general-purpose download command yet).
 */
#ifndef PURR_FETCH_H
#define PURR_FETCH_H

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * GETs into a heap_caps_malloc'd (MALLOC_CAP_SPIRAM where there is PSRAM) buffer, growing
 * as needed up to max_len. *out is set on success and must be freed by the caller; left
 * NULL on failure.
 */
esp_err_t purr_fetch_alloc(const char *url, uint8_t **out, size_t *out_len, size_t max_len);

/* GETs into a caller-supplied buffer. Fails if the response is larger than cap. */
esp_err_t purr_fetch_into(const char *url, uint8_t *buf, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* PURR_FETCH_H */
