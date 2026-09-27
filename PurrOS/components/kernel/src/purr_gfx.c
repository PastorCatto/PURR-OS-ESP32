#include "purr_gfx.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "purr_font.h"

int purr_gfx_text_width(const char *s, int scale)
{
    return (int)strlen(s) * PURR_FONT_W * scale;
}

int purr_gfx_text_height(int scale)
{
    return PURR_FONT_H * scale;
}

esp_err_t purr_gfx_text(const purr_display_v2_t *d, int x, int y, const char *s,
                        uint16_t fg, uint16_t bg, int scale)
{
    if (scale < 1) {
        scale = 1;
    }
    int w = purr_gfx_text_width(s, scale);
    int h = purr_gfx_text_height(scale);
    if (w == 0) {
        return ESP_OK;
    }

    /* Render the whole line into one buffer and send it with a single blit. */
    uint16_t *buf = heap_caps_malloc((size_t)w * h * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; s[i]; i++) {
        const uint8_t *g = purr_font_glyph(s[i]);
        for (int row = 0; row < PURR_FONT_H; row++) {
            for (int col = 0; col < PURR_FONT_W; col++) {
                uint16_t c = (g[row] & (0x80 >> col)) ? fg : bg;
                for (int sy = 0; sy < scale; sy++) {
                    uint16_t *dst = buf + (size_t)(row * scale + sy) * w +
                                    (size_t)(i * PURR_FONT_W + col) * scale;
                    for (int sx = 0; sx < scale; sx++) {
                        dst[sx] = c;
                    }
                }
            }
        }
    }
    esp_err_t e = d->blit(x, y, w, h, buf);
    free(buf);
    return e;
}
