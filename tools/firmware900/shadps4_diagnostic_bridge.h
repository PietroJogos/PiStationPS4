// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "common/logging/log.h"
#include "core/loader/symbols_resolver.h"
#include "firmware900_dependency_analyzer.h"
#include "source_service_catalog.h"

namespace PiStation {

inline DiagnosticSink Shadps4DiagnosticSink() {
    return [](const Diagnostic& diagnostic) {
        const auto text = Firmware900Manager::JsonString(diagnostic.message);
        const auto related = Firmware900Manager::JsonString(diagnostic.related);
        const auto service =
            diagnostic.service
                ? " requester=" + Firmware900Manager::JsonString(diagnostic.service->requester) +
                      " library=" + Firmware900Manager::JsonString(diagnostic.service->library) +
                      " module=" + Firmware900Manager::JsonString(diagnostic.service->module) +
                      " function=" + Firmware900Manager::JsonString(diagnostic.service->function) +
                      " nid=" + Firmware900Manager::JsonString(diagnostic.service->nid) +
                      " return_code=not_called reason=" +
                      Firmware900Manager::JsonString(diagnostic.service->reason)
                : std::string{};
        if (diagnostic.category == "ERROR") {
            LOG_ERROR(Core_Linker, "[{}] operation={} related={} result={} {}{}",
                      diagnostic.category, diagnostic.operation, related, diagnostic.result, text,
                      service);
        } else {
            LOG_INFO(Core_Linker, "[{}] operation={} related={} result={} {}{}",
                     diagnostic.category, diagnostic.operation, related, diagnostic.result, text,
                     service);
        }
    };
}

inline HleLookup Shadps4HleLookup(const Core::Loader::SymbolsResolver& symbols) {
    return [&symbols](const SymbolMetadata& symbol) {
        if ((symbol.type != 0 && symbol.type != 1 && symbol.type != 2) ||
            (symbol.qualified &&
             (!ValidNidSyntax(symbol.name) || symbol.library.empty() || symbol.module.empty())) ||
            (!symbol.qualified && symbol.name.find('#') != std::string::npos)) {
            return false;
        }
        Core::Loader::SymbolResolver query{};
        query.name = symbol.name;
        query.library = symbol.library;
        query.library_version = symbol.library_version;
        query.module = symbol.module;
        query.type = symbol.type == 2   ? Core::Loader::SymbolType::Function
                     : symbol.type == 1 ? Core::Loader::SymbolType::Object
                                        : Core::Loader::SymbolType::NoType;
        const auto* record = symbols.FindSymbol(query);
        return record && record->virtual_address != 0;
    };
}

} // namespace PiStation
