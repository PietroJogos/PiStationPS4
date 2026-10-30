// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "firmware900_manager.h"

namespace PiStation {

using HleLookup = std::function<bool(const SymbolMetadata&)>;

class Firmware900DependencyAnalyzer {
public:
    void Analyze(ScanReport& report, const HleLookup& registered_hle = {}) const;
};

} // namespace PiStation
