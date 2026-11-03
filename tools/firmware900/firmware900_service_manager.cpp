// SPDX-License-Identifier: GPL-2.0-or-later
#include "firmware900_service_manager.h"

#include <algorithm>
#include <map>
#include <tuple>
#include "source_service_catalog.h"

namespace PiStation {
namespace {

std::string Category(const ServiceRequirement& requirement) {
    if (std::ranges::any_of(requirement.function_names,
                            [](const auto& name) { return name.starts_with("sceAppMessaging"); })) {
        return "IPC";
    }
    if (requirement.library.starts_with("libkernel") || requirement.library == "libScePosix") {
        if (std::ranges::any_of(requirement.function_names, [](const auto& name) {
                return name.find("pthread") != std::string::npos ||
                       name.find("Pthread") != std::string::npos;
            })) {
            return "THREAD";
        }
        return "KERNEL";
    }
    if (requirement.module == "libSceSysmodule") {
        return "SYSMODULE";
    }
    if (requirement.module == "libSceUserService") {
        return "USERSERVICE";
    }
    if (requirement.module == "libSceSystemService") {
        return "SYSTEMSERVICE";
    }
    return requirement.resolution == "matched_lle_export_not_loaded" ? "LLE" : "HLE";
}

} // namespace

bool ValidNidSyntax(std::string_view nid) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    return nid.size() == 11 &&
           std::ranges::all_of(nid, [&](char value) { return alphabet.contains(value); });
}

void Firmware900ServiceManager::Analyze(ScanReport& report) const {
    auto& services = report.services;
    services = {};
    services.performed = true;
    services.complete = report.input_valid && report.complete && report.analysis.performed &&
                        report.analysis.complete && report.error_count == 0;
    services.live_hle_registry_checked = report.analysis.hle_registry_checked;
    services.source_catalog_digest = SourceServiceCatalogDigest();
    services.source_catalog_entries = SourceServiceCatalog().size();
    services.source_catalog_skipped = SourceServiceCatalogSkipped();
    services.classifications = {{"implemented_and_tested", 0},
                                {"partially_implemented", 0},
                                {"available_by_lle_metadata", 0},
                                {"absent_from_checked_providers", 0},
                                {"unknown_or_unverified", 0}};
    using Key = std::tuple<std::string_view, std::string_view, std::string_view>;
    std::map<Key, std::vector<const SourceServiceRecord*>> catalog;
    for (const auto& record : SourceServiceCatalog()) {
        catalog[{record.nid, record.library, record.module}].push_back(&record);
    }
    using ImportKey = std::tuple<std::string, std::string, std::string, std::string>;
    std::map<ImportKey, std::vector<const SymbolResolution*>> imports;
    for (const auto& resolution : report.analysis.imports) {
        imports[{resolution.from, resolution.name, resolution.library, resolution.module}]
            .push_back(&resolution);
    }
    for (const auto& file : report.files) {
        if (!file.elf || file.status != "metadata_validated") {
            continue;
        }
        const auto from = Firmware900Manager::PathText(file.path);
        for (const auto& symbol : file.symbols) {
            if (!symbol.imported) {
                continue;
            }
            ServiceRequirement requirement;
            requirement.requester = from;
            requirement.kind = "symbol_import";
            requirement.name = symbol.name;
            requirement.library = symbol.library;
            requirement.module = symbol.module;
            requirement.nid_validation = !symbol.qualified ? "not_applicable_unqualified_symbol"
                                         : ValidNidSyntax(symbol.name)
                                             ? "syntax_valid_identity_not_authenticated"
                                             : "invalid_nid_syntax";
            const auto found = imports.find({from, symbol.name, symbol.library, symbol.module});
            const SymbolResolution* resolution = found != imports.end() && found->second.size() == 1
                                                     ? found->second.front()
                                                     : nullptr;
            requirement.resolution =
                resolution ? resolution->status : "analysis_missing_or_ambiguous";
            requirement.provider = resolution ? resolution->provider : std::string{};
            requirement.reason = "no_runtime_call_or_functionality_test";
            const bool eligible = symbol.qualified && ValidNidSyntax(symbol.name) &&
                                  !symbol.library.empty() && !symbol.module.empty() &&
                                  (symbol.type == 0 || symbol.type == 1 || symbol.type == 2);
            bool stub_marker{};
            if (eligible) {
                const auto registered = catalog.find({symbol.name, symbol.library, symbol.module});
                if (registered != catalog.end()) {
                    for (const auto* record : registered->second) {
                        if (record->version != symbol.library_version ||
                            record->type != symbol.type) {
                            continue;
                        }
                        if (requirement.source_evidence.size() == 16) {
                            services.complete = false;
                            requirement.reason = "source_evidence_limit_reached";
                            break;
                        }
                        requirement.source_evidence.emplace_back(std::string(record->source) + ":" +
                                                                 std::string(record->evidence));
                        requirement.function_names.emplace_back(record->function);
                        stub_marker |= record->evidence == "source_contains_stub_marker";
                    }
                }
            }
            if (requirement.resolution == "matched_lle_export_not_loaded") {
                requirement.classification = "available_by_lle_metadata";
                requirement.reason = "export_found_not_loaded_abi_and_behavior_unverified";
            } else if (stub_marker &&
                       (requirement.resolution == "hle_registered_functionality_unknown" ||
                        !services.live_hle_registry_checked)) {
                requirement.classification = "partially_implemented";
                requirement.reason = "source_contains_stub_marker_runtime_selection_unverified";
            } else if (requirement.resolution == "hle_registry_error") {
                requirement.reason = "live_registry_query_failed_no_guest_call";
            } else if (!requirement.source_evidence.empty()) {
                requirement.reason = "literal_source_registration_does_not_prove_active_hle";
            } else if (requirement.resolution == "unresolved_static_runtime_unknown" &&
                       services.live_hle_registry_checked && services.complete &&
                       std::ranges::all_of(report.files, [](const auto& candidate) {
                           return candidate.elf && candidate.status == "metadata_validated" &&
                                  (candidate.symbol_status == "parsed_dynamic_symbols" ||
                                   candidate.symbol_status == "not_present");
                       })) {
                requirement.classification = "absent_from_checked_providers";
                requirement.reason = "not_found_in_supplied_live_registry_or_static_lle_scope";
            } else if (requirement.resolution == "invalid_nid_syntax" ||
                       requirement.resolution == "unverified_symbol_qualification" ||
                       requirement.resolution == "unsupported_symbol_type") {
                requirement.reason = "invalid_or_unsupported_symbol_not_queried";
            }
            ++services.classifications[requirement.classification];
            services.requirements.push_back(std::move(requirement));
        }
    }
    for (const auto& edge : report.analysis.edges) {
        ServiceRequirement requirement;
        requirement.requester = edge.from;
        requirement.kind = edge.kind;
        requirement.name = edge.name;
        requirement.resolution = edge.status;
        requirement.reason = "module_metadata_only_hle_service_coverage_not_inferred";
        if (edge.status == "available_metadata_not_loaded") {
            requirement.classification = "available_by_lle_metadata";
            requirement.provider = edge.targets.front();
        }
        ++services.classifications[requirement.classification];
        services.requirements.push_back(std::move(requirement));
    }
    std::map<ImportKey, const ServiceRequirement*> event_details;
    for (const auto& requirement : services.requirements) {
        if (requirement.kind == "symbol_import") {
            const ImportKey key{requirement.requester, requirement.name, requirement.library,
                                requirement.module};
            const auto [entry, inserted] = event_details.try_emplace(key, &requirement);
            if (!inserted) {
                entry->second = nullptr;
            }
        }
    }
    for (auto& event : report.diagnostics) {
        if (event.operation != "match_import" || !event.service) {
            continue;
        }
        const auto prefix = event.service->requester + ":";
        if (!event.related.starts_with(prefix)) {
            continue;
        }
        const auto found =
            event_details.find({event.service->requester, event.related.substr(prefix.size()),
                                event.service->library, event.service->module});
        if (found == event_details.end() || !found->second) {
            continue;
        }
        const auto& requirement = *found->second;
        event.category = Category(requirement);
        event.service = ServiceEvent{
            requirement.requester,
            requirement.library,
            requirement.module,
            requirement.function_names.empty() ? std::string{} : requirement.function_names.front(),
            requirement.nid_validation == "not_applicable_unqualified_symbol" ? std::string{}
                                                                              : requirement.name,
            requirement.resolution,
            std::nullopt,
            requirement.reason};
        event.message = requirement.classification + "; " + requirement.reason;
    }
}

} // namespace PiStation
