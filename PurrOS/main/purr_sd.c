#include "purr_sd.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

int purr_sd_is_path(const char *path)
{
    return path != NULL && strncmp(path, "/sd", 3) == 0 && (path[3] == '\0' || path[3] == '/');
}

#if CONFIG_PURR_PROFILE_FULL

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "diskio_impl.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "purr_driver.h"
#include "purr_driver_class.h"

#define SD_BASE "/sd"
#define SD_DEVICE "sd0"
#define RETRY_US (3 * 1000000LL)
#define CHECK_US (2 * 1000000LL)

static SemaphoreHandle_t s_lock;
static int s_mounted;
static uint8_t s_pdrv = 0xFF;
static FATFS *s_fatfs;
static const purr_block_ops_t *s_ops;
static void *s_handle;
static purr_block_info_t s_info;
static int64_t s_last_try_us = -RETRY_US;
static int64_t s_last_check_us;

static void lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

/* ---------------------------------------------------------------- FatFs over the block driver */

static DSTATUS dio_init(BYTE pdrv)
{
    (void)pdrv;
    return s_ops != NULL ? 0 : STA_NOINIT;
}

static DSTATUS dio_status(BYTE pdrv)
{
    (void)pdrv;
    return s_ops != NULL ? 0 : STA_NOINIT;
}

static DRESULT dio_read(BYTE pdrv, BYTE *buf, DWORD sector, UINT count)
{
    (void)pdrv;
    return s_ops != NULL && s_ops->read(s_handle, sector, buf, count) == 0 ? RES_OK : RES_ERROR;
}

static DRESULT dio_write(BYTE pdrv, const BYTE *buf, DWORD sector, UINT count)
{
    (void)pdrv;
    return s_ops != NULL && s_ops->write(s_handle, sector, buf, count) == 0 ? RES_OK : RES_ERROR;
}

static DRESULT dio_ioctl(BYTE pdrv, BYTE cmd, void *buf)
{
    (void)pdrv;
    if (s_ops == NULL) {
        return RES_NOTRDY;
    }
    switch (cmd) {
    case CTRL_SYNC:
        return s_ops->sync(s_handle) == 0 ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buf = s_info.sectors;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buf = (WORD)s_info.sector_size;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buf = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

static const ff_diskio_impl_t s_dio = {.init = dio_init, .status = dio_status, .read = dio_read, .write = dio_write, .ioctl = dio_ioctl};

/* ---------------------------------------------------------------- mounting */

static void say(char *why, size_t cap, const char *text)
{
    if (why != NULL && cap > 0) {
        snprintf(why, cap, "%s", text);
    }
}

static void unmount_locked(void)
{
    if (s_mounted) {
        char drv[3] = {(char)('0' + s_pdrv), ':', '\0'};
        f_mount(NULL, drv, 0);
        esp_vfs_fat_unregister_path(SD_BASE);
        ff_diskio_unregister(s_pdrv);
        s_fatfs = NULL;
        s_pdrv = 0xFF;
    }
    s_mounted = 0;
    s_ops = NULL;
    s_handle = NULL;
}

static int mount_locked(char *why, size_t cap)
{
    if (s_mounted) {
        return 0;
    }
    purr_class_entry_t e[4];
    int n = purr_class_find(PURR_DRV_CLASS_BLOCK, e, 4);
    int pick = -1;
    for (int i = 0; i < n && i < 4; i++) {
        if (e[i].device != NULL && strcmp(e[i].device, SD_DEVICE) == 0) {
            pick = i;
        }
    }
    if (pick < 0) {
        say(why, cap, "There is no driver for the SD card yet (the \"sd-spi\" driver; Device Manager can fetch it).");
        return -1;
    }
    const purr_block_ops_t *ops = e[pick].ops;
    void *h = e[pick].handle;
    purr_block_info_t info;
    if (ops == NULL || ops->info == NULL || ops->info(h, &info) != 0) {
        say(why, cap, "There is no card in the slot, or it does not answer.");
        return -1;
    }
    if (info.sector_size != 512) {
        say(why, cap, "The card does not use 512-byte sectors.");
        return -1;
    }
    s_ops = ops;
    s_handle = h;
    s_info = info;
    BYTE pdrv;
    if (ff_diskio_get_drive(&pdrv) != ESP_OK) {
        s_ops = NULL;
        say(why, cap, "No drive number is free for the card.");
        return -1;
    }
    ff_diskio_register(pdrv, &s_dio);
    char drv[3] = {(char)('0' + pdrv), ':', '\0'};
    FATFS *fat = NULL;
    if (esp_vfs_fat_register(SD_BASE, drv, 8, &fat) != ESP_OK) {
        ff_diskio_unregister(pdrv);
        s_ops = NULL;
        say(why, cap, "Could not register the card's folder.");
        return -1;
    }
    FRESULT fr = f_mount(fat, drv, 1);
    if (fr != FR_OK) {
        esp_vfs_fat_unregister_path(SD_BASE);
        ff_diskio_unregister(pdrv);
        s_ops = NULL;
        say(why, cap, fr == FR_NO_FILESYSTEM ? "The card has no FAT file system (format it as FAT32 on a computer; exFAT is not supported)."
                                             : "The card's file system could not be read.");
        return -1;
    }
    s_fatfs = fat;
    s_pdrv = pdrv;
    s_mounted = 1;
    s_last_check_us = esp_timer_get_time();
    return 0;
}

int purr_sd_mount(char *why, size_t cap)
{
    lock();
    int rc = mount_locked(why, cap);
    s_last_try_us = esp_timer_get_time();
    unlock();
    return rc;
}

void purr_sd_unmount(void)
{
    lock();
    unmount_locked();
    unlock();
}

int purr_sd_present(void)
{
    int ok;
    lock();
    int64_t now = esp_timer_get_time();
    if (s_mounted) {
        if (now - s_last_check_us >= CHECK_US) {                 /* is it still there? a read of the first sector tells */
            uint8_t sec[512];
            if (s_ops->read(s_handle, 0, sec, 1) != 0 && s_ops->read(s_handle, 0, sec, 1) != 0) {
                unmount_locked();
            }
            s_last_check_us = now;
        }
    } else if (now - s_last_try_us >= RETRY_US) {
        mount_locked(NULL, 0);
        s_last_try_us = now;
    }
    ok = s_mounted;
    unlock();
    return ok;
}

void purr_sd_status(purr_sd_status_t *out)
{
    memset(out, 0, sizeof(*out));
    lock();
    purr_class_entry_t e[4];
    int n = purr_class_find(PURR_DRV_CLASS_BLOCK, e, 4);
    for (int i = 0; i < n && i < 4; i++) {
        if (e[i].device != NULL && strcmp(e[i].device, SD_DEVICE) == 0) {
            out->driver = 1;
        }
    }
    if (!s_mounted) {
        mount_locked(out->why, sizeof(out->why));
        s_last_try_us = esp_timer_get_time();
    }
    if (s_mounted) {
        out->mounted = 1;
        out->card = s_info.card;
        out->sectors = s_info.sectors;
        esp_vfs_fat_info(SD_BASE, &out->total_bytes, &out->free_bytes);
    }
    unlock();
}

/* ---------------------------------------------------------------- files */

int purr_sd_read(const char *path, purr_fs_read_fn cb, void *ctx)
{
    if (!purr_sd_is_path(path) || !purr_sd_present()) {
        return -1;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    uint8_t chunk[512];
    int rc = 0;
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        int stop = cb(ctx, chunk, (uint32_t)n);
        if (stop != 0) {
            rc = stop;
            break;
        }
    }
    if (rc == 0 && ferror(f)) {
        rc = -1;
    }
    fclose(f);
    return rc;
}

int purr_sd_write(const char *path, const void *data, uint32_t len)
{
    if (!purr_sd_is_path(path) || !purr_sd_present()) {
        return -1;
    }
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return -1;
    }
    int rc = 0;
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        rc = -1;
    }
    if (fclose(f) != 0) {
        rc = -1;
    }
    return rc;
}

int purr_sd_mkdir(const char *path)
{
    if (!purr_sd_is_path(path) || !purr_sd_present()) {
        return -1;
    }
    return mkdir(path, 0777) == 0 ? 0 : -1;
}

int purr_sd_remove(const char *path)
{
    if (!purr_sd_is_path(path) || !purr_sd_present()) {
        return -1;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    return (S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path)) == 0 ? 0 : -1;
}

int purr_sd_stat(const char *path, int *is_dir, uint32_t *size)
{
    if (!purr_sd_is_path(path) || !purr_sd_present()) {
        return -1;
    }
    if (strcmp(path, SD_BASE) == 0 || strcmp(path, SD_BASE "/") == 0) {       /* the root of the card has no entry of its own */
        *is_dir = 1;
        *size = 0;
        return 0;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    *is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    *size = (uint32_t)st.st_size;
    return 0;
}

int purr_sd_list(const char *path, purr_fs_list_fn cb, void *ctx)
{
    if (!purr_sd_is_path(path) || !purr_sd_present()) {
        return -1;
    }
    DIR *d = opendir(path);
    if (d == NULL) {
        return -1;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        char full[320];
        struct stat st;
        snprintf(full, sizeof(full), "%s/%.200s", path, de->d_name);
        int is_dir = de->d_type == DT_DIR;
        uint32_t size = 0;
        if (stat(full, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
            size = (uint32_t)st.st_size;
        }
        cb(ctx, de->d_name, is_dir, size);
    }
    closedir(d);
    return 0;
}

#else /* not the full profile: there is no SD card support, and /sd is simply absent */

int purr_sd_mount(char *why, size_t cap)
{
    if (why != NULL && cap > 0) {
        snprintf(why, cap, "%s", "This build has no SD card support.");
    }
    return -1;
}
void purr_sd_unmount(void) {}
int purr_sd_present(void) { return 0; }
void purr_sd_status(purr_sd_status_t *out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->why, sizeof(out->why), "%s", "This build has no SD card support.");
}
int purr_sd_read(const char *path, purr_fs_read_fn cb, void *ctx) { (void)path; (void)cb; (void)ctx; return -1; }
int purr_sd_write(const char *path, const void *data, uint32_t len) { (void)path; (void)data; (void)len; return -1; }
int purr_sd_mkdir(const char *path) { (void)path; return -1; }
int purr_sd_remove(const char *path) { (void)path; return -1; }
int purr_sd_stat(const char *path, int *is_dir, uint32_t *size) { (void)path; (void)is_dir; (void)size; return -1; }
int purr_sd_list(const char *path, purr_fs_list_fn cb, void *ctx) { (void)path; (void)cb; (void)ctx; return -1; }

#endif
