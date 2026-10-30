// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "firmware900_manager.h"
#include "read_only_firmware_vfs.h"

namespace PiStation {

void AnalyzeElfMetadata(const ReadOnlyFirmwareVfs::File& input, FileRecord& file);

} // namespace PiStation
