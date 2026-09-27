/*
 * Copyright (c) 2026 Chad Attermann
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

// Only include if building locally and NOT testing
#if defined(LIBRARY_TEST) && !defined(PIO_UNIT_TESTING)
#include <stdio.h>

#if defined(ARDUINO)
#include <Arduino.h>
#if defined(ARDUINO_ARCH_NRF52) || defined(ARDUINO_NRF52_ADAFRUIT)
#include <Adafruit_TinyUSB.h>
#endif
#endif

#include "microStore/File.h"
#include "microStore/FileSystem.h"
#include "microStore/FileStore.h"
#include "microStore/HeapStore.h"
#include "microStore/Codec.h"
#include "microStore/TypedStore.h"

#if defined(USTORE_USE_POSIXFS)
#include "microStore/Adapters/PosixFileSystem.h"
#endif
#if defined(USTORE_USE_STDIOFS)
#include "microStore/Adapters/StdioFileSystem.h"
#endif
#if defined(USTORE_USE_LITTLEFS)
#include "microStore/Adapters/LittleFSFileSystem.h"
#endif
#if defined(USTORE_USE_SPIFFS)
#include "microStore/Adapters/SPIFFSFileSystem.h"
#endif
#if defined(USTORE_USE_INTERNALFS)
#include "microStore/Adapters/InternalFSFileSystem.h"
#endif
#if defined(USTORE_USE_FLASHFS)
#include "microStore/Adapters/FlashFSFileSystem.h"
#endif
#if defined(USTORE_USE_SD)
#include "microStore/Adapters/SDFileSystem.h"
#endif
#if defined(USTORE_USE_UNIVERSALFS)
#include "microStore/Adapters/UniversalFileSystem.h"
#endif
#if defined(USTORE_USE_NOOPFS)
#include "microStore/Adapters/NoopFileSystem.h"
#endif

void setup() {

#if defined(ARDUINO)
	Serial.begin(115200);
	while (!Serial) {
		if (millis() > 5000)
			break;
		delay(500);
	}
	Serial.println("Serial initialized");

#if defined(ESP32)
	Serial.print("Total SRAM: ");
	Serial.println(ESP.getHeapSize());
	Serial.print("Free SRAM: ");
	Serial.println(ESP.getFreeHeap());
#elif defined(ARDUINO_ARCH_NRF52) || defined(ARDUINO_NRF52_ADAFRUIT)
	Serial.print("Total SRAM: ");
	Serial.println(dbgHeapTotal());
	Serial.print("Free SRAM: ");
	Serial.println(dbgHeapFree());
#endif
#endif

#if defined(USTORE_USE_POSIXFS)
	{
		printf("Testing PosixFileSystem...\n");
		microStore::FileSystem filesystem{microStore::Adapters::PosixFileSystem()};
		if (filesystem.init()) {
			printf("size=%u available=%u\n", (uint32_t)filesystem.storageSize(), (uint32_t)filesystem.storageAvailable());
			microStore::FileStore filestore;
			if (filestore.init(filesystem, "./pfs_filestore")) {
				printf("put: foo=bar\n");
				filestore.put("foo", "bar");
				std::string value;
				filestore.get("foo", value);
				printf("got: foo=%s\n", value.c_str());
			}
			filestore.close();
			filestore.clear();
		}
	}
#endif
#if defined(USTORE_USE_STDIOFS)
#endif
#if defined(USTORE_USE_LITTLEFS)
#endif
#if defined(USTORE_USE_SPIFFS)
#endif
#if defined(USTORE_USE_INTERNALFS)
#endif
#if defined(USTORE_USE_FLASHFS)
	{
		printf("Testing FlashFSFileSystem...\n");
		static const SPIFlash_Device_t device = RAK15001;
		microStore::FileSystem filesystem{microStore::Adapters::FlashFSFileSystem(&device)};
		if (filesystem.init()) {
			//filesystem.format();
			printf("size=%u available=%u\n", filesystem.storageSize(), filesystem.storageAvailable());
			microStore::FileStore filestore;
		    if (filestore.init(filesystem, "./ffs_typedstore")) {
				microStore::TypedStore<std::string, std::string, microStore::FileStore> store(filestore);
				printf("put: foo=bar\n");
				store.put("foo", "bar");
				std::string value;
				store.get("foo", value);
				printf("got: foo=%s\n", value.c_str());
			}
			filestore.close();
			filestore.clear();
		}
	}
#endif
#if defined(USTORE_USE_SD)
	{
		printf("Testing SDFileSystem...\n");
		microStore::FileSystem filesystem{microStore::Adapters::SDFileSystem(SDCARD_SCLK, SDCARD_MISO, SDCARD_MOSI, SDCARD_CS)};
		if (filesystem.init()) {
			//filesystem.format();
			printf("size=%u available=%u\n", filesystem.storageSize(), filesystem.storageAvailable());
			microStore::FileStore filestore;
		    if (filestore.init(filesystem, "./sdfs_typedstore")) {
				microStore::TypedStore<std::string, std::string, microStore::FileStore> store(filestore);
				printf("put: foo=bar\n");
				store.put("foo", "bar");
				std::string value;
				store.get("foo", value);
				printf("got: foo=%s\n", value.c_str());
			}
			filestore.close();
			filestore.clear();
		}
	}
#endif
#if defined(USTORE_USE_UNIVERSALFS)
	{
		printf("Testing UniversalFileSystem...\n");
		microStore::FileSystem filesystem{microStore::Adapters::UniversalFileSystem()};
		if (filesystem.init()) {
			//filesystem.format();
			printf("size=%u available=%u\n", (uint32_t)filesystem.storageSize(), (uint32_t)filesystem.storageAvailable());
			microStore::FileStore filestore;
		    if (filestore.init(filesystem, "./ufs_typedstore")) {
				microStore::TypedStore<std::string, std::string, microStore::FileStore> store(filestore);
				printf("put: foo=bar\n");
				store.put("foo", "bar");
				std::string value;
				store.get("foo", value);
				printf("got: foo=%s\n", value.c_str());
			}
			filestore.close();
			filestore.clear();
		}
	}
#endif
#if defined(USTORE_USE_NOOPFS)
#endif

}

void loop() {
}

#if defined(ARDUINO)
int _write(int file, char *ptr, int len) {
    size_t wrote = Serial.write(ptr, len);
    Serial.flush();
    return wrote;
}
#else
int main(void) {
	setup();
	//while (true) {
	//	loop();
	//}
	return 0;
}
#endif

#endif
