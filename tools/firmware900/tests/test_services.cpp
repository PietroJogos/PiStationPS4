// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <limits>
#include <stdexcept>
#include <thread>
#include "firmware900_boot_diagnostic.h"
#include "firmware900_service_manager.h"
#include "source_service_catalog.h"

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

PiStation::ScanReport Input(const std::string& nid = "-W4xI5aVI8w", unsigned type = 2) {
    PiStation::ScanReport report;
    report.input_valid = true;
    PiStation::FileRecord file;
    file.path = "SceShellCore.synthetic.elf";
    file.shell_role = "SceShellCore_name_candidate";
    file.status = "metadata_validated";
    file.symbol_status = "parsed_dynamic_symbols";
    file.elf = PiStation::ElfMetadata{};
    file.elf->machine = 62;
    file.symbols.push_back(
        {nid, "libkernel", "libkernel", 1, static_cast<std::uint8_t>(type), false, true, true});
    report.files.push_back(file);
    return report;
}

} // namespace

int main() {
    using namespace PiStation;
    Check(ValidNidSyntax("-W4xI5aVI8w") && !ValidNidSyntax("bad/nidAAAA") &&
              !ValidNidSyntax("short") && !ValidNidSyntax(std::string(11, '\0')),
          "NID syntax validation failed");
    ScanReport empty;
    Firmware900ServiceManager{}.Analyze(empty);
    Check(empty.services.performed && !empty.services.complete &&
              empty.services.requirements.empty(),
          "uninitialized analysis must not be complete");

    auto report = Input();
    Firmware900Manager::DiagnoseServices(report);
    Check(report.services.complete && report.services.requirements.size() == 1 &&
              report.services.requirements.front().classification == "partially_implemented" &&
              !report.services.live_hle_registry_checked,
          "known source stub must be visible without pretending a live registry was queried");
    Check(!report.diagnostics.back().service,
          "summary events should not duplicate per-import service details");
    bool has_service_event{};
    for (const auto& event : report.diagnostics) {
        if (event.service) {
            has_service_event = true;
            Check(event.category == "KERNEL" && !event.service->return_code &&
                      event.service->function == "sceKernelSetProcessProperty",
                  "service event fabricated a call result or lost its source function");
        }
    }
    Check(has_service_event, "structured service log missing");

    report = Input("UnknownNidA");
    Firmware900Manager::DiagnoseServices(report);
    Check(report.services.requirements[0].classification == "unknown_or_unverified",
          "offline absence must not prove a service is missing");
    report = Input("UnknownNidA");
    Firmware900Manager::DiagnoseServices(report, [](const auto&) { return false; });
    Check(report.services.requirements[0].classification == "absent_from_checked_providers",
          "missing import in complete supplied registries was not identified");
    report = Input("UnknownNidA");
    Firmware900Manager::DiagnoseServices(report, [](const auto&) { return true; });
    Check(report.services.requirements[0].classification == "unknown_or_unverified",
          "an address does not establish implementation or successful execution");

    for (const auto& [name, type] :
         std::vector<std::pair<std::string, unsigned>>{{"bad", 2}, {"UnknownNidA", 6}}) {
        report = Input(name, type);
        unsigned calls{};
        Firmware900Manager::DiagnoseServices(report, [&](const auto&) {
            ++calls;
            return true;
        });
        Check(calls == 0 &&
                  report.services.requirements[0].classification == "unknown_or_unverified",
              "invalid imports must be rejected before querying HLE");
    }
    report = Input();
    Firmware900Manager::DiagnoseServices(report, [](const auto&) -> bool { throw 7; });
    Check(!report.services.complete && report.error_count == 1 &&
              report.services.requirements[0].resolution == "hle_registry_error",
          "registry exceptions must be isolated and recorded");
    const auto failed_event_count = report.diagnostics.size();
    Firmware900Manager::DiagnoseServices(report, [](const auto&) -> bool { throw 7; });
    Check(report.error_count == 1 && report.diagnostics.size() == failed_event_count,
          "reanalysis must not duplicate errors or logs");
    Firmware900Manager::DiagnoseServices(report, [](const auto&) { return true; });
    Check(report.error_count == 0 && report.services.complete &&
              report.analysis.hle_query_count == 1 && report.analysis.hle_query_failures == 0,
          "a recovered registry query must replace previous diagnostic failures");

    report = Input("UnknownNidA");
    auto& elf = *report.files.front().elf;
    elf.segments_validated = true;
    elf.entry = 0x400180;
    elf.entry_in_executable_segment = true;
    elf.tls_present = true;
    elf.segments = {{1, 5, 0, 0x400000, 0x1000, 0x1000, 0x1000},
                    {1, 6, 0x1000, 0x401000, 0x1000, 0x1000, 0x1000},
                    {7, 4, 0, 0x401000, 16, 128, 16}};
    Firmware900Manager::DiagnoseServices(report);
    Firmware900BootDiagnostic{}.Analyze(report);
    Check(report.boot.images.size() == 1 && report.boot.images[0].minimum_page_bytes == 16384 &&
              report.boot.images[0].segment_bytes == 8192 &&
              report.boot.images[0].tls_bytes == 128 &&
              report.boot.state == "stopped_safely_without_execution",
          "memory planning must union shared pages without allocating guest memory");
    report.files.front().elf->segments.push_back(
        {1, 4, 0, std::numeric_limits<std::uint64_t>::max() - 1024, 0, 4096, 1});
    Firmware900BootDiagnostic{}.Analyze(report);
    Check(report.boot.images[0].memory_estimate_status == "incomplete_overflow",
          "overflow must invalidate the memory estimate");

    std::atomic<unsigned> failures{};
    std::vector<std::jthread> workers;
    for (unsigned i = 0; i < 4; ++i) {
        workers.emplace_back([&] {
            auto local = Input();
            Firmware900Manager::DiagnoseServices(local);
            Firmware900BootDiagnostic{}.Analyze(local);
            if (local.services.requirements.size() != 1 ||
                local.boot.state != "stopped_safely_without_execution") {
                ++failures;
            }
        });
    }
    workers.clear();
    Check(failures == 0, "independent host diagnostic workers must finish and join");
}
