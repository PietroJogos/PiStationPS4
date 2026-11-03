// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>

namespace PiStation {

struct ParserBudget {
    static constexpr std::size_t ChunkEntries = 1024;
    static constexpr std::size_t MaxFileSymbols = 131072;
    static constexpr std::size_t MaxFileRelocations = 1048576;
    static constexpr std::size_t MaxSymbolEntries = 1048576;
    static constexpr std::size_t MaxRelocationEntries = 8388608;
    static constexpr std::size_t MaxValidationSteps = 67108864;
    static constexpr std::size_t MaxSymbolBytes = 128 * 1024 * 1024;
    static constexpr std::size_t MaxStructureBytes = 16 * 1024 * 1024;

    std::size_t symbol_entries = MaxSymbolEntries;
    std::size_t relocation_entries = MaxRelocationEntries;
    std::size_t validation_steps = MaxValidationSteps;
    std::size_t symbol_bytes = MaxSymbolBytes;
    std::size_t structure_bytes = MaxStructureBytes;
};

} // namespace PiStation
