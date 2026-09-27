
#include "utility.h"

#include <microStore/FileStore.h>
#if defined(USTORE_USE_SD)
#include <microStore/Adapters/SDFileSystem.h>
#elif defined(USTORE_USE_FLASHFS)
#include <microStore/Adapters/FlashFSFileSystem.h>
#else
#include <microStore/Adapters/UniversalFileSystem.h>
#endif

#define REFORMAT_FS

#ifndef PATH_TABLE_MAX_RECS
#define PATH_TABLE_MAX_RECS 0
#endif

#ifndef PATH_TABLE_SEGMENT_SIZE
#define PATH_TABLE_SEGMENT_SIZE 65536
#endif

#ifndef PATH_TABLE_SEGMENT_COUNT
#define PATH_TABLE_SEGMENT_COUNT 8
#endif

using namespace microStore;

typedef enum {
    KVDB_OK = 0,
    KVDB_ERR_INIT,        /* fdb_kvdb_init() failed — check FAL partition */
    KVDB_ERR_NOT_FOUND,   /* key does not exist in the database */
    KVDB_ERR_WRITE,       /* write / delete operation failed */
} kvdb_err_t;

#if defined(USTORE_USE_SD)
microStore::FileSystem filesystem{microStore::Adapters::SDFileSystem(SDCARD_SCLK, SDCARD_MISO, SDCARD_MOSI, SDCARD_CS)};
#elif defined(USTORE_USE_FLASHFS)
static const SPIFlash_Device_t device = RAK15001;
microStore::FileSystem filesystem{microStore::Adapters::FlashFSFileSystem(&device)};
#else
microStore::FileSystem filesystem{microStore::Adapters::UniversalFileSystem()};
#endif
FileStore store(PATH_TABLE_SEGMENT_SIZE, PATH_TABLE_SEGMENT_COUNT);

void make_key(uint8_t key[16], uint32_t v)
{
    memset(key,0,16);
    memcpy(key,&v,sizeof(v));
}

/* -------------------------------------------------- */
/* SHADOW MODEL                                       */
/* -------------------------------------------------- */

struct Value
{
    std::vector<uint8_t> data;
};

std::unordered_map<uint32_t,Value> model;

/* -------------------------------------------------- */
/* VERIFY DATABASE STATE                              */
/* -------------------------------------------------- */

void verify_db(FileStore& db)
{
    uint8_t buf[2048];
    uint16_t len;

    for(auto& it : model)
    {
        //uint8_t key[16];
        //make_key(key, it.first);
        char key[64];
        snprintf(key, sizeof(key), "%lu", it.first);

        bool ok = store.get(key, buf, &len);

        if (!ok)
        {
            printf("ERROR: missing key %lu\n",it.first);
            exit(1);
        }

        if (len != it.second.data.size())
        {
            printf("ERROR: length mismatch\n");
            exit(1);
        }

        if (memcmp(buf, it.second.data.data(), len) != 0)
        {
            printf("ERROR: value mismatch\n");
            exit(1);
        }
    }
}

kvdb_err_t kvdb_clear() {
	store.clear();
	return KVDB_OK;
}

kvdb_err_t kvdb_init(bool clear = false) {

    printf("Initializing filesystem...\n");
    if (!filesystem.init()) {
        printf("ERROR: Failed to initialize filesystem\n");
        return KVDB_ERR_INIT;
    }

    if (clear) printf("Initializing store and clearing storage...\n");
    else printf("Initializing store...\n");
    store.set_max_recs(PATH_TABLE_MAX_RECS);
	if (!store.init(filesystem, "./kvstress", clear)) {
        printf("ERROR: Failed to initialize store\n");
        return KVDB_ERR_INIT;
    }

    printf("FileSystem Size:      %u\n", filesystem.storageSize());
    printf("FileSystem Available: %u\n", filesystem.storageAvailable());

	return KVDB_OK;
}

unsigned long lastCompactMillis = 0;
void kvdb_loop() {
    // CBA TODO Perform compaction when required
/*
    unsigned long currentMillis = millis();
    if ((currentMillis - lastCompactMillis) > 60000) {
        store.compact();
        lastCompactMillis = currentMillis;
    }
*/
}

void kvdb_info() {
    printf("\n");
    store.dumpInfo();
    printf("\n");
}

void kvdb_dump() {
}

kvdb_err_t kvdb_get(const char* key, uint8_t* val, size_t* len) {
    if (!store.get(key, val, (uint16_t*)len)) return KVDB_ERR_NOT_FOUND;
	return KVDB_OK;
}

kvdb_err_t kvdb_get_str(const char* key, char* val, size_t* len) {
	return kvdb_get(key, (uint8_t*)val, len);
}

kvdb_err_t kvdb_set(const char* key, const uint8_t* val, size_t len) {
    if (!store.put(key, val, (uint16_t)len)) return KVDB_ERR_WRITE;
	return KVDB_OK;
}

kvdb_err_t kvdb_set_str(const char* key, const char* val) {
	return kvdb_set(key, (uint8_t*)val, strlen(val));
}

kvdb_err_t kvdb_del(const char* key) {
    if (!store.remove(key)) return KVDB_ERR_NOT_FOUND;
	return KVDB_OK;
}
