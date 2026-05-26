// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "CDVD/CDVDcommon.h"
#include "CDVD/IsoFileFormats.h"
#include "common/Pcsx2Defs.h"
#include "common/FileSystem.h"

#include <array>
#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Error;
struct VirtualAfs;

class VirtualIso
{
public:
	VirtualIso();
	~VirtualIso();

	bool Open(const std::string& root_dir, Error* error);
	void Close();
	bool IsOpen() const;
	void ReloadModsAndRebuild();

	s32 ReadSector(u8* buffer, u32 lsn, int mode);
	s32 ReadTrack(u32 lsn, int mode);
	s32 GetBuffer(u8* buffer);

	s32 ReadSubQ(u32 lsn, cdvdSubQ* subq) const;
	s32 GetTN(cdvdTN* Buffer) const;
	s32 GetTD(u8 Track, cdvdTD* Buffer) const;
	s32 GetTOC(void* toc) const;
	s32 GetDiskType() const;
	s32 GetDualInfo(s32* dualType, u32* _layer1start) const;

	u32 GetBlockCount() const;

private:
	static constexpr u32 SECTOR_SIZE = 2048;

	struct Entry
	{
		bool is_dir = false;
		std::string name;
		std::string iso_name;
		std::string iso_path;
		std::string host_path;
		s64 size = 0;
		s64 virtual_size = 0;
		u32 lsn = 0;
		u32 data_size = 0;
		u16 path_table_index = 0;
		std::time_t mtime = 0;
		Entry* parent = nullptr;
		std::vector<Entry*> children;
	};

	struct FileExtent
	{
		u32 start_lsn = 0;
		u32 end_lsn = 0;
		const Entry* entry = nullptr;
	};

	struct MetaSector
	{
		u32 lsn = 0;
		std::array<u8, SECTOR_SIZE> data = {};
	};

	struct CachedFile
	{
		std::string path;
		FileSystem::ManagedCFilePtr file;
	};

	bool BuildTree(const std::string& root_dir, Error* error);
	bool BuildDirectoryContents(Entry* dir, const std::string& host_path, Error* error);
	bool ValidateAndSetIsoName(Entry* entry, Error* error);
	bool AssignPathTableIndices();
	void CollectEntries(Entry* dir);
	bool ValidateSystemCnf(Error* error) const;
	void LoadMods(const std::string& root_dir);

	bool ComputeDirectoryDataSizes();
	u32 ComputeDirectorySizeBytes(const Entry* dir) const;
	u32 ComputeUdfDirectorySizeBytes(const Entry* dir) const;

	void AssignLsns();
	bool BuildMetadata(Error* error);
	void BuildFileExtents();
	void BuildIsoPaths();
	void BuildUdfMetadata();

	bool LoadLayoutFile(const std::string& root_dir, Error* error);
	bool LoadLayoutMetadata(const std::string& root_dir, Error* error);

	void AddMetaSectors(u32 start_lsn, const std::vector<u8>& data);
	void AddMetaSector(u32 lsn, const std::array<u8, SECTOR_SIZE>& data);

	bool ReadRawSector(u8* buffer, u32 lsn);
	bool ReadFileSector(u8* buffer, const FileExtent& extent, u32 lsn);
	std::optional<FileExtent> FindFileForLSN(u32 lsn) const;

	bool GetFileHandleLocked(const std::string& path, std::FILE** fp, Error* error);

	void Clear();

	std::vector<std::unique_ptr<Entry>> m_entries;
	Entry* m_root = nullptr;
	std::vector<Entry*> m_dirs;
	std::vector<Entry*> m_files;
	std::vector<FileExtent> m_file_extents;

	std::vector<MetaSector> m_meta_sectors;
	std::unordered_map<u32, size_t> m_meta_sector_lookup;
	std::unordered_map<std::string, std::pair<u32, u32>> m_layout; // iso_path -> (lsn, size)
	bool m_has_layout_metadata = false;
	bool m_use_layout_files = false;
	bool m_has_udf_metadata = false;
	u32 m_udf_partition_start = 0;

	std::array<u8, CD_FRAMESIZE_RAW> m_readbuffer = {};
	int m_read_mode = 0;

	u32 m_total_sectors = 0;
	s32 m_cdtype = CDVD_TYPE_PS2DVD;
	bool m_is_open = false;

	std::mutex m_file_cache_mutex;
	std::vector<CachedFile> m_file_cache;
	static constexpr size_t FILE_CACHE_LIMIT = 16;

	u32 m_missing_extent_logs = 0;

	std::string m_mods_root;
	std::vector<std::string> m_enabled_mods;
	std::unordered_map<std::string, std::unordered_map<std::string, std::string>> m_afs_replacements;
	std::unordered_map<std::string, std::unique_ptr<VirtualAfs>> m_afs_cache;
};
