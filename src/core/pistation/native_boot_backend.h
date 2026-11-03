// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include "common/types.h"

namespace PiStation {

enum class BootPhase : u32 {
    FileIdentified,
    FileLoaded,
    MemoryPrepared,
    EntryReached,
    GuestExecuted,
    ExecutionFinished,
    ResourcesReleased,
    Count,
};

struct BootEvent {
    std::string tag;
    std::string operation;
    std::string detail;
    std::string result;
};

struct PhaseRecord {
    bool reached{};
    std::string evidence;
};

class NativeBootBackend {
public:
    static NativeBootBackend& Instance();

    void Enable(std::filesystem::path report_directory);
    bool IsEnabled() const;

    void Record(std::string tag, std::string operation, std::string detail, std::string result);
    void ReachPhase(BootPhase phase, std::string evidence);
    void RecordFailure(std::string tag, std::string operation, std::string detail);
    void RecordUnresolvedSymbol(const std::string& library, const std::string& module,
                                const std::string& nid, const std::string& name);

    bool HasFailure() const;
    bool WriteReports() const;

    static const char* PhaseName(BootPhase phase);

private:
    NativeBootBackend() = default;

    mutable std::mutex mutex;
    bool enabled{};
    std::filesystem::path report_directory;
    std::vector<BootEvent> events;
    std::vector<BootEvent> unresolved;
    std::array<PhaseRecord, static_cast<size_t>(BootPhase::Count)> phases{};
    BootEvent first_failure;
    bool has_failure{};
};

std::vector<u8> BuildSyntheticGuestElf();

int RunSyntheticSelfTest(const std::filesystem::path& report_directory);

} // namespace PiStation
