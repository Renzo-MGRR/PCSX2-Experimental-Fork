// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "CDVD/VirtualIsoLayout.h"
#include "CDVD/CDVDcommon.h"
#include "CDVD/IsoFileFormats.h"
#include "CDVD/IsoReader.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include <algorithm>
#include <array>
#include <limits>
#include <unordered_set>
#include <vector>

#include "fmt/format.h"

namespace
{
	static constexpr u32 SECTOR_SIZE = 2048;

	static u32 ReadLE32(const u8* p)
	{
		return static_cast<u32>(p[0]) |
			(static_cast<u32>(p[1]) << 8) |
			(static_cast<u32>(p[2]) << 16) |
			(static_cast<u32>(p[3]) << 24);
	}

	static u32 AlignUp(u32 value, u32 alignment)
	{
		return (value + alignment - 1) / alignment * alignment;
	}

	static bool ReadSector2048(InputIsoFile& iso, u32 lsn, u8* out, Error* error)
	{
		iso.BeginRead2(lsn);
		const int ret = iso.FinishRead3(out, CDVD_MODE_2048);
		if (ret < 0)
		{
			Error::SetStringFmt(error, "Failed to read sector %u from ISO.", lsn);
			return false;
		}
		return true;
	}

	static std::string SanitizeIsoName(const std::string_view name)
	{
		std::string out = StringUtil::toUpper(name);
		for (char& c : out)
		{
			if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ' ')
				continue;
			c = '_';
		}
		return out;
	}

	struct DirTask
	{
		std::string path;
		u32 lsn;
		u32 size;
	};

	static bool EnumerateIsoLayout(InputIsoFile& iso, std::vector<std::string>& lines, u32* out_min_lsn, Error* error)
	{
		std::array<u8, SECTOR_SIZE> pvd_sector = {};
		if (!ReadSector2048(iso, 16, pvd_sector.data(), error))
			return false;

		const auto* pvd = reinterpret_cast<const IsoReader::ISOPrimaryVolumeDescriptor*>(pvd_sector.data());
		if (std::memcmp(pvd->header.standard_identifier, "CD001", 5) != 0)
		{
			Error::SetString(error, "ISO is missing a valid primary volume descriptor.");
			return false;
		}

		const u8* root = pvd->root_directory_entry;
		const u32 root_lsn = ReadLE32(root + 2);
		const u32 root_size = ReadLE32(root + 10);

		std::vector<DirTask> stack;
		stack.push_back({"", root_lsn, root_size});

		std::unordered_set<u32> visited_dirs;
		visited_dirs.insert(root_lsn);

		u32 min_lsn = std::numeric_limits<u32>::max();

		std::array<u8, SECTOR_SIZE> sector = {};

		while (!stack.empty())
		{
			DirTask task = std::move(stack.back());
			stack.pop_back();

			const u32 sector_count = AlignUp(task.size, SECTOR_SIZE) / SECTOR_SIZE;
			for (u32 i = 0; i < sector_count; i++)
			{
				const u32 lsn = task.lsn + i;
				if (!ReadSector2048(iso, lsn, sector.data(), error))
					return false;

				u32 offset = 0;
				while (offset < SECTOR_SIZE)
				{
					const u8 entry_len = sector[offset];
					if (entry_len == 0)
						break;
					if (offset + entry_len > SECTOR_SIZE)
						break;

					const u8 flags = sector[offset + 25];
					const u8 name_len = sector[offset + 32];
					const u8* name_ptr = sector.data() + offset + 33;

					// Skip '.' and '..'
					if (name_len == 1 && (name_ptr[0] == 0 || name_ptr[0] == 1))
					{
						offset += entry_len;
						continue;
					}

					const u32 entry_lsn = ReadLE32(sector.data() + offset + 2);
					const u32 entry_size = ReadLE32(sector.data() + offset + 10);
					const bool is_dir = (flags & IsoReader::ISODirectoryEntryFlag_Directory) != 0;

					std::string raw_name(reinterpret_cast<const char*>(name_ptr), name_len);
					const std::string_view nover = IsoReader::RemoveVersionIdentifierFromPath(raw_name);
					std::string iso_name = SanitizeIsoName(nover);
					if (!is_dir)
						iso_name += ";1";

					std::string full_path = task.path.empty() ? iso_name : (task.path + "\\" + iso_name);

					if (is_dir)
					{
						if (visited_dirs.insert(entry_lsn).second)
							stack.push_back({full_path, entry_lsn, entry_size});
					}
					else
					{
						lines.push_back(StringUtil::StdStringFromFormat("%s|%u|%u", full_path.c_str(), entry_lsn, entry_size));
						min_lsn = std::min(min_lsn, entry_lsn);
					}

					offset += entry_len;
				}
			}
		}

		if (out_min_lsn)
			*out_min_lsn = (min_lsn == std::numeric_limits<u32>::max()) ? 0 : min_lsn;

		return true;
	}
} // namespace

bool VirtualIsoLayout::GenerateLayoutFromIso(const std::string& iso_path, const std::string& output_dir, Error* error)
{
	if (iso_path.empty())
	{
		Error::SetString(error, "No ISO file specified.");
		return false;
	}
	if (!FileSystem::FileExists(iso_path.c_str()))
	{
		Error::SetStringFmt(error, "ISO file not found: '{}'", iso_path);
		return false;
	}
	if (output_dir.empty())
	{
		Error::SetString(error, "No output directory specified.");
		return false;
	}
	if (!FileSystem::DirectoryExists(output_dir.c_str()))
	{
		Error::SetStringFmt(error, "Output directory does not exist: '{}'", output_dir);
		return false;
	}

	InputIsoFile iso;
	if (!iso.Open(iso_path, error))
	{
		Error::AddPrefix(error, fmt::format("Failed to open ISO '{}': ", iso_path));
		return false;
	}

	std::vector<std::string> lines;
	u32 min_lsn = 0;
	if (!EnumerateIsoLayout(iso, lines, &min_lsn, error))
		return false;

	std::sort(lines.begin(), lines.end());

	const std::string layout_path = Path::Combine(output_dir, "PCSX2_LAYOUT.txt");
	FileSystem::ManagedCFilePtr fp = FileSystem::OpenManagedCFile(layout_path.c_str(), "wb", error);
	if (!fp)
	{
		Error::AddPrefix(error, fmt::format("Failed to open output layout file '{}': ", layout_path));
		return false;
	}

	const std::string header = StringUtil::StdStringFromFormat(
		"# PCSX2 Virtual ISO layout\n# Source ISO: %s\n# Format: PATH|LSN|SIZE\n",
		iso_path.c_str());
	std::fwrite(header.data(), 1, header.size(), fp.get());
	for (const std::string& line : lines)
	{
		std::fwrite(line.data(), 1, line.size(), fp.get());
		std::fwrite("\n", 1, 1, fp.get());
	}

	Console.WriteLn("Virtual ISO: Wrote layout file '%s' (%zu entries)", layout_path.c_str(), lines.size());

	// Write metadata sectors (0 .. min_lsn-1) so the virtual ISO can reuse the original descriptors.
	if (min_lsn > 0)
	{
		const std::string meta_path = Path::Combine(output_dir, "PCSX2_LAYOUT.bin");
		FileSystem::ManagedCFilePtr meta_fp = FileSystem::OpenManagedCFile(meta_path.c_str(), "wb", error);
		if (!meta_fp)
		{
			Error::AddPrefix(error, fmt::format("Failed to open layout metadata file '{}': ", meta_path));
			return false;
		}

		std::array<u8, SECTOR_SIZE> sector = {};
		for (u32 lsn = 0; lsn < min_lsn; lsn++)
		{
			if (!ReadSector2048(iso, lsn, sector.data(), error))
				return false;
			if (std::fwrite(sector.data(), 1, SECTOR_SIZE, meta_fp.get()) != SECTOR_SIZE)
			{
				Error::SetStringFmt(error, "Failed to write layout metadata at LSN %u.", lsn);
				return false;
			}
		}

		Console.WriteLn("Virtual ISO: Wrote layout metadata file '%s' (%u sectors)", meta_path.c_str(), min_lsn);
	}

	return true;
}
