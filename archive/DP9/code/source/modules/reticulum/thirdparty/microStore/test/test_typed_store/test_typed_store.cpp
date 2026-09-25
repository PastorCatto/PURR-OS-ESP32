/*
 * Unit tests for microStore::TypedStore wrapping a RAM-backed FileStore.
 *
 * TypedStore provides a type-safe key-value interface via Codec<T>
 * specialisations.  Because the underlying FileStore has a pre-existing
 * flush_buffer() bug (put() returns false before calling index_insert()),
 * all tests that write then read use the **two-store pattern**:
 *   1. "Writer" store: TypedStore<> wraps a FileStore that puts data on disk.
 *   2. "Reader" store: TypedStore<> wraps a fresh FileStore initialised from
 *      the same RAM filesystem; rebuild_index_from_segments() populates the
 *      in-memory index automatically.
 * The persistent index file is deleted between writer and reader so the
 * reader is forced to call rebuild_index_from_segments().
 */

#include <unity.h>
#include <microStore/Codec.h>
#include <microStore/TypedStore.h>
#include <microStore/FileStore.h>

#include <cstring>
#include <cstdio>
#include <iterator>
#include <map>
#include <string>
#include <vector>

/* ---- Custom Codec<int32_t> for integer-value test ---- */

namespace microStore {
template<>
struct Codec<int32_t>
{
    static std::vector<uint8_t> encode(int32_t v)
    {
        std::vector<uint8_t> out(sizeof(int32_t));
        memcpy(out.data(), &v, sizeof(int32_t));
        return out;
    }
    static bool decode(const std::vector<uint8_t>& data, int32_t& out)
    {
        if (data.size() < sizeof(int32_t)) return false;
        memcpy(&out, data.data(), sizeof(int32_t));
        return true;
    }
};
} // namespace microStore

/* ---- Minimal RAM filesystem (same boilerplate as test_file_store) ---- */

struct RamFile {
    std::vector<uint8_t> data;
    size_t pos = 0;
    bool open = false;
    bool write_mode = false;
    bool append_mode = false;
};

static RamFile g_files[16];
static char    g_names[16][64];
static int     g_nfiles = 0;

static int find_file(const char* name) {
    for (int i = 0; i < g_nfiles; i++)
        if (strcmp(g_names[i], name) == 0) return i;
    return -1;
}

class RamFileImpl : public microStore::FileImpl {
    int _idx;
    bool _closed = false;
public:
    RamFileImpl(int idx) : microStore::FileImpl(), _idx(idx) {}
    virtual ~RamFileImpl() { if (!_closed) close(); }
protected:
    virtual const char* name()  const override { return g_names[_idx]; }
    virtual size_t      size()  const override { return g_files[_idx].data.size(); }
    virtual void close() override { g_files[_idx].open = false; _closed = true; }
    virtual int read() override {
        RamFile& f = g_files[_idx];
        if (f.pos >= f.data.size()) return EOF;
        return f.data[f.pos++];
    }
    virtual size_t write(uint8_t ch) override {
        RamFile& f = g_files[_idx];
        if (f.append_mode) f.pos = f.data.size();
        if (f.pos >= f.data.size()) f.data.resize(f.pos + 1);
        f.data[f.pos++] = ch;
        return 1;
    }
    virtual size_t read(uint8_t* buffer, size_t size) override {
        RamFile& f = g_files[_idx];
        size_t avail = f.data.size() - f.pos;
        size_t n = (size < avail) ? size : avail;
        memcpy(buffer, f.data.data() + f.pos, n);
        f.pos += n;
        return n;
    }
    virtual size_t write(const uint8_t* buffer, size_t size) override {
        RamFile& f = g_files[_idx];
        if (f.append_mode) f.pos = f.data.size();
        size_t need = f.pos + size;
        if (f.data.size() < need) f.data.resize(need);
        memcpy(f.data.data() + f.pos, buffer, size);
        f.pos += size;
        return size;
    }
    virtual int  available() override { return (int)(g_files[_idx].data.size() - g_files[_idx].pos); }
    virtual int  peek()      override { RamFile& f=g_files[_idx]; return (f.pos<f.data.size())?f.data[f.pos]:EOF; }
    virtual size_t tell()    override { return g_files[_idx].pos; }
    virtual long seek(uint32_t pos, microStore::SeekMode mode) override {
        RamFile& f = g_files[_idx];
        size_t new_pos;
        switch (mode) {
            case microStore::SeekModeEnd: new_pos = (size_t)((long)f.data.size() + (long)pos); break;
            case microStore::SeekModeCur: new_pos = (size_t)((long)f.pos + (long)pos); break;
            default:                      new_pos = (size_t)pos; break;
        }
        f.pos = new_pos;
        return (long)new_pos;
    }
    virtual void flush() override {}
    virtual bool isValid() const override { return !_closed; }
};

class RamFileSystemImpl : public microStore::FileSystemImpl {
protected:
    virtual microStore::File open(const char* path, microStore::File::Mode mode, const bool create = false) override {
        bool wr = (mode == microStore::File::ModeWrite || mode == microStore::File::ModeReadWrite);
        bool ap = (mode == microStore::File::ModeAppend || mode == microStore::File::ModeReadAppend);
        int idx = find_file(path);
        if (wr) {
            if (idx < 0) { if (g_nfiles >= 16) return {}; idx = g_nfiles++; strncpy(g_names[idx], path, 63); g_names[idx][63] = '\0'; }
            g_files[idx].data.clear(); g_files[idx].pos = 0; g_files[idx].open = true;
            g_files[idx].write_mode = true; g_files[idx].append_mode = false;
        } else if (ap) {
            if (idx < 0) { if (g_nfiles >= 16) return {}; idx = g_nfiles++; strncpy(g_names[idx], path, 63); g_names[idx][63] = '\0'; }
            g_files[idx].open = true; g_files[idx].write_mode = true; g_files[idx].append_mode = true;
            g_files[idx].pos = g_files[idx].data.size();
        } else {
            if (idx < 0) return {};
            g_files[idx].pos = 0; g_files[idx].open = true;
            g_files[idx].write_mode = false; g_files[idx].append_mode = false;
        }
        (void)create;
        return microStore::File(new RamFileImpl(idx));
    }
    virtual bool exists(const char* path) override { return find_file(path) >= 0; }
    virtual bool remove(const char* path) override {
        int idx = find_file(path);
        if (idx < 0) return false;
        g_files[idx].data.clear(); g_files[idx].pos = 0;
        for (int i = idx; i < g_nfiles - 1; i++) { g_files[i] = g_files[i+1]; strncpy(g_names[i], g_names[i+1], 63); }
        g_nfiles--;
        return true;
    }
    virtual bool rename(const char* src, const char* dst) override {
        int si = find_file(src); if (si < 0) return false;
        int di = find_file(dst); if (di >= 0) remove(dst);
        di = find_file(src); strncpy(g_names[di], dst, 63); g_names[di][63] = '\0';
        return true;
    }
    virtual bool mkdir(const char* path) override { (void)path; return true; }
    virtual bool rmdir(const char* path) override { (void)path; return true; }
    virtual bool isDirectory(const char* path) override { (void)path; return false; }
    virtual std::list<std::string> listDirectory(const char* path, Callbacks::DirectoryListing cb = nullptr) override {
        (void)path; (void)cb; return {};
    }
    virtual size_t storageSize()      override { return 0; }
    virtual size_t storageAvailable() override { return 0; }
};

static void reset_ram_fs() {
    for (int i = 0; i < g_nfiles; i++) { g_files[i].data.clear(); g_files[i].pos = 0; g_files[i].open = false; }
    g_nfiles = 0;
}
static microStore::FileSystem make_ram_fs() { return microStore::FileSystem{new RamFileSystemImpl()}; }

static void remove_ram_file(const char* name) {
    int idx = find_file(name);
    if (idx < 0) return;
    g_files[idx].data.clear(); g_files[idx].pos = 0;
    for (int i = idx; i < g_nfiles - 1; i++) { g_files[i] = g_files[i+1]; strncpy(g_names[i], g_names[i+1], 63); }
    g_nfiles--;
}

/* ---- Type aliases ---- */

using StringStore = microStore::TypedStore<std::string, std::string, microStore::FileStore>;

/* ---- Tests ---- */

void test_typed_store_put_get() {
    reset_ram_fs();

    {
        microStore::FileStore fs_store;
        auto fs = make_ram_fs();
        fs_store.init(fs, "/p");
        StringStore writer(fs_store);
        writer.put("mykey", "hello");
    }
    remove_ram_file("/p_index.dat");

    microStore::FileStore fs_store2;
    auto fs2 = make_ram_fs();
    fs_store2.init(fs2, "/p");
    StringStore reader(fs_store2);

    TEST_ASSERT_EQUAL(1u, reader.size());
    std::string out;
    TEST_ASSERT_TRUE(reader.get("mykey", out));
    TEST_ASSERT_EQUAL_STRING("hello", out.c_str());
}

void test_typed_store_overwrite() {
    reset_ram_fs();

    {
        microStore::FileStore fs_store;
        auto fs = make_ram_fs();
        fs_store.init(fs, "/p");
        StringStore writer(fs_store);
        writer.put("k", "first");
        writer.put("k", "second");
    }
    remove_ram_file("/p_index.dat");

    microStore::FileStore fs_store2;
    auto fs2 = make_ram_fs();
    fs_store2.init(fs2, "/p");
    StringStore reader(fs_store2);

    TEST_ASSERT_EQUAL(1u, reader.size());
    std::string out;
    TEST_ASSERT_TRUE(reader.get("k", out));
    TEST_ASSERT_EQUAL_STRING("second", out.c_str());
}

void test_typed_store_remove() {
    reset_ram_fs();

    {
        microStore::FileStore fs_store;
        auto fs = make_ram_fs();
        fs_store.init(fs, "/p");
        StringStore writer(fs_store);
        writer.put("gone", "val");
        writer.remove("gone");
    }
    remove_ram_file("/p_index.dat");

    microStore::FileStore fs_store2;
    auto fs2 = make_ram_fs();
    fs_store2.init(fs2, "/p");
    StringStore reader(fs_store2);

    TEST_ASSERT_EQUAL(0u, reader.size());
    TEST_ASSERT_FALSE(reader.exists("gone"));
    std::string out;
    TEST_ASSERT_FALSE(reader.get("gone", out));
}

void test_typed_store_exists() {
    reset_ram_fs();

    {
        microStore::FileStore fs_store;
        auto fs = make_ram_fs();
        fs_store.init(fs, "/p");
        StringStore writer(fs_store);
        writer.put("present", "v");
    }
    remove_ram_file("/p_index.dat");

    microStore::FileStore fs_store2;
    auto fs2 = make_ram_fs();
    fs_store2.init(fs2, "/p");
    StringStore reader(fs_store2);

    TEST_ASSERT_TRUE(reader.exists("present"));
    TEST_ASSERT_FALSE(reader.exists("absent"));
}

void test_typed_store_size() {
    reset_ram_fs();

    {
        microStore::FileStore fs_store;
        auto fs = make_ram_fs();
        fs_store.init(fs, "/p");
        StringStore writer(fs_store);
        writer.put("a", "1");
        writer.put("b", "2");
        writer.put("a", "x");  // overwrite
        writer.remove("b");
    }
    remove_ram_file("/p_index.dat");

    microStore::FileStore fs_store2;
    auto fs2 = make_ram_fs();
    fs_store2.init(fs2, "/p");
    StringStore reader(fs_store2);

    TEST_ASSERT_EQUAL(1u, reader.size());
}

void test_typed_store_iterator() {
    reset_ram_fs();

    {
        microStore::FileStore fs_store;
        auto fs = make_ram_fs();
        fs_store.init(fs, "/p");
        StringStore writer(fs_store);
        writer.put("alpha", "AAA");
        writer.put("beta",  "BB");
        writer.put("gamma", "C");
    }
    remove_ram_file("/p_index.dat");

    microStore::FileStore fs_store2;
    auto fs2 = make_ram_fs();
    fs_store2.init(fs2, "/p");
    StringStore reader(fs_store2);

    std::map<std::string, std::string> seen;
    for (auto e : reader) {
        seen[e.key] = e.value;
    }

    TEST_ASSERT_EQUAL(3, (int)seen.size());
    TEST_ASSERT_EQUAL_STRING("AAA", seen["alpha"].c_str());
    TEST_ASSERT_EQUAL_STRING("BB",  seen["beta"].c_str());
    TEST_ASSERT_EQUAL_STRING("C",   seen["gamma"].c_str());
}

// TypedStore<string, vector<uint8_t>> round-trips raw binary values.
void test_typed_store_vector_value() {
    reset_ram_fs();

    using BinStore = microStore::TypedStore<std::string, std::vector<uint8_t>, microStore::FileStore>;

    const std::vector<uint8_t> blob = {0x00, 0x01, 0xFE, 0xFF, 0x42};

    {
        microStore::FileStore fs_store;
        auto fs = make_ram_fs();
        fs_store.init(fs, "/p");
        BinStore writer(fs_store);
        writer.put("blob", blob);
    }
    remove_ram_file("/p_index.dat");

    microStore::FileStore fs_store2;
    auto fs2 = make_ram_fs();
    fs_store2.init(fs2, "/p");
    BinStore reader(fs_store2);

    std::vector<uint8_t> out;
    TEST_ASSERT_TRUE(reader.get("blob", out));
    TEST_ASSERT_EQUAL(blob.size(), out.size());
    TEST_ASSERT_EQUAL(0, memcmp(blob.data(), out.data(), blob.size()));
}

// TypedKeyStore<Key, Store> writes only keys; each record consumes 24 fixed bytes
// (20-byte header + 4-byte commit) plus the key.  Exists/remove/iterate work
// through the standard TypedStore surface.
void test_typed_store_keyset() {
    reset_ram_fs();

    using StringStore = microStore::TypedKeyStore<std::string, microStore::FileStore>;

    {
        microStore::FileStore fs_store;
        auto fs = make_ram_fs();
        fs_store.init(fs, "/p");
        StringStore writer(fs_store);
        TEST_ASSERT_TRUE(writer.put("alpha", {}));

        int seg = find_file("/p_seg0.dat");
        TEST_ASSERT_TRUE(seg >= 0);
        TEST_ASSERT_EQUAL(24u + 5u, g_files[seg].data.size());

        TEST_ASSERT_TRUE(writer.put("beta",  {}));
        TEST_ASSERT_TRUE(writer.put("gamma", {}));
    }
    remove_ram_file("/p_index.dat");

    microStore::FileStore fs_store2;
    auto fs2 = make_ram_fs();
    fs_store2.init(fs2, "/p");
    StringStore reader(fs_store2);

    TEST_ASSERT_EQUAL(3u, reader.size());
    TEST_ASSERT_TRUE (reader.exists("alpha"));
    TEST_ASSERT_TRUE (reader.exists("beta"));
    TEST_ASSERT_TRUE (reader.exists("gamma"));
    TEST_ASSERT_FALSE(reader.exists("delta"));

    TEST_ASSERT_TRUE(reader.remove("beta"));
    TEST_ASSERT_FALSE(reader.exists("beta"));
    TEST_ASSERT_EQUAL(2u, reader.size());

    std::map<std::string, int> seen;
    for (auto e : reader) seen[e.key]++;
    TEST_ASSERT_EQUAL(2, (int)seen.size());
    TEST_ASSERT_EQUAL(1, seen["alpha"]);
    TEST_ASSERT_EQUAL(1, seen["gamma"]);
}

// TypedStore with a custom Codec<int32_t> stores and retrieves integer values.
void test_typed_store_custom_int_codec() {
    reset_ram_fs();

    using IntStore = microStore::TypedStore<std::string, int32_t, microStore::FileStore>;

    {
        microStore::FileStore fs_store;
        auto fs = make_ram_fs();
        fs_store.init(fs, "/p");
        IntStore writer(fs_store);
        writer.put("answer",   42);
        writer.put("negative", -1);
    }
    remove_ram_file("/p_index.dat");

    microStore::FileStore fs_store2;
    auto fs2 = make_ram_fs();
    fs_store2.init(fs2, "/p");
    IntStore reader(fs_store2);

    TEST_ASSERT_EQUAL(2u, reader.size());

    int32_t val;
    TEST_ASSERT_TRUE(reader.get("answer", val));
    TEST_ASSERT_EQUAL(42, val);

    TEST_ASSERT_TRUE(reader.get("negative", val));
    TEST_ASSERT_EQUAL(-1, val);
}

/* ---- Main ---- */

void setUp()    {}
void tearDown() {}

int runUnityTests(void) {
    UNITY_BEGIN();
    RUN_TEST(test_typed_store_put_get);
    RUN_TEST(test_typed_store_overwrite);
    RUN_TEST(test_typed_store_remove);
    RUN_TEST(test_typed_store_exists);
    RUN_TEST(test_typed_store_size);
    RUN_TEST(test_typed_store_iterator);
    RUN_TEST(test_typed_store_vector_value);
    RUN_TEST(test_typed_store_keyset);
    RUN_TEST(test_typed_store_custom_int_codec);
    return UNITY_END();
}

// For native dev-platform or for some embedded frameworks
int main(void) {
	return runUnityTests();
}

#ifdef ARDUINO
// For Arduino framework
void setup() {
	// Wait ~2 seconds before the Unity test runner
	// establishes connection with a board Serial interface
	delay(2000);

	runUnityTests();
}
void loop() {}
#endif

// For ESP-IDF framework
void app_main() {
	runUnityTests();
}
