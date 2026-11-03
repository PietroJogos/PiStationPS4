// SPDX-License-Identifier: GPL-2.0-or-later
#include "firmware900_dependency_analyzer.h"
#include "source_service_catalog.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace PiStation {
namespace {

bool Valid(const FileRecord& file) {
    return file.elf && file.status == "metadata_validated" && file.elf->machine == 62;
}

void Event(ScanReport& report, std::string category, std::string operation, std::string related,
           std::string result) {
    report.diagnostics.push_back(
        {std::move(category), result, std::move(operation), std::move(related), std::move(result)});
}

bool NameMatches(const std::string& provided, const std::string& wanted) {
    return provided == wanted || provided + ".sprx" == wanted;
}

} // namespace

void Firmware900DependencyAnalyzer::Analyze(ScanReport& report,
                                            const HleLookup& registered_hle) const {
    auto& analysis = report.analysis;
    report.error_count -= std::min(report.error_count, analysis.hle_query_failures);
    std::erase_if(report.diagnostics, [](const auto& event) {
        return event.operation == "match_import" || event.operation == "match_module_metadata" ||
               event.operation == "query_hle_registry" || event.operation == "inspect_registry" ||
               event.operation == "inspect_inventory";
    });
    analysis = {};
    analysis.performed = true;
    analysis.complete = report.complete;
    analysis.hle_registry_connected = static_cast<bool>(registered_hle);
    std::map<std::string, std::vector<std::string>> adjacency;
    std::map<std::string, std::set<std::string>> direct_providers;
    using SymbolKey = std::tuple<std::string, std::string, std::string>;
    std::map<SymbolKey, std::vector<std::pair<std::string, const SymbolMetadata*>>> exports;
    for (const auto& provider : report.files) {
        if (!Valid(provider)) {
            continue;
        }
        if (provider.symbol_status == "count_unavailable" ||
            provider.symbol_status == "incomplete_metadata_limit") {
            analysis.complete = false;
        }
        const auto path = Firmware900Manager::PathText(provider.path);
        adjacency[path];
        for (const auto& symbol : provider.symbols) {
            if (!symbol.imported) {
                exports[{symbol.name, symbol.library, symbol.module}].emplace_back(path, &symbol);
            }
        }
    }
    for (const auto& file : report.files) {
        if (!Valid(file)) {
            continue;
        }
        const auto from = Firmware900Manager::PathText(file.path);
        for (const auto& dep : file.dependencies) {
            DependencyEdge edge{from, dep.name, dep.kind, "missing_from_inventory", {}};
            bool version_difference{};
            for (const auto& provider : report.files) {
                const auto target = Firmware900Manager::PathText(provider.path);
                const auto filename = Firmware900Manager::PathText(provider.path.filename());
                bool named = false;
                if (dep.kind == "DT_SCE_IMPORT_LIB") {
                    for (const auto& identity : provider.exported_libraries) {
                        if (identity.name == dep.name) {
                            named = true;
                            version_difference |= identity.version != dep.version;
                        }
                    }
                } else if (dep.kind == "DT_SCE_NEEDED_MODULE") {
                    for (const auto& identity : provider.exported_modules) {
                        if (identity.name == dep.name) {
                            named = true;
                            version_difference |= identity.version != dep.version ||
                                                  identity.minor_version != dep.minor_version;
                        }
                    }
                } else {
                    named = filename == dep.name ||
                            (provider.module_identity &&
                             NameMatches(provider.module_identity->name, dep.name));
                }
                if (named && Valid(provider)) {
                    edge.targets.push_back(target);
                }
            }
            if (edge.targets.size() > 1) {
                edge.status = "ambiguous_provider";
            } else if (version_difference && !edge.targets.empty()) {
                edge.status = "version_difference_requires_review";
            } else if (!edge.targets.empty()) {
                edge.status = "available_metadata_not_loaded";
                adjacency[from].push_back(edge.targets.front());
                direct_providers[from].insert(edge.targets.front());
            } else if (!dep.filename_candidates.empty()) {
                edge.status = "unverified_or_incompatible_filename_candidate";
                edge.targets = dep.filename_candidates;
                if (edge.targets.size() == 1) {
                    for (const auto& candidate : report.files) {
                        if (Firmware900Manager::PathText(candidate.path) != edge.targets.front()) {
                            continue;
                        }
                        if (candidate.self && candidate.self->encrypted_segments) {
                            edge.status = "protected_self_unavailable";
                        } else if (candidate.status == "unsupported" ||
                                   candidate.status == "invalid_or_unreadable") {
                            edge.status = "incompatible_file_metadata";
                        } else if (Valid(candidate)) {
                            edge.status = "identity_metadata_mismatch";
                        }
                    }
                }
            }
            Event(report, "DEPENDENCIES", "match_module_metadata", from + ":" + dep.name,
                  edge.status);
            analysis.edges.push_back(std::move(edge));
        }
    }
    std::map<std::string, unsigned> color;
    for (auto& [node, links] : adjacency) {
        std::ranges::sort(links);
        links.erase(std::unique(links.begin(), links.end()), links.end());
        if (color[node]) {
            continue;
        }
        std::vector<std::pair<std::string, std::size_t>> stack{{node, 0}};
        color[node] = 1;
        while (!stack.empty()) {
            auto& [current, next] = stack.back();
            auto& neighbors = adjacency[current];
            if (next == neighbors.size()) {
                color[current] = 2;
                stack.pop_back();
                continue;
            }
            const auto target = neighbors[next++];
            if (color[target] == 0) {
                color[target] = 1;
                stack.emplace_back(target, 0);
            } else if (color[target] == 1) {
                if (analysis.cycles.size() == 64) {
                    analysis.complete = false;
                    continue;
                }
                std::vector<std::string> cycle;
                bool include{};
                for (const auto& [member, unused] : stack) {
                    (void)unused;
                    include |= member == target;
                    if (include) {
                        cycle.push_back(member);
                    }
                }
                cycle.push_back(target);
                analysis.cycles.push_back(std::move(cycle));
            }
        }
    }
    for (const auto& file : report.files) {
        if (!Valid(file)) {
            continue;
        }
        const auto from = Firmware900Manager::PathText(file.path);
        std::set<std::string> scope = direct_providers[from];
        std::vector<std::string> pending(scope.begin(), scope.end());
        for (std::size_t i = 0; i < pending.size(); ++i) {
            for (const auto& target : adjacency[pending[i]]) {
                if (scope.insert(target).second) {
                    pending.push_back(target);
                }
            }
        }
        for (const auto& symbol : file.symbols) {
            if (!symbol.imported) {
                continue;
            }
            SymbolResolution resolution{from,
                                        symbol.name,
                                        symbol.library,
                                        symbol.module,
                                        "unresolved_static_runtime_unknown",
                                        {}};
            bool hle_registered{};
            bool hle_error{};
            const bool supported_type = symbol.type == 0 || symbol.type == 1 || symbol.type == 2;
            const bool valid_qualification = symbol.qualified
                                                 ? !symbol.library.empty() && !symbol.module.empty()
                                                 : symbol.name.find('#') == std::string::npos;
            const bool valid_nid = !symbol.qualified || ValidNidSyntax(symbol.name);
            if (registered_hle && supported_type && valid_qualification && valid_nid) {
                analysis.hle_registry_checked = true;
                ++analysis.hle_query_count;
                try {
                    hle_registered = registered_hle(symbol);
                } catch (...) {
                    hle_error = true;
                    analysis.complete = false;
                    ++report.error_count;
                    ++analysis.hle_query_failures;
                    Event(report, "ERROR", "query_hle_registry", from + ":" + symbol.name,
                          "registry_query_failed");
                }
            }
            if (hle_error) {
                resolution.status = "hle_registry_error";
            } else if (!supported_type) {
                resolution.status = "unsupported_symbol_type";
            } else if (!valid_qualification) {
                resolution.status = "unverified_symbol_qualification";
            } else if (!valid_nid) {
                resolution.status = "invalid_nid_syntax";
            } else if (hle_registered) {
                resolution.status = "hle_registered_functionality_unknown";
            } else {
                const auto found = exports.find({symbol.name, symbol.library, symbol.module});
                std::vector<std::string> matched;
                if (found != exports.end()) {
                    for (const auto& [provider, exported] : found->second) {
                        if (scope.contains(provider) && exported->type == symbol.type &&
                            (!symbol.qualified ||
                             symbol.library_version == exported->library_version)) {
                            matched.push_back(provider);
                        }
                    }
                }
                const bool version_conflict =
                    std::ranges::any_of(analysis.edges, [&](const auto& edge) {
                        return edge.from == from &&
                               (edge.name == symbol.module || edge.name == symbol.library) &&
                               edge.status == "version_difference_requires_review";
                    });
                if (version_conflict) {
                    matched.clear();
                }
                if (matched.size() == 1) {
                    resolution.status = "matched_lle_export_not_loaded";
                    resolution.provider = matched.front();
                } else if (matched.size() > 1) {
                    resolution.status = "ambiguous_export";
                } else if (symbol.weak) {
                    resolution.status = "weak_undefined";
                }
            }
            Event(report, resolution.status.starts_with("hle_") ? "HLE" : "LLE", "match_import",
                  from + ":" + symbol.name, resolution.status);
            report.diagnostics.back().service =
                ServiceEvent{from,
                             symbol.library,
                             symbol.module,
                             {},
                             symbol.qualified ? symbol.name : std::string{},
                             resolution.status,
                             std::nullopt,
                             "no_guest_call"};
            analysis.imports.push_back(std::move(resolution));
        }
    }
    Event(report, "HLE", "inspect_registry", {},
          !registered_hle                 ? "registry_not_connected_runtime_unknown"
          : analysis.hle_query_count != 0 ? "live_registry_queried_implementation_unknown"
                                          : "registry_connected_no_eligible_imports");
    Event(report, "LLE", "inspect_inventory", {}, "metadata_only_no_modules_loaded");
}

} // namespace PiStation
