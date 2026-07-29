// doom_wad.c — find a WAD on the SD card and load it into PSRAM.
//
// New for PURR OS; there was no equivalent in the Espressif port, which read
// the WAD out of a flash partition and so had nothing to find. See the header
// comment above I_Open in i_system.c for why the bytes moved to PSRAM.

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "doom_wad.h"

static const char *TAG = "doom_wad";

static uint8_t *s_wad  = NULL;
static size_t   s_size = 0;

const uint8_t *doom_wad_data(void) { return s_wad; }
size_t         doom_wad_size(void) { return s_size; }

const char *doom_wad_err_str(doom_wad_err_t e)
{
    switch (e) {
    case DOOM_WAD_OK:          return "ok";
    case DOOM_WAD_ERR_NO_DIR:  return "No " DOOM_WAD_DIR " - is the SD card in?";
    case DOOM_WAD_ERR_NO_WAD:  return "No .wad file in " DOOM_WAD_DIR;
    case DOOM_WAD_ERR_NO_MEM:  return "Not enough PSRAM for this WAD";
    case DOOM_WAD_ERR_READ:    return "Could not read the WAD (card error?)";
    case DOOM_WAD_ERR_BAD_WAD: return "That file is not a DOOM WAD";
    }
    return "Unknown error";
}

static int ends_with_wad(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".wad") == 0;
}

// IWAD vs PWAD is the first four bytes. Checked before committing to a multi-
// megabyte read so that picking the wrong file costs 4 bytes, not 3MB and a
// failure further in.
static int read_magic(const char *path, char magic[5])
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t got = fread(magic, 1, 4, f);
    fclose(f);
    if (got != 4) return 0;
    magic[4] = '\0';
    return 1;
}

doom_wad_err_t doom_wad_load(char *out_name, size_t out_name_sz)
{
    doom_wad_free();

    DIR *dir = opendir(DOOM_WAD_DIR);
    if (!dir) {
        ESP_LOGE(TAG, "cannot open %s", DOOM_WAD_DIR);
        return DOOM_WAD_ERR_NO_DIR;
    }

    // Two candidates: the best IWAD seen, and the first .wad of any kind as a
    // fallback so that a file with an odd header still gets a real attempt (and
    // a real error) rather than being silently skipped.
    char iwad[256] = "";
    char anywad[256] = "";

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!ends_with_wad(ent->d_name)) continue;

        char path[300];
        snprintf(path, sizeof(path), "%s/%s", DOOM_WAD_DIR, ent->d_name);

        if (!anywad[0]) snprintf(anywad, sizeof(anywad), "%s", ent->d_name);

        char magic[5];
        if (read_magic(path, magic) && strcmp(magic, "IWAD") == 0 && !iwad[0]) {
            snprintf(iwad, sizeof(iwad), "%s", ent->d_name);
        }
        ESP_LOGI(TAG, "found %s", ent->d_name);
    }
    closedir(dir);

    const char *chosen = iwad[0] ? iwad : (anywad[0] ? anywad : NULL);
    if (!chosen) {
        ESP_LOGE(TAG, "no .wad in %s", DOOM_WAD_DIR);
        return DOOM_WAD_ERR_NO_WAD;
    }
    if (!iwad[0]) {
        // A PWAD alone cannot start the game — it patches an IWAD. Load it
        // anyway and let PrBoom produce the specific complaint; saying so here
        // makes the log readable when that happens.
        ESP_LOGW(TAG, "%s is not an IWAD - the game may not start", chosen);
    }

    char path[300];
    snprintf(path, sizeof(path), "%s/%s", DOOM_WAD_DIR, chosen);

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 12) {
        ESP_LOGE(TAG, "%s: too small to be a WAD", chosen);
        return DOOM_WAD_ERR_BAD_WAD;
    }
    s_size = (size_t)st.st_size;

    // PSRAM explicitly. This must never come out of internal DRAM: it is
    // megabytes, and internal is the resource speed demon exists to protect.
    s_wad = heap_caps_malloc(s_size, MALLOC_CAP_SPIRAM);
    if (!s_wad) {
        ESP_LOGE(TAG, "no PSRAM for %u bytes (largest free block matters, not total)",
                 (unsigned)s_size);
        s_size = 0;
        return DOOM_WAD_ERR_NO_MEM;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        doom_wad_free();
        return DOOM_WAD_ERR_READ;
    }

    // Chunked rather than one fread of the whole file. FATFS over SPI moves
    // this through a bounce buffer, and a single multi-megabyte request holds
    // the SPI bus for the entire transfer; 32KB at a time keeps each hold
    // short and lets the progress log show that it is making headway rather
    // than appearing hung for several seconds.
    const size_t CHUNK = 32 * 1024;
    size_t done = 0;
    while (done < s_size) {
        size_t want = s_size - done;
        if (want > CHUNK) want = CHUNK;
        size_t got = fread(s_wad + done, 1, want, f);
        if (got == 0) break;
        done += got;
    }
    fclose(f);

    if (done != s_size) {
        ESP_LOGE(TAG, "short read: %u of %u bytes", (unsigned)done, (unsigned)s_size);
        doom_wad_free();
        return DOOM_WAD_ERR_READ;
    }

    if (memcmp(s_wad, "IWAD", 4) != 0 && memcmp(s_wad, "PWAD", 4) != 0) {
        ESP_LOGE(TAG, "%s: bad magic", chosen);
        doom_wad_free();
        return DOOM_WAD_ERR_BAD_WAD;
    }

    if (out_name && out_name_sz) snprintf(out_name, out_name_sz, "%s", chosen);
    ESP_LOGI(TAG, "loaded %s (%u bytes) into PSRAM", chosen, (unsigned)s_size);
    return DOOM_WAD_OK;
}

void doom_wad_free(void)
{
    if (s_wad) heap_caps_free(s_wad);
    s_wad  = NULL;
    s_size = 0;
}
