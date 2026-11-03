// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <fstream>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/libraries/error_codes.h"
#include "core/linker.h"
#include "core/loader/elf_types.h"
#include "core/memory.h"
#include "core/pistation/native_boot_backend.h"

namespace PiStation {

namespace {

constexpr u64 GuestMagic = 0x5069537461746e50ULL;
constexpr u64 GuestArgument = 0x1234;
constexpr u64 CodeOffset = 0x200;
constexpr u64 DataOffset = 0x1000;
constexpr u64 SegmentSize = 0x1000;
constexpr u32 ExpectedReturn = static_cast<u32>(GuestMagic) ^ static_cast<u32>(GuestArgument);

nlohmann::json EventToJson(const BootEvent& event) {
    return {{"tag", event.tag},
            {"operation", event.operation},
            {"detail", event.detail},
            {"result", event.result}};
}

void Append(std::vector<u8>& out, const void* data, size_t size) {
    const auto* bytes = static_cast<const u8*>(data);
    out.insert(out.end(), bytes, bytes + size);
}

} // namespace

NativeBootBackend& NativeBootBackend::Instance() {
    static NativeBootBackend backend;
    return backend;
}

void NativeBootBackend::Enable(std::filesystem::path directory) {
    std::scoped_lock lk{mutex};
    report_directory = std::move(directory);
    enabled = true;
}

bool NativeBootBackend::IsEnabled() const {
    std::scoped_lock lk{mutex};
    return enabled;
}

bool NativeBootBackend::HasFailure() const {
    std::scoped_lock lk{mutex};
    return has_failure;
}

const char* NativeBootBackend::PhaseName(BootPhase phase) {
    switch (phase) {
    case BootPhase::FileIdentified:
        return "file_identified";
    case BootPhase::FileLoaded:
        return "file_loaded";
    case BootPhase::MemoryPrepared:
        return "memory_prepared";
    case BootPhase::EntryReached:
        return "entry_reached";
    case BootPhase::GuestExecuted:
        return "guest_code_executed";
    case BootPhase::ExecutionFinished:
        return "execution_finished";
    case BootPhase::ResourcesReleased:
        return "resources_released";
    default:
        return "unknown";
    }
}

void NativeBootBackend::Record(std::string tag, std::string operation, std::string detail,
                               std::string result) {
    std::scoped_lock lk{mutex};
    if (!enabled) {
        return;
    }
    LOG_INFO(Core_Linker, "[{}] {} {} -> {}", tag, operation, detail, result);
    events.push_back({std::move(tag), std::move(operation), std::move(detail), std::move(result)});
}

void NativeBootBackend::ReachPhase(BootPhase phase, std::string evidence) {
    {
        std::scoped_lock lk{mutex};
        if (!enabled) {
            return;
        }
        auto& record = phases[static_cast<size_t>(phase)];
        record.reached = true;
        record.evidence = std::move(evidence);
        LOG_INFO(Core_Linker, "[PISTATION_BOOT] phase={} evidence={}", PhaseName(phase),
                 record.evidence);
    }
    WriteReports();
}

void NativeBootBackend::RecordFailure(std::string tag, std::string operation, std::string detail) {
    {
        std::scoped_lock lk{mutex};
        if (!enabled) {
            return;
        }
        LOG_ERROR(Core_Linker, "[BOOT_ERROR] [{}] {} {}", tag, operation, detail);
        BootEvent event{std::move(tag), std::move(operation), std::move(detail), "failed"};
        if (!has_failure) {
            has_failure = true;
            first_failure = event;
        }
        events.push_back(std::move(event));
    }
    WriteReports();
}

void NativeBootBackend::RecordUnresolvedSymbol(const std::string& library,
                                               const std::string& module, const std::string& nid,
                                               const std::string& name) {
    std::scoped_lock lk{mutex};
    if (!enabled) {
        return;
    }
    unresolved.push_back(
        {"LINKER", "unresolved_symbol", fmt::format("{}:{}:{}", library, module, nid), name});
}

bool NativeBootBackend::WriteReports() const {
    std::scoped_lock lk{mutex};
    if (!enabled || report_directory.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(report_directory, ec);

    nlohmann::json json;
    json["schema"] = "pistation-native-boot-report-1";
    nlohmann::json phase_json = nlohmann::json::object();
    for (size_t i = 0; i < phases.size(); i++) {
        phase_json[PhaseName(static_cast<BootPhase>(i))] = {{"reached", phases[i].reached},
                                                            {"evidence", phases[i].evidence}};
    }
    json["phases"] = std::move(phase_json);
    json["first_failure"] = has_failure ? EventToJson(first_failure) : nlohmann::json(nullptr);
    json["events"] = nlohmann::json::array();
    for (const auto& event : events) {
        json["events"].push_back(EventToJson(event));
    }
    json["unresolved_symbols"] = nlohmann::json::array();
    for (const auto& event : unresolved) {
        json["unresolved_symbols"].push_back(EventToJson(event));
    }

    bool ok = true;
    {
        std::ofstream out(report_directory / "native_boot_report.json", std::ios::trunc);
        out << json.dump(2) << '\n';
        ok &= out.good();
    }
    {
        std::ofstream out(report_directory / "native_boot_report.txt", std::ios::trunc);
        out << "PiStation native boot report\n";
        for (size_t i = 0; i < phases.size(); i++) {
            out << (phases[i].reached ? "[x] " : "[ ] ") << PhaseName(static_cast<BootPhase>(i));
            if (phases[i].reached) {
                out << " : " << phases[i].evidence;
            }
            out << '\n';
        }
        if (has_failure) {
            out << "first failure: [" << first_failure.tag << "] " << first_failure.operation
                << " " << first_failure.detail << '\n';
        }
        out << "unresolved symbols: " << unresolved.size() << '\n';
        for (const auto& event : events) {
            out << '[' << event.tag << "] " << event.operation << ' ' << event.detail << " -> "
                << event.result << '\n';
        }
        ok &= out.good();
    }
    return ok;
}

std::vector<u8> BuildSyntheticGuestElf() {
    constexpr u16 ProgramHeaderCount = 4;
    constexpr u64 ProgramHeaderOffset = sizeof(elf_header);
    constexpr u64 DynamicOffset =
        ProgramHeaderOffset + ProgramHeaderCount * sizeof(elf_program_header);
    constexpr u64 DynlibDataOffset = DynamicOffset + 2 * sizeof(elf_dynamic);
    constexpr u64 DynlibDataSize = 0x10;

    elf_header header{};
    header.e_ident.magic[0] = ELFMAG0;
    header.e_ident.magic[1] = ELFMAG1;
    header.e_ident.magic[2] = ELFMAG2;
    header.e_ident.magic[3] = ELFMAG3;
    header.e_ident.ei_class = ELF_CLASS_64;
    header.e_ident.ei_data = ELF_DATA_2LSB;
    header.e_ident.ei_version = ELF_VERSION_CURRENT;
    header.e_ident.ei_osabi = ELF_OSABI_FREEBSD;
    header.e_ident.ei_abiversion = ELF_ABI_VERSION_AMDGPU_HSA_V2;
    header.e_type = ET_SCE_DYNAMIC;
    header.e_machine = EM_X86_64;
    header.e_version = EV_CURRENT;
    header.e_entry = CodeOffset;
    header.e_phoff = ProgramHeaderOffset;
    header.e_ehsize = sizeof(elf_header);
    header.e_phentsize = sizeof(elf_program_header);
    header.e_phnum = ProgramHeaderCount;

    elf_program_header text{};
    text.p_type = PT_LOAD;
    text.p_flags = static_cast<elf_program_flags>(PF_READ | PF_EXEC);
    text.p_filesz = SegmentSize;
    text.p_memsz = SegmentSize;
    text.p_align = SegmentSize;

    elf_program_header data{};
    data.p_type = PT_LOAD;
    data.p_flags = static_cast<elf_program_flags>(PF_READ | PF_WRITE);
    data.p_offset = DataOffset;
    data.p_vaddr = DataOffset;
    data.p_filesz = 0x10;
    data.p_memsz = SegmentSize;
    data.p_align = SegmentSize;

    elf_program_header dynamic{};
    dynamic.p_type = PT_DYNAMIC;
    dynamic.p_flags = PF_READ;
    dynamic.p_offset = DynamicOffset;
    dynamic.p_vaddr = DynamicOffset;
    dynamic.p_filesz = 2 * sizeof(elf_dynamic);
    dynamic.p_memsz = dynamic.p_filesz;
    dynamic.p_align = 8;

    elf_program_header dynlib{};
    dynlib.p_type = PT_SCE_DYNLIBDATA;
    dynlib.p_flags = PF_READ;
    dynlib.p_offset = DynlibDataOffset;
    dynlib.p_filesz = DynlibDataSize;
    dynlib.p_memsz = DynlibDataSize;
    dynlib.p_align = 16;

    elf_dynamic dynamic_entries[2]{};
    dynamic_entries[0].d_tag = DT_INIT;
    dynamic_entries[0].d_un.d_ptr = CodeOffset;
    dynamic_entries[1].d_tag = DT_NULL;

    std::vector<u8> code;
    const auto emit = [&code](std::initializer_list<u8> bytes) {
        code.insert(code.end(), bytes.begin(), bytes.end());
    };
    const s32 data_displacement = static_cast<s32>(DataOffset - (CodeOffset + 7));
    emit({0x48, 0x8D, 0x0D});
    Append(code, &data_displacement, sizeof(data_displacement));
    emit({0x48, 0xB8});
    Append(code, &GuestMagic, sizeof(GuestMagic));
    emit({0x48, 0x89, 0x01});
    emit({0x48, 0x89, 0x79, 0x08});
    emit({0x31, 0xF8});
    emit({0xC3});

    std::vector<u8> image(DataOffset + 0x10, 0);
    std::memcpy(image.data(), &header, sizeof(header));
    u8* phdr = image.data() + ProgramHeaderOffset;
    std::memcpy(phdr, &text, sizeof(text));
    std::memcpy(phdr + sizeof(text), &data, sizeof(data));
    std::memcpy(phdr + 2 * sizeof(text), &dynamic, sizeof(dynamic));
    std::memcpy(phdr + 3 * sizeof(text), &dynlib, sizeof(dynlib));
    std::memcpy(image.data() + DynamicOffset, dynamic_entries, sizeof(dynamic_entries));
    std::memcpy(image.data() + CodeOffset, code.data(), code.size());
    return image;
}

int RunSyntheticSelfTest(const std::filesystem::path& report_directory) {
    auto& backend = NativeBootBackend::Instance();
    backend.Enable(report_directory);

    std::error_code ec;
    std::filesystem::create_directories(report_directory, ec);
    const auto elf_path = report_directory / "synthetic_guest.elf";
    const auto image = BuildSyntheticGuestElf();
    {
        std::ofstream out(elf_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(image.data()),
                  static_cast<std::streamsize>(image.size()));
        if (!out.good()) {
            backend.RecordFailure("PISTATION_BOOT", "write_synthetic_elf", elf_path.string());
            return 1;
        }
    }
    backend.ReachPhase(BootPhase::FileIdentified,
                       fmt::format("synthetic ELF written, {} bytes, e_type=ET_SCE_DYNAMIC",
                                   image.size()));

    Common::ElfInfo::Instance().InitializeSynthetic(Common::ElfInfo::FW_900);
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    const s32 module_id = linker->LoadModule(elf_path, true);
    auto* module = linker->GetModule(module_id);
    if (module_id < 0 || !module || !module->IsValid()) {
        backend.RecordFailure("ELF_LOADER", "load_module", elf_path.string());
        return 1;
    }
    backend.ReachPhase(BootPhase::FileLoaded,
                       fmt::format("module '{}' loaded by Core::Linker at base {:#x}", module->name,
                                   module->GetBaseAddress()));

    const VAddr base = module->GetBaseAddress();
    const VAddr entry = module->GetEntryAddress();
    const VAddr data_address = base + DataOffset;
    backend.Record("MEMORY", "segments_mapped",
                   fmt::format("base={:#x} size={:#x} data={:#x}", base, module->aligned_base_size,
                               data_address),
                   "ok");
    if (module->info.num_segments != 3) {
        backend.RecordFailure(
            "MEMORY", "segment_count",
            fmt::format("expected 3 recorded segments (2 mapped, 1 dynamic), got {}", module->info.num_segments));
        return 1;
    }
    std::array<u8, 16> before{};
    std::memcpy(before.data(), reinterpret_cast<void*>(data_address), before.size());
    const bool data_clear = std::ranges::all_of(before, [](u8 b) { return b == 0; });
    backend.ReachPhase(BootPhase::MemoryPrepared,
                       fmt::format("RX and RW segments mapped; sentinel at {:#x} is {}",
                                   data_address, data_clear ? "zero" : "NOT zero"));
    if (!data_clear) {
        backend.RecordFailure("MEMORY", "sentinel_precondition", "sentinel was not zero");
        return 1;
    }

    backend.ReachPhase(BootPhase::EntryReached,
                       fmt::format("e_entry resolves to {:#x}, DT_INIT to {:#x}", entry,
                                   module->dynamic_info.init_virtual_addr + base));
    backend.Record("PROCESS", "invoke_guest", fmt::format("Module::Start arg={:#x}", GuestArgument),
                   "calling");
    const s32 result = module->Start(GuestArgument, nullptr, nullptr);

    u64 stored_magic{};
    u64 stored_argument{};
    std::memcpy(&stored_magic, reinterpret_cast<void*>(data_address), sizeof(stored_magic));
    std::memcpy(&stored_argument, reinterpret_cast<void*>(data_address + 8),
                sizeof(stored_argument));
    if (stored_magic != GuestMagic || stored_argument != GuestArgument) {
        backend.RecordFailure(
            "PROCESS", "guest_effect_missing",
            fmt::format("magic={:#x} argument={:#x}", stored_magic, stored_argument));
        return 1;
    }
    backend.ReachPhase(BootPhase::GuestExecuted,
                       fmt::format("guest code wrote magic {:#x} and argument {:#x} into guest "
                                   "memory; the host did not write them",
                                   stored_magic, stored_argument));
    if (static_cast<u32>(result) != ExpectedReturn) {
        backend.RecordFailure("PROCESS", "unexpected_return",
                              fmt::format("returned {:#x}, expected {:#x}",
                                          static_cast<u32>(result), ExpectedReturn));
        return 1;
    }
    backend.ReachPhase(BootPhase::ExecutionFinished,
                       fmt::format("guest returned {:#x}, computed by guest code",
                                   static_cast<u32>(result)));

    backend.Record("MEMORY", "release_module",
                   fmt::format("base={:#x} size={:#x}", base, module->aligned_base_size),
                   "not_released: Core::Linker and Core::MemoryManager have no module unload; "
                   "unmapping Flexible module segments asserts in AddressSpace::SplitRegion");
    backend.WriteReports();
    return 0;
}

} // namespace PiStation
