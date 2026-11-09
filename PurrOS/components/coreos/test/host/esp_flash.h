/* A stand-in for ESP-IDF's esp_flash.h, only for test_bootupdate.c: a RAM flash that behaves like the real chip (a write can only clear bits, so an
 * erase is needed first) and can be told to fail. The real API is the same shape. */
#ifndef FAKE_ESP_FLASH_H
#define FAKE_ESP_FLASH_H
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
typedef struct esp_flash_t esp_flash_t;
esp_err_t esp_flash_read(esp_flash_t *chip, void *buffer, uint32_t address, uint32_t length);
esp_err_t esp_flash_erase_region(esp_flash_t *chip, uint32_t start, uint32_t len);
esp_err_t esp_flash_write(esp_flash_t *chip, const void *buffer, uint32_t address, uint32_t length);
#endif
