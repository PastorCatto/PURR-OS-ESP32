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

#if defined(USTORE_USE_INTERNALFS)

#include "../File.h"
#include "../FileSystem.h"
#include "../Log.h"

#include <InternalFileSystem.h>

namespace microStore { namespace Adapters {

class InternalFSFileSystem : public microStore::FileSystem {

public:
	InternalFSFileSystem() : microStore::FileSystem(new FileSystemImpl()) {}
    virtual ~InternalFSFileSystem() {}

    // Disable heap allocation
    void* operator new(std::size_t) = delete;
    void* operator new[](std::size_t) = delete;
    void* operator new(std::size_t, void*) = delete;

protected:

	class FileImpl : public microStore::FileImpl {

	private:
		std::unique_ptr<Adafruit_LittleFS_Namespace::File> _file;
		bool _closed = false;

	public:
		FileImpl(Adafruit_LittleFS_Namespace::File* file) : microStore::FileImpl(), _file(file) {}
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
		// CBA The InternFileSystem `File` implementation does not expose the `whence` parameter to `File::seek()`,
		// which is required by microStore::FileStore. Since InternFileSystem lives inside the framework-arduinoadafruitnrf52,
		// it is difficult or impossible to cleanly override this behavior without messy patching.
		// The solution is to emulates whence-based seeking using absolute positions computed from `position()` and `size()`.
		// - SeekModeSet — passes pos directly, no change in behavior.
		// - SeekModeEnd — pos=0 (the only way FileStore uses it) correctly seeks to size(). If negative offsets from end were ever needed, pos would need to be a signed type in the interface — but that's not the case here.
		// - SeekModeCur — adds pos to current position. Same unsigned caveat for backward relative seeks, but FileStore never uses this mode.
		// - Return value now matches the interface contract: the new absolute position on success, -1 on failure (Adafruit returns bool).
/*
		inline virtual long seek(uint32_t pos, microStore::SeekMode mode) {
			uint8_t smode;
			switch (mode) {
				case microStore::SeekMode::SeekModeCur:
					smode = Adafruit_LittleFS_Namespace::SEEK_O_CUR;
					break;
				case microStore::SeekMode::SeekModeEnd:
					smode = Adafruit_LittleFS_Namespace::SEEK_O_END;
					break;
				case microStore::SeekMode::SeekModeSet:
				default:
					smode = Adafruit_LittleFS_Namespace::SEEK_O_SET;
					break;
			}
			return _file->seek(pos, smode);
		}
*/
/**/
		inline virtual long seek(uint32_t pos, microStore::SeekMode mode) {
			// Adafruit_LittleFS File::seek() only supports absolute seeks (SEEK_SET).
			// Emulate SEEK_CUR and SEEK_END by computing the absolute target position.
			uint32_t target;
//USTORE_LOG("[ustore] InternalFS: pre-position=%lu\n", _file->position());
			switch (mode) {
				case microStore::SeekMode::SeekModeCur:
					target = _file->position() + pos;
//USTORE_LOG("[ustore] InternalFS: SeekModeCur pos=%lu, target=%lu\n", pos, target);
					break;
				case microStore::SeekMode::SeekModeEnd:
					target = _file->size() + pos;
//USTORE_LOG("[ustore] InternalFS: SeekModeEnd pos=%lu, target=%lu\n", pos, target);
					break;
				case microStore::SeekMode::SeekModeSet:
				default:
					target = pos;
//USTORE_LOG("[ustore] InternalFS: SeekModeSet pos=%lu, target=%lu\n", pos, target);
					break;
			}
			//return _file->seek(target) ? (long)target : -1L;
			long new_pos = _file->seek(target) ? (long)target : -1L;
//USTORE_LOG("[ustore] InternalFS: new_pos=%ld, post-position=%lu\n", new_pos, _file->position());
			return new_pos;
		}
/**/
		inline virtual void flush() { _file->flush(); }

		inline virtual bool isValid() const { if (!_file) return false; return !_closed; }

	};

	class FileSystemImpl : public microStore::FileSystemImpl {

	public:
		FileSystemImpl() {}
	    virtual ~FileSystemImpl() {}

	public:

		virtual bool format() override {
			USTORE_LOG("[ustore] Formatting InternalFSFileSystem\n");
			if (!InternalFS.format()) {
				USTORE_LOG("[ustore] Failed to format InternalFSFileSystem!\n");
				return false;
			}
			return true;
		}

		virtual bool init(bool reformatOnFail = true) override {
			USTORE_LOG("[ustore] Initializing InternalFileSystem\n");
			// Initialize InternalFileSystem
			if (!InternalFS.begin()) {
				USTORE_LOG("[ustore] Failed to initialize InternalFSFileSystem!\n");
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
					USTORE_LOG("[ustore] WARNING: InternalFSFileSystem check failed, reformatting!\n");
					format();
				}
				else {
					remove("./__init_test__");
					USTORE_LOG("[ustore] InternalFSFileSystem check passed!\n");
				}
			}
			return true;
		}


		virtual microStore::File open(const char* path, microStore::File::Mode mode, const bool create = false) override {
			int pmode;
			switch (mode) {
				// Read only. File must exist. ("r")
				case microStore::File::ModeRead:
					pmode = Adafruit_LittleFS_Namespace::FILE_O_READ;
					break;
				// Write only. Creates file or truncates existing file. ("w")
				case microStore::File::ModeWrite:
					pmode = Adafruit_LittleFS_Namespace::FILE_O_WRITE;
					// CBA TODO Replace remove with working truncation
					if (InternalFS.exists(path)) {
						InternalFS.remove(path);
					}
					break;
				// Append only. Creates file if it doesn’t exist. Writes go to end. ("a")
				case microStore::File::ModeAppend:
					// CBA Append is the default write mode for nordicnrf52 LittleFS
					pmode = Adafruit_LittleFS_Namespace::FILE_O_WRITE;
					break;
				// Read and write. Creates file or truncates existing file. ("w+")
				case microStore::File::ModeReadWrite:
					pmode = Adafruit_LittleFS_Namespace::FILE_O_READ | Adafruit_LittleFS_Namespace::FILE_O_WRITE;
					// CBA TODO Replace remove with working truncation
					if (InternalFS.exists(path)) {
						InternalFS.remove(path);
					}
					break;
				// Read and append. Creates file if it doesn’t exist. ("a+")
				case microStore::File::ModeReadAppend:
					// CBA Append is the default write mode for nordicnrf52 LittleFS
					pmode = Adafruit_LittleFS_Namespace::FILE_O_READ | Adafruit_LittleFS_Namespace::FILE_O_WRITE;
					break;
				// Read and write. File must exist. ("r+") ???
				default:
					return {};
			}
			Adafruit_LittleFS_Namespace::File* file = new Adafruit_LittleFS_Namespace::File(InternalFS);
//USTORE_LOG("[ustore] opening file: %s, mode: %u\n", path, pmode);
			if (!file->open(path, pmode)) {
				return {};
			}
			// Seek to beginning to overwrite (this is failing on nrf52)
			//if (mode == microStore::File::ModeWrite) {
			//	file->seek(0);
			//	file->truncate(0);
			//}
			return microStore::File(new FileImpl(file));
		}


		virtual bool exists(const char* path) override {
			return InternalFS.exists(path);
		}

		virtual bool remove(const char* path) override {
			return InternalFS.remove(path);
		}

		virtual bool rename(const char* from_path, const char* to_path) override {
			return InternalFS.rename(from_path, to_path);
		}

		virtual bool mkdir(const char* path) override {
			if (!InternalFS.mkdir(path)) {
				return false;
			}
			return true;
		}

		virtual bool rmdir(const char* path) override {
			if (!InternalFS.rmdir_r(path)) {
				return false;
			}
			return true;
		}


		virtual bool isDirectory(const char* path) override {
			Adafruit_LittleFS_Namespace::File file(InternalFS);
			if (file.open(path, Adafruit_LittleFS_Namespace::FILE_O_READ)) {
				bool is_directory = file.isDirectory();
				file.close();
				return is_directory;
			}
			return false;
		}

		virtual std::list<std::string> listDirectory(const char* path, Callbacks::DirectoryListing callback = nullptr) override {
			std::list<std::string> files;
			Adafruit_LittleFS_Namespace::File root = InternalFS.open(path);
			if (!root) {
				return files;
			}
			Adafruit_LittleFS_Namespace::File file = root.openNextFile();
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


		static int _countLfsBlock(void *p, lfs_block_t block){
			lfs_size_t *size = (lfs_size_t*) p;
			*size += 1;
			return 0;
		}

		static lfs_ssize_t getUsedBlockCount() {
			lfs_size_t size = 0;
			lfs_traverse(InternalFS._getFS(), _countLfsBlock, &size);
			return size;
		}

		static int totalBytes() {
			const lfs_config* config = InternalFS._getFS()->cfg;
			return config->block_size * config->block_count;
		}

		static int usedBytes() {
			const lfs_config* config = InternalFS._getFS()->cfg;
			const int usedBlockCount = getUsedBlockCount();
			return config->block_size * usedBlockCount;
		}

		virtual size_t storageSize()  override{
			//return totalBytes();
			return totalBytes();
		}

		virtual size_t storageAvailable() override {
			//return (totalBytes() - usedBytes());
			return (totalBytes() - usedBytes());
		}

	};

};

} }

#endif
