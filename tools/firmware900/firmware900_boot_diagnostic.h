// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "firmware900_manager.h"

namespace PiStation {

class Firmware900BootDiagnostic {
public:
    void Analyze(ScanReport& report) const;
};

} // namespace PiStation
