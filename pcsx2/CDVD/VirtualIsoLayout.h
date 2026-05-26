// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <string>

class Error;

namespace VirtualIsoLayout
{
	// Generates a PCSX2_LAYOUT.txt file from an ISO image.
	// Returns false on error and fills the Error parameter.
	bool GenerateLayoutFromIso(const std::string& iso_path, const std::string& output_dir, Error* error);
}
