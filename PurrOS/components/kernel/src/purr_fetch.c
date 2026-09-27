#include "purr_fetch.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "fetch";

#define MAX_REDIRECTS 5

/* GitHub (and most release hosting) answers a release asset URL with a redirect to the
 * real object store. esp_http_client only follows redirects on its own inside
 * esp_http_client_perform(); this uses the lower-level open/fetch_headers/read instead
 * (to stream into our own buffer), so redirects are followed by hand here. */
static esp_err_t open_get(const char *url, esp_http_client_handle_t *out, int64_t *content_length)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        /* GitHub's release redirect (to a signed, long-URL S3 object) carries header lines
         * well past a couple of KB; too small a buffer here fails with "Out of buffer". */
        .buffer_size = 8192,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (int hop = 0; ; hop++) {
        esp_err_t e = esp_http_client_open(client, 0);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "%s: could not open (%s)", url, esp_err_to_name(e));
            esp_http_client_cleanup(client);
            return e;
        }
        int64_t len = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            esp_http_client_close(client);
            if (hop >= MAX_REDIRECTS || esp_http_client_set_redirection(client) != ESP_OK) {
                ESP_LOGW(TAG, "%s: too many redirects", url);
                esp_http_client_cleanup(client);
                return ESP_ERR_HTTP_MAX_REDIRECT;
            }
            continue;
        }
        if (status != 200) {
            ESP_LOGW(TAG, "%s: HTTP %d", url, status);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        *out = client;
        *content_length = len;                       /* -1 if the server did not say */
        return ESP_OK;
    }
}

static void close_client(esp_http_client_handle_t client)
{
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

esp_err_t purr_fetch_into(const char *url, uint8_t *buf, size_t cap, size_t *out_len)
{
    esp_http_client_handle_t client;
    int64_t content_length;
    esp_err_t e = open_get(url, &client, &content_length);
    if (e != ESP_OK) {
        return e;
    }
    if (content_length > 0 && (size_t)content_length > cap) {
        ESP_LOGW(TAG, "%s: %lld bytes, more than the %u byte buffer", url,
                 (long long)content_length, (unsigned)cap);
        close_client(client);
        return ESP_ERR_NO_MEM;
    }

    size_t got = 0;
    while (got < cap) {
        int n = esp_http_client_read(client, (char *)buf + got, (int)(cap - got));
        if (n < 0) {
            close_client(client);
            return ESP_FAIL;
        }
        if (n == 0) {
            break;                                  /* the body is done */
        }
        got += (size_t)n;
    }
    close_client(client);
    *out_len = got;
    return ESP_OK;
}

esp_err_t purr_fetch_alloc(const char *url, uint8_t **out, size_t *out_len, size_t max_len)
{
    *out = NULL;
    esp_http_client_handle_t client;
    int64_t content_length;
    esp_err_t e = open_get(url, &client, &content_length);
    if (e != ESP_OK) {
        return e;
    }

    /* A known, sane content length lets us allocate exactly once. Otherwise grow in chunks,
     * bounded by max_len (the manifest is small; this path is not for large images). */
    size_t cap = (content_length > 0 && (size_t)content_length <= max_len)
                     ? (size_t)content_length + 1 : 4096;
    uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = malloc(cap);
    }
    if (buf == NULL) {
        close_client(client);
        return ESP_ERR_NO_MEM;
    }

    size_t got = 0;
    for (;;) {
        if (got == cap) {
            if (cap >= max_len) {
                free(buf);
                close_client(client);
                return ESP_ERR_NO_MEM;
            }
            size_t next = cap * 2 > max_len ? max_len : cap * 2;
            uint8_t *bigger = heap_caps_malloc(next, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (bigger == NULL) {
                bigger = malloc(next);
            }
            if (bigger == NULL) {
                free(buf);
                close_client(client);
                return ESP_ERR_NO_MEM;
            }
            memcpy(bigger, buf, got);
            free(buf);
            buf = bigger;
            cap = next;
        }
        int n = esp_http_client_read(client, (char *)buf + got, (int)(cap - got));
        if (n < 0) {
            free(buf);
            close_client(client);
            return ESP_FAIL;
        }
        if (n == 0) {
            break;
        }
        got += (size_t)n;
    }
    close_client(client);
    *out = buf;
    *out_len = got;
    return ESP_OK;
}
