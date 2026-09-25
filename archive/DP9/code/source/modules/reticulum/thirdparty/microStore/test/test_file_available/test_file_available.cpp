#include <unity.h>
#include <microStore/FileSystem.h>
#include <microStore/Adapters/UniversalFileSystem.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char* TEST_FILE = "./test_file_available.bin";
microStore::Adapters::UniversalFileSystem filesystem;

// Create a file with known content (10 bytes) before each test
static void create_test_file() {
    FILE* f = ::fopen(TEST_FILE, "wb");
    const uint8_t data[10] = {0,1,2,3,4,5,6,7,8,9};
    ::fwrite(data, 1, 10, f);
    ::fclose(f);
}

static microStore::File open_test_file(microStore::File::Mode mode) {
    return filesystem.open(TEST_FILE, mode);
}

void remove_test_file() {
    filesystem.remove(TEST_FILE);
}

void setUp() {
    filesystem.init();
    create_test_file();
}

void tearDown() {
    remove_test_file();
}

void test_available_initial() {
    microStore::File f = open_test_file(microStore::File::ModeRead);
    TEST_ASSERT_TRUE(f);
    TEST_ASSERT_EQUAL(10, f.available());
    f.close();
}

void test_available_after_read() {
    microStore::File f = open_test_file(microStore::File::ModeRead);
    TEST_ASSERT_TRUE(f);
    uint8_t buf[3];
    f.read(buf, 3);
    TEST_ASSERT_EQUAL(7, f.available());
    f.close();
}

void test_available_after_seek_set() {
    microStore::File f = open_test_file(microStore::File::ModeRead);
    TEST_ASSERT_TRUE(f);
    f.seek(5, microStore::SeekModeSet);
    TEST_ASSERT_EQUAL(5, f.available());
    f.close();
}

void test_available_after_seek_end() {
    microStore::File f = open_test_file(microStore::File::ModeRead);
    TEST_ASSERT_TRUE(f);
    f.seek(0, microStore::SeekModeEnd);
    TEST_ASSERT_EQUAL(0, f.available());
    f.close();
}

void test_available_after_seek_cur() {
    microStore::File f = open_test_file(microStore::File::ModeRead);
    TEST_ASSERT_TRUE(f);
    f.seek(3, microStore::SeekModeCur);
    TEST_ASSERT_EQUAL(7, f.available());
    f.close();
}

void test_available_after_write_extend() {
    // Create an empty file, write 10 bytes — should be at new EOF
    ::unlink(TEST_FILE);
    microStore::File f = open_test_file(microStore::File::ModeReadWrite);
    TEST_ASSERT_TRUE(f);
    const uint8_t data[10] = {0,1,2,3,4,5,6,7,8,9};
    f.write(data, 10);
    TEST_ASSERT_EQUAL(0, f.available());
    f.close();
}

void test_available_after_write_overwrite() {
    // File has 10 bytes. Seek to start, read 4, write 3 in the middle.
    // Position becomes 7, file stays 10 bytes → available == 3.
    microStore::File f = open_test_file(microStore::File::ModeReadAppend);
    TEST_ASSERT_TRUE(f);
    // ModeReadAppend opens with O_RDWR|O_CREAT|O_APPEND; seek back to start
    f.seek(0, microStore::SeekModeSet);
    uint8_t buf[4];
    f.read(buf, 4);                          // position = 4, available = 6
    const uint8_t patch[3] = {0xAA, 0xBB, 0xCC};
    f.write(patch, 3);                       // position = 7, available = 3
    TEST_ASSERT_EQUAL(3, f.available());
    f.close();
}

void test_available_platform_path() {

#ifdef ARDUINO
    static const char* WRITE_PATH = "/test_file.bin";
    static const char* READ_PATH = "/test_file.bin";
#else
    static const char* WRITE_PATH = "test_file.bin";
    static const char* READ_PATH = "test_file.bin";
#endif

    {
        microStore::File f = filesystem.open(WRITE_PATH, microStore::File::ModeReadAppend);
        TEST_ASSERT_TRUE(f);
        const uint8_t buf[4] = {0xBB, 0xEE, 0xEE, 0xFF};
        int wrote = f.write(buf, 4);
        TEST_ASSERT_EQUAL(4, wrote);
        f.close();
    }

    {
        microStore::File f = filesystem.open(READ_PATH, microStore::File::ModeRead);
        TEST_ASSERT_TRUE(f);
        uint8_t buf[4] = {0xDD, 0xEE, 0xAA, 0xDD};
        uint8_t cmp[4] = {0xBB, 0xEE, 0xEE, 0xFF};
        int read = f.read(buf, 4);
        TEST_ASSERT_EQUAL(4, read);
        TEST_ASSERT_EQUAL(0, memcmp(buf, cmp, 4));
        f.close();
    }

    filesystem.remove(WRITE_PATH);
    filesystem.remove(READ_PATH);
}

void test_available_universal_path() {

    static const char* WRITE_PATH = "./test_file.bin";
    static const char* READ_PATH = "./test_file.bin";

    {
        microStore::File f = filesystem.open(WRITE_PATH, microStore::File::ModeReadAppend);
        TEST_ASSERT_TRUE(f);
        const uint8_t buf[4] = {0xBB, 0xEE, 0xEE, 0xFF};
        int wrote = f.write(buf, 4);
        TEST_ASSERT_EQUAL(4, wrote);
        f.close();
    }

    {
        microStore::File f = filesystem.open(READ_PATH, microStore::File::ModeRead);
        TEST_ASSERT_TRUE(f);
        uint8_t buf[4] = {0xDD, 0xEE, 0xAA, 0xDD};
        uint8_t cmp[4] = {0xBB, 0xEE, 0xEE, 0xFF};
        int read = f.read(buf, 4);
        TEST_ASSERT_EQUAL(4, read);
        TEST_ASSERT_EQUAL(0, memcmp(buf, cmp, 4));
        f.close();
    }

    filesystem.remove(WRITE_PATH);
    filesystem.remove(READ_PATH);
}

void test_available_mixed_path() {

#ifdef ARDUINO
    static const char* WRITE_PATH = "/test_file.bin";
    static const char* READ_PATH = "./test_file.bin";
#else
    static const char* WRITE_PATH = "test_file.bin";
    static const char* READ_PATH = "./test_file.bin";
#endif

    {
        microStore::File f = filesystem.open(WRITE_PATH, microStore::File::ModeReadAppend);
        TEST_ASSERT_TRUE(f);
        const uint8_t buf[4] = {0xBB, 0xEE, 0xEE, 0xFF};
        int wrote = f.write(buf, 4);
        TEST_ASSERT_EQUAL(4, wrote);
        f.close();
    }

    {
        microStore::File f = filesystem.open(READ_PATH, microStore::File::ModeRead);
        TEST_ASSERT_TRUE(f);
        uint8_t buf[4] = {0xDD, 0xEE, 0xAA, 0xDD};
        uint8_t cmp[4] = {0xBB, 0xEE, 0xEE, 0xFF};
        int read = f.read(buf, 4);
        TEST_ASSERT_EQUAL(4, read);
        TEST_ASSERT_EQUAL(0, memcmp(buf, cmp, 4));
        f.close();
    }

    filesystem.remove(WRITE_PATH);
    filesystem.remove(READ_PATH);
}

int runUnityTests(void) {
    UNITY_BEGIN();
    RUN_TEST(test_available_initial);
    RUN_TEST(test_available_after_read);
    RUN_TEST(test_available_after_seek_set);
    RUN_TEST(test_available_after_seek_end);
    RUN_TEST(test_available_after_seek_cur);
    RUN_TEST(test_available_after_write_extend);
    RUN_TEST(test_available_after_write_overwrite);
    RUN_TEST(test_available_platform_path);
    RUN_TEST(test_available_universal_path);
    RUN_TEST(test_available_mixed_path);
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
