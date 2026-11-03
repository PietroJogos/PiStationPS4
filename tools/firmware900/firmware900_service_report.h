// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <iosfwd>
#include "firmware900_manager.h"

namespace PiStation {

void WriteServiceJson(std::ostream& out, const ScanReport& report);
void WriteServiceText(std::ostream& out, const ScanReport& report);
void WriteServiceEventJson(std::ostream& out, const ServiceEvent& event);

} // namespace PiStation
