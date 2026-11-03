// SPDX-License-Identifier: GPL-2.0-or-later
#include "firmware900_service_report.h"

#include <ostream>
#include <utility>

namespace PiStation {
namespace {

std::string Quote(const std::string& value) {
    return Firmware900Manager::JsonString(value);
}

void Strings(std::ostream& out, const std::vector<std::string>& values) {
    out << '[';
    bool first = true;
    for (const auto& value : values) {
        if (!std::exchange(first, false)) {
            out << ',';
        }
        out << Quote(value);
    }
    out << ']';
}

} // namespace

void WriteServiceEventJson(std::ostream& out, const ServiceEvent& event) {
    out << "{\"requester\":" << Quote(event.requester) << ",\"library\":" << Quote(event.library)
        << ",\"module\":" << Quote(event.module) << ",\"function\":" << Quote(event.function)
        << ",\"nid\":" << Quote(event.nid) << ",\"resolution\":" << Quote(event.resolution)
        << ",\"return_code\":";
    if (event.return_code) {
        out << *event.return_code;
    } else {
        out << "null";
    }
    out << ",\"reason\":" << Quote(event.reason) << '}';
}

void WriteServiceJson(std::ostream& out, const ScanReport& report) {
    const auto& services = report.services;
    out << ",\"services\":{\"performed\":" << services.performed
        << ",\"complete\":" << services.complete
        << ",\"coverage\":\"observed_imports_and_dependencies_only\""
        << ",\"live_hle_registry_checked\":" << services.live_hle_registry_checked
        << ",\"source_catalog\":{\"scope\":\"selected_literal_registrations_no_preprocessing\","
           "\"runtime_registry\":false,\"digest_sha256\":"
        << Quote(services.source_catalog_digest)
        << ",\"entries\":" << services.source_catalog_entries
        << ",\"skipped_registrations\":" << services.source_catalog_skipped
        << "},\"classification_counts\":{";
    bool first = true;
    for (const auto& [classification, count] : services.classifications) {
        if (!std::exchange(first, false)) {
            out << ',';
        }
        out << Quote(classification) << ':' << count;
    }
    out << "},\"requirements\":[";
    first = true;
    for (const auto& requirement : services.requirements) {
        if (!std::exchange(first, false)) {
            out << ',';
        }
        out << "{\"requester\":" << Quote(requirement.requester)
            << ",\"kind\":" << Quote(requirement.kind) << ",\"name\":" << Quote(requirement.name)
            << ",\"library\":" << Quote(requirement.library)
            << ",\"module\":" << Quote(requirement.module)
            << ",\"nid_validation\":" << Quote(requirement.nid_validation)
            << ",\"resolution\":" << Quote(requirement.resolution)
            << ",\"classification\":" << Quote(requirement.classification)
            << ",\"reason\":" << Quote(requirement.reason)
            << ",\"provider\":" << Quote(requirement.provider)
            << ",\"function_called\":false,\"return_code\":null,\"source_evidence\":";
        Strings(out, requirement.source_evidence);
        out << ",\"function_names\":";
        Strings(out, requirement.function_names);
        out << '}';
    }
    const auto& boot = report.boot;
    out << "]},\"boot_diagnostic\":{\"performed\":" << boot.performed
        << ",\"state\":" << Quote(boot.state)
        << ",\"boot_allowed\":false,\"runtime_budget_checked\":false,"
           "\"sequence_kind\":\"diagnostic_stages_not_guest_boot_order\","
           "\"module_start_order\":\"not_determined\",\"stages\":";
    Strings(out, boot.stages);
    out << ",\"blockers\":";
    Strings(out, boot.blockers);
    out << ",\"images\":[";
    first = true;
    for (const auto& image : boot.images) {
        if (!std::exchange(first, false)) {
            out << ',';
        }
        out << "{\"path\":" << Quote(image.path) << ",\"role\":" << Quote(image.role)
            << ",\"process_candidate\":" << image.process_candidate << ",\"entry\":" << image.entry
            << ",\"load_segments\":" << image.load_segments
            << ",\"segment_bytes\":" << image.segment_bytes
            << ",\"minimum_page_bytes\":" << image.minimum_page_bytes
            << ",\"memory_estimate_status\":" << Quote(image.memory_estimate_status)
            << ",\"tls_bytes\":" << image.tls_bytes
            << ",\"process_parameters\":" << Quote(image.process_parameters) << ",\"blockers\":";
        Strings(out, image.blockers);
        out << '}';
    }
    out << "]}";
}

void WriteServiceText(std::ostream& out, const ScanReport& report) {
    out << "\nServices: source evidence is not an active HLE registry.\n"
        << "Live registry checked: " << report.services.live_hle_registry_checked << '\n';
    for (const auto& requirement : report.services.requirements) {
        out << Quote(requirement.requester) << " requires " << Quote(requirement.name)
            << " lib=" << Quote(requirement.library) << " module=" << Quote(requirement.module)
            << ": " << requirement.classification << " (" << requirement.resolution
            << ") reason=" << Quote(requirement.reason) << " return_code=null (not called)\n";
        for (const auto& source : requirement.source_evidence) {
            out << "  source=" << Quote(source) << '\n';
        }
    }
    if (!report.boot.performed) {
        return;
    }
    out << "\nPS4 Firmware 9.00 Boot Diagnostic\n" << report.boot.state << '\n';
    for (const auto& stage : report.boot.stages) {
        out << "Stage: " << stage << '\n';
    }
    for (const auto& blocker : report.boot.blockers) {
        out << "Blocker: " << blocker << '\n';
    }
    for (const auto& image : report.boot.images) {
        out << "Image: " << Quote(image.path) << " entry=" << image.entry
            << " minimum_page_bytes=" << image.minimum_page_bytes
            << " memory_estimate=" << image.memory_estimate_status << '\n';
        for (const auto& blocker : image.blockers) {
            out << "  Blocker: " << blocker << '\n';
        }
    }
}

} // namespace PiStation
