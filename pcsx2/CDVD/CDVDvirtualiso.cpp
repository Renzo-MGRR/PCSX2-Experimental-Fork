// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "CDVD/VirtualIso.h"
#include "CDVD/CDVD.h"

#include "common/Error.h"

static VirtualIso s_virtual_iso;

static void VISOclose()
{
	s_virtual_iso.Close();
}

void CDVDvirtualiso_ReloadMods()
{
	s_virtual_iso.ReloadModsAndRebuild();
}

static bool VISOopen(std::string filename, Error* error)
{
	VISOclose();
	return s_virtual_iso.Open(std::move(filename), error);
}

static bool VISOprecache(ProgressCallback* /*progress*/, Error* /*error*/)
{
	return true;
}

static s32 VISOreadSubQ(u32 lsn, cdvdSubQ* subq)
{
	return s_virtual_iso.ReadSubQ(lsn, subq);
}

static s32 VISOgetTN(cdvdTN* Buffer)
{
	return s_virtual_iso.GetTN(Buffer);
}

static s32 VISOgetTD(u8 Track, cdvdTD* Buffer)
{
	return s_virtual_iso.GetTD(Track, Buffer);
}

static s32 VISOgetTOC(void* toc)
{
	return s_virtual_iso.GetTOC(toc);
}

static s32 VISOgetDiskType()
{
	return s_virtual_iso.GetDiskType();
}

static s32 VISOreadSector(u8* buffer, u32 lsn, int mode)
{
	return s_virtual_iso.ReadSector(buffer, lsn, mode);
}

static s32 VISOreadTrack(u32 lsn, int mode)
{
	return s_virtual_iso.ReadTrack(lsn, mode);
}

static s32 VISOgetBuffer(u8* buffer)
{
	return s_virtual_iso.GetBuffer(buffer);
}

static s32 VISOgetDualInfo(s32* dualType, u32* _layer1start)
{
	return s_virtual_iso.GetDualInfo(dualType, _layer1start);
}

static s32 VISOgetTrayStatus()
{
	return CDVD_DISC_ENGAGED;
}

static s32 VISOctrlTrayOpen()
{
	return 0;
}

static s32 VISOctrlTrayClose()
{
	return 0;
}

static void VISOnewDiskCB(void (* /* callback */)())
{
}

const CDVD_API CDVDapi_VirtualIso =
	{
		VISOclose,
		VISOopen,
		VISOprecache,
		VISOreadTrack,
		VISOgetBuffer,
		VISOreadSubQ,
		VISOgetTN,
		VISOgetTD,
		VISOgetTOC,
		VISOgetDiskType,
		VISOgetTrayStatus,
		VISOctrlTrayOpen,
		VISOctrlTrayClose,
		VISOnewDiskCB,
		VISOreadSector,
		VISOgetDualInfo,
	};
