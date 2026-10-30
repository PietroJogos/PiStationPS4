// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdexcept>
#include "firmware900_manager.h"
#include "read_only_firmware_vfs.h"

namespace PiStation {

class MetadataIssue : public std::runtime_error {
public:
    MetadataIssue(std::string kind, std::string stage, const std::string& message)
        : std::runtime_error(message), kind{std::move(kind)}, stage{std::move(stage)} {}
    std::string kind;
    std::string stage;
};

void AnalyzeElfMetadata(const ReadOnlyFirmwareVfs::File& input, FileRecord& file,
                        ParserBudget& budget);

} // namespace PiStation
