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

#pragma once

#if defined(USTORE_USE_SPIFFS)

#include "../File.h"
#include "../FileSystem.h"
#include "../Log.h"

#include <SPIFFS.h>

namespace microStore { namespace Adapters {

class SPIFFSFileSystem : public microStore::FileSystem {

public:
	SPIFFSFileSystem() : microStore::FileSystem(new FileSystemImpl()) {}
    virtual ~SPIFFSFileSystem() {}

    // Disable heap allocation
    void* operator new(std::size_t) = delete;
    void* operator new[](std::size_t) = delete;
    void* operator new(std::size_t, void*) = delete;

protected:

	class FileImpl : public microStore::FileImpl {

	private:
		std::unique_ptr<fs::File> _file;
		bool _closed = false;

	public:
		FileImpl(fs::File* file) : microStore::FileImpl(), _file(file) {}
		virtual ~FileImpl() { if (!_closed) close(); }

	public:
		inline virtual const char* name() const { return _file->name(); }
		inline virtual size_t size() const { return _file->size(); }
		inline virtual void close() { _file->close(); _closed = true; }

		inline virtual int read() { return _file->read(); }
		inline virtual size_t write(uint8_t ch) { return _file->write(ch); }
		inline virtual size_t read(uint8_t* buffer, size_t size) { return _file->read((uint8_t*)buffer, size); }
		inline virtual size_t write(const uint8_t* buffer, size_t size) { return _file->write(buffer, size); }

		inline virtual int available() { return _file->available(); }
		inline virtual int peek() { return _file->peek(); }
		inline virtual size_t tell() { return _file->position(); }
		inline virtual long seek(uint32_t pos, microStore::SeekMode mode) {
			fs::SeekMode smode;
			switch (mode) {
				case microStore::SeekMode::SeekModeCur:
					smode = fs::SeekMode::SeekCur;
					break;
				case microStore::SeekMode::SeekModeEnd:
					smode = fs::SeekMode::SeekEnd;
					break;
				case microStore::SeekMode::SeekModeSet:
				default:
					smode = fs::SeekMode::SeekSet;
					break;
			}
			return _file->seek(pos, smode);
		}
		inline virtual void flush() { _file->flush(); }

		inline virtual bool isValid() const { if (!_file) return false; return !_closed; }

	};

	class FileSystemImpl : public microStore::FileSystemImpl {

	public:
		FileSystemImpl() {}
	    virtual ~FileSystemImpl() {}

	public:

		virtual bool format() override {
			USTORE_LOG("[ustore] Formatting SPIFFSFileSystem\n");
			if (!SPIFFS.format()) {
				USTORE_LOG("[ustore] Failed to format SPIFFSFileSystem!\n");
				return false;
			}
			return true;
		}

		virtual bool init(bool reformatOnFail = true) override {
			USTORE_LOG("[ustore] Initializing SPIFFSFileSystem\n");
			// Initialize SPIFFS
			if (!SPIFFS.begin(true, "")) {
				USTORE_LOG("[ustore] Failed to initialize SPIFFSFileSystem!\n");
				return false;
			}
			if (reformatOnFail) {
				// Ensure filesystem is writable and reformat if not
				bool verified = false;
				microStore::File init_test = open("./__init_test__", microStore::File::ModeWrite, true);
				if (init_test) {
					if (init_test.write("test", 4) == 4) {
						verified = true;
					}
					init_test.close();
				}
				if (!verified) {
					USTORE_LOG("[ustore] WARNING: SPIFFSFileSystem check failed, reformatting!\n");
					format();
				}
				else {
					remove("./__init_test__");
					USTORE_LOG("[ustore] SPIFFSFileSystem check passed!\n");
				}
			}
			return true;
		}


		virtual microStore::File open(const char* path, microStore::File::Mode mode, const bool create = false) override {
			const char* pmode;
			switch (mode) {
				// Read only. File must exist. ("r")
				case microStore::File::ModeRead:
					pmode = FILE_READ;
					break;
				// Write only. Creates file or truncates existing file. ("w")
				case microStore::File::ModeWrite:
					pmode = FILE_WRITE;
					break;
				// Append only. Creates file if it doesn’t exist. Writes go to end. ("a")
				case microStore::File::ModeAppend:
					pmode = FILE_APPEND;
					break;
				// Read and write. Creates file or truncates existing file. ("w+")
				case microStore::File::ModeReadWrite:
					pmode = "w+";
					break;
				// Read and append. Creates file if it doesn’t exist. ("a+")
				case microStore::File::ModeReadAppend:
					pmode = "a+";
					break;
				// Read and write. File must exist. ("r+") ???
				default:
					return {};
			}
			// CBA Using copy constructor to obtain File*
			fs::File* file = new fs::File(SPIFFS.open(path, pmode));
			if (file == nullptr || !(*file)) {
				return {};
			}
			return microStore::File(new FileImpl(file));
		}


		virtual bool exists(const char* path) override {
			return SPIFFS.exists(path);
		}

		virtual bool remove(const char* path) override {
			return SPIFFS.remove(path);
		}

		virtual bool rename(const char* from_path, const char* to_path) override {
			return SPIFFS.rename(from_path, to_path);
		}

		virtual bool mkdir(const char* path) override {
			if (!SPIFFS.mkdir(path)) {
				return false;
			}
			return true;
		}

		virtual bool rmdir(const char* path) override {
			if (!SPIFFS.rmdir(path)) {
				return false;
			}
			return true;
		}


		virtual bool isDirectory(const char* path) override {
			fs::File file = SPIFFS.open(path, FILE_READ);
			if (file) {
				bool is_directory = file.isDirectory();
				file.close();
				return is_directory;
			}
			return false;
		}

		virtual std::list<std::string> listDirectory(const char* path, Callbacks::DirectoryListing callback = nullptr) override {
			std::list<std::string> files;
			fs::File root = SPIFFS.open(path);
			if (!root) {
				return files;
			}
			fs::File file = root.openNextFile();
			while (file) {
				char* name = (char*)file.name();
				if (callback) callback(name);
				else files.push_back(name);
				// CBA Following close required to avoid leaking memory
				file.close();
				file = root.openNextFile();
			}
			root.close();
			return files;
		}

		virtual size_t storageSize() override {
			return SPIFFS.totalBytes();
		}

		virtual size_t storageAvailable() override {
			return (SPIFFS.totalBytes() - SPIFFS.usedBytes());
		}

	};

};

} }

#endif
