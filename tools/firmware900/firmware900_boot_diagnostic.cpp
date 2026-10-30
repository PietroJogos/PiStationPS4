// SPDX-License-Identifier: GPL-2.0-or-later
#include "firmware900_boot_diagnostic.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include "core/loader/elf_types.h"

namespace PiStation {
namespace {

void Block(std::vector<std::string>& blockers, std::string value) {
    if (!std::ranges::contains(blockers, value)) {
        blockers.push_back(std::move(value));
    }
}

void InspectMemory(const ElfMetadata& elf, BootImagePlan& image) {
    image.memory_estimate_status = "lower_bound_metadata_only";
    constexpr std::uint64_t PageSize = 16384;
    constexpr auto Maximum = std::numeric_limits<std::uint64_t>::max();
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pages;
    for (const auto& segment : elf.segments) {
        if (segment.type == PT_SCE_PROCPARAM) {
            image.process_parameters = "present_structure_and_pointers_unvalidated";
        }
        if (segment.type == PT_TLS) {
            image.tls_bytes = std::max(image.tls_bytes, segment.memory_size);
        }
        if (segment.type != PT_LOAD) {
            continue;
        }
        ++image.load_segments;
        if (segment.memory_size > Maximum - image.segment_bytes ||
            segment.memory_size > Maximum - segment.address) {
            Block(image.blockers, "memory_size_or_address_overflow");
            image.memory_estimate_status = "incomplete_overflow";
            continue;
        }
        image.segment_bytes += segment.memory_size;
        if ((segment.flags & 3) == 3) {
            Block(image.blockers, "writable_executable_segment_requires_review");
        }
        if (segment.memory_size == 0) {
            continue;
        }
        const auto end = segment.address + segment.memory_size;
        ranges.emplace_back(segment.address, end);
        if (end > Maximum - (PageSize - 1)) {
            Block(image.blockers, "page_rounding_overflow");
            image.memory_estimate_status = "incomplete_overflow";
            continue;
        }
        pages.emplace_back(segment.address / PageSize * PageSize,
                           (end + PageSize - 1) / PageSize * PageSize);
    }
    std::ranges::sort(ranges);
    std::uint64_t previous_end{};
    for (const auto& [begin, end] : ranges) {
        if (begin < previous_end) {
            Block(image.blockers, "overlapping_load_segments_requires_review");
        }
        previous_end = std::max(previous_end, end);
    }
    std::ranges::sort(pages);
    previous_end = 0;
    for (const auto& [begin, end] : pages) {
        const auto effective_begin = std::max(begin, previous_end);
        if (end > effective_begin) {
            image.minimum_page_bytes += end - effective_begin;
        }
        previous_end = std::max(previous_end, end);
    }
    if (image.load_segments == 0) {
        Block(image.blockers, "no_load_segments");
    }
}

} // namespace

void Firmware900BootDiagnostic::Analyze(ScanReport& report) const {
    auto& boot = report.boot;
    boot = {};
    boot.performed = true;
    boot.state = "stopped_safely_without_execution";
    boot.stages = {report.input_valid ? "directory:validated" : "directory:invalid",
                   report.complete ? "inventory:enumerated" : "inventory:incomplete",
                   "headers:inspected_without_payload_execution",
                   "dependencies:static_analysis_only",
                   report.services.performed ? "services:analyzed" : "services:not_analyzed",
                   "memory:metadata_only_no_allocation",
                   "blockers:recorded",
                   "diagnostic:stopped_without_guest_threads_or_module_initializers"};
    boot.blockers = {"firmware_version_unverified", "runtime_execution_disabled",
                     "guest_multiprocess_lifecycle_unverified", "shell_ipc_protocols_unverified",
                     "graphics_and_input_runtime_unverified"};
    if (!report.input_valid || !report.complete || report.error_count != 0 ||
        !report.analysis.performed || !report.analysis.complete || !report.services.performed ||
        !report.services.complete) {
        Block(boot.blockers, "input_or_analysis_incomplete");
    }
    if (!report.analysis.hle_registry_connected) {
        Block(boot.blockers, "live_hle_registry_not_connected");
    }
    std::map<std::string, std::vector<const ServiceRequirement*>> requirements;
    for (const auto& requirement : report.services.requirements) {
        requirements[requirement.requester].push_back(&requirement);
    }
    std::set<std::string> cyclic_images;
    for (const auto& cycle : report.analysis.cycles) {
        cyclic_images.insert(cycle.begin(), cycle.end());
    }
    bool ui{}, core{};
    for (const auto& file : report.files) {
        ui |= file.shell_role.starts_with("SceShellUI");
        core |= file.shell_role.starts_with("SceShellCore");
        if (!file.elf && !file.self && file.shell_role.empty()) {
            continue;
        }
        BootImagePlan image;
        image.path = Firmware900Manager::PathText(file.path);
        image.role = file.module_role;
        image.process_candidate = !file.shell_role.empty();
        image.blockers = {"runtime_execution_disabled", "module_initialization_not_attempted"};
        if (file.status != "metadata_validated" || !file.elf) {
            Block(image.blockers, file.self ? "self_payload_unavailable_for_analysis"
                                            : "executable_metadata_invalid_or_unavailable");
        } else {
            const auto& elf = *file.elf;
            image.entry = elf.entry;
            image.process_candidate |=
                elf.type == ET_EXEC || elf.type == ET_SCE_EXEC || elf.type == ET_SCE_DYNEXEC;
            image.process_parameters = "not_present";
            InspectMemory(elf, image);
            if (image.process_candidate && !elf.entry_in_executable_segment) {
                Block(image.blockers, "entry_not_covered_by_executable_segment");
            }
            if (image.process_candidate) {
                Block(image.blockers, "guest_arguments_stack_and_process_context_not_prepared");
                Block(image.blockers, image.process_parameters == "not_present"
                                          ? "ps4_process_parameters_missing"
                                          : "ps4_process_parameters_unvalidated");
            }
            if (elf.tls_present) {
                Block(image.blockers, "guest_tls_not_allocated");
            }
            if (elf.relocation_status != "not_present") {
                Block(image.blockers, "relocations_not_applied");
            }
            if (file.symbol_status == "count_unavailable" ||
                file.symbol_status == "incomplete_metadata_limit") {
                Block(image.blockers, "symbol_inventory_incomplete");
            }
        }
        for (const auto* requirement : requirements[image.path]) {
            if (requirement->classification == "partially_implemented") {
                Block(image.blockers, "service_source_contains_stub_marker");
            } else if (requirement->classification != "available_by_lle_metadata" &&
                       requirement->resolution != "weak_undefined") {
                Block(image.blockers, "service_dependencies_unverified_or_missing");
            }
        }
        if (cyclic_images.contains(image.path)) {
            Block(image.blockers, "module_dependency_cycles_require_start_order_review");
        }
        boot.images.push_back(std::move(image));
    }
    if (!ui) {
        Block(boot.blockers, "SceShellUI_not_identified_even_by_filename");
    }
    if (!core) {
        Block(boot.blockers, "SceShellCore_not_identified_even_by_filename");
    }
    std::erase_if(report.diagnostics, [](const auto& event) {
        return event.operation == "plan_boot" || event.operation == "plan_process";
    });
    for (const auto& image : boot.images) {
        report.diagnostics.push_back({"PROCESS",
                                      "metadata plan; no process or guest thread created",
                                      "plan_process", image.path, "blocked_without_execution"});
    }
    report.diagnostics.push_back({"BOOT",
                                  "PS4 Firmware 9.00 Boot Diagnostic",
                                  "plan_boot",
                                  {},
                                  "stopped_safely_without_execution"});
}

} // namespace PiStation
