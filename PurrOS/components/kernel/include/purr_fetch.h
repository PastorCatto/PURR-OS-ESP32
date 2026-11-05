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

/* Progress of the purr_fetch_alloc() download in progress: bytes received so far, and the total if the server said
 * (0 if it did not). Called from the fetching task after each read; NULL (the default) turns it off. */
typedef void (*purr_fetch_progress_fn)(size_t got, size_t total);
void purr_fetch_set_progress(purr_fetch_progress_fn fn);

/* "<file>: <why>" for the last fetch that failed (ESP error name), or "" if the last one worked: so a failure can say what it was. */
const char *purr_fetch_last_error(void);

/* GETs into a caller-supplied buffer. Fails if the response is larger than cap. */
esp_err_t purr_fetch_into(const char *url, uint8_t *buf, size_t cap, size_t *out_len);

/* Like purr_fetch_into(), for a repository on the local network: plain http or https to the address
 * given, a short timeout, and a redirect is a FAILURE, never followed. (A local server must not be able
 * to send the device to a public host past the local-only rule.) */
esp_err_t purr_fetch_local_into(const char *url, uint8_t *buf, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* PURR_FETCH_H */
