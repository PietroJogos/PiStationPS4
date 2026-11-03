// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>

namespace PiStation {

int RunIpcSelfTest(const std::filesystem::path& report_directory);

int RunModuleLoadProbe(const std::filesystem::path& module_path,
                       const std::filesystem::path& report_directory);

} // namespace PiStation
