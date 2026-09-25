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

#if defined(USTORE_USE_STDIOFS)

#include "../File.h"
#include "../FileSystem.h"
#include "../Log.h"

#if defined(ESP32)
#include <LittleFS.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <dirent.h>

namespace microStore { namespace Adapters {

class  StdioFileSystem : public microStore::FileSystem {

public:
	 StdioFileSystem(const char* basepath = "") : microStore::FileSystem(new  FileSystemImpl(basepath)) {}
    virtual ~ StdioFileSystem() {}

    // Disable heap allocation
    void* operator new(std::size_t) = delete;
    void* operator new[](std::size_t) = delete;
    void* operator new(std::size_t, void*) = delete;

protected:

	class FileImpl : public microStore::FileImpl {

	private:
		FILE* _file = nullptr;
		bool _closed = false;
		size_t _available = 0;
		char _filename[1024];

	public:
		FileImpl(FILE* file) : microStore::FileImpl(), _file(file) { _available = size(); }
		virtual ~FileImpl() { if (!_closed) close(); }

	public:
		inline virtual const char* name() const {
			assert(_file != nullptr);
#if 0
			char proclnk[1024];
			snprintf(proclnk, sizeof(proclnk), "/proc/self/fd/%d", fileno(_file));
			int r = ::readlink(proclnk, _filename, sizeof(_filename));
			if (r < 0) {
				return nullptr);
			}
			_filename[r] = '\0';
			return _filename;
#elif 0
			if (::fcntl(fd, F_GETPATH, _filename) < 0) {
				rerturn nullptr;
			}
			return _filename;
#else
			return nullptr;
#endif
		}

		inline virtual size_t size() const {
			assert(_file != nullptr);
			struct stat st;
			::fstat(fileno(_file), &st);
			return st.st_size;
		}

		inline virtual void close() {
			assert(_file != nullptr);
			::fclose(_file);
			_closed = true;
			_file = nullptr;
		}

		inline virtual int read() {
			if (_available <= 0) {
				return EOF;
			}
			assert(_file != nullptr);
			int ch = ::fgetc(_file);
			if (ch == EOF) {
				return ch;
			}
			--_available;
			return ch;
		}
		inline virtual size_t write(uint8_t ch) {
			assert(_file != nullptr);
			if (::fputc(ch, _file) == EOF) {
				return 0;
			}
			_available = (_available > 0) ? _available - 1 : 0;
			return 1;
		}
		inline virtual size_t read(uint8_t* buffer, size_t size) {
			assert(_file != nullptr);
			size_t read = ::fread(buffer, sizeof(uint8_t), size, _file);
			_available -= read;
			return read;
		}
		inline virtual size_t write(const uint8_t* buffer, size_t size) {
			assert(_file != nullptr);
			size_t wrote = ::fwrite(buffer, sizeof(uint8_t), size, _file);
			_available = (wrote <= _available) ? _available - wrote : 0;
			return wrote;
		}

		inline virtual int available() {
#if 0
			assert(_file != nullptr);
			int size = 0;
			::ioctl(::fileno(_file), FIONREAD, &size);
			return size;
#else
			return _available;
#endif
		}
		inline virtual int peek() {
			if (_available <= 0) {
				return EOF;
			}
			assert(_file != nullptr);
			int ch = ::fgetc(_file);
			::ungetc(ch, _file);
			return ch;
		}
		inline virtual size_t tell() {
			assert(_file != nullptr);
			return (size_t)::ftell(_file);
		}
		inline virtual long seek(uint32_t pos, microStore::SeekMode mode) {
			assert(_file != nullptr);
			int whence;
			switch (mode) {
				case microStore::SeekMode::SeekModeCur:
					whence = SEEK_CUR;
					break;
				case microStore::SeekMode::SeekModeEnd:
					whence = SEEK_END;
					break;
				case microStore::SeekMode::SeekModeSet:
				default:
					whence = SEEK_SET;
					break;
			}
			long result = ::fseek(_file, pos, whence);
			if (result == 0) {
				long new_pos = ::ftell(_file);
				size_t file_size = size();
				_available = (new_pos >= 0 && (size_t)new_pos <= file_size)
				             ? file_size - (size_t)new_pos : 0;
			}
			return result;
		}
		inline virtual void flush() {
			assert(_file != nullptr);
			::fflush(_file);
		}

		inline virtual bool isValid() const { if (_file == nullptr) return false; return !_closed; }

	};

	class FileSystemImpl : public microStore::FileSystemImpl {

	public:
		FileSystemImpl(const char* basepath = "") : _basepath(basepath) {}
	    virtual ~FileSystemImpl() {}

	public:

		virtual bool format() override {
#if defined(ESP32)
			USTORE_LOG("[ustore] Formatting StdioFileSystem\n");
			if (!LittleFS.format()) {
				USTORE_LOG("[ustore] Failed to format StdioFileSystem!\n");
				return false;
			}
			return true;
#else
			return false;
#endif
		}

		inline virtual bool init(bool reformatOnFail = true) override {
			USTORE_LOG("[ustore] Initializing StdioFileSystem\n");
#if defined(ESP32)
			// Initialize LittleFS for POSIX file access
			if (!LittleFS.begin(true, _basepath)) {
				USTORE_LOG("[ustore] Failed to initialize StdioFileSystem!\n");
				return false;
			}
#endif
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
					USTORE_LOG("[ustore] WARNING: StdioFileSystem check failed, reformatting!\n");
					format();
				}
				else {
					remove("./__init_test__");
					USTORE_LOG("[ustore] StdioFileSystem check passed!\n");
				}
			}
			return true;
		}

		virtual microStore::File open(const char* path, microStore::File::Mode mode, const bool create = false) override {
			const char* pmode;
			switch (mode) {
				// Read only. File must exist. ("r")
				case microStore::File::ModeRead:
					pmode = "r";
					break;
				// Write only. Creates file or truncates existing file. ("w")
				case microStore::File::ModeWrite:
					pmode = "w";
					break;
				// Append only. Creates file if it doesn’t exist. Writes go to end. ("a")
				case microStore::File::ModeAppend:
					pmode = "a";
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
			FILE* file = ::fopen(path, pmode);
			if (file == nullptr) {
				return {};
			}
			return microStore::File(new FileImpl(file));
		}


		inline virtual bool exists(const char* path) override {
			FILE* file = ::fopen(path, "r");
			if (file != nullptr) {
				::fclose(file);
				return true;
			}
			return false;
		}

		inline virtual bool remove(const char* path) override {
			return (::remove(path) == 0);
		}

		inline virtual bool rename(const char* from_path, const char* to_path) override {
			return (::rename(from_path, to_path) == 0);
		}

		inline virtual bool isDirectory(const char* path) override {
			struct stat st = {0};
			return (::stat(path, &st) == 0 && S_ISDIR(st.st_mode));
		}

		inline virtual bool mkdir(const char* path) override {
			struct stat st = {0};
			if (::stat(path, &st) == 0) {
				if (S_ISDIR(st.st_mode)) return true;
				return false;
			}
			return (::mkdir(path, 0700) == 0);
		}

		inline virtual bool rmdir(const char* path) override {
			if (::rmdir(path) == 0) {
				return false;
			}
			return true;
		}


		virtual std::list<std::string> listDirectory(const char* path, Callbacks::DirectoryListing callback = nullptr) override {
			std::list<std::string> files;
			DIR *dir = ::opendir(path);
			if (dir == NULL) {
				return files;
			}
			struct dirent *entry;
			while ((entry = ::readdir(dir)) != NULL) {
				if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
					continue;
				}
				char* name = entry->d_name;
				if (callback) callback(name);
				else files.push_back(name);
			}
			::closedir(dir);
			return files;
		}


		inline virtual size_t storageSize() override {
	#if defined(ESP32)
			return LittleFS.totalBytes();
	#else
			return 0;
	#endif
		}

		inline virtual size_t storageAvailable() override {
	#if defined(ESP32)
			return (LittleFS.totalBytes() - LittleFS.usedBytes());
	#else
			return 0;
	#endif
		}

	private:
		const char* _basepath = "";
	};

};

} }

#endif
