// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "CDVD/VirtualIso.h"
#include "CDVD/IsoReader.h"
#include "common/ByteSwap.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "pcsx2/Host.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <queue>

#include "fmt/format.h"


	static constexpr u32 SECTOR_SIZE = 2048;
	static constexpr u32 ISO_SYSTEM_AREA_SECTORS = 16;
	static constexpr u32 UDF_ANCHOR_LSN_PRIMARY = 256;
	static constexpr u32 UDF_ANCHOR_LSN_SECONDARY = 257;
	static constexpr u32 UDF_MAIN_VDS_LSN = 32;
	static constexpr u32 UDF_VDS_SECTORS = 16;
	static constexpr u32 UDF_RESERVE_VDS_LSN = UDF_MAIN_VDS_LSN + UDF_VDS_SECTORS;
	static constexpr u32 UDF_LVID_LSN = UDF_RESERVE_VDS_LSN + UDF_VDS_SECTORS;
	static constexpr u16 UDF_REVISION = 0x0102;

	static u32 AlignUp(u32 value, u32 alignment)
	{
		return (value + alignment - 1) / alignment * alignment;
	}

	static void WriteLE16(u8* p, u16 v)
	{
		p[0] = static_cast<u8>(v & 0xFF);
		p[1] = static_cast<u8>((v >> 8) & 0xFF);
	}

	static void WriteBE16(u8* p, u16 v)
	{
		p[0] = static_cast<u8>((v >> 8) & 0xFF);
		p[1] = static_cast<u8>(v & 0xFF);
	}

	static void WriteLE32(u8* p, u32 v)
	{
		p[0] = static_cast<u8>(v & 0xFF);
		p[1] = static_cast<u8>((v >> 8) & 0xFF);
		p[2] = static_cast<u8>((v >> 16) & 0xFF);
		p[3] = static_cast<u8>((v >> 24) & 0xFF);
	}

	static void WriteBE32(u8* p, u32 v)
	{
		p[0] = static_cast<u8>((v >> 24) & 0xFF);
		p[1] = static_cast<u8>((v >> 16) & 0xFF);
		p[2] = static_cast<u8>((v >> 8) & 0xFF);
		p[3] = static_cast<u8>(v & 0xFF);
	}

	static void WriteBoth16(u8* p, u16 v)
	{
		WriteLE16(p, v);
		WriteBE16(p + 2, v);
	}

	static void WriteBoth32(u8* p, u32 v)
	{
		WriteLE32(p, v);
		WriteBE32(p + 4, v);
	}

	static u32 ReadLE32(const u8* p)
	{
		return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
			(static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
	}

	static void WriteRecordingDateTime(u8* p, std::time_t t)
	{
		std::tm tm = {};
#ifdef _MSC_VER
		localtime_s(&tm, &t);
#else
		localtime_r(&t, &tm);
#endif
		p[0] = static_cast<u8>(tm.tm_year);
		p[1] = static_cast<u8>(tm.tm_mon + 1);
		p[2] = static_cast<u8>(tm.tm_mday);
		p[3] = static_cast<u8>(tm.tm_hour);
		p[4] = static_cast<u8>(tm.tm_min);
		p[5] = static_cast<u8>(tm.tm_sec);
		p[6] = 0;
	}

	static void WritePVDDateTime(IsoReader::ISOPVDDateTime* dt, std::time_t t)
	{
		std::tm tm = {};
#ifdef _MSC_VER
		localtime_s(&tm, &t);
#else
		localtime_r(&t, &tm);
#endif
		char temp[5] = {};
		std::snprintf(temp, sizeof(temp), "%04d", tm.tm_year + 1900);
		std::memcpy(dt->year, temp, 4);
		std::snprintf(temp, sizeof(temp), "%02d", tm.tm_mon + 1);
		std::memcpy(dt->month, temp, 2);
		std::snprintf(temp, sizeof(temp), "%02d", tm.tm_mday);
		std::memcpy(dt->day, temp, 2);
		std::snprintf(temp, sizeof(temp), "%02d", tm.tm_hour);
		std::memcpy(dt->hour, temp, 2);
		std::snprintf(temp, sizeof(temp), "%02d", tm.tm_min);
		std::memcpy(dt->minute, temp, 2);
		std::snprintf(temp, sizeof(temp), "%02d", tm.tm_sec);
		std::memcpy(dt->second, temp, 2);
		std::memcpy(dt->milliseconds, "00", 2);
		dt->gmt_offset = 0;
	}

	static bool IsValidIsoNameChar(char c)
	{
		if (c >= 'A' && c <= 'Z')
			return true;
		if (c >= '0' && c <= '9')
			return true;
		switch (c)
		{
			case '-':
			case '_':
			case '.':
			case ' ':
				return true;
			default:
				return false;
		}
	}

	static std::string SanitizeIsoName(const std::string_view name)
	{
		std::string out = StringUtil::toUpper(name);
		for (char& c : out)
		{
			if (!IsValidIsoNameChar(c))
				c = '_';
		}
		return out;
	}

	static size_t DirectoryRecordSize(u8 id_len)
	{
		size_t len = 33 + id_len;
		if ((id_len % 2) == 0)
			len++;
		return len;
	}

	static u16 UdfCrc16(const u8* data, size_t len)
	{
		u16 crc = 0;
		for (size_t i = 0; i < len; i++)
		{
			crc ^= static_cast<u16>(data[i] << 8);
			for (int bit = 0; bit < 8; bit++)
			{
				if (crc & 0x8000)
					crc = static_cast<u16>((crc << 1) ^ 0x1021);
				else
					crc <<= 1;
			}
		}
		return crc;
	}

	static void WriteUdfTag(u8* sector, u16 tag_id, u32 lsn, u16 crc_len, u16 crc)
	{
		WriteLE16(sector + 0, tag_id);
		WriteLE16(sector + 2, 2); // Descriptor Version
		sector[4] = 0; // checksum (filled later)
		sector[5] = 0;
		WriteLE16(sector + 6, 1); // Tag Serial Number
		WriteLE16(sector + 8, crc);
		WriteLE16(sector + 10, crc_len);
		WriteLE32(sector + 12, lsn);

		u8 checksum = 0;
		for (int i = 0; i < 16; i++)
		{
			if (i == 4)
				continue;
			checksum = static_cast<u8>(checksum + sector[i]);
		}
		sector[4] = checksum;
	}

	static void FinalizeUdfDescriptor(u8* sector, u16 tag_id, u32 lsn, u16 descriptor_len)
	{
		const u16 crc_len = static_cast<u16>(descriptor_len - 16);
		const u16 crc = UdfCrc16(sector + 16, crc_len);
		WriteUdfTag(sector, tag_id, lsn, crc_len, crc);
	}

	static void WriteUdfDstring(u8* p, size_t max_len, const std::string& value, bool utf16)
	{
		std::memset(p, 0, max_len);

		if (value.empty() || value == "\0")
		{
			p[0] = utf16 ? 16 : 8;
			return;
		}

		p[0] = utf16 ? 16 : 8;
		size_t out_pos = 1;
		if (utf16)
		{
			for (char c : value)
			{
				p[out_pos++] = 0;
				p[out_pos++] = static_cast<u8>(c);
				if (out_pos >= max_len)
					break;
			}
		}
		else
		{
			const size_t to_copy = std::min(value.size(), max_len - 1);
			std::memcpy(p + 1, value.data(), to_copy);
			out_pos = 1 + to_copy;
		}

		const size_t min_len = std::min(max_len, out_pos);
		if (max_len > min_len)
			p[max_len - 1] = static_cast<u8>(min_len);
	}

	static u32 UdfDstringDataLength(const std::string& value, bool utf16)
	{
		if (value.empty() || value == "\0")
			return 1;
		const u32 bytes = utf16 ? static_cast<u32>(value.size() * 2) : static_cast<u32>(value.size());
		return 1 + bytes;
	}

	static u32 UdfFileIdentifierSize(const std::string& name)
	{
		const u32 data_len = UdfDstringDataLength(name, true);
		return AlignUp(38 + data_len, 4);
	}

	static void WriteUdfCharSpec(u8* p, const char* spec)
	{
		std::memset(p, 0, 64);
		p[0] = 0; // CharacterSetType = 0
		const size_t len = std::min<size_t>(63, std::strlen(spec));
		std::memcpy(p + 1, spec, len);
	}

	static void WriteUdfEntityIdentifier(u8* p, const std::string& identifier, u16 udf_revision, u8 flags, bool udf_suffix_le)
	{
		std::memset(p, 0, 32);
		p[0] = flags;
		const size_t len = std::min<size_t>(23, identifier.size());
		std::memcpy(p + 1, identifier.data(), len);
		if (udf_suffix_le)
		{
			WriteLE16(p + 24, udf_revision);
		}
		else
		{
			WriteBE16(p + 24, udf_revision);
		}
	}

	static void WriteUdfTimeStamp(u8* p)
	{
		std::time_t now = std::time(nullptr);
		std::tm tm = {};
#ifdef _MSC_VER
		gmtime_s(&tm, &now);
#else
		gmtime_r(&now, &tm);
#endif
		WriteLE16(p + 0, 0x1000);
		WriteLE16(p + 2, static_cast<u16>(tm.tm_year + 1900));
		p[4] = static_cast<u8>(tm.tm_mon + 1);
		p[5] = static_cast<u8>(tm.tm_mday);
		p[6] = static_cast<u8>(tm.tm_hour);
		p[7] = static_cast<u8>(tm.tm_min);
		p[8] = static_cast<u8>(tm.tm_sec);
		p[9] = 0;
		p[10] = 0;
		p[11] = 0;
	}

	static void WriteUdfLogicalBlockAddress(u8* p, u32 lbn, u16 partition)
	{
		WriteLE32(p + 0, lbn);
		WriteLE16(p + 4, partition);
	}

	static void WriteUdfShortAD(u8* p, u32 length, u32 lbn)
	{
		const u32 len = length; // flags in upper 2 bits are zero
		WriteLE32(p + 0, len);
		WriteLE32(p + 4, lbn);
	}

	static void WriteUdfLongAD(u8* p, u32 length, u32 lbn, u16 partition)
	{
		WriteLE32(p + 0, length);
		WriteUdfLogicalBlockAddress(p + 4, lbn, partition);
		std::memset(p + 10, 0, 6);
	}

	static void WriteUdfAnchor(u8* sector, u32 lsn)
	{
		// Minimal Anchor Volume Descriptor Pointer (AVDP).
		std::memset(sector, 0, SECTOR_SIZE);
		WriteLE32(sector + 16, UDF_VDS_SECTORS * SECTOR_SIZE);
		WriteLE32(sector + 20, UDF_MAIN_VDS_LSN);
		WriteLE32(sector + 24, UDF_VDS_SECTORS * SECTOR_SIZE);
		WriteLE32(sector + 28, UDF_RESERVE_VDS_LSN);

		FinalizeUdfDescriptor(sector, 2, lsn, SECTOR_SIZE);
	}

	static void WriteUdfTerminatingDescriptor(u8* sector, u32 lsn)
	{
		std::memset(sector, 0, SECTOR_SIZE);
		FinalizeUdfDescriptor(sector, 8, lsn, SECTOR_SIZE);
	}

	static std::string NormalizePathKey(const std::string& path)
	{
		std::string out = Path::Canonicalize(path);
		Path::ToNativePath(&out);
		return StringUtil::toUpper(out);
	}

	static bool IsAfsPath(const std::string& path)
	{
		const std::string_view ext = Path::GetExtension(path);
		return StringUtil::compareNoCase(ext, "afs");
	}

	struct VirtualAfsEntry
	{
		std::string name;
		std::string name_upper;
		u32 original_index = 0;
		u32 original_offset = 0;
		u32 original_size = 0;
		u32 virtual_offset = 0;
		u32 virtual_size = 0;
		std::string source_path;
		u64 source_offset = 0;
		std::time_t mtime = 0;
		bool replaced = false;
		bool read_logged = false;
	};

	struct VirtualAfs
	{
		struct CachedFile
		{
			std::string path;
			FileSystem::ManagedCFilePtr file;
		};

		std::string afs_path;
		std::vector<VirtualAfsEntry> entries;
		std::vector<u8> header;
		std::vector<u8> entry_info;
		u32 data_start = 0;
		u32 entry_info_offset = 0;
		u32 entry_info_size = 0;
		u64 virtual_size = 0;

		FileSystem::ManagedCFilePtr base_file;
		std::vector<CachedFile> file_cache;

		bool Load(const std::string& path, const std::unordered_map<std::string, std::string>& replacements, Error* error);
		bool Read(u8* buffer, u64 offset, u32 size);

	private:
		std::FILE* GetCachedFile(const std::string& path, Error* error);
		VirtualAfsEntry* FindEntry(u64 offset);
	};

VirtualIso::VirtualIso() = default;

VirtualIso::~VirtualIso()
{
	Close();
}

bool VirtualIso::Open(const std::string& root_dir, Error* error)
{
	Close();

	if (root_dir.empty())
	{
		Error::SetString(error, "No directory specified.");
		return false;
	}

	std::string root_dir_clean = root_dir;
	while (root_dir_clean.size() > 1 &&
		(root_dir_clean.back() == '\\' || root_dir_clean.back() == '/'))
	{
		root_dir_clean.pop_back();
	}

	if (!FileSystem::DirectoryExists(root_dir_clean.c_str()))
	{
		Error::SetStringFmt(error, "Directory '{}' does not exist.", root_dir_clean);
		return false;
	}

	if (!BuildTree(root_dir_clean, error))
	{
		Close();
		return false;
	}

	BuildIsoPaths();

	if (!ValidateSystemCnf(error))
	{
		Close();
		return false;
	}

	const bool use_layout_files = Host::GetBoolSettingValue("EmuCore/VirtualIso", "UseLayoutFiles", false);
	m_use_layout_files = use_layout_files;
	if (use_layout_files)
	{
		if (!LoadLayoutFile(root_dir_clean, error))
		{
			Close();
			return false;
		}
	}
	else
	{
		const std::string layout_txt = Path::Combine(root_dir_clean, "PCSX2_LAYOUT.txt");
		if (FileSystem::FileExists(layout_txt.c_str()))
			DevCon.WriteLn("Virtual ISO: Layout file present but ignored (UseLayoutFiles disabled).");
	}

	if (!m_use_layout_files)
		LoadMods(root_dir_clean);

	if (!AssignPathTableIndices())
	{
		Error::SetString(error, "Failed to assign path table indices.");
		Close();
		return false;
	}

	if (!ComputeDirectoryDataSizes())
	{
		Error::SetString(error, "Failed to compute directory sizes.");
		Close();
		return false;
	}

	AssignLsns();

	if (use_layout_files && !LoadLayoutMetadata(root_dir_clean, error))
	{
		Close();
		return false;
	}

	if (m_has_layout_metadata)
	{
		BuildFileExtents();
	}
	else if (!BuildMetadata(error))
	{
		Close();
		return false;
	}

	const u64 total_bytes = static_cast<u64>(m_total_sectors) * SECTOR_SIZE;
	m_cdtype = (total_bytes <= (700ull * 1024 * 1024)) ? CDVD_TYPE_PS2CD : CDVD_TYPE_PS2DVD;
	m_is_open = true;

	Console.WriteLn(Color_StrongBlue, "Virtual ISO open ok: %s", root_dir_clean.c_str());
	DevCon.WriteLn("  blocks      = %u", m_total_sectors);
	DevCon.WriteLn("  type        = %s", (m_cdtype == CDVD_TYPE_PS2CD) ? "CD" : "DVD");

	return true;
}

void VirtualIso::Close()
{
	Clear();
}

bool VirtualIso::IsOpen() const
{
	return m_is_open;
}

void VirtualIso::ReloadModsAndRebuild()
{
	if (!m_is_open || !m_root || m_use_layout_files)
		return;

	LoadMods(m_root->host_path);

	if (!AssignPathTableIndices())
		return;

	if (!ComputeDirectoryDataSizes())
		return;

	AssignLsns();

	Error error;
	if (!BuildMetadata(&error))
		Console.Warning("Virtual ISO: Failed to rebuild metadata after mod reload: %s", error.GetDescription().c_str());
}

s32 VirtualIso::ReadSector(u8* buffer, u32 lsn, int mode)
{
	std::array<u8, SECTOR_SIZE> raw = {};
	if (!ReadRawSector(raw.data(), lsn))
		return -1;

	int offset = 0;
	int length = 0;
	switch (mode)
	{
		case CDVD_MODE_2352:
			offset = 0;
			length = 2352;
			break;
		case CDVD_MODE_2340:
			offset = 12;
			length = 2340;
			break;
		case CDVD_MODE_2328:
			offset = 24;
			length = 2328;
			break;
		case CDVD_MODE_2048:
		default:
			offset = 24;
			length = 2048;
			break;
	}

	std::array<u8, CD_FRAMESIZE_RAW> frame = {};
	std::memcpy(frame.data() + 24, raw.data(), SECTOR_SIZE);
	std::memcpy(buffer, frame.data() + offset, length);
	return 0;
}

s32 VirtualIso::ReadTrack(u32 lsn, int mode)
{
	std::array<u8, SECTOR_SIZE> raw = {};
	if (!ReadRawSector(raw.data(), lsn))
		return -1;

	m_readbuffer.fill(0);
	std::memcpy(m_readbuffer.data() + 24, raw.data(), SECTOR_SIZE);
	m_read_mode = mode;
	return 0;
}

s32 VirtualIso::GetBuffer(u8* buffer)
{
	int offset = 0;
	int length = 0;
	switch (m_read_mode)
	{
		case CDVD_MODE_2352:
			offset = 0;
			length = 2352;
			break;
		case CDVD_MODE_2340:
			offset = 12;
			length = 2340;
			break;
		case CDVD_MODE_2328:
			offset = 24;
			length = 2328;
			break;
		case CDVD_MODE_2048:
		default:
			offset = 24;
			length = 2048;
			break;
	}

	std::memcpy(buffer, m_readbuffer.data() + offset, length);
	return 0;
}

s32 VirtualIso::ReadSubQ(u32 lsn, cdvdSubQ* subq) const
{
	u8 min, sec, frm;
	subq->ctrl = 4;
	subq->adr = 1;
	subq->trackNum = itob(1);
	subq->trackIndex = itob(1);

	lba_to_msf(lsn, &min, &sec, &frm);
	subq->trackM = itob(min);
	subq->trackS = itob(sec);
	subq->trackF = itob(frm);

	subq->pad = 0;

	lba_to_msf(lsn + (2 * 75), &min, &sec, &frm);
	subq->discM = itob(min);
	subq->discS = itob(sec);
	subq->discF = itob(frm);

	return 0;
}

s32 VirtualIso::GetTN(cdvdTN* Buffer) const
{
	Buffer->strack = 1;
	Buffer->etrack = 1;
	return 0;
}

s32 VirtualIso::GetTD(u8 Track, cdvdTD* Buffer) const
{
	if (Track == 0)
	{
		Buffer->lsn = m_total_sectors;
	}
	else
	{
		Buffer->type = CDVD_MODE1_TRACK;
		Buffer->lsn = 0;
	}

	return 0;
}

s32 VirtualIso::GetTOC(void* toc) const
{
	u8* tocBuff = static_cast<u8*>(toc);
	std::memset(tocBuff, 0, 2048);

	if (m_cdtype == CDVD_TYPE_PS2DVD || m_cdtype == CDVD_TYPE_DVDV)
	{
		tocBuff[0] = 0x04;
		tocBuff[1] = 0x02;
		tocBuff[2] = 0xF2;
		tocBuff[3] = 0x00;
		tocBuff[4] = 0x86;
		tocBuff[5] = 0x72;

		tocBuff[12] = 0x01;
		tocBuff[13] = 0x02;
		tocBuff[14] = 0x01;
		tocBuff[15] = 0x00;

		tocBuff[16] = 0x00;
		tocBuff[17] = 0x03;
		tocBuff[18] = 0x00;
		tocBuff[19] = 0x00;

		const u32 max_lsn = m_total_sectors;
		tocBuff[20] = 0x00;
		tocBuff[21] = 0x00;
		tocBuff[22] = (max_lsn >> 24) & 0xFF;
		tocBuff[23] = (max_lsn >> 16) & 0xFF;
		tocBuff[24] = (max_lsn >> 8) & 0xFF;
		tocBuff[25] = max_lsn & 0xFF;
	}

	return 0;
}

s32 VirtualIso::GetDiskType() const
{
	return m_cdtype;
}

s32 VirtualIso::GetDualInfo(s32* dualType, u32* _layer1start) const
{
	*dualType = 0;
	*_layer1start = m_total_sectors;
	return 0;
}

u32 VirtualIso::GetBlockCount() const
{
	return m_total_sectors;
}

bool VirtualIso::BuildTree(const std::string& root_dir, Error* error)
{
	m_entries.clear();
	m_dirs.clear();
	m_files.clear();
	m_layout.clear();
	m_has_layout_metadata = false;
	m_has_udf_metadata = false;

	auto root = std::make_unique<Entry>();
	root->is_dir = true;
	root->name = Path::GetFileName(root_dir);
	root->iso_name = std::string();
	root->host_path = root_dir;
	root->parent = nullptr;
	root->mtime = std::time(nullptr);
	m_root = root.get();
	m_entries.push_back(std::move(root));

	if (!BuildDirectoryContents(m_root, root_dir, error))
		return false;

	CollectEntries(m_root);
	return true;
}

bool VirtualIso::BuildDirectoryContents(Entry* dir, const std::string& host_path, Error* error)
{
	FileSystem::FindResultsArray results;
	const u32 flags = FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_SORT_BY_NAME;
	if (!FileSystem::FindFiles(host_path.c_str(), "*", flags, &results))
	{
		return true;
	}

	std::unordered_map<std::string, bool> seen_names;

	for (const FILESYSTEM_FIND_DATA& fd : results)
	{
		if (fd.FileName.empty())
			continue;

		const std::string raw_name = fd.FileName;
		std::string base_name(Path::GetFileName(raw_name));
		if (base_name.empty())
			base_name = raw_name;

		auto entry = std::make_unique<Entry>();
		entry->name = base_name;
		entry->host_path = Path::IsAbsolute(raw_name) ? raw_name : Path::Combine(host_path, raw_name);
		entry->parent = dir;
		entry->mtime = fd.ModificationTime;
		entry->is_dir = ((fd.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY) != 0);
		if (!entry->is_dir)
		{
			entry->size = fd.Size;
			entry->virtual_size = fd.Size;
		}

		if (!ValidateAndSetIsoName(entry.get(), error))
			return false;

		if (seen_names.find(entry->iso_name) != seen_names.end())
		{
			Error::SetStringFmt(error, "Duplicate ISO name '{}' in directory '{}'.", entry->iso_name, host_path);
			return false;
		}
		seen_names[entry->iso_name] = true;

		Entry* entry_ptr = entry.get();
		m_entries.push_back(std::move(entry));
		dir->children.push_back(entry_ptr);

		if (entry_ptr->is_dir)
		{
			if (!BuildDirectoryContents(entry_ptr, entry_ptr->host_path, error))
				return false;
		}
	}

	return true;
}

bool VirtualIso::ValidateAndSetIsoName(Entry* entry, Error* error)
{
	if (!entry)
		return false;

	if (entry->name == "." || entry->name == "..")
	{
		Error::SetString(error, "Invalid directory entry name.");
		return false;
	}

	std::string iso_name = SanitizeIsoName(entry->name);
	if (iso_name.empty())
	{
		Error::SetStringFmt(error, "Entry '{}' has an invalid ISO name.", entry->name);
		return false;
	}
	const size_t max_len = entry->is_dir ? 31 : 29; // leave space for ";1"
	if (iso_name.size() > max_len)
	{
		Error::SetStringFmt(error, "Entry '{}' exceeds ISO9660 Level 2 name length.", entry->name);
		return false;
	}

	if (!entry->is_dir)
		iso_name += ";1";

	entry->iso_name = std::move(iso_name);
	return true;
}

void VirtualIso::CollectEntries(Entry* dir)
{
	if (!dir)
		return;

	// Build directory list in breadth-first order. This produces a path table
	// ordering that is friendlier to some ISO readers (all level-1 dirs first).
	std::queue<Entry*> q;
	q.push(dir);
	while (!q.empty())
	{
		Entry* current = q.front();
		q.pop();
		m_dirs.push_back(current);

		for (Entry* child : current->children)
		{
			if (child->is_dir)
				q.push(child);
			else
				m_files.push_back(child);
		}
	}
}

void VirtualIso::BuildIsoPaths()
{
	if (!m_root)
		return;

	std::queue<Entry*> q;
	m_root->iso_path.clear();
	q.push(m_root);

	while (!q.empty())
	{
		Entry* current = q.front();
		q.pop();

		for (Entry* child : current->children)
		{
			if (current->iso_path.empty())
				child->iso_path = child->iso_name;
			else
				child->iso_path = current->iso_path + "\\" + child->iso_name;

			if (child->is_dir)
				q.push(child);
		}
	}
}

bool VirtualIso::ValidateSystemCnf(Error* error) const
{
	const Entry* system_cnf = nullptr;
	for (const Entry* file : m_files)
	{
		if (StringUtil::compareNoCase(file->iso_name, "SYSTEM.CNF;1"))
		{
			system_cnf = file;
			break;
		}
	}

	if (!system_cnf)
	{
		Error::SetString(error, "SYSTEM.CNF not found in selected folder.");
		return false;
	}

	const auto content = FileSystem::ReadFileToString(system_cnf->host_path.c_str());
	if (!content.has_value())
	{
		Error::SetStringFmt(error, "Failed to read SYSTEM.CNF at '{}'.", system_cnf->host_path);
		return false;
	}

	std::optional<std::string> boot_path;
	const std::vector<std::string> lines = StringUtil::splitOnNewLine(*content);
	for (const std::string& line_str : lines)
	{
		std::string_view line = StringUtil::StripWhitespace(line_str);
		if (line.empty() || line[0] == '#' || line[0] == ';')
			continue;

		std::string_view key;
		std::string_view value;
		if (!StringUtil::ParseAssignmentString(line, &key, &value))
			continue;

		key = StringUtil::StripWhitespace(key);
		value = StringUtil::StripWhitespace(value);
		if (key.empty() || value.empty())
			continue;

		if (StringUtil::compareNoCase(key, "BOOT2") || StringUtil::compareNoCase(key, "BOOT"))
		{
			std::string out(value);
			StringUtil::StripWhitespace(&out);

			if (out.size() >= 2 && out.front() == '"' && out.back() == '"')
				out = out.substr(1, out.size() - 2);

			const std::string lower = StringUtil::toLower(out);
			if (StringUtil::StartsWithNoCase(lower, "cdrom0:") || StringUtil::StartsWithNoCase(lower, "cdrom1:"))
			{
				out = out.substr(7);
			}

			while (!out.empty() && (out.front() == '\\' || out.front() == '/'))
				out.erase(out.begin());

			StringUtil::ReplaceAll(&out, "/", "\\");
			out = StringUtil::toUpper(out);
			if (out.find(';') == std::string::npos)
				out += ";1";

			boot_path = std::move(out);
			break;
		}
	}

	if (!boot_path.has_value())
	{
		Error::SetString(error, "SYSTEM.CNF does not contain a BOOT or BOOT2 entry.");
		return false;
	}

	const std::string& expected = boot_path.value();
	for (const Entry* file : m_files)
	{
		if (StringUtil::compareNoCase(file->iso_path, expected))
			return true;
	}

	Error::SetStringFmt(error, "Executable '{}' referenced by SYSTEM.CNF was not found in the folder.", expected);
	return false;
}

void VirtualIso::LoadMods(const std::string& root_dir)
{
	m_mods_root.clear();
	m_enabled_mods.clear();
	m_afs_replacements.clear();
	m_afs_cache.clear();

	m_mods_root = Host::GetStringSettingValue("Mods", "Root", "");
	if (!m_mods_root.empty() && !FileSystem::DirectoryExists(m_mods_root.c_str()))
	{
		if (!Path::IsAbsolute(m_mods_root))
			m_mods_root = Path::Combine(root_dir, m_mods_root);
		if (!FileSystem::DirectoryExists(m_mods_root.c_str()))
			m_mods_root.clear();
	}

	if (m_mods_root.empty())
	{
		m_mods_root = Path::Combine(root_dir, "mods");
		if (!FileSystem::DirectoryExists(m_mods_root.c_str()))
		{
			const std::string alt_root = Path::Combine(root_dir, "Mods");
			if (FileSystem::DirectoryExists(alt_root.c_str()))
				m_mods_root = alt_root;
		}
	}

	if (!FileSystem::DirectoryExists(m_mods_root.c_str()))
	{
		Console.WriteLn("Virtual ISO: Mods root '%s' not found.", m_mods_root.c_str());
		return;
	}

	m_enabled_mods = Host::GetStringListSetting("Mods", "Enabled");
	if (m_enabled_mods.empty())
	{
		Console.WriteLn("Virtual ISO: No enabled mods found in '%s'.", m_mods_root.c_str());
		return;
	}

	Console.WriteLn("Virtual ISO: Mods root '%s' (%zu enabled)", m_mods_root.c_str(), m_enabled_mods.size());
	for (const std::string& mod_id : m_enabled_mods)
		Console.WriteLn("  Mod enabled: %s", mod_id.c_str());

	std::unordered_map<std::string, std::unordered_map<std::string, std::string>> replacements;

	for (const std::string& mod_id : m_enabled_mods)
	{
		std::string mod_path = mod_id;
		if (!Path::IsAbsolute(mod_path))
			mod_path = Path::Combine(m_mods_root, mod_path);

		if (!FileSystem::DirectoryExists(mod_path.c_str()))
		{
			Console.WriteLn("Virtual ISO: Enabled mod '%s' folder was not found at '%s'.", mod_id.c_str(), mod_path.c_str());
			continue;
		}

		Console.WriteLn("Virtual ISO: Scanning enabled mod '%s' at '%s'.", mod_id.c_str(), mod_path.c_str());

		FileSystem::FindResultsArray files;
		if (!FileSystem::FindFiles(mod_path.c_str(), "*",
				FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_RECURSIVE, &files))
		{
			Console.WriteLn("Virtual ISO: Mod '%s' had no files to scan.", mod_id.c_str());
			continue;
		}

		for (const FILESYSTEM_FIND_DATA& fd : files)
		{
			if (fd.FileName.empty())
				continue;

			std::string rel = fd.FileName;
			if (StringUtil::StartsWithNoCase(rel, mod_path))
			{
				rel = rel.substr(mod_path.size());
				while (!rel.empty() && (rel.front() == '\\' || rel.front() == '/'))
					rel.erase(rel.begin());
			}

			if (rel.empty())
				continue;

			Console.WriteLn("Virtual ISO: Mod file candidate '%s' -> '%s'.", mod_id.c_str(), rel.c_str());

			const std::vector<std::string_view> parts = Path::SplitNativePath(rel);
			int afs_index = -1;
			for (size_t i = 0; i < parts.size(); i++)
			{
				if (StringUtil::EndsWithNoCase(parts[i], ".afs"))
				{
					afs_index = static_cast<int>(i);
					break;
				}
			}

			if (afs_index < 0 || (afs_index + 1) >= static_cast<int>(parts.size()))
			{
				Console.WriteLn("Virtual ISO:   Ignored, file is not below an .AFS folder.");
				continue;
			}

			const std::vector<std::string_view> afs_parts(parts.begin(), parts.begin() + afs_index + 1);
			const std::string afs_rel = Path::JoinNativePath(afs_parts);
			const std::string entry_name = std::string(parts.back());

			std::string afs_host = Path::Combine(root_dir, afs_rel);
			if (!FileSystem::FileExists(afs_host.c_str()))
			{
				Console.WriteLn("Virtual ISO:   Ignored, target AFS archive does not exist: '%s'.", afs_host.c_str());
				continue;
			}

			const std::string afs_key = NormalizePathKey(afs_host);
			const std::string entry_key = StringUtil::toUpper(entry_name);

			auto& afs_map = replacements[afs_key];
			if (afs_map.find(entry_key) != afs_map.end())
			{
				Console.WriteLn("Virtual ISO:   Ignored, higher-priority mod already maps '%s' in '%s'.",
					entry_name.c_str(), afs_rel.c_str());
				continue; // higher priority mod already set
			}

			afs_map[entry_key] = fd.FileName;
			Console.WriteLn("Virtual ISO:   Maps name '%s' in '%s' -> '%s'.",
				entry_name.c_str(), afs_rel.c_str(), fd.FileName.c_str());
		}
	}

	for (Entry* file : m_files)
	{
		if (!IsAfsPath(file->host_path))
			continue;

		const std::string afs_key = NormalizePathKey(file->host_path);
		auto it = replacements.find(afs_key);
		if (it == replacements.end())
			continue;

		Error error;
		auto afs = std::make_unique<VirtualAfs>();
		if (afs->Load(file->host_path, it->second, &error))
		{
			file->virtual_size = static_cast<s64>(afs->virtual_size);
			m_afs_cache[afs_key] = std::move(afs);
			m_afs_replacements[afs_key] = it->second;
		}
	}

	size_t total_name_repl = 0;
	for (const auto& pair : m_afs_replacements)
		total_name_repl += pair.second.size();

	if (total_name_repl == 0)
	{
		Console.WriteLn("Virtual ISO: No AFS replacements matched any archives.");
	}
	else
	{
		Console.WriteLn("Virtual ISO: AFS replacements active for %zu archive(s) (%zu name).",
			m_afs_replacements.size(), total_name_repl);
	}
}

bool VirtualIso::LoadLayoutFile(const std::string& root_dir, Error* error)
{
	const std::string layout_path = Path::Combine(root_dir, "PCSX2_LAYOUT.txt");
	if (!FileSystem::FileExists(layout_path.c_str()))
	{
		DevCon.WriteLn("Virtual ISO: No layout file found (%s). Using sequential layout.", layout_path.c_str());
		return true;
	}

	const auto content = FileSystem::ReadFileToString(layout_path.c_str());
	if (!content.has_value())
	{
		Error::SetStringFmt(error, "Failed to read layout file '{}'.", layout_path);
		return false;
	}

	size_t loaded = 0;
	const std::vector<std::string> lines = StringUtil::splitOnNewLine(*content);
	for (const std::string& line_str : lines)
	{
		std::string_view line = StringUtil::StripWhitespace(line_str);
		if (line.empty() || line[0] == '#' || line[0] == ';')
			continue;

		const std::vector<std::string_view> parts = StringUtil::SplitString(line, '|', true);
		if (parts.size() != 3)
		{
			Console.Warning("Virtual ISO: Invalid layout line (expected PATH|LSN|SIZE): %s", std::string(line).c_str());
			continue;
		}

		std::string path = StringUtil::toUpper(std::string(StringUtil::StripWhitespace(parts[0])));
		StringUtil::ReplaceAll(&path, "/", "\\");

		const auto lsn = StringUtil::FromChars<u32>(StringUtil::StripWhitespace(parts[1]));
		const auto size = StringUtil::FromChars<u32>(StringUtil::StripWhitespace(parts[2]));
		if (!lsn.has_value() || !size.has_value())
		{
			Console.Warning("Virtual ISO: Invalid layout values in line: %s", std::string(line).c_str());
			continue;
		}

		m_layout[path] = {lsn.value(), size.value()};
		loaded++;
	}

	DevCon.WriteLn("Virtual ISO: Loaded layout file '%s' (%zu entries)", layout_path.c_str(), loaded);
	return true;
}

bool VirtualIso::AssignPathTableIndices()
{
	for (size_t i = 0; i < m_dirs.size(); i++)
	{
		m_dirs[i]->path_table_index = static_cast<u16>(i + 1);
	}

	return !m_dirs.empty();
}

bool VirtualIso::ComputeDirectoryDataSizes()
{
	for (Entry* dir : m_dirs)
		dir->data_size = ComputeDirectorySizeBytes(dir);

	return true;
}

u32 VirtualIso::ComputeDirectorySizeBytes(const Entry* dir) const
{
	size_t size = 0;

	auto add_record = [&size](u8 id_len) {
		const size_t record_len = DirectoryRecordSize(id_len);
		const size_t sector_offset = size % SECTOR_SIZE;
		if (sector_offset + record_len > SECTOR_SIZE)
			size += (SECTOR_SIZE - sector_offset);
		size += record_len;
	};

	add_record(1);
	add_record(1);
	for (const Entry* child : dir->children)
		add_record(static_cast<u8>(child->iso_name.size()));

	const size_t aligned = AlignUp(static_cast<u32>(size), SECTOR_SIZE);
	return static_cast<u32>(aligned);
}

u32 VirtualIso::ComputeUdfDirectorySizeBytes(const Entry* dir) const
{
	if (!dir)
		return 0;

	size_t size = 0;

	// Parent/self entry (name "\0")
	size += UdfFileIdentifierSize("\0");

	for (const Entry* child : dir->children)
	{
		const u32 record_len = UdfFileIdentifierSize(child->name);
		const size_t sector_offset = size % SECTOR_SIZE;
		if (sector_offset + record_len > SECTOR_SIZE)
			size += (SECTOR_SIZE - sector_offset);
		size += record_len;
	}

	const size_t aligned = AlignUp(static_cast<u32>(size), SECTOR_SIZE);
	return static_cast<u32>(aligned);
}

void VirtualIso::AssignLsns()
{
	const u32 pvd_lsn = ISO_SYSTEM_AREA_SECTORS;
	const u32 term_lsn = pvd_lsn + 1;

	// Path tables
	u32 path_table_size = 0;
	for (const Entry* dir : m_dirs)
	{
		u8 id_len = dir == m_root ? 1 : static_cast<u8>(dir->iso_name.size());
		u32 entry_len = 8 + id_len + ((id_len % 2) ? 1 : 0);
		path_table_size += entry_len;
	}
	const u32 path_table_sectors = AlignUp(path_table_size, SECTOR_SIZE) / SECTOR_SIZE;

	const bool generate_udf = !m_use_layout_files;
	const u32 path_table_l_lsn = generate_udf ? (UDF_ANCHOR_LSN_PRIMARY + 1) : (term_lsn + 1);
	const u32 path_table_m_lsn = path_table_l_lsn + path_table_sectors;

	u32 lsn = path_table_m_lsn + path_table_sectors;

	for (Entry* dir : m_dirs)
	{
		dir->lsn = lsn;
		lsn += AlignUp(dir->data_size, SECTOR_SIZE) / SECTOR_SIZE;
	}

	// Leave a small gap before file data so UDF probe sectors (e.g. 256) don't land inside file data.
	const u32 min_file_lsn = 512;
	if (lsn < min_file_lsn)
		lsn = min_file_lsn;

	u32 udf_metadata_sectors = 0;
	if (generate_udf)
	{
		m_udf_partition_start = lsn;
		// File Set Descriptor + Terminating Descriptor.
		udf_metadata_sectors += 2;

		// Directory data
		for (const Entry* dir : m_dirs)
		{
			const u32 dir_bytes = ComputeUdfDirectorySizeBytes(dir);
			udf_metadata_sectors += AlignUp(dir_bytes, SECTOR_SIZE) / SECTOR_SIZE;
		}

		// File entries (1 sector per dir and file)
		udf_metadata_sectors += static_cast<u32>(m_dirs.size() + m_files.size());

		lsn = m_udf_partition_start + udf_metadata_sectors;
		if (lsn < min_file_lsn)
			lsn = min_file_lsn;
	}

	u32 max_layout_end = 0;
	for (Entry* file : m_files)
	{
		if (file->iso_path.empty())
			continue;

		const auto it = m_layout.find(file->iso_path);
		if (it == m_layout.end())
			continue;

		const u32 layout_lsn = it->second.first;
		const u32 layout_size = it->second.second;
		const u32 sectors = AlignUp(layout_size, SECTOR_SIZE) / SECTOR_SIZE;
		file->lsn = layout_lsn;
		max_layout_end = std::max(max_layout_end, layout_lsn + sectors);

		if (layout_size != static_cast<u32>(file->size))
		{
			Console.Warning("Virtual ISO: Layout size mismatch for '%s' (layout %u, file %lld)",
				file->iso_path.c_str(), layout_size, static_cast<long long>(file->size));
		}

		if (layout_lsn < lsn)
		{
			Console.Warning("Virtual ISO: Layout LSN %u for '%s' is inside metadata region (<= %u).",
				layout_lsn, file->iso_path.c_str(), lsn);
		}
	}

	u32 next_lsn = std::max(lsn, max_layout_end);
	for (Entry* file : m_files)
	{
		if (!file->iso_path.empty() && m_layout.find(file->iso_path) != m_layout.end())
			continue;

		file->lsn = next_lsn;
		const u32 sectors = AlignUp(static_cast<u32>(file->virtual_size), SECTOR_SIZE) / SECTOR_SIZE;
		next_lsn += sectors;
	}

	m_total_sectors = std::max(next_lsn, max_layout_end);
	if (generate_udf)
		m_total_sectors = std::max(m_total_sectors, next_lsn + 1); // reserve last sector for AVDP

	// Store metadata LSNs for building later.
	(void)pvd_lsn;
	(void)term_lsn;
	(void)path_table_l_lsn;
	(void)path_table_m_lsn;
}

bool VirtualIso::LoadLayoutMetadata(const std::string& root_dir, Error* error)
{
	const std::string meta_path = Path::Combine(root_dir, "PCSX2_LAYOUT.bin");
	if (!FileSystem::FileExists(meta_path.c_str()))
		return true;

	FileSystem::ManagedCFilePtr fp = FileSystem::OpenManagedCFile(meta_path.c_str(), "rb", error);
	if (!fp)
	{
		Error::AddPrefix(error, fmt::format("Failed to open layout metadata file '{}': ", meta_path));
		return false;
	}

	const s64 size = FileSystem::FSize64(fp.get());
	if (size <= 0 || (size % SECTOR_SIZE) != 0)
	{
		Error::SetStringFmt(error, "Layout metadata file '{}' has invalid size.", meta_path);
		return false;
	}

	const u32 sector_count = static_cast<u32>(size / SECTOR_SIZE);
	std::vector<u8> data(static_cast<size_t>(size));
	const size_t read = std::fread(data.data(), 1, data.size(), fp.get());
	if (read != data.size())
	{
		Error::SetStringFmt(error, "Failed to read layout metadata file '{}'.", meta_path);
		return false;
	}

	m_meta_sectors.clear();
	m_meta_sector_lookup.clear();
	AddMetaSectors(0, data);
	m_has_layout_metadata = true;

	DevCon.WriteLn("Virtual ISO: Loaded layout metadata file '%s' (%u sectors)", meta_path.c_str(), sector_count);
	return true;
}

bool VirtualIso::BuildMetadata(Error* error)
{
	m_meta_sectors.clear();
	m_meta_sector_lookup.clear();

	if (!m_root)
		return false;

	const u32 pvd_lsn = ISO_SYSTEM_AREA_SECTORS;
	const u32 term_lsn = pvd_lsn + 1;

	u32 path_table_size = 0;
	for (const Entry* dir : m_dirs)
	{
		u8 id_len = dir == m_root ? 1 : static_cast<u8>(dir->iso_name.size());
		u32 entry_len = 8 + id_len + ((id_len % 2) ? 1 : 0);
		path_table_size += entry_len;
	}
	const u32 path_table_sectors = AlignUp(path_table_size, SECTOR_SIZE) / SECTOR_SIZE;

	const u32 path_table_l_lsn = m_use_layout_files ? (term_lsn + 1) : (UDF_ANCHOR_LSN_PRIMARY + 1);
	const u32 path_table_m_lsn = path_table_l_lsn + path_table_sectors;

	// PVD
	IsoReader::ISOPrimaryVolumeDescriptor pvd = {};
	pvd.header.type_code = 1;
	std::memcpy(pvd.header.standard_identifier, "CD001", 5);
	pvd.header.version = 1;
	std::memset(pvd.system_identifier, ' ', sizeof(pvd.system_identifier));
	std::memcpy(pvd.system_identifier, "PCSX2VISO", 9);
	std::string volume_id = SanitizeIsoName(Path::GetFileName(m_root->host_path));
	if (volume_id.empty())
		volume_id = "VIRTUALISO";
	std::memset(pvd.volume_identifier, ' ', sizeof(pvd.volume_identifier));
	std::memcpy(pvd.volume_identifier, volume_id.c_str(), std::min(volume_id.size(), sizeof(pvd.volume_identifier)));

	pvd.total_sectors_le = m_total_sectors;
	pvd.total_sectors_be = ByteSwap(m_total_sectors);
	pvd.volume_set_size_le = 1;
	pvd.volume_set_size_be = ByteSwap<u16>(1);
	pvd.volume_sequence_number_le = 1;
	pvd.volume_sequence_number_be = ByteSwap<u16>(1);
	pvd.block_size_le = SECTOR_SIZE;
	pvd.block_size_be = ByteSwap<u16>(SECTOR_SIZE);
	pvd.path_table_size_le = path_table_size;
	pvd.path_table_size_be = ByteSwap(path_table_size);
	pvd.path_table_location_le = path_table_l_lsn;
	pvd.optional_path_table_location_le = 0;
	pvd.path_table_location_be = ByteSwap(path_table_m_lsn);
	pvd.optional_path_table_location_be = 0;

	std::array<u8, 34> root_record = {};
	{
		const u8 id_len = 1;
		const u8 record_len = static_cast<u8>(DirectoryRecordSize(id_len));
		root_record[0] = record_len;
		root_record[1] = 0;
		WriteBoth32(root_record.data() + 2, m_root->lsn);
		WriteBoth32(root_record.data() + 10, m_root->data_size);
		WriteRecordingDateTime(root_record.data() + 18, m_root->mtime);
		root_record[25] = IsoReader::ISODirectoryEntryFlag_Directory;
		root_record[26] = 0;
		root_record[27] = 0;
		WriteBoth16(root_record.data() + 28, 1);
		root_record[32] = id_len;
		root_record[33] = 0;
	}
	std::memcpy(pvd.root_directory_entry, root_record.data(), root_record.size());

	const std::time_t now = std::time(nullptr);
	WritePVDDateTime(&pvd.volume_creation_time, now);
	WritePVDDateTime(&pvd.volume_modification_time, now);
	WritePVDDateTime(&pvd.volume_effective_time, now);
	WritePVDDateTime(&pvd.volume_expiration_time, now);
	pvd.structure_version = 1;

	std::array<u8, SECTOR_SIZE> pvd_sector = {};
	std::memcpy(pvd_sector.data(), &pvd, sizeof(pvd));
	AddMetaSector(pvd_lsn, pvd_sector);

	// Volume descriptor terminator
	std::array<u8, SECTOR_SIZE> term = {};
	term[0] = 255;
	std::memcpy(term.data() + 1, "CD001", 5);
	term[6] = 1;
	AddMetaSector(term_lsn, term);

	// Path tables
	std::vector<u8> path_table_l;
	std::vector<u8> path_table_m;
	path_table_l.reserve(path_table_size);
	path_table_m.reserve(path_table_size);

	for (const Entry* dir : m_dirs)
	{
		const u8 id_len = dir == m_root ? 1 : static_cast<u8>(dir->iso_name.size());
		const u8 ext_attr = 0;
		const u16 parent_index = dir->parent ? dir->parent->path_table_index : dir->path_table_index;

		const size_t entry_len = 8 + id_len + ((id_len % 2) ? 1 : 0);
		const size_t start_l = path_table_l.size();
		const size_t start_m = path_table_m.size();
		path_table_l.resize(start_l + entry_len, 0);
		path_table_m.resize(start_m + entry_len, 0);

		path_table_l[start_l + 0] = id_len;
		path_table_l[start_l + 1] = ext_attr;
		WriteLE32(path_table_l.data() + start_l + 2, dir->lsn);
		WriteLE16(path_table_l.data() + start_l + 6, parent_index);
		if (dir == m_root)
			path_table_l[start_l + 8] = 0;
		else
			std::memcpy(path_table_l.data() + start_l + 8, dir->iso_name.data(), id_len);

		path_table_m[start_m + 0] = id_len;
		path_table_m[start_m + 1] = ext_attr;
		WriteBE32(path_table_m.data() + start_m + 2, dir->lsn);
		WriteBE16(path_table_m.data() + start_m + 6, parent_index);
		if (dir == m_root)
			path_table_m[start_m + 8] = 0;
		else
			std::memcpy(path_table_m.data() + start_m + 8, dir->iso_name.data(), id_len);
	}

	path_table_l.resize(AlignUp(static_cast<u32>(path_table_l.size()), SECTOR_SIZE), 0);
	path_table_m.resize(AlignUp(static_cast<u32>(path_table_m.size()), SECTOR_SIZE), 0);
	AddMetaSectors(path_table_l_lsn, path_table_l);
	AddMetaSectors(path_table_m_lsn, path_table_m);

	// Directory data
	for (const Entry* dir : m_dirs)
	{
		std::vector<u8> dir_data;
		dir_data.reserve(dir->data_size);

		auto add_record = [&](const std::vector<u8>& id, bool is_dir, u32 lsn, u32 size, std::time_t time) {
			const u8 id_len = static_cast<u8>(id.size());
			const size_t record_len = DirectoryRecordSize(id_len);
			const size_t sector_offset = dir_data.size() % SECTOR_SIZE;
			if (sector_offset + record_len > SECTOR_SIZE)
				dir_data.resize(dir_data.size() + (SECTOR_SIZE - sector_offset), 0);

			const size_t start = dir_data.size();
			dir_data.resize(start + record_len, 0);
			u8* p = dir_data.data() + start;

			p[0] = static_cast<u8>(record_len);
			p[1] = 0;
			WriteBoth32(p + 2, lsn);
			WriteBoth32(p + 10, size);
			WriteRecordingDateTime(p + 18, time);
			p[25] = is_dir ? IsoReader::ISODirectoryEntryFlag_Directory : 0;
			p[26] = 0;
			p[27] = 0;
			WriteBoth16(p + 28, 1);
			p[32] = id_len;
			if (!id.empty())
				std::memcpy(p + 33, id.data(), id.size());
		};

		add_record(std::vector<u8>{0}, true, dir->lsn, dir->data_size, dir->mtime);
		const Entry* parent = dir->parent ? dir->parent : dir;
		add_record(std::vector<u8>{1}, true, parent->lsn, parent->data_size, parent->mtime);

		for (const Entry* child : dir->children)
		{
			std::vector<u8> id(child->iso_name.begin(), child->iso_name.end());
			const u32 child_size = child->is_dir ? child->data_size : static_cast<u32>(child->virtual_size);
			add_record(id, child->is_dir, child->lsn, child_size, child->mtime);
		}

		dir_data.resize(AlignUp(static_cast<u32>(dir_data.size()), SECTOR_SIZE), 0);
		AddMetaSectors(dir->lsn, dir_data);
	}

	if (!m_use_layout_files)
		BuildUdfMetadata();

	// File extents
	BuildFileExtents();

	return true;
}

void VirtualIso::BuildUdfMetadata()
{
	if (!m_root || m_dirs.empty())
		return;

	m_has_udf_metadata = true;

	const u32 partition_start = m_udf_partition_start;
	const u32 fsd_lsn = partition_start;
	const u32 fsd_term_lsn = partition_start + 1;

	std::unordered_map<const Entry*, u32> dir_data_lsn;
	std::unordered_map<const Entry*, u32> entry_lsn;
	std::unordered_map<const Entry*, u32> dir_data_size;

	u32 current = partition_start + 2;
	for (const Entry* dir : m_dirs)
	{
		const u32 size_bytes = ComputeUdfDirectorySizeBytes(dir);
		const u32 sectors = AlignUp(size_bytes, SECTOR_SIZE) / SECTOR_SIZE;
		dir_data_lsn[dir] = current;
		dir_data_size[dir] = size_bytes;
		current += sectors;
	}

	for (const Entry* dir : m_dirs)
		entry_lsn[dir] = current++;
	for (const Entry* file : m_files)
		entry_lsn[file] = current++;

	if (!m_files.empty() && m_files.front()->lsn < current)
		Console.Warning("Virtual ISO: UDF metadata overlaps file data (file LSN %u, metadata end %u).", m_files.front()->lsn, current);

	const std::string volume_id = [&]() {
		std::string vid = SanitizeIsoName(Path::GetFileName(m_root->host_path));
		if (vid.empty())
			vid = "VIRTUALISO";
		return vid;
	}();

	// AVDP (primary and last)
	std::array<u8, SECTOR_SIZE> avdp = {};
	WriteUdfAnchor(avdp.data(), UDF_ANCHOR_LSN_PRIMARY);
	AddMetaSector(UDF_ANCHOR_LSN_PRIMARY, avdp);

	if (m_total_sectors > 0)
	{
		const u32 last_lsn = m_total_sectors - 1;
		std::array<u8, SECTOR_SIZE> avdp_last = {};
		WriteUdfAnchor(avdp_last.data(), last_lsn);
		AddMetaSector(last_lsn, avdp_last);
	}

	// Volume Descriptor Sequence (main and reserve)
	auto write_vds_descriptor = [&](u32 lsn, u16 tag_id, const std::function<void(u8*)>& writer) {
		std::array<u8, SECTOR_SIZE> sector = {};
		writer(sector.data());
		FinalizeUdfDescriptor(sector.data(), tag_id, lsn, SECTOR_SIZE);
		AddMetaSector(lsn, sector);
	};

	auto write_pvd = [&](u8* p) {
		WriteLE32(p + 16, 0); // VDS number
		WriteLE32(p + 20, 0); // PVD number
		WriteUdfDstring(p + 24, 32, volume_id, false);
		WriteLE16(p + 56, 1);
		WriteLE16(p + 58, 1);
		WriteLE16(p + 60, 2);
		WriteLE16(p + 62, 2);
		WriteLE32(p + 64, 1);
		WriteLE32(p + 68, 1);
		WriteUdfDstring(p + 72, 128, volume_id, true);
		WriteUdfCharSpec(p + 200, "OSTA Compressed Unicode");
		WriteUdfCharSpec(p + 264, "OSTA Compressed Unicode");
		std::memset(p + 328, 0, 16); // VolumeAbstract + Copyright extents
		WriteUdfEntityIdentifier(p + 344, "*PCSX2", UDF_REVISION, 0, true);
		WriteUdfTimeStamp(p + 376);
		WriteUdfEntityIdentifier(p + 388, "*PCSX2", UDF_REVISION, 0, true);
	};

	auto write_lvd = [&](u8* p) {
		WriteLE32(p + 16, 1); // VDS number
		WriteUdfCharSpec(p + 20, "OSTA Compressed Unicode");
		WriteUdfDstring(p + 84, 128, volume_id, true);
		WriteLE32(p + 212, SECTOR_SIZE);
		WriteUdfEntityIdentifier(p + 216, "*OSTA UDF Compliant", UDF_REVISION, 0, true);

		// LogicalVolumeContentsUse: File Set Descriptor location (LongAD)
		WriteUdfLongAD(p + 248, SECTOR_SIZE, fsd_lsn - partition_start, 0);

		WriteLE32(p + 264, 6); // Map table length
		WriteLE32(p + 268, 1); // Number of partition maps
		WriteUdfEntityIdentifier(p + 272, "*PCSX2", UDF_REVISION, 0, true);
		std::memset(p + 304, 0, 128); // Implementation use

		// Integrity sequence extent
		WriteLE32(p + 432, SECTOR_SIZE);
		WriteLE32(p + 436, UDF_LVID_LSN);

		// Partition map (Type 1)
		p[440] = 1; // Type
		p[441] = 6; // Length
		WriteLE16(p + 442, 1); // Volume sequence number
		WriteLE16(p + 444, 0); // Partition number
	};

	auto write_pd = [&](u8* p, u32 partition_length) {
		WriteLE32(p + 16, 2); // VDS number
		WriteLE16(p + 20, 1); // Partition flags
		WriteLE16(p + 22, 0); // Partition number
		WriteUdfEntityIdentifier(p + 24, "+NSR02", UDF_REVISION, 0, true);
		std::memset(p + 56, 0, 128); // Partition contents use
		WriteLE32(p + 184, 0); // Access type
		WriteLE32(p + 188, partition_start);
		WriteLE32(p + 192, partition_length);
		WriteUdfEntityIdentifier(p + 196, "*PCSX2", UDF_REVISION, 0, true);
	};

	auto write_iuvd = [&](u8* p) {
		WriteLE32(p + 16, 3); // VDS number
		WriteUdfEntityIdentifier(p + 20, "*PCSX2", UDF_REVISION, 0, true);
		// Implementation use: LVInformation
		WriteUdfCharSpec(p + 52, "OSTA Compressed Unicode");
		WriteUdfDstring(p + 116, 128, volume_id, true);
		WriteUdfDstring(p + 244, 36, "", true);
		WriteUdfDstring(p + 280, 36, "", true);
		WriteUdfDstring(p + 316, 36, "", true);
		WriteUdfEntityIdentifier(p + 352, "*PCSX2", UDF_REVISION, 0, true);
	};

	auto write_usd = [&](u8* p) {
		WriteLE32(p + 16, 4); // VDS number
		WriteLE32(p + 20, 0); // Number of allocation descriptors
	};

	auto write_td = [&](u8* /*p*/) {};

	const u32 partition_length = (m_total_sectors > partition_start) ? (m_total_sectors - partition_start) : 0;

	const std::array<std::pair<u16, std::function<void(u8*)>>, 6> vds_descs = {{
		{1, write_pvd},
		{6, write_lvd},
		{5, [partition_length, &write_pd](u8* p) { write_pd(p, partition_length); }},
		{4, write_iuvd},
		{7, write_usd},
		{8, write_td},
	}};

	for (size_t i = 0; i < vds_descs.size(); i++)
	{
		const u32 lsn = UDF_MAIN_VDS_LSN + static_cast<u32>(i);
		write_vds_descriptor(lsn, vds_descs[i].first, vds_descs[i].second);
	}
	for (size_t i = 0; i < vds_descs.size(); i++)
	{
		const u32 lsn = UDF_RESERVE_VDS_LSN + static_cast<u32>(i);
		write_vds_descriptor(lsn, vds_descs[i].first, vds_descs[i].second);
	}

	// Logical Volume Integrity Descriptor
	{
		std::array<u8, SECTOR_SIZE> lvid = {};
		WriteUdfTimeStamp(lvid.data() + 16);
		WriteLE32(lvid.data() + 28, 1); // IntegrityType = Close
		std::memset(lvid.data() + 32, 0, 8); // NextIntegrityExtent
		WriteLE32(lvid.data() + 40, 0xFFFFFFFFu);
		std::memset(lvid.data() + 44, 0, 28);
		WriteLE32(lvid.data() + 72, 1); // Number of partitions
		const u32 impl_use_len = 46; // LogicalVolumeIntegrityImplementationUse without extra data
		WriteLE32(lvid.data() + 76, impl_use_len);
		WriteLE32(lvid.data() + 80, 0); // Free space
		WriteLE32(lvid.data() + 84, partition_length); // Size table
		WriteUdfEntityIdentifier(lvid.data() + 88, "*PCSX2", UDF_REVISION, 0, true);
		WriteLE32(lvid.data() + 120, static_cast<u32>(m_files.size()));
		WriteLE32(lvid.data() + 124, static_cast<u32>(m_dirs.size()));
		WriteLE16(lvid.data() + 128, UDF_REVISION);
		WriteLE16(lvid.data() + 130, UDF_REVISION);
		WriteLE16(lvid.data() + 132, UDF_REVISION);
		FinalizeUdfDescriptor(lvid.data(), 9, UDF_LVID_LSN, SECTOR_SIZE);
		AddMetaSector(UDF_LVID_LSN, lvid);
	}

	// File Set Descriptor
	{
		std::array<u8, SECTOR_SIZE> fsd = {};
		WriteUdfTimeStamp(fsd.data() + 16);
		WriteLE16(fsd.data() + 28, 3);
		WriteLE16(fsd.data() + 30, 3);
		WriteLE32(fsd.data() + 32, 1);
		WriteLE32(fsd.data() + 36, 1);
		WriteLE32(fsd.data() + 40, 0);
		WriteLE32(fsd.data() + 44, 0);
		WriteUdfCharSpec(fsd.data() + 48, "OSTA Compressed Unicode");
		WriteUdfDstring(fsd.data() + 112, 128, volume_id, true);
		WriteUdfCharSpec(fsd.data() + 240, "OSTA Compressed Unicode");
		WriteUdfDstring(fsd.data() + 304, 32, "PLAYSTATION2 DVD-ROM FILE SET", true);
		WriteUdfDstring(fsd.data() + 336, 32, "", true);
		WriteUdfDstring(fsd.data() + 368, 32, "", true);

		const Entry* root_dir = m_root;
		const u32 root_entry_lsn = entry_lsn[root_dir];
		const u32 root_size = dir_data_size[root_dir];
		WriteUdfLongAD(fsd.data() + 400, root_size, root_entry_lsn - partition_start, 0);
		WriteUdfEntityIdentifier(fsd.data() + 416, "*OSTA UDF Compliant", UDF_REVISION, 0, true);
		WriteUdfLongAD(fsd.data() + 448, 0, 0, 0);
		WriteUdfLongAD(fsd.data() + 464, 0, 0, 0);

		FinalizeUdfDescriptor(fsd.data(), 0x0100, fsd_lsn, SECTOR_SIZE);
		AddMetaSector(fsd_lsn, fsd);

		std::array<u8, SECTOR_SIZE> fsd_term = {};
		FinalizeUdfDescriptor(fsd_term.data(), 8, fsd_term_lsn, SECTOR_SIZE);
		AddMetaSector(fsd_term_lsn, fsd_term);
	}

	// UDF directory data
	for (const Entry* dir : m_dirs)
	{
		const u32 dir_lsn = dir_data_lsn[dir];
		const u32 dir_size = dir_data_size[dir];
		const u32 dir_size_aligned = AlignUp(dir_size, SECTOR_SIZE);
		std::vector<u8> data(dir_size_aligned);

		size_t pos = 0;
		auto write_fid = [&](const std::string& name, u8 characteristics, const Entry* target) {
			const u32 fid_size = UdfFileIdentifierSize(name);
			const u32 sector_offset = static_cast<u32>(pos % SECTOR_SIZE);
			if (sector_offset + fid_size > SECTOR_SIZE)
			{
				const size_t pad = SECTOR_SIZE - sector_offset;
				std::memset(data.data() + pos, 0, pad);
				pos += pad;
			}

			u8* p = data.data() + pos;
			std::memset(p, 0, fid_size);
			WriteLE16(p + 16, 1); // File version number
			p[18] = characteristics;
			const u32 name_len = UdfDstringDataLength(name, true);
			p[19] = static_cast<u8>(name_len);
			const u32 icb_lsn = entry_lsn[target] - partition_start;
			WriteUdfLongAD(p + 20, 0x13C, icb_lsn, 0);
			WriteLE16(p + 36, 0); // impl use length
			WriteUdfDstring(p + 38, name_len, name, true);
			FinalizeUdfDescriptor(p, 0x0101, dir_lsn, static_cast<u16>(fid_size));
			pos += fid_size;
		};

		// Parent/self entry
		write_fid("\0", 0x08 | 0x02, dir);

		for (const Entry* child : dir->children)
		{
			u8 characteristics = 0;
			if (child->is_dir)
				characteristics |= 0x02;
			write_fid(child->name, characteristics, child);
		}

		data.resize(AlignUp(static_cast<u32>(pos), SECTOR_SIZE), 0);
		AddMetaSectors(dir_lsn, data);
	}

	// UDF file entries
	u64 unique_id = 0;
	auto advance_unique_id = [&]() {
		if ((unique_id & 0xFFFFFFFFu) > 0)
			unique_id++;
		else
			unique_id = (unique_id & 0xFFFFFFFF00000000ull) | 16;
	};

	auto write_file_entry = [&](const Entry* entry, bool is_dir) {
		const u32 entry_sector = entry_lsn[entry];
		std::array<u8, SECTOR_SIZE> fe = {};

		// ICB Tag
		WriteLE32(fe.data() + 16, 0); // PriorRecordedNumberofDirectEntries
		WriteLE16(fe.data() + 20, 4); // StrategyType
		WriteLE16(fe.data() + 22, 0); // StrategyParameter
		WriteLE16(fe.data() + 24, 1); // MaxNumberofEntries
		fe[27] = is_dir ? 4 : 0; // FileType (Directory=4)
		WriteUdfLogicalBlockAddress(fe.data() + 28, 0, 0);
		WriteLE16(fe.data() + 34, 0); // Flags (AllocationType=0)

		WriteLE32(fe.data() + 36, 0xFFFFFFFFu); // UID
		WriteLE32(fe.data() + 40, 0xFFFFFFFFu); // GID
		WriteLE32(fe.data() + 44, 0x1FF); // Permissions
		WriteLE16(fe.data() + 48, 1); // FileLinkCount

		const u64 info_length = is_dir ? dir_data_size[entry] : static_cast<u64>(entry->virtual_size);
		const u64 blocks = (info_length + SECTOR_SIZE - 1) / SECTOR_SIZE;
		WriteLE32(fe.data() + 56, static_cast<u32>(info_length & 0xFFFFFFFFu));
		WriteLE32(fe.data() + 60, static_cast<u32>((info_length >> 32) & 0xFFFFFFFFu));
		WriteLE32(fe.data() + 64, static_cast<u32>(blocks & 0xFFFFFFFFu));
		WriteLE32(fe.data() + 68, static_cast<u32>((blocks >> 32) & 0xFFFFFFFFu));

		WriteUdfTimeStamp(fe.data() + 72);  // AccessTime
		WriteUdfTimeStamp(fe.data() + 84);  // ModificationTime
		WriteUdfTimeStamp(fe.data() + 96);  // AttributeTime
		WriteLE32(fe.data() + 108, 1); // Checkpoint

		std::memset(fe.data() + 112, 0, 16); // ExtendedAttributeICB
		WriteUdfEntityIdentifier(fe.data() + 128, "*PCSX2", UDF_REVISION, 0, true);
		WriteLE32(fe.data() + 160, static_cast<u32>(unique_id & 0xFFFFFFFFu));
		WriteLE32(fe.data() + 164, static_cast<u32>((unique_id >> 32) & 0xFFFFFFFFu));
		WriteLE32(fe.data() + 168, 0); // LengthOfExtendedAttributes
		WriteLE32(fe.data() + 172, 8); // LengthOfAllocationDescriptors

		const u32 data_lsn = is_dir ? dir_data_lsn[entry] : entry->lsn;
		const u32 data_len = is_dir ? dir_data_size[entry] : static_cast<u32>(entry->virtual_size);
		WriteUdfShortAD(fe.data() + 176, data_len, data_lsn - partition_start);

		FinalizeUdfDescriptor(fe.data(), 0x0105, entry_sector, 184);
		AddMetaSector(entry_sector, fe);
		advance_unique_id();
	};

	for (const Entry* dir : m_dirs)
		write_file_entry(dir, true);
	for (const Entry* file : m_files)
		write_file_entry(file, false);

	DevCon.WriteLn("Virtual ISO: Built UDF metadata (partition start %u, total sectors %u)", partition_start, m_total_sectors);
}

std::FILE* VirtualAfs::GetCachedFile(const std::string& path, Error* error)
{
	for (auto it = file_cache.begin(); it != file_cache.end(); ++it)
	{
		if (StringUtil::compareNoCase(it->path, path))
		{
			CachedFile cached = std::move(*it);
			file_cache.erase(it);
			file_cache.insert(file_cache.begin(), std::move(cached));
			return file_cache.front().file.get();
		}
	}

	FileSystem::ManagedCFilePtr file =
		FileSystem::OpenManagedSharedCFile(path.c_str(), "rb", FileSystem::FileShareMode::DenyNone, error);
	if (!file)
		return nullptr;

	if (file_cache.size() >= 8)
		file_cache.pop_back();

	file_cache.insert(file_cache.begin(), CachedFile{path, std::move(file)});
	return file_cache.front().file.get();
}

VirtualAfsEntry* VirtualAfs::FindEntry(u64 offset)
{
	if (entries.empty())
		return nullptr;

	auto it = std::upper_bound(entries.begin(), entries.end(), offset, [](u64 value, const VirtualAfsEntry& entry) {
		return value < entry.virtual_offset;
	});
	if (it == entries.begin())
		return nullptr;
	--it;
	if (offset < it->virtual_offset || offset >= (it->virtual_offset + it->virtual_size))
		return nullptr;
	return &(*it);
}

bool VirtualAfs::Load(const std::string& path, const std::unordered_map<std::string, std::string>& replacements, Error* error)
{
	afs_path = path;
	base_file = FileSystem::OpenManagedSharedCFile(path.c_str(), "rb", FileSystem::FileShareMode::DenyNone, error);
	if (!base_file)
		return false;

	std::array<u8, 8> header_raw = {};
	if (std::fread(header_raw.data(), 1, header_raw.size(), base_file.get()) != header_raw.size())
	{
		Error::SetStringFmt(error, "AFS: Failed to read header from '{}'.", path);
		return false;
	}

	if (std::memcmp(header_raw.data(), "AFS", 3) != 0)
	{
		Error::SetStringFmt(error, "AFS: '{}' is not an AFS archive.", path);
		return false;
	}

	const u32 file_count = ReadLE32(header_raw.data() + 4);
	entries.clear();
	entries.resize(file_count);

	for (u32 i = 0; i < file_count; i++)
	{
		std::array<u8, 8> entry_raw = {};
		if (std::fread(entry_raw.data(), 1, entry_raw.size(), base_file.get()) != entry_raw.size())
		{
			Error::SetStringFmt(error, "AFS: Failed to read entry table in '{}'.", path);
			return false;
		}
		entries[i].original_index = i;
		entries[i].original_offset = ReadLE32(entry_raw.data() + 0);
		entries[i].original_size = ReadLE32(entry_raw.data() + 4);
	}

	std::array<u8, 8> info_raw = {};
	if (std::fread(info_raw.data(), 1, info_raw.size(), base_file.get()) == info_raw.size())
	{
		entry_info_offset = ReadLE32(info_raw.data() + 0);
		entry_info_size = ReadLE32(info_raw.data() + 4);
	}

	// Load entry info table for names
	if (entry_info_offset != 0)
	{
		if (FileSystem::FSeek64(base_file.get(), static_cast<s64>(entry_info_offset), SEEK_SET) == 0)
		{
			for (u32 i = 0; i < file_count; i++)
			{
				std::array<u8, 48> info = {};
				if (std::fread(info.data(), 1, info.size(), base_file.get()) != info.size())
					break;

				std::string name(reinterpret_cast<const char*>(info.data()),
					std::find(info.begin(), info.begin() + 32, 0) - info.begin());
				name = StringUtil::StripWhitespace(name);
				if (name.empty())
					name = fmt::format("unnamed_{}", i);
				entries[i].name = name;
				entries[i].name_upper = StringUtil::toUpper(name);
				entries[i].mtime = std::time(nullptr);
			}
		}
	}

	for (u32 i = 0; i < file_count; i++)
	{
		if (entries[i].name.empty())
		{
			entries[i].name = fmt::format("unnamed_{}", i);
			entries[i].name_upper = StringUtil::toUpper(entries[i].name);
		}
	}

	// Log AFS contents for mod troubleshooting.
	if (file_count > 0)
	{
		Console.WriteLn("Virtual ISO: AFS '%s' contains %u entries:", afs_path.c_str(), file_count);
		const u32 max_log = 200;
		for (u32 i = 0; i < file_count && i < max_log; i++)
			Console.WriteLn("  [%u] %s", i, entries[i].name.c_str());
		if (file_count > max_log)
			Console.WriteLn("  ... %u more entries not shown ...", file_count - max_log);
	}

	const u32 header_size = 8 + (file_count * 8) + 8;
	data_start = AlignUp(header_size, SECTOR_SIZE);
	u32 data_cursor = data_start;

	for (u32 i = 0; i < file_count; i++)
	{
		const auto repl_it = replacements.find(entries[i].name_upper);
		if (repl_it != replacements.end())
		{
			const std::string& repl_path = repl_it->second;
			FILESYSTEM_STAT_DATA sd;
			if (FileSystem::StatFile(repl_path.c_str(), &sd))
			{
				entries[i].virtual_size = static_cast<u32>(sd.Size);
				entries[i].source_path = repl_path;
				entries[i].source_offset = 0;
				entries[i].mtime = sd.ModificationTime;
				entries[i].replaced = true;
				Console.WriteLn("Virtual ISO: AFS '%s' entry %u '%s' replaced by name match: '%s' (%u -> %u bytes).",
					afs_path.c_str(), i, entries[i].name.c_str(), repl_path.c_str(),
					entries[i].original_size, entries[i].virtual_size);
			}
			else
			{
				entries[i].virtual_size = entries[i].original_size;
				entries[i].source_path = afs_path;
				entries[i].source_offset = entries[i].original_offset;
				Console.WriteLn("Virtual ISO: AFS '%s' entry %u '%s' replacement file missing/unreadable: '%s'.",
					afs_path.c_str(), i, entries[i].name.c_str(), repl_path.c_str());
			}
		}
		else
		{
			entries[i].virtual_size = entries[i].original_size;
			entries[i].source_path = afs_path;
			entries[i].source_offset = entries[i].original_offset;
		}

		entries[i].virtual_offset = data_cursor;
		data_cursor += AlignUp(entries[i].virtual_size, SECTOR_SIZE);
	}

	for (const auto& repl : replacements)
	{
		const bool matched = std::any_of(entries.begin(), entries.end(), [&repl](const VirtualAfsEntry& entry) {
			return entry.name_upper == repl.first;
		});
		if (!matched)
			Console.WriteLn("Virtual ISO: AFS '%s' name replacement did not match any entry: '%s' -> '%s'.",
				afs_path.c_str(), repl.first.c_str(), repl.second.c_str());
	}

	entry_info_offset = AlignUp(data_cursor, SECTOR_SIZE);
	entry_info_size = file_count * 48;
	virtual_size = entry_info_offset + entry_info_size;

	// Build header + entry table
	header.assign(data_start, 0);
	std::memcpy(header.data(), "AFS\0", 4);
	WriteLE32(header.data() + 4, file_count);
	u32 table_offset = 8;
	for (u32 i = 0; i < file_count; i++)
	{
		WriteLE32(header.data() + table_offset + 0, entries[i].virtual_offset);
		WriteLE32(header.data() + table_offset + 4, entries[i].virtual_size);
		table_offset += 8;
	}
	WriteLE32(header.data() + table_offset + 0, entry_info_offset);
	WriteLE32(header.data() + table_offset + 4, entry_info_size);

	// Build entry info table
	entry_info.assign(entry_info_size, 0);
	for (u32 i = 0; i < file_count; i++)
	{
		u8* info = entry_info.data() + (i * 48);
		const std::string& name = entries[i].name;
		const size_t copy_len = std::min<size_t>(31, name.size());
		std::memcpy(info, name.data(), copy_len);

		std::tm tm = {};
#ifdef _MSC_VER
		localtime_s(&tm, &entries[i].mtime);
#else
		localtime_r(&entries[i].mtime, &tm);
#endif
		WriteLE16(info + 32, static_cast<u16>(tm.tm_year + 1900));
		WriteLE16(info + 34, static_cast<u16>(tm.tm_mon + 1));
		WriteLE16(info + 36, static_cast<u16>(tm.tm_mday));
		WriteLE16(info + 38, static_cast<u16>(tm.tm_hour));
		WriteLE16(info + 40, static_cast<u16>(tm.tm_min));
		WriteLE16(info + 42, static_cast<u16>(tm.tm_sec));
		WriteLE32(info + 44, entries[i].virtual_size);
	}

	std::sort(entries.begin(), entries.end(), [](const VirtualAfsEntry& a, const VirtualAfsEntry& b) {
		return a.virtual_offset < b.virtual_offset;
	});

	return true;
}

bool VirtualAfs::Read(u8* buffer, u64 offset, u32 size)
{
	std::memset(buffer, 0, size);
	u64 cur = offset;
	u32 remaining = size;

	while (remaining > 0)
	{
		if (cur < header.size())
		{
			const u32 take = static_cast<u32>(std::min<u64>(remaining, header.size() - cur));
			std::memcpy(buffer + (size - remaining), header.data() + cur, take);
			cur += take;
			remaining -= take;
			continue;
		}

		if (cur >= entry_info_offset && cur < static_cast<u64>(entry_info_offset + entry_info_size))
		{
			const u64 rel = cur - entry_info_offset;
			const u32 take = static_cast<u32>(std::min<u64>(remaining, entry_info_size - rel));
			std::memcpy(buffer + (size - remaining), entry_info.data() + rel, take);
			cur += take;
			remaining -= take;
			continue;
		}

		VirtualAfsEntry* entry = FindEntry(cur);
		if (!entry)
		{
			const u64 next_boundary = entry_info_offset > cur ? entry_info_offset : (cur + remaining);
			const u32 take = static_cast<u32>(std::min<u64>(remaining, next_boundary - cur));
			cur += take;
			remaining -= take;
			continue;
		}

		const u64 entry_offset = cur - entry->virtual_offset;
		const u64 available = entry->virtual_size > entry_offset ? entry->virtual_size - entry_offset : 0;
		const u32 take = static_cast<u32>(std::min<u64>(remaining, available));
		if (take == 0)
		{
			cur += 1;
			remaining -= 1;
			continue;
		}

		Error error;
		std::FILE* fp = GetCachedFile(entry->source_path, &error);
		if (!fp)
			return false;

		const u64 src_offset = entry->source_offset + entry_offset;
		if (FileSystem::FSeek64(fp, static_cast<s64>(src_offset), SEEK_SET) != 0)
			return false;

		if (entry->replaced && !entry->read_logged)
		{
			Console.WriteLn("Virtual ISO: AFS '%s' serving replacement entry %u '%s' from '%s' (read offset %llu, %u bytes).",
				afs_path.c_str(), entry->original_index, entry->name.c_str(), entry->source_path.c_str(),
				static_cast<unsigned long long>(entry_offset), take);
			entry->read_logged = true;
		}

		const size_t read_bytes = std::fread(buffer + (size - remaining), 1, take, fp);
		if (read_bytes < take)
			std::memset(buffer + (size - remaining) + read_bytes, 0, take - read_bytes);

		cur += take;
		remaining -= take;
	}

	return true;
}

void VirtualIso::BuildFileExtents()
{
	m_file_extents.clear();
	for (const Entry* file : m_files)
	{
		const u32 sectors = AlignUp(static_cast<u32>(file->virtual_size), SECTOR_SIZE) / SECTOR_SIZE;
		FileExtent extent;
		extent.start_lsn = file->lsn;
		extent.end_lsn = file->lsn + sectors;
		extent.entry = file;
		m_file_extents.push_back(extent);
	}

	std::sort(m_file_extents.begin(), m_file_extents.end(), [](const FileExtent& a, const FileExtent& b) {
		return a.start_lsn < b.start_lsn;
	});
}

void VirtualIso::AddMetaSectors(u32 start_lsn, const std::vector<u8>& data)
{
	const u32 count = static_cast<u32>(data.size() / SECTOR_SIZE);
	for (u32 i = 0; i < count; i++)
	{
		std::array<u8, SECTOR_SIZE> sector = {};
		std::memcpy(sector.data(), data.data() + (i * SECTOR_SIZE), SECTOR_SIZE);
		AddMetaSector(start_lsn + i, sector);
	}
}

void VirtualIso::AddMetaSector(u32 lsn, const std::array<u8, SECTOR_SIZE>& data)
{
	MetaSector ms;
	ms.lsn = lsn;
	ms.data = data;
	m_meta_sector_lookup[lsn] = m_meta_sectors.size();
	m_meta_sectors.push_back(std::move(ms));
}

bool VirtualIso::ReadRawSector(u8* buffer, u32 lsn)
{
	if (lsn >= m_total_sectors)
		return false;

	const auto meta_it = m_meta_sector_lookup.find(lsn);
	if (meta_it != m_meta_sector_lookup.end())
	{
		const MetaSector& ms = m_meta_sectors[meta_it->second];
		std::memcpy(buffer, ms.data.data(), SECTOR_SIZE);
		return true;
	}

	// Provide minimal UDF anchor sectors so the IOP's UDF probe
	// doesn't crash or fail the disc. Skip when we have real metadata.
	if (!m_has_layout_metadata && !m_has_udf_metadata)
	{
		if (lsn == UDF_ANCHOR_LSN_PRIMARY || lsn == UDF_ANCHOR_LSN_SECONDARY)
		{
			WriteUdfAnchor(buffer, lsn);
			return true;
		}
		if (lsn == UDF_MAIN_VDS_LSN)
		{
			WriteUdfTerminatingDescriptor(buffer, lsn);
			return true;
		}
	}

	const auto extent = FindFileForLSN(lsn);
	if (!extent.has_value())
	{
		if (lsn >= ISO_SYSTEM_AREA_SECTORS && m_missing_extent_logs < 20)
		{
			Console.Warning(fmt::format("Virtual ISO: No file extent for LSN {} (total sectors {})", lsn, m_total_sectors));
			m_missing_extent_logs++;
		}
		// Return a zero-filled sector for unused space. Some probes (e.g. UDF anchor at 256/257)
		// expect a readable sector and will fail the disc if we return an I/O error.
		std::memset(buffer, 0, SECTOR_SIZE);
		return true;
	}

	return ReadFileSector(buffer, *extent, lsn);
}

bool VirtualIso::ReadFileSector(u8* buffer, const FileExtent& extent, u32 lsn)
{
	const Entry* entry = extent.entry;
	if (!entry)
		return false;

	const u64 sector_offset = static_cast<u64>(lsn - extent.start_lsn) * SECTOR_SIZE;
	if (!m_afs_replacements.empty() && IsAfsPath(entry->host_path))
	{
		const std::string afs_key = NormalizePathKey(entry->host_path);
		auto it = m_afs_cache.find(afs_key);
		if (it == m_afs_cache.end())
		{
			auto repl_it = m_afs_replacements.find(afs_key);
			if (repl_it != m_afs_replacements.end())
			{
				Error error;
				auto afs = std::make_unique<VirtualAfs>();
				if (afs->Load(entry->host_path, repl_it->second, &error))
				{
					it = m_afs_cache.emplace(afs_key, std::move(afs)).first;
				}
				else
				{
					Console.Warning("Virtual ISO: Failed to build AFS overlay for '%s': %s",
						entry->host_path.c_str(), error.GetDescription().c_str());
				}
			}
		}

		if (it != m_afs_cache.end())
		{
			if (it->second->Read(buffer, sector_offset, SECTOR_SIZE))
				return true;
		}
	}

	const u64 remaining = (entry->virtual_size > static_cast<s64>(sector_offset)) ? static_cast<u64>(entry->virtual_size) - sector_offset : 0;
	const u32 to_read = static_cast<u32>(std::min<u64>(SECTOR_SIZE, remaining));

	std::FILE* fp = nullptr;
	Error error;
	std::scoped_lock lock(m_file_cache_mutex);
	if (!GetFileHandleLocked(entry->host_path, &fp, &error) || !fp)
	{
		Console.Error(fmt::format("Virtual ISO: Failed to open '{}': {}", entry->host_path, error.GetDescription()));
		std::memset(buffer, 0, SECTOR_SIZE);
		return false;
	}

	if (FileSystem::FSeek64(fp, static_cast<s64>(sector_offset), SEEK_SET) != 0)
	{
		std::memset(buffer, 0, SECTOR_SIZE);
		return false;
	}

	const size_t read_bytes = std::fread(buffer, 1, to_read, fp);
	if (read_bytes < to_read)
		std::memset(buffer + read_bytes, 0, SECTOR_SIZE - read_bytes);
	else if (to_read < SECTOR_SIZE)
		std::memset(buffer + to_read, 0, SECTOR_SIZE - to_read);

	return true;
}

std::optional<VirtualIso::FileExtent> VirtualIso::FindFileForLSN(u32 lsn) const
{
	if (m_file_extents.empty())
		return std::nullopt;

	auto it = std::upper_bound(m_file_extents.begin(), m_file_extents.end(), lsn,
		[](u32 value, const FileExtent& extent) { return value < extent.start_lsn; });
	if (it == m_file_extents.begin())
	{
		// lsn is before the first extent
		if (lsn < it->start_lsn)
			return std::nullopt;
	}
	else
	{
		--it;
	}

	if (lsn < it->start_lsn || lsn >= it->end_lsn)
		return std::nullopt;

	return *it;
}

bool VirtualIso::GetFileHandleLocked(const std::string& path, std::FILE** fp, Error* error)
{
	for (auto it = m_file_cache.begin(); it != m_file_cache.end(); ++it)
	{
		if (StringUtil::compareNoCase(it->path, path))
		{
			CachedFile cached = std::move(*it);
			m_file_cache.erase(it);
			m_file_cache.insert(m_file_cache.begin(), std::move(cached));
			*fp = m_file_cache.front().file.get();
			return true;
		}
	}

	FileSystem::ManagedCFilePtr file = FileSystem::OpenManagedSharedCFile(path.c_str(), "rb", FileSystem::FileShareMode::DenyNone, error);
	if (!file)
		return false;

	if (m_file_cache.size() >= FILE_CACHE_LIMIT)
		m_file_cache.pop_back();

	DevCon.WriteLn("Virtual ISO: Open file '%s'", path.c_str());
	m_file_cache.insert(m_file_cache.begin(), CachedFile{path, std::move(file)});
	*fp = m_file_cache.front().file.get();
	return true;
}

void VirtualIso::Clear()
{
	m_entries.clear();
	m_dirs.clear();
	m_files.clear();
	m_file_extents.clear();
	m_meta_sectors.clear();
	m_meta_sector_lookup.clear();
	m_root = nullptr;
	m_total_sectors = 0;
	m_cdtype = CDVD_TYPE_PS2DVD;
	m_is_open = false;
	m_file_cache.clear();
	m_read_mode = CDVD_MODE_2048;
	m_readbuffer.fill(0);
	m_missing_extent_logs = 0;
	m_layout.clear();
	m_has_layout_metadata = false;
	m_has_udf_metadata = false;
	m_udf_partition_start = 0;
	m_use_layout_files = false;
	m_mods_root.clear();
	m_enabled_mods.clear();
	m_afs_replacements.clear();
	m_afs_cache.clear();
}
