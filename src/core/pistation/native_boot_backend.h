// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <map>
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

    void RecordStubCall(const std::string& name, const std::string& nid,
                        const std::string& thread, u64 return_address);

    void SkipGraphicsHle() {
        graphics_hle_skipped = true;
    }
    bool IsGraphicsHleSkipped() const {
        return graphics_hle_skipped;
    }

    void FinalizeReports(const char* reason);

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
    struct StubStat {
        std::string name;
        std::string nid;
        std::string first_thread;
        u64 first_return{};
        u64 first_sequence{};
        u64 calls{};
    };
    std::map<std::string, StubStat> stub_calls;
    u64 stub_sequence{};
    std::array<PhaseRecord, static_cast<size_t>(BootPhase::Count)> phases{};
    BootEvent first_failure;
    bool has_failure{};
    bool graphics_hle_skipped{};
};

std::vector<u8> BuildSyntheticGuestElf();

int RunSyntheticSelfTest(const std::filesystem::path& report_directory);

} // namespace PiStation
